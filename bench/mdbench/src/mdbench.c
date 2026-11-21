// SPDX-License-Identifier: MIT
/*
 * mdbench: synthetic Vulkan 1.0/1.1 benchmark that runs identically on any ICD (MaliBridge, PanVK, our open driver, lavapipe).
 * Single C11 file. Needs only VK_KHR_swapchain + VK_KHR_wayland_surface (or headless: offscreen target + readback).
 * Loader: dlopen("libvulkan.so.1") by default, or --icd <lib.so> to dlopen an ICD directly (loader-free, like t_vk_present).
 * See bench/README.md. Output: one JSON object on stdout (line starting {"mdbench"), progress on stderr, optional CSV.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define VK_NO_PROTOTYPES
#ifdef MDB_WAYLAND
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#endif
#include <vulkan/vulkan.h>
#include "spv_embed.h"

/* ------------------------------------------------------------------ util */
static double now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
static const char *g_phase = "init";
static void fail(const char *fmt, ...)
{
   char msg[512];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(msg, sizeof msg, fmt, ap);
   va_end(ap);
   for (char *c = msg; *c; c++) if (*c == '"' || *c == '\\' || *c == '\n') *c = ' ';
   printf("{\"mdbench\":{\"error\":\"%s\",\"phase\":\"%s\"},\"pass\":false}\n", msg, g_phase);
   fflush(stdout);
   fprintf(stderr, "mdbench: FAIL (%s): %s\n", g_phase, msg);
   _exit(1);
}
#define FAIL(...) fail(__VA_ARGS__)
#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) FAIL("%s = %d (line %d)", #x, (int)r_, __LINE__); } while (0)

static uint32_t rng_state = 1;
static uint32_t rnd(void) { uint32_t x = rng_state; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return rng_state = x; }
static float rndf(void) { return (rnd() & 0xffffff) / 16777216.0f; }
static uint32_t hash32(uint32_t x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }

typedef float mat4[16]; /* column-major */
static void m_ident(mat4 m) { memset(m, 0, sizeof(mat4)); m[0] = m[5] = m[10] = m[15] = 1; }
static void m_mul(mat4 o, const mat4 a, const mat4 b)
{
   mat4 t;
   for (int c = 0; c < 4; c++)
      for (int r = 0; r < 4; r++) {
         float s = 0;
         for (int k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
         t[c * 4 + r] = s;
      }
   memcpy(o, t, sizeof t);
}
static void m_persp(mat4 m, float fovy, float asp, float n, float f)
{
   float t = 1.0f / tanf(fovy * 0.5f);
   memset(m, 0, sizeof(mat4));
   m[0] = t / asp; m[5] = -t; /* Vulkan: y down */
   m[10] = f / (n - f); m[11] = -1; m[14] = n * f / (n - f);
}
static void m_roty(mat4 m, float a) { m_ident(m); m[0] = cosf(a); m[2] = -sinf(a); m[8] = sinf(a); m[10] = cosf(a); }
static void m_rotx(mat4 m, float a) { m_ident(m); m[5] = cosf(a); m[6] = sinf(a); m[9] = -sinf(a); m[10] = cosf(a); }
static void m_trans(mat4 m, float x, float y, float z) { m_ident(m); m[12] = x; m[13] = y; m[14] = z; }
static void m_scale(mat4 m, float s) { m_ident(m); m[0] = m[5] = m[10] = s; }

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

/* ------------------------------------------------------------------ options */
enum { SC_CUBE, SC_DRAWCALLS, SC_OVERDRAW, SC_ALU, SC_TEX, SC_COMPUTE, SC_MIXED, SC_STREAM, SC_STATE, SC_N };
static const char *scene_names[] = {"cube", "drawcalls", "overdraw", "alu", "tex", "compute", "mixed", "stream", "state"};
static struct {
   int scene;
   const char *wsi;       /* auto|wayland|headless */
   const char *mode;      /* fifo|mailbox|immediate */
   int w, h;
   double seconds;
   long frames;
   int warmup;
   const char *dump_ppm, *icd, *loader, *spv_dir, *json_path, *csv_path, *label;
   double gpu_scale, cpu_scale;
   long n_draws, layers, iters, tex, groups, stream;     /* base knobs (-1 = scene default) */
   double cpu_work;                              /* base CPU busy work in 1000-iteration units (-1 = scene default) */
   int compute_iters, tex_layers, tex_size, max_draws, max_groups;
   const char *filter;
   int auto_scale; const char *auto_axis;        /* 0/1, gpu|cpu|"" (scene default) */
   double target_fps, step_secs;
   int start_level, max_level;
   int fif, images, readback_every, fullscreen, gpu_time, no_teardown;
   int draw_idx;                                 /* indices per draw (36 = whole cube, 3 = one triangle) */
} O = {.scene = SC_CUBE, .wsi = "auto", .mode = "fifo", .seconds = 10, .warmup = 30, .gpu_scale = 1, .cpu_scale = 1,
       .n_draws = -1, .layers = -1, .iters = -1, .tex = -1, .groups = -1, .stream = -1, .cpu_work = -1, .compute_iters = 16, .tex_layers = 16,
       .tex_size = 256, .max_draws = 20000, .max_groups = 4096, .filter = "linear", .auto_axis = "", .target_fps = 60,
       .step_secs = 3, .max_level = 48, .fif = 2, .fullscreen = 1, .gpu_time = 1, .draw_idx = 36};

static void usage(void)
{
   fprintf(stderr,
      "usage: mdbench [--scene NAME[:K]] [options]\n"
      "  scenes: cube | drawcalls[:N] | overdraw[:L] | alu[:K] | tex[:T] | compute[:C] | mixed |\n"
      "          stream[:U] (U x 4 MB texture uploads/frame, then sampled) | state[:N] (N small draws with pipeline/descriptor/push/scissor/vertex-buffer changes, Unity/DXVK-like)\n"
      "  --n N --layers L --iters K --tex T --groups C --stream U   base knobs (override scene[:K]); --cpu-work U (x1000 busy iterations/frame)\n"
      "  --gpu-scale X   scales layers/iters/tex/groups;  --cpu-scale X   scales draws and the CPU busy work\n"
      "  --auto-scale [gpu|cpu] --target-fps F --step-secs S --start-level L --max-level M   ramp 2^(level/4) until fps < target\n"
      "  --mode fifo|mailbox|immediate  --wsi auto|wayland|headless  --size WxH  --windowed  --images N  --frames-in-flight N\n"
      "  --seconds S | --frames N (measured frames)  --warmup N  --readback N (headless: full readback every N frames)\n"
      "  --icd lib.so (direct ICD)  --loader libvulkan.so.1  --spv-dir dir (override embedded shaders)\n"
      "  --filter nearest|linear|aniso  --tex-size P  --tex-layers N  --compute-iters N  --max-draws N  --max-groups N  --draw-idx I (indices per draw, 3 = triangle)\n"
      "  --json file  --csv file  --dump-ppm file (headless: last frame)  --label text  --gpu-time on|off|auto  --no-teardown\n");
   exit(2);
}

static void parse_args(int argc, char **argv)
{
   for (int i = 1; i < argc; i++) {
      const char *a = argv[i];
#define IS(s) (!strcmp(a, s))
#define NEXT() (i + 1 < argc ? argv[++i] : (usage(), (char *)0))
      if (IS("--scene")) {
         char *v = strdup(NEXT());
         char *col = strchr(v, ':');
         if (col) { *col = 0; long k = atol(col + 1);
            O.scene = -1;
            for (int s = 0; s < SC_N; s++) if (!strcmp(v, scene_names[s])) O.scene = s;
            if (O.scene == SC_DRAWCALLS) O.n_draws = k; else if (O.scene == SC_OVERDRAW) O.layers = k;
            else if (O.scene == SC_ALU) O.iters = k; else if (O.scene == SC_TEX) O.tex = k; else if (O.scene == SC_COMPUTE) O.groups = k;
            else if (O.scene == SC_STREAM) O.stream = k; else if (O.scene == SC_STATE) O.n_draws = k;
         } else { O.scene = -1; for (int s = 0; s < SC_N; s++) if (!strcmp(v, scene_names[s])) O.scene = s; }
         if (O.scene < 0) usage();
      }
      else if (IS("--wsi")) O.wsi = NEXT();
      else if (IS("--mode")) O.mode = NEXT();
      else if (IS("--size")) { if (sscanf(NEXT(), "%dx%d", &O.w, &O.h) != 2) usage(); }
      else if (IS("--seconds")) O.seconds = atof(NEXT());
      else if (IS("--frames")) O.frames = atol(NEXT());
      else if (IS("--warmup")) O.warmup = atoi(NEXT());
      else if (IS("--icd")) O.icd = NEXT();
      else if (IS("--dump-ppm")) O.dump_ppm = NEXT();
      else if (IS("--loader")) O.loader = NEXT();
      else if (IS("--spv-dir")) O.spv_dir = NEXT();
      else if (IS("--json")) O.json_path = NEXT();
      else if (IS("--csv")) O.csv_path = NEXT();
      else if (IS("--label")) O.label = NEXT();
      else if (IS("--gpu-scale")) O.gpu_scale = atof(NEXT());
      else if (IS("--cpu-scale")) O.cpu_scale = atof(NEXT());
      else if (IS("--n")) O.n_draws = atol(NEXT());
      else if (IS("--layers")) O.layers = atol(NEXT());
      else if (IS("--iters")) O.iters = atol(NEXT());
      else if (IS("--tex")) O.tex = atol(NEXT());
      else if (IS("--groups")) O.groups = atol(NEXT());
      else if (IS("--stream")) O.stream = atol(NEXT());
      else if (IS("--cpu-work")) O.cpu_work = atof(NEXT());
      else if (IS("--compute-iters")) O.compute_iters = atoi(NEXT());
      else if (IS("--tex-layers")) O.tex_layers = atoi(NEXT());
      else if (IS("--tex-size")) O.tex_size = atoi(NEXT());
      else if (IS("--max-draws")) O.max_draws = atoi(NEXT());
      else if (IS("--max-groups")) O.max_groups = atoi(NEXT());
      else if (IS("--filter")) O.filter = NEXT();
      else if (IS("--auto-scale")) { O.auto_scale = 1; if (i + 1 < argc && (!strcmp(argv[i + 1], "gpu") || !strcmp(argv[i + 1], "cpu"))) O.auto_axis = argv[++i]; }
      else if (IS("--target-fps")) O.target_fps = atof(NEXT());
      else if (IS("--step-secs")) O.step_secs = atof(NEXT());
      else if (IS("--start-level")) O.start_level = atoi(NEXT());
      else if (IS("--max-level")) O.max_level = atoi(NEXT());
      else if (IS("--frames-in-flight")) O.fif = atoi(NEXT());
      else if (IS("--images")) O.images = atoi(NEXT());
      else if (IS("--readback")) O.readback_every = atoi(NEXT());
      else if (IS("--windowed")) O.fullscreen = 0;
      else if (IS("--fullscreen")) O.fullscreen = 1;
      else if (IS("--gpu-time")) { const char *v = NEXT(); O.gpu_time = !strcmp(v, "off") ? 0 : !strcmp(v, "on") ? 2 : 1; }
      else if (IS("--no-teardown")) O.no_teardown = 1;
      else if (IS("--draw-idx")) { O.draw_idx = atoi(NEXT()); if (O.draw_idx < 3) O.draw_idx = 3; if (O.draw_idx > 36) O.draw_idx = 36; }
      else usage();
   }
   if (O.fif < 1) O.fif = 1;
   if (O.fif > 4) O.fif = 4;
   if (O.tex_layers < 1) O.tex_layers = 1;
   if (O.tex_size < 4) O.tex_size = 4;
   if (O.max_draws < 1) O.max_draws = 1;
}

/* effective load for a given (gpu_scale, cpu_scale) */
typedef struct { int draws, layers, iters, tex, groups, stream; double cpu_iters; int cubes, particles_on, fs_blend; } Load;
#define MAX_STREAM 8                  /* uploads of STREAM_BYTES per frame (cap) */
#define STREAM_DIM 1024              /* 1024x1024 RGBA8 = 4 MiB per upload */
#define STREAM_BYTES (STREAM_DIM * STREAM_DIM * 4)
static long ovr(long v, long d) { return v >= 0 ? v : d; }
static long scaled(long base, double s, long minv) { long v = (long)floor(base * s + 0.5); return v < minv ? minv : v; }
static Load make_load(double gs, double cs, int *capped)
{
   Load l = {0};
   long N = 0, L = 0, K = 0, T = 0, C = 0, S = 0; double cw = 0;
   switch (O.scene) {
   case SC_CUBE: N = 1; break;
   case SC_DRAWCALLS: N = ovr(O.n_draws, 1000); break;
   case SC_OVERDRAW: L = ovr(O.layers, 8); break;
   case SC_ALU: K = ovr(O.iters, 64); break;
   case SC_TEX: T = ovr(O.tex, 8); break;
   case SC_COMPUTE: C = ovr(O.groups, 256); break;
   case SC_STREAM: S = ovr(O.stream, 1); break;
   case SC_STATE: N = ovr(O.n_draws, 2000); break;
   case SC_MIXED: N = ovr(O.n_draws, 300); L = ovr(O.layers, 2); K = ovr(O.iters, 16); T = ovr(O.tex, 4); C = ovr(O.groups, 64); cw = 100; break;
   }
   /* explicit knobs also work on the scenes that would not use them (e.g. cube + --layers 2) */
   if (O.scene != SC_MIXED) {
      if (O.n_draws >= 0 && O.scene != SC_DRAWCALLS && O.scene != SC_STATE) N = O.n_draws;
      if (O.layers >= 0 && O.scene != SC_OVERDRAW) L = O.layers;
      if (O.iters >= 0 && O.scene != SC_ALU) K = O.iters;
      if (O.tex >= 0 && O.scene != SC_TEX) T = O.tex;
      if (O.groups >= 0 && O.scene != SC_COMPUTE) C = O.groups;
      if (O.stream >= 0 && O.scene != SC_STREAM) S = O.stream;
   }
   if (O.cpu_work >= 0) cw = O.cpu_work;
   l.draws = N ? (int)scaled(N, N == 1 && O.scene == SC_CUBE ? 1 : cs, 1) : 0;
   if (O.scene == SC_CUBE) l.draws = 1;
   if (l.draws > O.max_draws) { l.draws = O.max_draws; if (capped) *capped |= 1; }
   l.layers = L ? (int)scaled(L, gs, 1) : 0;
   l.iters = K ? (int)scaled(K, gs, 1) : 0;
   l.tex = T ? (int)scaled(T, gs, 1) : 0;
   l.stream = S ? (int)scaled(S, gs, 1) : 0;
   if (l.stream > MAX_STREAM) { l.stream = MAX_STREAM; if (capped) *capped |= 4; }
   l.groups = C ? (int)scaled(C, gs, 1) : 0;
   if (l.groups > O.max_groups) { l.groups = O.max_groups; if (capped) *capped |= 2; }
   l.cpu_iters = cw * 1000.0 * cs;
   l.cubes = l.draws > 0;
   l.particles_on = l.groups > 0;
   l.fs_blend = (O.scene == SC_MIXED);
   return l;
}

/* ------------------------------------------------------------------ vulkan function tables */
#define INST_FNS(X) X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceMemoryProperties) \
   X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceFeatures) X(GetPhysicalDeviceFormatProperties) X(CreateDevice) \
   X(GetDeviceProcAddr) X(EnumerateDeviceExtensionProperties)
#define SURF_FNS(X) X(DestroySurfaceKHR) X(GetPhysicalDeviceSurfaceSupportKHR) X(GetPhysicalDeviceSurfaceCapabilitiesKHR) \
   X(GetPhysicalDeviceSurfaceFormatsKHR) X(GetPhysicalDeviceSurfacePresentModesKHR)
#define DEV_FNS(X) \
   X(DestroyDevice) X(GetDeviceQueue) X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory) X(CreateFence) X(DestroyFence) \
   X(WaitForFences) X(ResetFences) X(QueueSubmit) X(DeviceWaitIdle) X(CreateSemaphore) X(DestroySemaphore) X(CreateImage) \
   X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) X(CreateImageView) X(DestroyImageView) X(CreateBuffer) \
   X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(CreateShaderModule) X(DestroyShaderModule) \
   X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) \
   X(DestroyFramebuffer) X(CreateGraphicsPipelines) X(CreateComputePipelines) X(DestroyPipeline) X(CreateCommandPool) \
   X(DestroyCommandPool) X(AllocateCommandBuffers) X(ResetCommandBuffer) X(BeginCommandBuffer) X(EndCommandBuffer) \
   X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdBindPipeline) X(CmdSetViewport) X(CmdSetScissor) X(CmdBindVertexBuffers) \
   X(CmdBindIndexBuffer) X(CmdPushConstants) X(CmdDraw) X(CmdDrawIndexed) X(CmdDispatch) X(CmdPipelineBarrier) \
   X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) X(CmdClearColorImage) X(CmdFillBuffer) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
   X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateSampler) \
   X(DestroySampler) X(CmdBindDescriptorSets) X(CreateQueryPool) X(DestroyQueryPool) X(CmdResetQueryPool) \
   X(CmdWriteTimestamp) X(GetQueryPoolResults) X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) \
   X(AcquireNextImageKHR) X(QueuePresentKHR)
#define DECL(n) static PFN_vk##n n;
INST_FNS(DECL) SURF_FNS(DECL) DEV_FNS(DECL)
static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2_p;
#ifdef MDB_WAYLAND
static PFN_vkCreateWaylandSurfaceKHR CreateWaylandSurfaceKHR_p;
#endif

static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memprops;
static VkPhysicalDeviceProperties pprops;
static VkPhysicalDeviceFeatures pfeat;

/* ------------------------------------------------------------------ wayland */
static int g_headless = 1;
#ifdef MDB_WAYLAND
static struct {
   struct wl_display *dpy; struct wl_registry *reg; struct wl_compositor *comp; struct xdg_wm_base *wm;
   struct wl_surface *surf; struct xdg_surface *xs; struct xdg_toplevel *top;
   int configured, closed, cfg_w, cfg_h;
} W;
static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t s) { (void)d; xdg_wm_base_pong(wm, s); }
static const struct xdg_wm_base_listener wm_l = {wm_ping};
static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) { (void)d; xdg_surface_ack_configure(xs, serial); W.configured = 1; }
static const struct xdg_surface_listener xs_l = {xs_configure};
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) { (void)d; (void)t; (void)st; W.cfg_w = w; W.cfg_h = h; }
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; W.closed = 1; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_l = {top_configure, top_close, top_bounds, top_caps};
static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
   (void)d;
   if (!strcmp(iface, "wl_compositor")) W.comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
   else if (!strcmp(iface, "xdg_wm_base")) { W.wm = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 2 ? ver : 2); xdg_wm_base_add_listener(W.wm, &wm_l, NULL); }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = {reg_global, reg_remove};
static int wl_setup(void)
{
   W.dpy = wl_display_connect(NULL);
   if (!W.dpy) return -1;
   W.reg = wl_display_get_registry(W.dpy);
   wl_registry_add_listener(W.reg, &reg_l, NULL);
   wl_display_roundtrip(W.dpy);
   wl_display_roundtrip(W.dpy);
   if (!W.comp || !W.wm) return -2;
   W.surf = wl_compositor_create_surface(W.comp);
   W.xs = xdg_wm_base_get_xdg_surface(W.wm, W.surf);
   xdg_surface_add_listener(W.xs, &xs_l, NULL);
   W.top = xdg_surface_get_toplevel(W.xs);
   xdg_toplevel_add_listener(W.top, &top_l, NULL);
   xdg_toplevel_set_title(W.top, "mdbench");
   xdg_toplevel_set_app_id(W.top, "mdbench");
   if (O.fullscreen) xdg_toplevel_set_fullscreen(W.top, NULL);
   wl_surface_commit(W.surf);
   for (int i = 0; i < 20 && !W.configured; i++) wl_display_roundtrip(W.dpy);
   return 0;
}
#endif

/* ------------------------------------------------------------------ device objects */
static const VkFormat DEPTH_FORMAT_PREF[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM};
static VkFormat color_fmt, depth_fmt;
static uint32_t TW, TH;
static VkSurfaceKHR surface;
static VkSwapchainKHR swapchain;
static VkPresentModeKHR pmode;
static VkRenderPass rp;
static VkCommandPool cpool;
static VkSampler sampler;
static VkDescriptorSetLayout dsl_gfx, dsl_cmp;
static VkPipelineLayout pl_gfx, pl_cmp;
static VkPipeline pipe_cube, pipe_overdraw, pipe_alu, pipe_alu_b, pipe_tex, pipe_tex_b, pipe_part, pipe_cmp, pipe_stream, pipe_st_nd, pipe_st_b;
static VkDescriptorPool dpool;
static uint32_t g_ubo_stride;
static VkDescriptorSet dset_str[8];

typedef struct { VkBuffer b; VkDeviceMemory m; void *p; VkDeviceSize size; } Buf;
typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; } Img;
static Buf str_stg[8];
static Img str_img[8];
static void *str_src;
#define MAXT 8
static uint32_t ntargets;
static Img tcolor[MAXT], tdepth[MAXT];       /* tcolor: headless owns; swapchain only view */
static VkImage sc_images[MAXT];
static VkFramebuffer tfb[MAXT];
static Buf readback_buf[MAXT];
#define MAXF 4
static int F;
static VkCommandBuffer cmdb[MAXF];
static VkFence fen[MAXF];
static VkSemaphore sem_acq[MAXF], sem_done[MAXT];
static VkDescriptorSet dset[MAXF], dset_cmp;
static Buf ubo[MAXF], vbuf, ibuf, pbuf;
static Img tex0, texa;
static VkQueryPool qpool;
static Buf g_sb, g_early;
static char g_grid[16 * 9 * 6 + 1];
static int have_ts;
static double ts_period_ms;
static uint64_t vmask = ~0ull;   /* timestampValidBits mask */
static int64_t slot_frame[MAXF];

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
   return UINT32_MAX;
}
static Buf mkbuf(VkDeviceSize size, VkBufferUsageFlags usage)
{
   Buf b = {.size = size};
   VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   VK(CreateBuffer(dev, &bi, NULL, &b.b));
   VkMemoryRequirements mr;
   GetBufferMemoryRequirements(dev, b.b, &mr);
   uint32_t t = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (t == UINT32_MAX) FAIL("no host-visible coherent memory type");
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t};
   VK(AllocateMemory(dev, &ai, NULL, &b.m));
   VK(BindBufferMemory(dev, b.b, b.m, 0));
   VK(MapMemory(dev, b.m, 0, VK_WHOLE_SIZE, 0, &b.p));
   return b;
}
static Img mkimg(VkFormat fmt, uint32_t w, uint32_t h, uint32_t mips, uint32_t layers, VkImageUsageFlags usage, VkImageAspectFlags aspect, int is_array)
{
   Img im = {0};
   VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = {w, h, 1},
                           .mipLevels = mips, .arrayLayers = layers, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                           .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   VK(CreateImage(dev, &ii, NULL, &im.img));
   VkMemoryRequirements mr;
   GetImageMemoryRequirements(dev, im.img, &mr);
   uint32_t t = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (t == UINT32_MAX) t = find_mem(mr.memoryTypeBits, 0);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t};
   VK(AllocateMemory(dev, &ai, NULL, &im.mem));
   VK(BindImageMemory(dev, im.img, im.mem, 0));
   VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im.img,
                               .viewType = is_array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                               .subresourceRange = {aspect, 0, mips, 0, layers}};
   VK(CreateImageView(dev, &vi, NULL, &im.view));
   return im;
}
static VkShaderModule mkshader(const char *name)
{
   const uint32_t *code = NULL; size_t sz = 0; void *own = NULL;
   if (O.spv_dir) {
      char path[512]; snprintf(path, sizeof path, "%s/%s.spv", O.spv_dir, name);
      FILE *f = fopen(path, "rb");
      if (!f) FAIL("cannot open %s", path);
      fseek(f, 0, SEEK_END); sz = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
      own = malloc(sz);
      if (fread(own, 1, sz, f) != sz) FAIL("short read %s", path);
      fclose(f); code = own;
   } else {
      char want[128]; snprintf(want, sizeof want, "%s.spv", name);
      for (size_t i = 0; i < sizeof spv_table / sizeof *spv_table; i++)
         if (!strcmp(spv_table[i].name, want)) { code = (const uint32_t *)spv_table[i].data; sz = spv_table[i].size; }
      if (!code) FAIL("no embedded shader %s", name);
   }
   VkShaderModuleCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sz, .pCode = code};
   VkShaderModule m;
   VK(CreateShaderModule(dev, &si, NULL, &m));
   free(own);
   return m;
}

/* kind: 0 none (fullscreen triangle), 1 cube, 2 particle instances */
static VkPipeline mkgfx(const char *vs, const char *fs, int kind, int depth, int blend)
{
   VkShaderModule v = mkshader(vs), f = mkshader(fs);
   VkPipelineShaderStageCreateInfo st[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = v, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = f, .pName = "main"}};
   VkVertexInputBindingDescription vb = {0, kind == 1 ? 32 : 16, kind == 2 ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
   VkVertexInputAttributeDescription va[3] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12}, {2, 0, VK_FORMAT_R32G32_SFLOAT, 24}};
   if (kind == 2) va[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
   VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = kind ? 1 : 0, .pVertexBindingDescriptions = &vb, .vertexAttributeDescriptionCount = kind == 1 ? 3 : kind == 2 ? 1 : 0, .pVertexAttributeDescriptions = va};
   VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
   VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
   VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = depth, .depthWriteEnable = depth,
      .depthCompareOp = VK_COMPARE_OP_LESS};
   VkPipelineColorBlendAttachmentState cba = {.blendEnable = blend, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask = 0xf};
   VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba};
   VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
   VkPipelineDynamicStateCreateInfo dsi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
   VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
      .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
      .pColorBlendState = &cb, .pDynamicState = &dsi, .layout = pl_gfx, .renderPass = rp, .subpass = 0};
   VkPipeline p;
   VkResult pr = CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gi, NULL, &p);
   if (pr != VK_SUCCESS && kind == 2) { /* instance-rate vertex input: optional for the ICD, only compute/mixed scenes need it */
      fprintf(stderr, "mdbench: particle pipeline (instance-rate vertex input) failed: %d\n", (int)pr); p = VK_NULL_HANDLE; pr = VK_SUCCESS; }
   VK(pr);
   DestroyShaderModule(dev, v, NULL); DestroyShaderModule(dev, f, NULL);
   return p;
}

/* ------------------------------------------------------------------ targets (swapchain or headless images) */
static void destroy_targets(void)
{
   if (!dev) return;
   DeviceWaitIdle(dev);
   for (uint32_t i = 0; i < ntargets; i++) {
      if (tfb[i]) DestroyFramebuffer(dev, tfb[i], NULL);
      if (tcolor[i].view) DestroyImageView(dev, tcolor[i].view, NULL);
      if (g_headless) { if (tcolor[i].img) DestroyImage(dev, tcolor[i].img, NULL); if (tcolor[i].mem) FreeMemory(dev, tcolor[i].mem, NULL); }
      DestroyImageView(dev, tdepth[i].view, NULL); DestroyImage(dev, tdepth[i].img, NULL); FreeMemory(dev, tdepth[i].mem, NULL);
      memset(&tcolor[i], 0, sizeof tcolor[i]); tfb[i] = 0;
   }
   ntargets = 0;
}
static void create_targets(void)
{
   VkImage imgs[MAXT];
   if (g_headless) ntargets = (uint32_t)F;
   else {
      VkSurfaceCapabilitiesKHR caps;
      VK(GetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps));
      if (caps.currentExtent.width != 0xffffffffu) { TW = caps.currentExtent.width; TH = caps.currentExtent.height; }
      uint32_t want = O.images ? (uint32_t)O.images : (pmode == VK_PRESENT_MODE_MAILBOX_KHR ? 3u : 2u);
      if (want < caps.minImageCount) want = caps.minImageCount;
      if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
      if (want > MAXT) want = MAXT;
      VkCompositeAlphaFlagBitsKHR ca = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
         : (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
      VkSwapchainCreateInfoKHR sci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface, .minImageCount = want, .imageFormat = color_fmt,
         .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, .imageExtent = {TW, TH}, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
         .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform, .compositeAlpha = ca, .presentMode = pmode, .clipped = VK_TRUE};
      VK(CreateSwapchainKHR(dev, &sci, NULL, &swapchain));
      VK(GetSwapchainImagesKHR(dev, swapchain, &ntargets, NULL));
      if (ntargets > MAXT) ntargets = MAXT;
      VK(GetSwapchainImagesKHR(dev, swapchain, &ntargets, imgs));
   }
   for (uint32_t i = 0; i < ntargets; i++) {
      if (g_headless) tcolor[i] = mkimg(color_fmt, TW, TH, 1, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | (getenv("MDB_CLEARTEST") ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0), VK_IMAGE_ASPECT_COLOR_BIT, 0);
      else {
         sc_images[i] = imgs[i];
         VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = imgs[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = color_fmt,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
         VK(CreateImageView(dev, &vi, NULL, &tcolor[i].view));
         tcolor[i].img = imgs[i];
      }
      VkImageAspectFlags da = VK_IMAGE_ASPECT_DEPTH_BIT;
      tdepth[i] = mkimg(depth_fmt, TW, TH, 1, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, da, 0);
      VkImageView at[2] = {tcolor[i].view, tdepth[i].view};
      VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 2, .pAttachments = at, .width = TW, .height = TH, .layers = 1};
      VK(CreateFramebuffer(dev, &fi, NULL, &tfb[i]));
      if (!sem_done[i]) { VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VK(CreateSemaphore(dev, &si, NULL, &sem_done[i])); }
   }
}

/* ------------------------------------------------------------------ textures */
static void gen_layer(uint8_t *px, int size, int layer)
{
   uint32_t seed = hash32((uint32_t)layer * 7919u + 17u);
   int cell = 4 << (seed & 3);
   for (int y = 0; y < size; y++)
      for (int x = 0; x < size; x++) {
         int chk = ((x / cell) + (y / cell)) & 1;
         uint32_t h = hash32((uint32_t)(x * 73856093) ^ (uint32_t)(y * 19349663) ^ seed);
         uint8_t *p = px + (y * size + x) * 4;
         p[0] = (uint8_t)((chk ? 220 : 60) + (h & 15) + (seed & 31));
         p[1] = (uint8_t)((chk ? 200 : 80) + ((h >> 4) & 15) + ((seed >> 5) & 31));
         p[2] = (uint8_t)((chk ? 180 : 100) + ((h >> 8) & 15) + ((seed >> 10) & 31));
         p[3] = 255;
      }
}
static void box_down(const uint8_t *src, int s, uint8_t *dst)
{
   int d = s / 2;
   if (d < 1) d = 1;
   for (int y = 0; y < d; y++)
      for (int x = 0; x < d; x++)
         for (int c = 0; c < 4; c++) {
            int sx = x * 2, sy = y * 2, sx1 = sx + 1 < s ? sx + 1 : sx, sy1 = sy + 1 < s ? sy + 1 : sy;
            dst[(y * d + x) * 4 + c] = (uint8_t)((src[(sy * s + sx) * 4 + c] + src[(sy * s + sx1) * 4 + c] + src[(sy1 * s + sx) * 4 + c] + src[(sy1 * s + sx1) * 4 + c] + 2) / 4);
         }
}
/* creates a mip-mapped RGBA8 image of `layers` layers (CPU-generated, uploaded via staging buffer) */
static Img upload_tex(int size, int layers, int seed_base, int is_array)
{
   int mips = 1;
   for (int s = size; s > 1; s >>= 1) mips++;
   size_t per_layer = 0;
   for (int m = 0, s = size; m < mips; m++, s = s > 1 ? s / 2 : 1) per_layer += (size_t)s * s * 4;
   Buf st = mkbuf(per_layer * layers, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
   VkBufferImageCopy *rg = calloc((size_t)mips * layers, sizeof *rg);
   int nr = 0;
   for (int l = 0; l < layers; l++) {
      uint8_t *base = (uint8_t *)st.p + per_layer * l;
      size_t off = 0; int s = size;
      gen_layer(base, size, seed_base + l);
      for (int m = 0; m < mips; m++) {
         rg[nr++] = (VkBufferImageCopy){.bufferOffset = per_layer * l + off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)m, (uint32_t)l, 1},
                                        .imageExtent = {(uint32_t)s, (uint32_t)s, 1}};
         if (m + 1 < mips) { box_down(base + off, s, base + off + (size_t)s * s * 4); off += (size_t)s * s * 4; s = s > 1 ? s / 2 : 1; }
      }
   }
   Img im = mkimg(VK_FORMAT_R8G8B8A8_UNORM, size, size, mips, layers, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, is_array);
   VkCommandBuffer cb;
   VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   VK(AllocateCommandBuffers(dev, &ai, &cb));
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   VK(BeginCommandBuffer(cb, &bi));
   VkImageMemoryBarrier b1 = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im.img,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)mips, 0, (uint32_t)layers}};
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
   CmdCopyBufferToImage(cb, st.b, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)nr, rg);
   VkImageMemoryBarrier b2 = b1;
   b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
   b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
   VK(EndCommandBuffer(cb));
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence f;
   VK(CreateFence(dev, &fi, NULL, &f));
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   VK(QueueSubmit(queue, 1, &si, f));
   VK(WaitForFences(dev, 1, &f, VK_TRUE, 30000000000ull));
   DestroyFence(dev, f, NULL);
   DestroyBuffer(dev, st.b, NULL); FreeMemory(dev, st.m, NULL);
   free(rg);
   return im;
}

/* ------------------------------------------------------------------ stats */
typedef struct { size_t n; double mean, min, p50, p90, p95, p99, max; } Pct;
static Pct pct(const double *v, size_t n)
{
   Pct p = {0};
   double *s = malloc((n ? n : 1) * sizeof *s);
   size_t m = 0;
   for (size_t i = 0; i < n; i++) if (v[i] == v[i]) s[m++] = v[i];
   p.n = m;
   if (m) {
      double sum = 0;
      for (size_t i = 0; i < m; i++) sum += s[i];
      qsort(s, m, sizeof *s, cmp_d);
#define Q(f) s[(size_t)ceil((f) * m) - 1 < m ? (size_t)ceil((f) * m) - 1 : m - 1]
      p.mean = sum / m; p.min = s[0]; p.max = s[m - 1]; p.p50 = Q(0.5); p.p90 = Q(0.9); p.p95 = Q(0.95); p.p99 = Q(0.99);
   }
   free(s);
   return p;
}
static void jpct(FILE *o, const char *name, Pct p, int last)
{
   if (name[0]) fprintf(o, "\"%s\":", name);
   if (!p.n) { fprintf(o, "null%s", last ? "" : ","); return; }
   fprintf(o, "{\"n\":%zu,\"mean\":%.4f,\"min\":%.4f,\"p50\":%.4f,\"p90\":%.4f,\"p95\":%.4f,\"p99\":%.4f,\"max\":%.4f}%s", p.n, p.mean, p.min, p.p50, p.p90, p.p95, p.p99, p.max, last ? "" : ",");
}

/* per-frame records */
static struct { size_t n, cap; double *t0, *ft, *rec, *sub, *pres, *wait, *work, *gpu, *gs, *ge; int16_t *lvl; uint8_t *warm; } R;
static void r_reserve(void)
{
   if (R.n < R.cap) return;
   R.cap = R.cap ? R.cap * 2 : 1 << 14;
#define G(a) R.a = realloc(R.a, R.cap * sizeof *R.a)
   G(t0); G(ft); G(rec); G(sub); G(pres); G(wait); G(work); G(gpu); G(gs); G(ge); G(lvl); G(warm);
}

static volatile uint64_t cpu_sink;
static void cpu_work(uint64_t iters)
{
   uint64_t x = 88172645463325252ull;
   for (uint64_t i = 0; i < iters; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; }
   cpu_sink = x;
}

/* ------------------------------------------------------------------ main */
typedef struct { int level; double scale; Load load; size_t f0, f1; double fps, low1, p95, p99; int pass; } Step;

int main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IOLBF, 0);
   parse_args(argc, argv);
   const char *axis = O.auto_axis[0] ? O.auto_axis : (O.scene == SC_DRAWCALLS || O.scene == SC_STATE ? "cpu" : "gpu");
   int axis_cpu = !strcmp(axis, "cpu");

   /* --- loader / ICD */
   g_phase = "loader";
   const char *loader_desc;
   VkResult (*negotiate)(uint32_t *) = NULL;
   if (O.icd) {
      void *lib = dlopen(O.icd, RTLD_NOW | RTLD_LOCAL);
      if (!lib) FAIL("dlopen icd: %s", dlerror());
      gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
      negotiate = (VkResult(*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
      uint32_t iv = 5;
      if (!gipa) FAIL("%s has no vk_icdGetInstanceProcAddr", O.icd);
      if (negotiate && negotiate(&iv)) FAIL("icd interface negotiation failed");
      loader_desc = "icd-direct";
   } else {
      void *lib = dlopen(O.loader ? O.loader : "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
      if (!lib) FAIL("dlopen loader: %s", dlerror());
      gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
      if (!gipa) FAIL("no vkGetInstanceProcAddr");
      loader_desc = "loader";
   }
   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   PFN_vkEnumerateInstanceExtensionProperties EnumIExt = (PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL, "vkEnumerateInstanceExtensionProperties");
   if (!CreateInstance || !EnumIExt) FAIL("missing global entry points");

   /* --- wsi choice */
   int want_wl = !strcmp(O.wsi, "wayland") || (!strcmp(O.wsi, "auto") && getenv("WAYLAND_DISPLAY"));
   uint32_t nie = 0;
   EnumIExt(NULL, &nie, NULL);
   VkExtensionProperties *ie = calloc(nie + 1, sizeof *ie);
   EnumIExt(NULL, &nie, ie);
   int has_surf = 0, has_wl = 0, has_p2 = 0;
   for (uint32_t i = 0; i < nie; i++) {
      has_surf |= !strcmp(ie[i].extensionName, "VK_KHR_surface");
      has_wl |= !strcmp(ie[i].extensionName, "VK_KHR_wayland_surface");
      has_p2 |= !strcmp(ie[i].extensionName, "VK_KHR_get_physical_device_properties2");
   }
#ifdef MDB_WAYLAND
   if (want_wl && !(has_surf && has_wl)) { if (!strcmp(O.wsi, "wayland")) FAIL("instance lacks VK_KHR_surface/VK_KHR_wayland_surface"); want_wl = 0; }
   if (want_wl) {
      int rc = wl_setup();
      if (rc) { if (!strcmp(O.wsi, "wayland")) FAIL("wayland setup failed (%d)", rc); want_wl = 0; }
   }
   g_headless = !want_wl;
#else
   if (!strcmp(O.wsi, "wayland")) FAIL("built without MDB_WAYLAND");
   g_headless = 1;
#endif
   if (g_headless) { if (!O.w) O.w = 1280; if (!O.h) O.h = 720; TW = O.w; TH = O.h; }
#ifdef MDB_WAYLAND
   else { TW = O.w ? O.w : (W.cfg_w ? W.cfg_w : 1280); TH = O.h ? O.h : (W.cfg_h ? W.cfg_h : 720); }
#endif

   /* --- instance */
   g_phase = "instance";
   const char *iexts[4]; uint32_t nix = 0;
   if (!g_headless) { iexts[nix++] = "VK_KHR_surface"; iexts[nix++] = "VK_KHR_wayland_surface"; }
   if (has_p2) iexts[nix++] = "VK_KHR_get_physical_device_properties2";
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "mdbench", .applicationVersion = 1, .apiVersion = VK_API_VERSION_1_0};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app, .enabledExtensionCount = nix, .ppEnabledExtensionNames = iexts};
   VK(CreateInstance(&ici, NULL, &inst));
#define ILOAD(n) n = (PFN_vk##n)gipa(inst, "vk" #n); if (!n) FAIL("missing vk" #n);
   INST_FNS(ILOAD)
   if (!g_headless) { SURF_FNS(ILOAD) }
   if (has_p2) {
      GetPhysicalDeviceProperties2_p = (PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2");
      if (!GetPhysicalDeviceProperties2_p) GetPhysicalDeviceProperties2_p = (PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2KHR");
   }
   uint32_t cnt = 1;
   VkResult r = EnumeratePhysicalDevices(inst, &cnt, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || cnt < 1) FAIL("no physical device");
   GetPhysicalDeviceProperties(pd, &pprops);
   GetPhysicalDeviceMemoryProperties(pd, &memprops);
   GetPhysicalDeviceFeatures(pd, &pfeat);

   /* --- surface */
#ifdef MDB_WAYLAND
   if (!g_headless) {
      CreateWaylandSurfaceKHR_p = (PFN_vkCreateWaylandSurfaceKHR)gipa(inst, "vkCreateWaylandSurfaceKHR");
      if (!CreateWaylandSurfaceKHR_p) FAIL("missing vkCreateWaylandSurfaceKHR");
      VkWaylandSurfaceCreateInfoKHR wsi = {.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR, .display = W.dpy, .surface = W.surf};
      VK(CreateWaylandSurfaceKHR_p(inst, &wsi, NULL, &surface));
      VkBool32 sup = VK_FALSE;
      VK(GetPhysicalDeviceSurfaceSupportKHR(pd, 0, surface, &sup));
      if (!sup) FAIL("queue family 0 cannot present");
      uint32_t nf = 16, np = 8;
      VkSurfaceFormatKHR fm[16]; VkPresentModeKHR pm[8];
      VK(GetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &nf, fm));
      VK(GetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &np, pm));
      color_fmt = fm[0].format;
      static const VkFormat pref[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
      for (int p = 1; p >= 0; p--) for (uint32_t i = 0; i < nf; i++) if (fm[i].format == pref[p]) color_fmt = pref[p];
      for (uint32_t i = 0; i < nf; i++) if (fm[i].format == pref[0]) color_fmt = pref[0];
      VkPresentModeKHR want = !strcmp(O.mode, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR : !strcmp(O.mode, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
      pmode = VK_PRESENT_MODE_FIFO_KHR;
      for (uint32_t i = 0; i < np; i++) if (pm[i] == want) pmode = want;
   }
#endif
   if (g_headless) color_fmt = VK_FORMAT_R8G8B8A8_UNORM;
   depth_fmt = VK_FORMAT_UNDEFINED;
   for (unsigned i = 0; i < 3 && depth_fmt == VK_FORMAT_UNDEFINED; i++) {
      VkFormatProperties fp;
      GetPhysicalDeviceFormatProperties(pd, DEPTH_FORMAT_PREF[i], &fp);
      if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) depth_fmt = DEPTH_FORMAT_PREF[i];
   }
   if (depth_fmt == VK_FORMAT_UNDEFINED) FAIL("no depth format");

   /* driver identification (VK_KHR_driver_properties when available) */
   uint32_t nde = 0;
   EnumerateDeviceExtensionProperties(pd, NULL, &nde, NULL);
   VkExtensionProperties *de = calloc(nde + 1, sizeof *de);
   EnumerateDeviceExtensionProperties(pd, NULL, &nde, de);
   int has_swap = 0, has_drvprops = 0;
   for (uint32_t i = 0; i < nde; i++) {
      has_swap |= !strcmp(de[i].extensionName, "VK_KHR_swapchain");
      has_drvprops |= !strcmp(de[i].extensionName, "VK_KHR_driver_properties");
   }
   if (!g_headless && !has_swap) FAIL("device lacks VK_KHR_swapchain");
   VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
   int have_drv = 0;
   if (GetPhysicalDeviceProperties2_p && has_drvprops) {
      VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
      GetPhysicalDeviceProperties2_p(pd, &p2);
      have_drv = 1;
   }

   /* --- device */
   g_phase = "device";
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   const char *dext[1] = {"VK_KHR_swapchain"};
   VkPhysicalDeviceFeatures en = {0};
   int aniso_ok = pfeat.samplerAnisotropy && !strcmp(O.filter, "aniso");
   en.samplerAnisotropy = aniso_ok;
   VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi, .enabledExtensionCount = g_headless ? 0 : 1,
                            .ppEnabledExtensionNames = dext, .pEnabledFeatures = &en};
   VK(CreateDevice(pd, &di, NULL, &dev));
#define DLOAD(n) n = (PFN_vk##n)GetDeviceProcAddr(dev, "vk" #n); if (!n && !(g_headless && strstr(#n, "KHR")) && !strstr(#n, "Query") && !strstr(#n, "Timestamp")) FAIL("missing vk" #n);
   DEV_FNS(DLOAD)
   GetDeviceQueue(dev, 0, 0, &queue);
   F = O.fif;
   {
      uint32_t nq = 4; VkQueueFamilyProperties qf[4];
      GetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
      have_ts = O.gpu_time && CreateQueryPool && CmdWriteTimestamp && CmdResetQueryPool && GetQueryPoolResults && DestroyQueryPool && nq && qf[0].timestampValidBits > 0 && pprops.limits.timestampPeriod > 0;
      if (O.gpu_time == 2 && !have_ts) FAIL("--gpu-time on but no timestamps");
      ts_period_ms = pprops.limits.timestampPeriod / 1e6;
      vmask = qf[0].timestampValidBits >= 64 ? ~0ull : ((1ull << qf[0].timestampValidBits) - 1);
      if (!(qf[0].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !(qf[0].queueFlags & VK_QUEUE_COMPUTE_BIT)) FAIL("queue family 0 lacks graphics+compute");
   }
   g_ubo_stride = (uint32_t)pprops.limits.minUniformBufferOffsetAlignment;
   if (g_ubo_stride < 1) g_ubo_stride = 1;
   g_ubo_stride = (160 + g_ubo_stride - 1) / g_ubo_stride * g_ubo_stride;

   /* --- render pass */
   g_phase = "setup";
   VkAttachmentDescription ad[2] = {
      {.format = color_fmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
       .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
       .finalLayout = g_headless ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR},
      {.format = depth_fmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
       .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
       .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
   VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &cref, .pDepthStencilAttachment = &dref};
   VkSubpassDependency dep[2] = {
      {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 
       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, 0},
      {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, g_headless ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, g_headless ? VK_ACCESS_TRANSFER_READ_BIT : 0, 0}};
   VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2, .pAttachments = ad, .subpassCount = 1, .pSubpasses = &sp,
                                 .dependencyCount = 2, .pDependencies = dep};
   VK(CreateRenderPass(dev, &rpi, NULL, &rp));
   VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
   VK(CreateCommandPool(dev, &cpi, NULL, &cpool));

   /* --- descriptors / layouts */
   VkDescriptorSetLayoutBinding b[3] = {
      {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
      {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
   VK(CreateDescriptorSetLayout(dev, &dli, NULL, &dsl_gfx));
   VkDescriptorSetLayoutBinding bc = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
   VkDescriptorSetLayoutCreateInfo dlc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &bc};
   VK(CreateDescriptorSetLayout(dev, &dlc, NULL, &dsl_cmp));
   VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl_gfx, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
   VK(CreatePipelineLayout(dev, &pli, NULL, &pl_gfx));
   VkPushConstantRange pcc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
   VkPipelineLayoutCreateInfo plc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl_cmp, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcc};
   VK(CreatePipelineLayout(dev, &plc, NULL, &pl_cmp));
   VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2u * F}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4u * F}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2u * (uint32_t)F + 1, .poolSizeCount = 3, .pPoolSizes = ps};
   VK(CreateDescriptorPool(dev, &dpi, NULL, &dpool));

   /* --- pipelines */
   g_phase = "pipelines";
   pipe_cube = mkgfx("cube.vert", "cube.frag", 1, 1, 0);
   pipe_overdraw = mkgfx("fs.vert", "overdraw.frag", 0, 0, 1);
   pipe_alu = mkgfx("fs.vert", "alu.frag", 0, 0, 0);
   pipe_alu_b = mkgfx("fs.vert", "alu.frag", 0, 0, 1);
   pipe_tex = mkgfx("fs.vert", "tex.frag", 0, 0, 0);
   pipe_tex_b = mkgfx("fs.vert", "tex.frag", 0, 0, 1);
   pipe_stream = mkgfx("fs.vert", "stream.frag", 0, 0, 0);
   pipe_st_nd = mkgfx("cube.vert", "cube.frag", 1, 0, 0);
   pipe_st_b = mkgfx("cube.vert", "cube.frag", 1, 1, 1);
   pipe_part = mkgfx("particle.vert", "particle.frag", 2, 0, 1);
   if (!pipe_part && (O.scene == SC_COMPUTE || O.scene == SC_MIXED)) FAIL("particle pipeline: ICD rejects instance-rate vertex input (VK_ERROR_FEATURE_NOT_PRESENT)");
   {
      VkShaderModule cs = mkshader("particle.comp");
      VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl_cmp,
         .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cs, .pName = "main"}};
      VK(CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &pipe_cmp));
      DestroyShaderModule(dev, cs, NULL);
   }

   /* --- geometry / resources */
   g_phase = "resources";
   {
      static const float fn[6][3] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
      static const float fu[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}};
      static const float fv[6][3] = {{0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
      vbuf = mkbuf(24 * 32, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
      ibuf = mkbuf(36 * 2, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
      float *vv = vbuf.p; uint16_t *ii = ibuf.p;
      for (int f = 0; f < 6; f++) {
         for (int c = 0; c < 4; c++) {
            float su = (c & 1) ? 1 : -1, sv = (c & 2) ? 1 : -1;
            float *v = vv + (f * 4 + c) * 8;
            for (int k = 0; k < 3; k++) { v[k] = fn[f][k] + su * fu[f][k] + sv * fv[f][k]; v[3 + k] = fn[f][k]; }
            v[6] = (su + 1) * 0.5f; v[7] = (sv + 1) * 0.5f;
         }
         uint16_t base = (uint16_t)(f * 4);
         const uint16_t idx[6] = {0, 1, 3, 0, 3, 2};
         for (int k = 0; k < 6; k++) ii[f * 6 + k] = base + idx[k];
      }
      /* the winding is not meaningful: culling is disabled in every pipeline */
   }
   int max_groups = O.max_groups;
   size_t npart = (size_t)max_groups * 64;
   pbuf = mkbuf(npart * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
   rng_state = 0x1234567u;
   {
      float *p = pbuf.p;
      for (size_t i = 0; i < npart; i++) { p[i * 4] = rndf() * 2 - 1; p[i * 4 + 1] = rndf() * 2 - 1; p[i * 4 + 2] = rndf(); p[i * 4 + 3] = rndf(); }
   }
   for (int i = 0; i < F; i++) ubo[i] = mkbuf((VkDeviceSize)O.max_draws * g_ubo_stride, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
   int tl = O.tex_layers;
   tex0 = upload_tex(O.tex_size, 1, 1000, 0);
   texa = upload_tex(O.tex_size, tl, 2000, 1);
   VkSamplerCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT, .maxLod = 16.0f, .maxAnisotropy = 1.0f};
   if (!strcmp(O.filter, "nearest")) { smi.magFilter = smi.minFilter = VK_FILTER_NEAREST; smi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; }
   if (aniso_ok) { smi.anisotropyEnable = VK_TRUE; smi.maxAnisotropy = pprops.limits.maxSamplerAnisotropy < 8.0f ? pprops.limits.maxSamplerAnisotropy : 8.0f; }
   VK(CreateSampler(dev, &smi, NULL, &sampler));
   {
      VkDescriptorSetLayout ls[2 * MAXF + 1];
      VkDescriptorSet ds[2 * MAXF + 1];
      for (int i = 0; i < 2 * F; i++) ls[i] = dsl_gfx;
      ls[2 * F] = dsl_cmp;
      VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 2u * (uint32_t)F + 1, .pSetLayouts = ls};
      VK(AllocateDescriptorSets(dev, &ai, ds));
      for (int i = 0; i < F; i++) { dset[i] = ds[i]; dset_str[i] = ds[F + i]; }
      dset_cmp = ds[2 * F];
      const int use_str = O.scene == SC_STREAM || O.stream > 0;   /* --stream U also works on the other scenes: the same condition allocates and records */
      if (use_str) for (int i = 0; i < F; i++) {   /* per-frame staging buffer + sampled image (overwritten by the upload each frame) */
         str_stg[i] = mkbuf(STREAM_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
         str_img[i] = mkimg(VK_FORMAT_R8G8B8A8_UNORM, STREAM_DIM, STREAM_DIM, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0);
      }
      if (use_str) { str_src = malloc(STREAM_BYTES); uint32_t *q = (uint32_t *)str_src; for (size_t i = 0; i < STREAM_BYTES / 4; i++) q[i] = hash32((uint32_t)i) | 0xff000000u; }
      for (int i = 0; i < F; i++) {
         VkDescriptorBufferInfo bi = {ubo[i].b, 0, 160};
         VkDescriptorImageInfo i0 = {sampler, tex0.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, i1 = {sampler, texa.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
         VkWriteDescriptorSet w[3] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset[i], .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &bi},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset[i], .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &i0},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset[i], .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &i1}};
         UpdateDescriptorSets(dev, 3, w, 0, NULL);
         /* scene "stream": same layout, binding 1/2 = this frame's streamed image (written only when that scene runs) */
         VkImageView sv = use_str ? str_img[i].view : tex0.view;
         VkDescriptorImageInfo si0 = {sampler, sv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
         VkWriteDescriptorSet w2[3] = {w[0], w[1], w[2]};
         w2[0].dstSet = w2[1].dstSet = w2[2].dstSet = dset_str[i];
         w2[1].pImageInfo = &si0; w2[2].pImageInfo = &si0;
         UpdateDescriptorSets(dev, 3, w2, 0, NULL);
      }
      VkDescriptorBufferInfo pb = {pbuf.b, 0, VK_WHOLE_SIZE};
      VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset_cmp, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &pb};
      UpdateDescriptorSets(dev, 1, &w, 0, NULL);
   }
   VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = (uint32_t)F};
   VK(AllocateCommandBuffers(dev, &cai, cmdb));
   for (int i = 0; i < F; i++) {
      VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
      VK(CreateFence(dev, &fi, NULL, &fen[i]));
      VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      VK(CreateSemaphore(dev, &si, NULL, &sem_acq[i]));
      slot_frame[i] = -1;
   }
   if (have_ts) {
      VkQueryPoolCreateInfo qpi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = (uint32_t)F * 2};
      VK(CreateQueryPool(dev, &qpi, NULL, &qpool));
   }
   create_targets();
   uint64_t nonblank = 0, checksum = 0; double nb_frac = -1;
   if (g_headless && O.readback_every > 0) for (int i = 0; i < F; i++) readback_buf[i] = mkbuf((VkDeviceSize)TW * TH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
   Buf final_rb = {0};
   if (g_headless) final_rb = mkbuf((VkDeviceSize)TW * TH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
   (void)nonblank; (void)checksum;

   fprintf(stderr, "mdbench: %s | %s | %ux%u %s | scene %s%s\n", pprops.deviceName, loader_desc, TW, TH, g_headless ? "headless" : "wayland", scene_names[O.scene], O.auto_scale ? " (auto-scale)" : "");

   /* --- frame loop */
   g_phase = "run";
   if (getenv("MDB_EARLY")) {
      Buf sb = mkbuf(4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT); memset(sb.p, 0xAB, 4096); g_early = sb;
      VkCommandBufferAllocateInfo c2 = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
      VkCommandBuffer cb2; VK(AllocateCommandBuffers(dev, &c2, &cb2));
      VkCommandBufferBeginInfo bi2 = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      VK(BeginCommandBuffer(cb2, &bi2)); CmdFillBuffer(cb2, sb.b, 0, 4096, 0x11223344u); VK(EndCommandBuffer(cb2));
      VK(ResetFences(dev, 1, &fen[0]));
      VkSubmitInfo s2 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb2};
      VK(QueueSubmit(queue, 1, &s2, fen[0])); VK(WaitForFences(dev, 1, &fen[0], VK_TRUE, 20000000000ull));
      fprintf(stderr, "mdbench: early fill %08x\n", ((uint32_t *)sb.p)[1000]);
      if (getenv("MDB_EARLY")[0] == '2') {   /* empty render pass (clear only), then fill again */
         VkClearValue cv0[2] = {{.color = {{0.04f, 0.05f, 0.09f, 1.0f}}}, {.depthStencil = {1.0f, 0}}};
         VkCommandBuffer cb3; VK(AllocateCommandBuffers(dev, &c2, &cb3));
         VK(BeginCommandBuffer(cb3, &bi2));
         VkRenderPassBeginInfo rb0 = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = tfb[0], .renderArea = {{0, 0}, {TW, TH}}, .clearValueCount = 2, .pClearValues = cv0};
         CmdBeginRenderPass(cb3, &rb0, VK_SUBPASS_CONTENTS_INLINE); CmdEndRenderPass(cb3); VK(EndCommandBuffer(cb3));
         VK(ResetFences(dev, 1, &fen[0])); VkSubmitInfo s3 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb3};
         VK(QueueSubmit(queue, 1, &s3, fen[0])); VK(WaitForFences(dev, 1, &fen[0], VK_TRUE, 20000000000ull));
         memset(sb.p, 0xAB, 4096);
         VkCommandBuffer cb4; VK(AllocateCommandBuffers(dev, &c2, &cb4));
         VK(BeginCommandBuffer(cb4, &bi2)); CmdFillBuffer(cb4, sb.b, 0, 4096, 0x11223344u); VK(EndCommandBuffer(cb4));
         VK(ResetFences(dev, 1, &fen[0])); VkSubmitInfo s4 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb4};
         VK(QueueSubmit(queue, 1, &s4, fen[0])); VK(WaitForFences(dev, 1, &fen[0], VK_TRUE, 20000000000ull));
         fprintf(stderr, "mdbench: fill after empty renderpass %08x\n", ((uint32_t *)sb.p)[1000]);
      }
   }
   long total_frames = O.frames > 0 ? O.frames + O.warmup : 0;
   double t_loop = now_ms();
   int level = O.auto_scale ? O.start_level : -1;
   double gs = O.gpu_scale, cs = O.cpu_scale;
   int capped = 0;
   Step steps[256]; int nsteps = 0;
   int step_first = 0; double step_t0 = t_loop; size_t step_f0 = 0; int step_retry = 0; int best_step = -1; int auto_done = 0;
   double scale_base = 1.0;
   if (O.auto_scale) { scale_base = pow(2.0, level / 4.0); if (axis_cpu) cs = O.cpu_scale * scale_base; else gs = O.gpu_scale * scale_base; }
   Load cur = make_load(gs, cs, &capped);
   const float dt = 1.0f / 60.0f;
   int nsc_err = 0, nood = 0, nsub = 0;
   uint64_t frame = 0;
   mat4 view, proj, vp;
   (void)step_first;
   while (1) {
      double t0 = now_ms();
      if (!auto_done) {
         if (total_frames ? (long)frame >= total_frames : (O.auto_scale ? 0 : (t0 - t_loop) >= O.seconds * 1e3)) break;
      } else break;
#ifdef MDB_WAYLAND
      if (!g_headless) { wl_display_dispatch_pending(W.dpy); if (W.closed) break; }
#endif
      r_reserve();
      size_t k = R.n++;
      R.t0[k] = t0; R.ft[k] = NAN; R.rec[k] = R.sub[k] = R.pres[k] = R.wait[k] = R.work[k] = NAN; R.gpu[k] = NAN; R.gs[k] = R.ge[k] = NAN; R.lvl[k] = (int16_t)level;
      R.warm[k] = frame < (uint64_t)O.warmup;
      if (k) R.ft[k - 1] = t0 - R.t0[k - 1];

      /* auto-scale controller: evaluate a finished step */
      if (O.auto_scale && k > 0) {
         double el = t0 - step_t0;
         if (el >= O.step_secs * 1e3 && k - step_f0 >= 30) {
            size_t a = step_f0 + (k - step_f0) / 5, bnd = k - 1;  /* drop first 20 % as ramp-up; last frame has no ft yet */
            Pct p = pct(R.ft + a, bnd - a);
            double fps = p.mean > 0 ? 1000.0 / p.mean : 0, low = p.p99 > 0 ? 1000.0 / p.p99 : 0;
            int pass = fps >= O.target_fps * 0.97 && low >= O.target_fps * 0.75;
            Step *s = &steps[nsteps++];
            *s = (Step){level, scale_base, cur, a, bnd, fps, low, p.p95, p.p99, pass};
            fprintf(stderr, "mdbench: level %d scale %.3f draws %d layers %d iters %d tex %d groups %d: %.1f fps, 1%%low %.1f, p99 %.1f ms -> %s\n", level, scale_base, cur.draws, cur.layers,
                    cur.iters, cur.tex, cur.groups, fps, low, p.p99, pass ? "pass" : "FAIL");
            if (pass) { best_step = nsteps - 1; step_retry = 0; level++; if (level > O.max_level || nsteps >= 250) auto_done = 1; }
            else if (!step_retry && best_step >= 0 && level == steps[best_step].level + 1) step_retry = 1; /* one re-test of the failing level */
            else auto_done = 1;
            if (!auto_done) {
               scale_base = pow(2.0, level / 4.0);
               if (axis_cpu) cs = O.cpu_scale * scale_base; else gs = O.gpu_scale * scale_base;
               cur = make_load(gs, cs, &capped);
               step_t0 = now_ms(); step_f0 = k;
            }
            if (auto_done) break;
         }
      }

      int fi = (int)(frame % (uint64_t)F);
      VK(WaitForFences(dev, 1, &fen[fi], VK_TRUE, 20000000000ull));
      double t_w = now_ms();
      R.wait[k] = t_w - t0;
      if (have_ts && slot_frame[fi] >= 0) {
         uint64_t ts[2];
         if (GetQueryPoolResults(dev, qpool, (uint32_t)fi * 2, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            uint64_t mask = pprops.limits.timestampPeriod > 0 ? ~0ull : 0;
            R.gpu[slot_frame[fi]] = (double)((ts[1] - ts[0]) & mask) * ts_period_ms;
            R.gs[slot_frame[fi]] = (double)(ts[0] & vmask) * ts_period_ms; R.ge[slot_frame[fi]] = (double)(ts[1] & vmask) * ts_period_ms;   /* low timestampValidBits only; a counter narrower than 64 bits wraps (the CSV idle gap is meaningless across a wrap) */
            if (getenv("MDB_TSDBG") && (slot_frame[fi] % 30) == 0) fprintf(stderr, "mdbench: ts frame %ld t0 %llu t1 %llu d %llu wall %.3f ms\n", (long)slot_frame[fi], (unsigned long long)ts[0], (unsigned long long)ts[1], (unsigned long long)(ts[1] - ts[0]), now_ms());
         }
      }
      if (g_headless && O.readback_every > 0 && slot_frame[fi] >= 0 && (slot_frame[fi] % O.readback_every) == 0) {
         const uint8_t *px = readback_buf[fi].p;
         uint64_t h = 1469598103934665603ull, nb = 0, tot = 0;
         for (size_t i = 0; i < (size_t)TW * TH; i += 61) { uint32_t v; memcpy(&v, px + i * 4, 4); h = (h ^ v) * 1099511628211ull; nb += v != 0xff170d0au; tot++; }
         checksum = h; nb_frac = (double)nb / (double)tot;
      }

      /* ----- CPU simulation work + per-draw uniforms */
      double tc0 = now_ms();
      if (cur.cpu_iters > 0) cpu_work((uint64_t)cur.cpu_iters);
      R.work[k] = now_ms() - tc0;
      float t = (float)frame * dt;
      float asp = (float)TW / (float)TH;
      mat4 la; m_trans(la, 0, 0, -3.0f);
      m_persp(proj, 0.7854f, asp, 0.1f, 20.0f);
      m_mul(vp, proj, la);
      (void)view;
      uint8_t *ub = ubo[fi].p;
      int nd = cur.cubes ? cur.draws : 0;
      if (nd) {
         double hh = 2.0 * 3.0 * tan(0.7854 * 0.5), hw = hh * asp;
         int cols = (int)ceil(sqrt((double)nd * hw / hh)); if (cols < 1) cols = 1;
         int rows = (nd + cols - 1) / cols;
         float cell = (float)fmin(hw / cols, hh / rows);
         for (int i = 0; i < nd; i++) {
            uint32_t h = hash32((uint32_t)i + 12345u);
            float ph = (h & 0xffff) / 65536.0f, sp = 0.4f + ((h >> 16) & 0xff) / 255.0f;
            mat4 mr, mx, ms, mt, m, mv;
            m_roty(mr, t * sp + ph * 6.28f); m_rotx(mx, t * 0.5f * sp + ph * 3.0f);
            float sc = nd == 1 ? 0.7f : cell * 0.38f;
            m_scale(ms, sc);
            float px = nd == 1 ? 0 : ((i % cols) - (cols - 1) * 0.5f) * cell;
            float py = nd == 1 ? 0 : ((i / cols) - (rows - 1) * 0.5f) * cell;
            m_trans(mt, px, py, nd == 1 ? 0 : (((h >> 8) & 0xff) / 255.0f - 0.5f) * cell);
            m_mul(m, mx, ms); m_mul(m, mr, m); m_mul(m, mt, m);
            m_mul(mv, vp, m);
            float *u = (float *)(ub + (size_t)i * g_ubo_stride);
            memcpy(u, mv, 64); memcpy(u + 16, m, 64);
            u[32] = 0.4f + 0.6f * ((h >> 3) & 0xff) / 255.0f; u[33] = 0.4f + 0.6f * ((h >> 11) & 0xff) / 255.0f; u[34] = 0.4f + 0.6f * ((h >> 19) & 0xff) / 255.0f; u[35] = 1;
            u[36] = 0.5f; u[37] = 0.8f; u[38] = 0.6f; u[39] = 0;
         }
      }

      /* ----- acquire */
      uint32_t idx = (uint32_t)fi % (ntargets ? ntargets : 1);
      double tp0 = now_ms();
      if (!g_headless) {
         r = AcquireNextImageKHR(dev, swapchain, 20000000000ull, sem_acq[fi], VK_NULL_HANDLE, &idx);
         if (r == VK_ERROR_OUT_OF_DATE_KHR) { nood++; destroy_targets(); DestroySwapchainKHR(dev, swapchain, NULL); create_targets(); R.n--; continue; }
         if (r == VK_SUBOPTIMAL_KHR) nsub++; else if (r != VK_SUCCESS) FAIL("AcquireNextImageKHR = %d", (int)r);
      }
      double tp1 = now_ms();
      VK(ResetFences(dev, 1, &fen[fi]));

      /* ----- record */
      double tr0 = now_ms();
      VkCommandBuffer cb = cmdb[fi];
      ResetCommandBuffer(cb, 0);
      VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      VK(BeginCommandBuffer(cb, &cbi));
      if (have_ts) { CmdResetQueryPool(cb, qpool, (uint32_t)fi * 2, 2); CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, (uint32_t)fi * 2); }
      float pcv[8] = {0};
      if (cur.particles_on) {
         VkBufferMemoryBarrier bm = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = pbuf.b, .size = VK_WHOLE_SIZE};
         CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 1, &bm, 0, NULL);
         CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_cmp);
         CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl_cmp, 0, 1, &dset_cmp, 0, NULL);
         pcv[0] = t * 1000.0f; pcv[2] = (float)O.compute_iters; pcv[3] = (float)cur.groups * 64.0f;
         CmdPushConstants(cb, pl_cmp, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, pcv);
         CmdDispatch(cb, (uint32_t)cur.groups, 1, 1);
         bm.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; bm.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
         CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, NULL, 1, &bm, 0, NULL);
      }
      if (cur.stream) {
         memcpy(str_stg[fi].p, str_src, STREAM_BYTES);       /* CPU side of the upload: fresh 4 MiB written into host-visible memory every frame */
         ((uint64_t *)str_stg[fi].p)[0] = frame;
         VkImageMemoryBarrier ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_READ_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = str_img[fi].img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
         VkBufferImageCopy rg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {STREAM_DIM, STREAM_DIM, 1}};
         for (int u = 0; u < cur.stream; u++) {              /* u uploads of 4 MiB into the same image (WAW chain), the last one is sampled */
            CmdPipelineBarrier(cb, u ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
            CmdCopyBufferToImage(cb, str_stg[fi].b, str_img[fi].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
            ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
         }
         ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
         ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
         CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
      }
      VkClearValue cv[2] = {{.color = {{0.04f, 0.05f, 0.09f, 1.0f}}}, {.depthStencil = {1.0f, 0}}};
      VkRenderPassBeginInfo rb = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = tfb[idx], .renderArea = {{0, 0}, {TW, TH}}, .clearValueCount = 2, .pClearValues = cv};
      CmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
      VkViewport vpt = {0, 0, (float)TW, (float)TH, 0.0f, 1.0f};
      VkRect2D scr = {{0, 0}, {TW, TH}};
      CmdSetViewport(cb, 0, 1, &vpt);
      CmdSetScissor(cb, 0, 1, &scr);
      if (nd) {
         CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_cube);
         VkDeviceSize zo = 0;
         CmdBindVertexBuffers(cb, 0, 1, &vbuf.b, &zo);
         CmdBindIndexBuffer(cb, ibuf.b, 0, VK_INDEX_TYPE_UINT16);
         if (O.scene == SC_STATE) {
            /* Unity/DXVK-like: every draw changes pipeline (3 variants), descriptor set (2 sets, dynamic offset), push constants, index+vertex buffer binds;
               the scissor changes every 4th draw. Tiny draws (draw_idx indices) so the CPU/driver state path dominates, not the GPU. */
            VkPipeline pv[3] = {pipe_cube, pipe_st_nd, pipe_st_b};
            VkDeviceSize vo[2] = {0, 0};
            for (int i = 0; i < nd; i++) {
               uint32_t off = (uint32_t)i * g_ubo_stride;
               CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pv[i % 3]);
               CmdBindVertexBuffers(cb, 0, 1, &vbuf.b, &vo[i & 1]);
               CmdBindIndexBuffer(cb, ibuf.b, 0, VK_INDEX_TYPE_UINT16);
               CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_gfx, 0, 1, (i & 1) ? &dset_str[fi] : &dset[fi], 1, &off);
               float spc[8] = {(float)i, 0, 0, 0, 0, 0, 0, 0};
               CmdPushConstants(cb, pl_gfx, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, spc);
               if ((i & 3) == 0) { VkRect2D sc2 = {{0, 0}, {TW > (uint32_t)((i >> 2) & 7) ? TW - (uint32_t)((i >> 2) & 7) : 1u, TH}}; CmdSetScissor(cb, 0, 1, &sc2); }
               CmdDrawIndexed(cb, (uint32_t)O.draw_idx, 1, 0, 0, 0);
            }
            CmdSetScissor(cb, 0, 1, &scr);
         } else
         for (int i = 0; i < nd; i++) {
            uint32_t off = (uint32_t)i * g_ubo_stride;
            CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_gfx, 0, 1, &dset[fi], 1, &off);
            CmdDrawIndexed(cb, (uint32_t)O.draw_idx, 1, 0, 0, 0);
         }
      }
      if (cur.particles_on) {
         CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_part);
         VkDeviceSize zo = 0;
         CmdBindVertexBuffers(cb, 0, 1, &pbuf.b, &zo);
         CmdDraw(cb, 6, (uint32_t)cur.groups * 64, 0, 0);
      }
      CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_gfx, 0, 1, &dset[fi], 1, &(uint32_t){0});
      if (cur.stream) {
         CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_stream);
         CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_gfx, 0, 1, &dset_str[fi], 1, &(uint32_t){0});
         CmdDraw(cb, 3, 1, 0, 0);
         CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_gfx, 0, 1, &dset[fi], 1, &(uint32_t){0});   /* tex.frag reads binding 2 as an array: back to the standard set */
      }
      if (cur.tex) {
         CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, cur.fs_blend ? pipe_tex_b : pipe_tex);
         float pc[8] = {0, 0, 0, (float)cur.tex, (float)tl, 1.0f + 0.5f * sinf(t), cur.fs_blend ? 0.3f : 1.0f, 0};
         CmdPushConstants(cb, pl_gfx, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
         CmdDraw(cb, 3, 1, 0, 0);
      }
      if (cur.iters) {
         CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, cur.fs_blend ? pipe_alu_b : pipe_alu);
         float pc[8] = {0, cur.fs_blend ? 0.3f : 1.0f, (float)cur.iters, 0, 0, 0, 0, 0};
         CmdPushConstants(cb, pl_gfx, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
         CmdDraw(cb, 3, 1, 0, 0);
      }
      if (cur.layers) {
         CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_overdraw);
         for (int l = 0; l < cur.layers; l++) {
            float pc[8] = {(float)l, 0.15f + 0.02f * (float)(l & 3), 0, 0, 0, 0, 0, 0};
            CmdPushConstants(cb, pl_gfx, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
            CmdDraw(cb, 3, 1, 0, 0);
         }
      }
      CmdEndRenderPass(cb);
      if (have_ts) CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, (uint32_t)fi * 2 + 1);
      if (g_headless && O.readback_every > 0 && (frame % (uint64_t)O.readback_every) == 0) {
         VkBufferImageCopy rg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {TW, TH, 1}};
         CmdCopyImageToBuffer(cb, tcolor[idx].img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buf[fi].b, 1, &rg);
      }
      VK(EndCommandBuffer(cb));
      double tr1 = now_ms();
      R.rec[k] = tr1 - tr0;
      /* ----- submit + present */
      VkPipelineStageFlags wst = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = g_headless ? 0 : 1, .pWaitSemaphores = &sem_acq[fi], .pWaitDstStageMask = &wst,
                         .commandBufferCount = 1, .pCommandBuffers = &cb, .signalSemaphoreCount = g_headless ? 0 : 1, .pSignalSemaphores = &sem_done[idx]};
      VK(QueueSubmit(queue, 1, &si, fen[fi]));
      double ts1 = now_ms();
      R.sub[k] = ts1 - tr1;
      double tq0 = ts1;
      if (!g_headless) {
         VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &sem_done[idx], .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &idx};
         r = QueuePresentKHR(queue, &pi);
         if (r == VK_ERROR_OUT_OF_DATE_KHR) { nood++; destroy_targets(); DestroySwapchainKHR(dev, swapchain, NULL); create_targets(); }
         else if (r == VK_SUBOPTIMAL_KHR) nsub++;
         else if (r != VK_SUCCESS) FAIL("QueuePresentKHR = %d", (int)r);
      }
      R.pres[k] = (tp1 - tp0) + (now_ms() - tq0);
      slot_frame[fi] = (int64_t)k;
      frame++;
      if (!O.auto_scale && (frame % 120) == 0)
         fprintf(stderr, "mdbench: frame %llu, %.1f fps (last 120)\n", (unsigned long long)frame, 120000.0 / (now_ms() - R.t0[k + 1 - 120 > 0 ? k + 1 - 120 : 0] + 1e-9));
      (void)nsc_err;
   }
   double t_end = now_ms();
   if (R.n) R.ft[R.n - 1] = t_end - R.t0[R.n - 1];
   VK(DeviceWaitIdle(dev));
   /* drain the last timestamps */
   if (have_ts) for (int i = 0; i < F; i++) if (slot_frame[i] >= 0) {
      uint64_t ts[2];
      if (GetQueryPoolResults(dev, qpool, (uint32_t)i * 2, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) { R.gpu[slot_frame[i]] = (double)(ts[1] - ts[0]) * ts_period_ms; R.gs[slot_frame[i]] = (double)(ts[0] & vmask) * ts_period_ms; R.ge[slot_frame[i]] = (double)(ts[1] & vmask) * ts_period_ms; }
   }
   /* headless: read back the last rendered target once (determinism checksum + proof something was drawn) */
   if (g_headless && frame > 0) {
      uint32_t last = (uint32_t)((frame - 1) % (uint64_t)F);
      VkCommandBuffer cb = cmdb[0];
      const char *rbt = getenv("MDB_RBTEST");
      if (rbt && rbt[0] == '1') { VkCommandBufferAllocateInfo c2 = cai; c2.commandBufferCount = 1; VK(AllocateCommandBuffers(dev, &c2, &cb)); }
      else ResetCommandBuffer(cb, 0);
      VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      VK(BeginCommandBuffer(cb, &cbi));
      VkBufferImageCopy rg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {TW, TH, 1}};
      if (rbt && rbt[0] == '2') { VkImageMemoryBarrier ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = tcolor[last].img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
         CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib); }
      if (getenv("MDB_IMGDBG")) memset(final_rb.p, 0xAB, (size_t)TW * TH * 4);
      if (rbt && rbt[0] == '4') { Buf sb = mkbuf(4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT); memset(sb.p, 0xAB, 4096); CmdFillBuffer(cb, sb.b, 0, 4096, 0x11223344u); g_sb = sb; }
      if (rbt && rbt[0] == '5') { memset(g_early.p, 0xAB, 4096); CmdFillBuffer(cb, g_early.b, 0, 4096, 0x55667788u); g_sb = g_early; }
      if (rbt && rbt[0] == '3') CmdFillBuffer(cb, final_rb.b, 0, VK_WHOLE_SIZE, 0x11223344u);
      if (getenv("MDB_CLEARTEST")) { VkClearColorValue cc = {{1.0f, 0.0f, 0.0f, 1.0f}}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
         CmdClearColorImage(cb, tcolor[last].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rr); }
            if (!(rbt && (rbt[0] == '3' || rbt[0] == '4' || rbt[0] == '5'))) CmdCopyImageToBuffer(cb, tcolor[last].img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, final_rb.b, 1, &rg);
      VK(EndCommandBuffer(cb));
      VK(ResetFences(dev, 1, &fen[0]));
      VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
      VK(QueueSubmit(queue, 1, &si, fen[0]));
      VK(WaitForFences(dev, 1, &fen[0], VK_TRUE, 20000000000ull));
      if (getenv("MDB_RBSLEEP")) usleep(300000);
      if (rbt && rbt[0] == '5') for (int rep = 0; rep < 3; rep++) {
         fprintf(stderr, "mdbench: smallbuf after fill %d: %08x\n", rep, ((uint32_t *)g_early.p)[1000]);
         memset(g_early.p, 0xAB, 4096);
         VkCommandBufferAllocateInfo c2 = cai; c2.commandBufferCount = 1; VkCommandBuffer cb2; VK(AllocateCommandBuffers(dev, &c2, &cb2));
         VK(BeginCommandBuffer(cb2, &cbi)); CmdFillBuffer(cb2, g_early.b, 0, 4096, 0x55667788u + rep); VK(EndCommandBuffer(cb2));
         VK(ResetFences(dev, 1, &fen[0])); VkSubmitInfo s2 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb2};
         VK(QueueSubmit(queue, 1, &s2, fen[0])); VK(WaitForFences(dev, 1, &fen[0], VK_TRUE, 20000000000ull));
      }
      if (g_sb.p) fprintf(stderr, "mdbench: smallbuf %08x %08x\n", ((uint32_t *)g_sb.p)[0], ((uint32_t *)g_sb.p)[1000]);
      const uint8_t *px = final_rb.p;
      uint64_t h = 1469598103934665603ull, nb = 0, tot = 0;
      for (size_t i = 0; i < (size_t)TW * TH; i += 61) { uint32_t v; memcpy(&v, px + i * 4, 4); h = (h ^ v) * 1099511628211ull; nb += v != 0xff170d0au; tot++; }
      checksum = h; nb_frac = (double)nb / (double)tot;
      for (unsigned gy = 0; gy < 9; gy++) for (unsigned gx = 0; gx < 16; gx++) {   /* 16x9 grid of block-mean RGB, hex */
         uint64_t sum[3] = {0, 0, 0}; unsigned n = 0;
         for (unsigned y = gy * TH / 9; y < (gy + 1) * TH / 9; y += 2) for (unsigned x = gx * TW / 16; x < (gx + 1) * TW / 16; x += 2) {
            const uint8_t *q = px + ((size_t)y * TW + x) * 4; sum[0] += q[0]; sum[1] += q[1]; sum[2] += q[2]; n++; }
         for (int c = 0; c < 3; c++) snprintf(g_grid + (gy * 16 + gx) * 6 + c * 2, 3, "%02x", n ? (unsigned)(sum[c] / n) : 0);
      }
      if (getenv("MDB_IMGDBG")) {
         fprintf(stderr, "mdbench: img %ux%u first px %08x center px %08x\n", TW, TH, *(const uint32_t *)px, *(const uint32_t *)(px + ((size_t)(TH / 2) * TW + TW / 2) * 4));
         for (unsigned y = 0; y < 18; y++) { fprintf(stderr, "mdbench: row%02u ", y); for (unsigned x = 0; x < 32; x++) { const uint8_t *q = px + ((size_t)(y * TH / 18) * TW + x * TW / 32) * 4; fprintf(stderr, "%02x%02x%02x ", q[0], q[1], q[2]); } fprintf(stderr, "\n"); }
      }
      if (O.dump_ppm) {
         FILE *pf = fopen(O.dump_ppm, "wb");
         if (pf) {
            fprintf(pf, "P6\n%u %u\n255\n", TW, TH);
            for (size_t i = 0; i < (size_t)TW * TH; i++) fwrite(px + i * 4, 1, 3, pf);
            fclose(pf);
         }
      }
   }

   /* ------------------------------------------------------------------ report */
   g_phase = "report";
   size_t first = 0;
   while (first < R.n && R.warm[first]) first++;
   size_t sa = first, sb = R.n;
   if (O.auto_scale && best_step >= 0) { sa = steps[best_step].f0; sb = steps[best_step].f1; }
   size_t n = sb > sa ? sb - sa : 0;
   Pct pft = pct(R.ft + sa, n), prec = pct(R.rec + sa, n), psub = pct(R.sub + sa, n), ppres = pct(R.pres + sa, n), pwait = pct(R.wait + sa, n), pwork = pct(R.work + sa, n), pgpu = pct(R.gpu + sa, n);
   size_t o25 = 0, o50 = 0; double sumft = 0;
   for (size_t i = sa; i < sb; i++) { o25 += R.ft[i] > 25.0; o50 += R.ft[i] > 50.0; sumft += R.ft[i]; }
   double fps = sumft > 0 ? n * 1000.0 / sumft : 0, low1 = pft.p99 > 0 ? 1000.0 / pft.p99 : 0;
   Load fl = make_load(O.auto_scale && best_step >= 0 ? (axis_cpu ? O.gpu_scale : O.gpu_scale * steps[best_step].scale) : gs, O.auto_scale && best_step >= 0 ? (axis_cpu ? O.cpu_scale * steps[best_step].scale : O.cpu_scale) : cs, NULL);
   if (!O.auto_scale) fl = cur;
   const char *pm_name = g_headless ? "none(headless)" : pmode == VK_PRESENT_MODE_FIFO_KHR ? "fifo" : pmode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : pmode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "other";
   char tbuf[64]; time_t tt = time(NULL); strftime(tbuf, sizeof tbuf, "%Y-%m-%dT%H:%M:%S", localtime(&tt));

   FILE *jo = O.json_path ? fopen(O.json_path, "w") : NULL;
   for (int pass = 0; pass < 2; pass++) {
      FILE *o = pass == 0 ? stdout : jo;
      if (!o) continue;
      fprintf(o, "{\"mdbench\":{\"version\":1,\"label\":\"%s\",\"time\":\"%s\",", O.label ? O.label : "", tbuf);
      fprintf(o, "\"device\":{\"name\":\"%s\",\"vendor_id\":%u,\"device_id\":%u,\"api_version\":\"%u.%u.%u\",\"driver_version\":%u,\"driver_version_hex\":\"0x%x\",",
              pprops.deviceName, pprops.vendorID, pprops.deviceID, VK_VERSION_MAJOR(pprops.apiVersion), VK_VERSION_MINOR(pprops.apiVersion), VK_VERSION_PATCH(pprops.apiVersion), pprops.driverVersion, pprops.driverVersion);
      if (have_drv) fprintf(o, "\"driver_id\":%d,\"driver_name\":\"%s\",\"driver_info\":\"%s\",", (int)drv.driverID, drv.driverName, drv.driverInfo);
      else fprintf(o, "\"driver_name\":null,\"driver_info\":null,");
      fprintf(o, "\"loader\":\"%s\",\"icd\":%s%s%s},", loader_desc, O.icd ? "\"" : "", O.icd ? O.icd : "null", O.icd ? "\"" : "");
      fprintf(o, "\"wsi\":\"%s\",\"present_mode\":\"%s\",\"present_mode_requested\":\"%s\",\"swapchain_images\":%u,\"color_format\":%d,\"depth_format\":%d,\"width\":%u,\"height\":%u,\"suboptimal\":%d,\"out_of_date\":%d,",
              g_headless ? "headless" : "wayland", pm_name, O.mode, g_headless ? 0u : ntargets, (int)color_fmt, (int)depth_fmt, TW, TH, nsub, nood);
      fprintf(o, "\"config\":{\"scene\":\"%s\",\"gpu_scale\":%g,\"cpu_scale\":%g,\"frames_in_flight\":%d,\"warmup\":%d,\"seconds\":%g,\"frames\":%ld,\"filter\":\"%s\",\"tex_size\":%d,\"tex_layers\":%d,\"compute_iters\":%d,"
                 "\"readback_every\":%d,\"max_draws\":%d,\"max_groups\":%d,\"knob_capped\":%d},",
              scene_names[O.scene], O.gpu_scale, O.cpu_scale, F, O.warmup, O.seconds, O.frames, aniso_ok ? "aniso" : O.filter, O.tex_size, tl, O.compute_iters, O.readback_every, O.max_draws, O.max_groups, capped);
      fprintf(o, "\"load\":{\"draws\":%d,\"overdraw_layers\":%d,\"alu_iters\":%d,\"tex_samples\":%d,\"compute_groups\":%d,\"particles\":%d,\"cpu_work_iters\":%.0f,\"stream_uploads\":%d,\"stream_mib_per_frame\":%d},", fl.draws, fl.layers, fl.iters, fl.tex, fl.groups, fl.groups * 64, fl.cpu_iters, fl.stream, fl.stream * (STREAM_BYTES >> 20));
      fprintf(o, "\"frames_total\":%zu,\"frames_measured\":%zu,\"duration_s\":%.3f,\"fps\":%.3f,\"fps_1pct_low\":%.3f,\"over_25ms\":%zu,\"over_50ms\":%zu,\"gpu_time_available\":%s,", R.n, n, (t_end - t_loop) / 1e3, fps, low1, o25, o50, have_ts ? "true" : "false");
      fprintf(o, "\"frame_time_ms\":{"); jpct(o, "all", pft, 1); fprintf(o, "},");
      fprintf(o, "\"cpu_ms\":{"); jpct(o, "work", pwork, 0); jpct(o, "record", prec, 0); jpct(o, "submit", psub, 0); jpct(o, "present_acquire", ppres, 0); jpct(o, "fence_wait", pwait, 1); fprintf(o, "},");
      fprintf(o, "\"gpu_ms\":"); jpct(o, "", pgpu, 1);
      fprintf(o, ",\"cpu_submit_ms_mean\":%.4f,\"cpu_busy_ms_mean\":%.4f,", psub.mean, pwork.mean + prec.mean + psub.mean);
      fprintf(o, "\"readback\":{\"checksum\":\"%016llx\",\"nonclear_fraction\":%.4f,\"grid\":\"%s\"},", (unsigned long long)checksum, nb_frac, g_grid);
      if (O.auto_scale) {
         fprintf(o, "\"auto_scale\":{\"axis\":\"%s\",\"target_fps\":%g,\"step_secs\":%g,\"criterion\":\"fps>=0.97*target && 1%%low>=0.75*target\",\"score_level\":%d,\"score_scale\":%.4f,\"steps\":[",
                 axis, O.target_fps, O.step_secs, best_step >= 0 ? steps[best_step].level : -1, best_step >= 0 ? steps[best_step].scale : 0.0);
         for (int i = 0; i < nsteps; i++) {
            Step *s2 = &steps[i];
            fprintf(o, "%s{\"level\":%d,\"scale\":%.4f,\"draws\":%d,\"layers\":%d,\"iters\":%d,\"tex\":%d,\"groups\":%d,\"fps\":%.2f,\"fps_1pct_low\":%.2f,\"p95_ms\":%.3f,\"p99_ms\":%.3f,\"pass\":%s}",
                    i ? "," : "", s2->level, s2->scale, s2->load.draws, s2->load.layers, s2->load.iters, s2->load.tex, s2->load.groups, s2->fps, s2->low1, s2->p95, s2->p99, s2->pass ? "true" : "false");
         }
         fprintf(o, "]},\"score\":%d,", best_step >= 0 ? steps[best_step].level : -1);
      }
      fprintf(o, "\"drawn\":%s},\"pass\":%s}\n", (g_headless ? nb_frac > 0.001 : 1) ? "true" : "false", (n > 0 && (!g_headless || nb_frac > 0.001)) ? "true" : "false");
      if (o == jo) fflush(jo);
   }
   if (O.csv_path) {
      FILE *c = fopen(O.csv_path, "w");
      if (c) {
         fprintf(c, "frame,t_ms,frame_ms,cpu_work_ms,cpu_record_ms,cpu_submit_ms,cpu_present_acquire_ms,cpu_fence_wait_ms,gpu_ms,level,warmup,gpu_start_ms,gpu_end_ms\n");
         for (size_t i = 0; i < R.n; i++)
            fprintf(c, "%zu,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%d,%.4f,%.4f\n", i, R.t0[i] - R.t0[0], R.ft[i], R.work[i], R.rec[i], R.sub[i], R.pres[i], R.wait[i], R.gpu[i], (int)R.lvl[i], (int)R.warm[i], R.gs[i], R.ge[i]);
         fclose(c);
      }
   }
   if (jo) fclose(jo);
   fflush(stdout);
   int ok = n > 0 && (!g_headless || nb_frac > 0.001);
   if (O.no_teardown) _exit(ok ? 0 : 1);
   fprintf(stderr, "mdbench: teardown\n");
   destroy_targets();
   if (!g_headless) { DestroySwapchainKHR(dev, swapchain, NULL); DestroySurfaceKHR(inst, surface, NULL); }
   DestroyDevice(dev, NULL);
   DestroyInstance(inst, NULL);
   return ok ? 0 : 1;
}
