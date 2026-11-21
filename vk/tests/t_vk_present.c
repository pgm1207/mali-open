/* SPDX-License-Identifier: MIT */
/* M14 test: vkcube-like Wayland client on the own Mali Vulkan ICD through Mesa wsi_common (dma-buf linear
 * swapchain images, implicit sync via sync_file). Loader-free: dlopens the ICD, creates its own xdg toplevel and
 * wl_surface, then vkCreateWaylandSurfaceKHR on the ICD. Rotating textured, depth-tested cube.
 * One JSON line per event; exit 0 = ran the requested time without Vulkan errors.
 *
 *   t_vk_present --icd <libvulkan_mali.so> [--spv-dir dir] [--seconds S] [--mode fifo|mailbox|immediate]
 *                [--size WxH] [--resize-test] [--min-images N] [--acquire-ms N]
 * Client-side copies per frame: 0 (the swapchain image is rendered into directly; wsi attaches it as wl_buffer).
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#ifndef VK_USE_PLATFORM_WAYLAND_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif
#include <vulkan/vulkan.h>
#include <wayland-client.h>

#include <dlfcn.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ucontext.h>

#include "xdg-shell-client-protocol.h"

#define NF 2 /* frames in flight */

static volatile sig_atomic_t g_stop;
static const char *g_spv = "shader", *g_mode = "fifo";
static double g_seconds = 10;
static uint32_t g_w, g_h, g_minimg = 3;
static int g_resize_test;
static int g_present_ids;          /* --present-ids: chain VkPresentIdKHR (what Steam/gamescope do) */
static double g_max_present_ms;    /* --max-present-ms N: the limit (0 = report only) */
static double g_present_max_ms;    /* the longest vkQueuePresentKHR measured */
static uint64_t g_acq_ms = 20000;   /* --acquire-ms: acquire timeout; VK_TIMEOUT within it exits 3 (bounded wait: the compositor holds every buffer) */

/* ------------------------------------------------------------------ wayland */
static struct {
   struct wl_display *dpy;
   struct wl_compositor *comp;
   struct xdg_wm_base *wm;
   struct wl_surface *surf;
   struct xdg_surface *xs;
   struct xdg_toplevel *top;
   int configured, closed;
   int32_t cfg_w, cfg_h;
} W;

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t s) { (void)d; xdg_wm_base_pong(wm, s); }
static const struct xdg_wm_base_listener wm_l = {wm_ping};
static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
   (void)d;
   xdg_surface_ack_configure(xs, serial);
   W.configured = 1;
}
static const struct xdg_surface_listener xs_l = {xs_configure};
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st)
{
   (void)d; (void)t; (void)st;
   if (w > 0 && h > 0) { W.cfg_w = w; W.cfg_h = h; }
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; W.closed = 1; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_l = {top_configure, top_close, top_bounds, top_caps};
static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
   (void)d;
   if (!strcmp(iface, "wl_compositor")) W.comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
   else if (!strcmp(iface, "xdg_wm_base")) {
      W.wm = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 2 ? ver : 2);
      xdg_wm_base_add_listener(W.wm, &wm_l, NULL);
   }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = {reg_global, reg_remove};

/* ------------------------------------------------------------------ vulkan */
#define DEV_FNS(X)                                                                                         \
   X(DestroyDevice) X(GetDeviceQueue) X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory)          \
   X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(QueueSubmit) X(DeviceWaitIdle)         \
   X(CreateSemaphore) X(DestroySemaphore) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements)     \
   X(BindImageMemory) X(CreateImageView) X(DestroyImageView) X(GetImageSubresourceLayout) X(CreateBuffer)  \
   X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(CreateShaderModule)               \
   X(DestroyShaderModule) X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateRenderPass)             \
   X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) X(CreateGraphicsPipelines)              \
   X(DestroyPipeline) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers)                 \
   X(ResetCommandBuffer) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdBeginRenderPass)                   \
   X(CmdEndRenderPass) X(CmdBindPipeline) X(CmdSetViewport) X(CmdSetScissor) X(CmdBindVertexBuffers)       \
   X(CmdPushConstants) X(CmdDraw) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout)               \
   X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets)      \
   X(CreateSampler) X(DestroySampler) X(CmdBindDescriptorSets) X(CreateSwapchainKHR)                       \
   X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) X(AcquireNextImageKHR) X(QueuePresentKHR)

#define DECL(n) static PFN_vk##n n;
DEV_FNS(DECL)
static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memprops;

static double now_us(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

#define FAIL(...)                                                                                  \
   do {                                                                                            \
      printf("{\"fail\":\"%s:%d\",\"detail\":\"", __FILE__, __LINE__);                             \
      printf(__VA_ARGS__);                                                                         \
      printf("\"}\n");                                                                             \
      fflush(stdout);                                                                              \
      exit(1);                                                                                     \
   } while (0)
#define VK(call)                                                                                   \
   do {                                                                                            \
      VkResult r_ = (call);                                                                        \
      if (r_ != VK_SUCCESS) FAIL("%s = %d", #call, r_);                                            \
   } while (0)

static int memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return (int)i;
   return -1;
}

static VkShaderModule load_spv(const char *name)
{
   char path[512];
   snprintf(path, sizeof path, "%s/%s", g_spv, name);
   FILE *f = fopen(path, "rb");
   if (!f) FAIL("open %s", path);
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint32_t *buf = malloc(n);
   if (fread(buf, 1, n, f) != (size_t)n) FAIL("read %s", path);
   fclose(f);
   VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = buf};
   VkShaderModule m;
   VK(CreateShaderModule(dev, &ci, NULL, &m));
   free(buf);
   return m;
}

static void on_sig(int s) { (void)s; g_stop = 1; }
static void on_crash(int sig, siginfo_t *si, void *uc_)
{
   ucontext_t *uc = uc_;
   fprintf(stderr, "crash: signal %d addr %p\n", sig, si->si_addr);
#ifdef __aarch64__
   uint64_t pcs[2] = {uc->uc_mcontext.pc, uc->uc_mcontext.regs[30]};
   for (int i = 0; i < 2; i++) {
      Dl_info di;
      if (dladdr((void *)pcs[i], &di))
         fprintf(stderr, "  %s %s+0x%lx (base %p, off 0x%lx)\n", di.dli_fname, di.dli_sname ? di.dli_sname : "?",
                 (unsigned long)(pcs[i] - (uint64_t)di.dli_saddr), di.dli_fbase, (unsigned long)(pcs[i] - (uint64_t)di.dli_fbase));
      else fprintf(stderr, "  pc %p\n", (void *)pcs[i]);
   }
#endif
   (void)uc;
   _exit(128 + sig);
}

#define MAXIMG 8
static struct {
   VkSwapchainKHR sc;
   uint32_t n, w, h;
   VkImage img[MAXIMG];
   VkImageView view[MAXIMG];
   VkFramebuffer fb[MAXIMG];
   VkImage dep;
   VkDeviceMemory depmem;
   VkImageView depv;
} S;

static VkRenderPass rp;
static VkPipeline gpipe;
static VkPipelineLayout layout;
static VkDescriptorSet dset;
static VkBuffer vb[2];
static VkImage g_tim;
static VkImageView g_tiv;
static VkSampler g_smp;
static VkDeviceMemory g_timem, g_vbmem;
static VkDescriptorPool g_dpool;
static VkCommandPool pool;
static VkCommandBuffer cmdb[NF];
static VkSemaphore acq[NF], done[NF];
static VkFence fen[NF];

static void destroy_swapchain_res(void)
{
   if (dev) DeviceWaitIdle(dev);
   for (uint32_t i = 0; i < S.n; i++) {
      if (S.fb[i]) DestroyFramebuffer(dev, S.fb[i], NULL);
      if (S.view[i]) DestroyImageView(dev, S.view[i], NULL);
      S.fb[i] = VK_NULL_HANDLE;
      S.view[i] = VK_NULL_HANDLE;
   }
   if (S.depv) DestroyImageView(dev, S.depv, NULL);
   if (S.dep) DestroyImage(dev, S.dep, NULL);
   if (S.depmem) FreeMemory(dev, S.depmem, NULL);
   S.depv = VK_NULL_HANDLE; S.dep = VK_NULL_HANDLE; S.depmem = VK_NULL_HANDLE;
}

static VkSurfaceKHR surface;
static VkPresentModeKHR pmode;
static PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GSC;
static VkPhysicalDevice pd;

static void create_swapchain(uint32_t w, uint32_t h)
{
   VkSurfaceCapabilitiesKHR caps;
   VK(GSC(pd, surface, &caps));
   if (caps.currentExtent.width != 0xFFFFFFFFu) { w = caps.currentExtent.width; h = caps.currentExtent.height; }
   if (w > caps.maxImageExtent.width) w = caps.maxImageExtent.width;
   if (h > caps.maxImageExtent.height) h = caps.maxImageExtent.height;
   if (w < caps.minImageExtent.width) w = caps.minImageExtent.width;
   if (h < caps.minImageExtent.height) h = caps.minImageExtent.height;
   uint32_t n = g_minimg < caps.minImageCount ? caps.minImageCount : g_minimg;
   if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;
   VkSwapchainKHR old = S.sc;
   VkSwapchainCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface,
                                  .minImageCount = n, .imageFormat = VK_FORMAT_R8G8B8A8_UNORM,
                                  .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, .imageExtent = {w, h},
                                  .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                  .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                  .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                  .compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                                                       ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                                       : VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                  .presentMode = pmode, .clipped = VK_TRUE, .oldSwapchain = old};
   destroy_swapchain_res();
   VK(CreateSwapchainKHR(dev, &ci, NULL, &S.sc));
   if (old) DestroySwapchainKHR(dev, old, NULL);
   S.w = w; S.h = h;
   S.n = MAXIMG;
   VK(GetSwapchainImagesKHR(dev, S.sc, &S.n, S.img));

   VkImageCreateInfo di = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                           .format = VK_FORMAT_D32_SFLOAT, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                           .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                           .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                           .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   VK(CreateImage(dev, &di, NULL, &S.dep));
   VkMemoryRequirements mr;
   GetImageMemoryRequirements(dev, S.dep, &mr);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                              .memoryTypeIndex = memtype(mr.memoryTypeBits, 0)};
   VK(AllocateMemory(dev, &ai, NULL, &S.depmem));
   VK(BindImageMemory(dev, S.dep, S.depmem, 0));
   VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = S.dep,
                               .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT,
                               .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
   VK(CreateImageView(dev, &vi, NULL, &S.depv));
   for (uint32_t i = 0; i < S.n; i++) {
      VkImageViewCreateInfo cv = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = S.img[i],
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      VK(CreateImageView(dev, &cv, NULL, &S.view[i]));
      VkImageView at[2] = {S.view[i], S.depv};
      VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
                                     .attachmentCount = 2, .pAttachments = at, .width = w, .height = h, .layers = 1};
      VK(CreateFramebuffer(dev, &fbi, NULL, &S.fb[i]));
   }
   printf("{\"swapchain\":{\"images\":%u,\"w\":%u,\"h\":%u,\"min_requested\":%u}}\n", S.n, w, h, n);
}

static void make_cube(float *pos, float *col)
{
   static const float N[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
   static const float U[6][3] = {{0, 0, 1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}};
   static const float V[6][3] = {{0, 1, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
   static const int cu[6] = {-1, 1, 1, -1, 1, -1}, cv[6] = {-1, -1, 1, -1, 1, 1};
   static const float shade[6] = {1.0f, 0.55f, 0.9f, 0.45f, 0.75f, 0.65f};
   for (int f = 0; f < 6; f++)
      for (int k = 0; k < 6; k++) {
         int i = f * 6 + k;
         float u = (float)cu[k], v = (float)cv[k];
         for (int c = 0; c < 3; c++) pos[i * 4 + c] = 0.8f * (N[f][c] + u * U[f][c] + v * V[f][c]);
         pos[i * 4 + 3] = 1;
         col[i * 4 + 0] = (u + 1) / 2; col[i * 4 + 1] = (v + 1) / 2; col[i * 4 + 2] = shade[f]; col[i * 4 + 3] = 1;
      }
}

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

int main(int argc, char **argv)
{
   const char *icd = NULL;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--icd") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--spv-dir") && i + 1 < argc) g_spv = argv[++i];
      else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) g_seconds = atof(argv[++i]);
      else if (!strcmp(argv[i], "--mode") && i + 1 < argc) g_mode = argv[++i];
      else if (!strcmp(argv[i], "--min-images") && i + 1 < argc) g_minimg = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--size") && i + 1 < argc) { if (sscanf(argv[++i], "%ux%u", &g_w, &g_h) != 2) return 2; }
      else if (!strcmp(argv[i], "--resize-test")) g_resize_test = 1;
      else if (!strcmp(argv[i], "--acquire-ms") && i + 1 < argc) g_acq_ms = strtoull(argv[++i], NULL, 0);
      else if (!strcmp(argv[i], "--present-ids")) g_present_ids = 1;
      else if (!strcmp(argv[i], "--max-present-ms") && i + 1 < argc) g_max_present_ms = atof(argv[++i]);
      else { fprintf(stderr, "usage: t_vk_present --icd x.so [--spv-dir d] [--seconds S] [--mode fifo|mailbox|immediate] [--size WxH] [--resize-test]\n"); return 2; }
   }
   if (!icd) { fprintf(stderr, "--icd required\n"); return 2; }
   { struct sigaction sa = {.sa_sigaction = on_crash, .sa_flags = SA_SIGINFO};
      sigaction(SIGSEGV, &sa, NULL); sigaction(SIGABRT, &sa, NULL); sigaction(SIGBUS, &sa, NULL); }
   signal(SIGINT, on_sig);
   signal(SIGTERM, on_sig);
   setvbuf(stdout, NULL, _IOLBF, 0);

   /* wayland window */
   W.dpy = wl_display_connect(NULL);
   if (!W.dpy) FAIL("wl_display_connect failed");
   struct wl_registry *reg = wl_display_get_registry(W.dpy);
   wl_registry_add_listener(reg, &reg_l, NULL);
   wl_display_roundtrip(W.dpy);
   if (!W.comp || !W.wm) FAIL("missing wl_compositor / xdg_wm_base");
   W.surf = wl_compositor_create_surface(W.comp);
   W.xs = xdg_wm_base_get_xdg_surface(W.wm, W.surf);
   xdg_surface_add_listener(W.xs, &xs_l, NULL);
   W.top = xdg_surface_get_toplevel(W.xs);
   xdg_toplevel_add_listener(W.top, &top_l, NULL);
   xdg_toplevel_set_title(W.top, "t_vk_present");
   xdg_toplevel_set_app_id(W.top, "t_vk_present");
   xdg_toplevel_set_fullscreen(W.top, NULL);
   wl_surface_commit(W.surf);
   for (int i = 0; i < 20 && !W.configured; i++) wl_display_roundtrip(W.dpy);
   if (!g_w || !g_h) { g_w = W.cfg_w ? (uint32_t)W.cfg_w : 1280; g_h = W.cfg_h ? (uint32_t)W.cfg_h : 720; }
   printf("{\"window\":{\"configured\":%d,\"cfg_w\":%d,\"cfg_h\":%d,\"w\":%u,\"h\":%u}}\n", W.configured, W.cfg_w, W.cfg_h, g_w, g_h);

   /* ICD, loader-free */
   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) FAIL("dlopen: %s", dlerror());
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   VkResult (*negotiate)(uint32_t *) = (VkResult(*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   uint32_t iv = 5;
   if (!gipa || !negotiate || negotiate(&iv)) FAIL("icd entry");
   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
   const char *iext[] = {"VK_KHR_surface", "VK_KHR_wayland_surface", "VK_KHR_get_physical_device_properties2",
                         "VK_KHR_get_surface_capabilities2"};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = 4, .ppEnabledExtensionNames = iext};
   VkInstance inst;
   VK(CreateInstance(&ici, NULL, &inst));
#define IFN(n) PFN_vk##n n##_i = (PFN_vk##n)gipa(inst, "vk" #n); if (!n##_i) FAIL("missing vk" #n)
   IFN(EnumeratePhysicalDevices); IFN(GetPhysicalDeviceMemoryProperties); IFN(CreateDevice); IFN(GetDeviceProcAddr);
   IFN(DestroyInstance); IFN(CreateWaylandSurfaceKHR); IFN(DestroySurfaceKHR); IFN(GetPhysicalDeviceSurfaceSupportKHR);
   IFN(GetPhysicalDeviceSurfaceFormatsKHR); IFN(GetPhysicalDeviceSurfacePresentModesKHR);
   IFN(GetPhysicalDeviceQueueFamilyProperties); IFN(GetPhysicalDeviceProperties);
   GSC = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)gipa(inst, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
   if (!GSC) FAIL("missing GetPhysicalDeviceSurfaceCapabilitiesKHR");

   uint32_t cnt = 1;
   VkResult r = EnumeratePhysicalDevices_i(inst, &cnt, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || cnt < 1) FAIL("no device");
   VkPhysicalDeviceProperties pp;
   GetPhysicalDeviceProperties_i(pd, &pp);
   GetPhysicalDeviceMemoryProperties_i(pd, &memprops);
   printf("{\"device\":\"%s\"}\n", pp.deviceName);

   VkWaylandSurfaceCreateInfoKHR wsi = {.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
                                        .display = W.dpy, .surface = W.surf};
   VK(CreateWaylandSurfaceKHR_i(inst, &wsi, NULL, &surface));
   uint32_t nq = 0;
   GetPhysicalDeviceQueueFamilyProperties_i(pd, &nq, NULL);
   VkBool32 sup = VK_FALSE;
   VK(GetPhysicalDeviceSurfaceSupportKHR_i(pd, 0, surface, &sup));
   if (!sup) FAIL("queue family 0 cannot present");

   VkSurfaceCapabilitiesKHR caps;
   VK(GSC(pd, surface, &caps));
   /* Two-call enumeration: a compositor can offer any number of formats (gamescope's wl_shm list gives more than 16 entries, the
    * fixed 16-entry array this test used to have made the call return VK_INCOMPLETE, which VK() reads as a failure). A truncated
    * query must return VK_INCOMPLETE and fill exactly the room given. */
   uint32_t nf = 0, np = 0;
   VK(GetPhysicalDeviceSurfaceFormatsKHR_i(pd, surface, &nf, NULL));
   VK(GetPhysicalDeviceSurfacePresentModesKHR_i(pd, surface, &np, NULL));
   if (nf < 1 || np < 1) FAIL("surface offers %u formats, %u present modes", nf, np);
   if (nf > 1) {
      uint32_t one = 1;
      VkSurfaceFormatKHR f1;
      VkResult tr = GetPhysicalDeviceSurfaceFormatsKHR_i(pd, surface, &one, &f1);
      if (tr != VK_INCOMPLETE || one != 1) FAIL("truncated surface format query = %d count %u (want VK_INCOMPLETE, 1)", tr, one);
   }
   VkSurfaceFormatKHR *fmts = calloc(nf, sizeof *fmts);
   VkPresentModeKHR *pms = calloc(np, sizeof *pms);
   VK(GetPhysicalDeviceSurfaceFormatsKHR_i(pd, surface, &nf, fmts));
   VK(GetPhysicalDeviceSurfacePresentModesKHR_i(pd, surface, &np, pms));
   printf("{\"surface\":{\"min_images\":%u,\"max_images\":%u,\"cur_w\":%u,\"cur_h\":%u,\"formats\":[", caps.minImageCount,
          caps.maxImageCount, caps.currentExtent.width, caps.currentExtent.height);
   int have_fmt = 0;
   for (uint32_t i = 0; i < nf; i++) {
      printf("%s%d", i ? "," : "", fmts[i].format);
      have_fmt |= fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM;
   }
   printf("],\"present_modes\":[");
   VkPresentModeKHR want = !strcmp(g_mode, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
                           : !strcmp(g_mode, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
   pmode = VK_PRESENT_MODE_FIFO_KHR;
   for (uint32_t i = 0; i < np; i++) {
      printf("%s%d", i ? "," : "", pms[i]);
      if (pms[i] == want) pmode = want;
   }
   printf("]}}\n");
   if (!have_fmt) FAIL("R8G8B8A8_UNORM not offered");

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   const char *dext[] = {"VK_KHR_swapchain", "VK_KHR_incremental_present",
                         "VK_KHR_present_id", "VK_KHR_present_wait"};
   /* --present-ids enables both, like gamescope's client: present_wait_enabled (which is what makes Mesa expect a
    * vkWaitForPresentKHR from the driver) is only set when the device enables present-wait. */
   VkPhysicalDevicePresentWaitFeaturesKHR pwf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
                                                 .presentWait = g_present_ids ? VK_TRUE : VK_FALSE};
   VkPhysicalDevicePresentIdFeaturesKHR pidf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
                                                .pNext = &pwf,
                                                .presentId = g_present_ids ? VK_TRUE : VK_FALSE};
   VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = g_present_ids ? &pidf : NULL,
                            .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
                            .enabledExtensionCount = g_present_ids ? 4 : 2, .ppEnabledExtensionNames = dext};
   VK(CreateDevice_i(pd, &di, NULL, &dev));
#define LOAD(n) n = (PFN_vk##n)GetDeviceProcAddr_i(dev, "vk" #n); if (!n) FAIL("missing vk" #n);
   DEV_FNS(LOAD)
   GetDeviceQueue(dev, 0, 0, &queue);

   /* render pass: colour -> PRESENT_SRC, depth */
   VkAttachmentDescription ad[2] = {
      {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
       .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
       .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
       .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR},
      {.format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
       .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
       .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
       .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
   VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkAttachmentReference dref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                              .pColorAttachments = &cref, .pDepthStencilAttachment = &dref};
   VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2,
                                 .pAttachments = ad, .subpassCount = 1, .pSubpasses = &sp};
   VK(CreateRenderPass(dev, &rpi, NULL, &rp));

   /* texture (64x64 linear checker), sampler, descriptor set */
   VkDescriptorSetLayoutBinding lb = {.binding = 0, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &lb};
   VkDescriptorSetLayout dsl;
   VK(CreateDescriptorSetLayout(dev, &dli, NULL, &dsl));
   VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = 32};
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
   VK(CreatePipelineLayout(dev, &pli, NULL, &layout));
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
   VkDescriptorPool dpool;
   VK(CreateDescriptorPool(dev, &dpi, NULL, &dpool));
   g_dpool = dpool;
   VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool,
                                      .descriptorSetCount = 1, .pSetLayouts = &dsl};
   VK(AllocateDescriptorSets(dev, &dai, &dset));
   VkImageCreateInfo ti = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                           .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {64, 64, 1}, .mipLevels = 1, .arrayLayers = 1,
                           .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR,
                           .usage = VK_IMAGE_USAGE_SAMPLED_BIT, .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED};
   VkImage tim;
   VK(CreateImage(dev, &ti, NULL, &tim));
   VkMemoryRequirements mr;
   GetImageMemoryRequirements(dev, tim, &mr);
   VkMemoryAllocateInfo ta = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                              .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
   VkDeviceMemory timem;
   VK(AllocateMemory(dev, &ta, NULL, &timem));
   g_tim = tim; g_timem = timem;
   VK(BindImageMemory(dev, tim, timem, 0));
   uint8_t *tp;
   VK(MapMemory(dev, timem, 0, VK_WHOLE_SIZE, 0, (void **)&tp));
   VkImageSubresource isr = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
   VkSubresourceLayout tl;
   GetImageSubresourceLayout(dev, tim, &isr, &tl);
   for (int y = 0; y < 64; y++)
      for (int x = 0; x < 64; x++) {
         uint8_t *t = tp + tl.offset + y * tl.rowPitch + x * 4;
         int c = ((x >> 3) ^ (y >> 3)) & 1;
         t[0] = c ? 255 : 40 + x * 2; t[1] = c ? 220 : 60 + y * 2; t[2] = c ? 60 : 200; t[3] = 255;
      }
   UnmapMemory(dev, timem);
   VkImageViewCreateInfo tvi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = tim,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   VkImageView tiv;
   VK(CreateImageView(dev, &tvi, NULL, &tiv));
   g_tiv = tiv;
   VkSamplerCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
   VkSampler smp;
   VK(CreateSampler(dev, &smi, NULL, &smp));
   g_smp = smp;
   VkDescriptorImageInfo dii = {smp, tiv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
   VkWriteDescriptorSet wds = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0,
                               .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii};
   UpdateDescriptorSets(dev, 1, &wds, 0, NULL);

   /* cube vertex buffers */
   float pos[36 * 4], col[36 * 4];
   make_cube(pos, col);
   VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sizeof pos, .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
   for (int i = 0; i < 2; i++) VK(CreateBuffer(dev, &bi, NULL, &vb[i]));
   GetBufferMemoryRequirements(dev, vb[0], &mr);
   VkDeviceSize stride = (mr.size + mr.alignment - 1) & ~(mr.alignment - 1);
   VkMemoryAllocateInfo va = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = stride * 2,
                              .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
   VkDeviceMemory vbmem;
   VK(AllocateMemory(dev, &va, NULL, &vbmem));
   g_vbmem = vbmem;
   uint8_t *vp;
   VK(MapMemory(dev, vbmem, 0, VK_WHOLE_SIZE, 0, (void **)&vp));
   memcpy(vp, pos, sizeof pos);
   memcpy(vp + stride, col, sizeof col);
   UnmapMemory(dev, vbmem);
   VK(BindBufferMemory(dev, vb[0], vbmem, 0));
   VK(BindBufferMemory(dev, vb[1], vbmem, stride));

   /* pipeline */
   VkShaderModule vs = load_spv("cube.vert.spv"), fs = load_spv("cube.frag.spv");
   VkPipelineShaderStageCreateInfo st[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
   VkVertexInputBindingDescription bd[2] = {{0, 16, VK_VERTEX_INPUT_RATE_VERTEX}, {1, 16, VK_VERTEX_INPUT_RATE_VERTEX}};
   VkVertexInputAttributeDescription ad2[2] = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0}};
   VkPipelineVertexInputStateCreateInfo vis = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = 2, .pVertexBindingDescriptions = bd,
                                               .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = ad2};
   VkPipelineInputAssemblyStateCreateInfo ias = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
   VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
   VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineDepthStencilStateCreateInfo dss = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
                                                .depthCompareOp = VK_COMPARE_OP_LESS, .maxDepthBounds = 1.0f};
   VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
   VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba};
   VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
   VkPipelineDynamicStateCreateInfo dys = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
   VkGraphicsPipelineCreateInfo gp = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
                                      .pVertexInputState = &vis, .pInputAssemblyState = &ias, .pViewportState = &vps,
                                      .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &dss,
                                      .pColorBlendState = &cbs, .pDynamicState = &dys, .layout = layout, .renderPass = rp};
   VK(CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &gpipe));

   VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
   VK(CreateCommandPool(dev, &pci, NULL, &pool));
   VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = NF};
   VK(AllocateCommandBuffers(dev, &cai, cmdb));
   for (int i = 0; i < NF; i++) {
      VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
      VK(CreateSemaphore(dev, &sci, NULL, &acq[i]));
      VK(CreateSemaphore(dev, &sci, NULL, &done[i]));
      VK(CreateFence(dev, &fci, NULL, &fen[i]));
   }

   create_swapchain(g_w, g_h);
   printf("{\"setup\":{\"ok\":true,\"present_mode\":%d,\"requested\":\"%s\"}}\n", pmode, g_mode);

   /* frame loop */
   size_t cap = 1 << 16, nft = 0;
   double *ft = malloc(cap * sizeof *ft);
   double t_start = now_us(), t_prev = t_start, t_sec = t_start;
   uint64_t frames = 0, sec_frames = 0, recreates = 0, subopt = 0, ood = 0;
   int did_resize = 0;
   while (!g_stop && !W.closed && (now_us() - t_start) < g_seconds * 1e6) {
      int fi = (int)(frames % NF);
      wl_display_dispatch_pending(W.dpy);
      if (g_resize_test && !did_resize && (now_us() - t_start) > g_seconds * 0.4e6) {
         did_resize = 1;
         create_swapchain(S.w / 2 > 64 ? S.w / 2 : S.w, S.h / 2 > 64 ? S.h / 2 : S.h);
         recreates++;
      }
      VK(WaitForFences(dev, 1, &fen[fi], VK_TRUE, 20000000000ull));
      uint32_t idx;
      r = AcquireNextImageKHR(dev, S.sc, g_acq_ms * 1000000ull, acq[fi], VK_NULL_HANDLE, &idx);
      if (r == VK_ERROR_OUT_OF_DATE_KHR) { ood++; create_swapchain(S.w, S.h); recreates++; continue; }
      if (r == VK_TIMEOUT && g_acq_ms < 20000) {
         printf("{\"acquire_timeout\":{\"ms\":%llu,\"frames\":%llu}}\n", (unsigned long long)g_acq_ms, (unsigned long long)frames);
         return 3;
      }
      if (r == VK_SUBOPTIMAL_KHR) subopt++;
      else if (r != VK_SUCCESS) FAIL("AcquireNextImageKHR = %d", r);
      ResetFences(dev, 1, &fen[fi]);

      double t = (now_us() - t_start) / 1e6;
      float ay = (float)(t * 0.9), ax = (float)(0.45 + 0.35 * sin(t * 0.6));
      float pc[8] = {cosf(ay), sinf(ay), cosf(ax), sinf(ax), 1.6f * (float)S.h / (float)S.w, 1.6f, 0, 0};
      VkCommandBuffer cb = cmdb[fi];
      ResetCommandBuffer(cb, 0);
      VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      VK(BeginCommandBuffer(cb, &cbi));
      VkClearValue cv[2] = {{.color = {{0.04f, 0.05f, 0.09f, 1.0f}}}, {.depthStencil = {1.0f, 0}}};
      VkRenderPassBeginInfo rb = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = S.fb[idx],
                                  .renderArea = {{0, 0}, {S.w, S.h}}, .clearValueCount = 2, .pClearValues = cv};
      CmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
      CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gpipe);
      VkViewport vpt = {0, 0, (float)S.w, (float)S.h, 0.0f, 1.0f};
      VkRect2D sc = {{0, 0}, {S.w, S.h}};
      CmdSetViewport(cb, 0, 1, &vpt);
      CmdSetScissor(cb, 0, 1, &sc);
      VkBuffer vbs[2] = {vb[0], vb[1]};
      VkDeviceSize offs[2] = {0, 0};
      CmdBindVertexBuffers(cb, 0, 2, vbs, offs);
      CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &dset, 0, NULL);
      CmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pc);
      CmdDraw(cb, 36, 1, 0, 0);
      CmdEndRenderPass(cb);
      VK(EndCommandBuffer(cb));
      VkPipelineStageFlags wst = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &acq[fi],
                         .pWaitDstStageMask = &wst, .commandBufferCount = 1, .pCommandBuffers = &cb,
                         .signalSemaphoreCount = 1, .pSignalSemaphores = &done[fi]};
      VK(QueueSubmit(queue, 1, &si, fen[fi]));
      /* VK_KHR_incremental_present: alternate a full-frame damage rect (whole frame changes), an empty region (rectangleCount 0 == whole image) and a NULL pRegions entry */
      VkRectLayerKHR rect = {.offset = {0, 0}, .extent = {S.w, S.h}, .layer = 0};
      VkPresentRegionKHR reg = {.rectangleCount = (frames % 3 == 0) ? 1 : 0, .pRectangles = &rect};
      VkPresentRegionsKHR prg = {.sType = VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR, .swapchainCount = 1, .pRegions = (frames % 3 == 2) ? NULL : &reg};
      VkPresentIdKHR pid = {.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .pNext = &prg, .swapchainCount = 1};
      const uint64_t this_id = (uint64_t)frames + 1;
      pid.pPresentIds = &this_id;   /* frames+1: present ids must increase */
      VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = g_present_ids ? (void *)&pid : (void *)&prg,
                             .waitSemaphoreCount = 1, .pWaitSemaphores = &done[fi],
                             .swapchainCount = 1, .pSwapchains = &S.sc, .pImageIndices = &idx};
      const double pt0 = now_us();
      r = QueuePresentKHR(queue, &pi);
      const double present_ms = (now_us() - pt0) / 1e3;
      if (present_ms > g_present_max_ms) g_present_max_ms = present_ms;
      if (r == VK_ERROR_OUT_OF_DATE_KHR) { ood++; create_swapchain(S.w, S.h); recreates++; }
      else if (r == VK_SUBOPTIMAL_KHR) subopt++;
      else if (r != VK_SUCCESS) FAIL("QueuePresentKHR = %d", r);

      double now = now_us();
      if (nft == cap) { cap *= 2; ft = realloc(ft, cap * sizeof *ft); }
      ft[nft++] = (now - t_prev) / 1e3;
      t_prev = now;
      frames++; sec_frames++;
      if (now - t_sec >= 1e6) {
         printf("{\"sec\":{\"t\":%.1f,\"fps\":%.1f}}\n", (now - t_start) / 1e6, sec_frames * 1e6 / (now - t_sec));
         t_sec = now; sec_frames = 0;
      }
   }
   double total = (now_us() - t_start) / 1e6;
   DeviceWaitIdle(dev);
   if (nft) {
      double sum = 0, mx = 0;
      uint64_t over50 = 0;
      for (size_t i = 0; i < nft; i++) { sum += ft[i]; if (ft[i] > mx) mx = ft[i]; over50 += ft[i] > 50.0; }
      double *sorted = malloc(nft * sizeof *sorted);
      memcpy(sorted, ft, nft * sizeof *sorted);
      qsort(sorted, nft, sizeof *sorted, cmp_d);
      printf("{\"stats\":{\"frames\":%llu,\"seconds\":%.2f,\"fps\":%.2f,\"ft_mean_ms\":%.2f,\"ft_p50_ms\":%.2f,"
             "\"ft_p99_ms\":%.2f,\"ft_max_ms\":%.2f,\"over_50ms\":%llu,\"client_copies_per_frame\":0,"
             "\"recreates\":%llu,\"suboptimal\":%llu,\"out_of_date\":%llu,\"final_w\":%u,\"final_h\":%u}}\n",
             (unsigned long long)frames, total, frames / total, sum / nft, sorted[nft / 2], sorted[(size_t)(nft * 0.99)], mx,
             (unsigned long long)over50, (unsigned long long)recreates, (unsigned long long)subopt, (unsigned long long)ood,
             S.w, S.h);
   }
   /* --max-present-ms: vkQueuePresentKHR must not wait on the compositor (CR #288: the 20 ms vkWaitForPresentKHR
    * this driver used to run after every successful present with present ids). */
   if (g_present_ids) {
      printf("{\"present_ms\":{\"max\":%.2f,\"frames\":%llu,\"limit\":%.2f}}\n", g_present_max_ms,
             (unsigned long long)frames, g_max_present_ms);
      if (g_max_present_ms > 0 && g_present_max_ms > g_max_present_ms) {
         fprintf(stderr, "FAIL: one vkQueuePresentKHR took %.2f ms (limit %.2f): the present path waited on the "
                 "compositor\n", g_present_max_ms, g_max_present_ms);
         fprintf(stderr, "td:done\n");
         printf("{\"pass\":false}\n");
         return 1;
      }
   }
   fprintf(stderr, "td:res\n"); destroy_swapchain_res();
   /* test-owned resources (were leaked: 5 BOs reported by kb_close) */
   DeviceWaitIdle(dev);
   if (g_dpool) DestroyDescriptorPool(dev, g_dpool, NULL);
   if (g_tiv) DestroyImageView(dev, g_tiv, NULL);
   if (g_smp) DestroySampler(dev, g_smp, NULL);
   if (g_tim) DestroyImage(dev, g_tim, NULL);
   if (g_timem) FreeMemory(dev, g_timem, NULL);
   for (int i = 0; i < 2; i++) if (vb[i]) DestroyBuffer(dev, vb[i], NULL);
   if (g_vbmem) FreeMemory(dev, g_vbmem, NULL);
   fprintf(stderr, "td:sc\n"); DestroySwapchainKHR(dev, S.sc, NULL);
   fprintf(stderr, "td:surf\n"); DestroySurfaceKHR_i(inst, surface, NULL);
   fprintf(stderr, "td:dev\n"); DestroyDevice(dev, NULL);
   fprintf(stderr, "td:inst\n"); DestroyInstance_i(inst, NULL);
   fprintf(stderr, "td:done\n");
   printf("{\"pass\":%s}\n", frames ? "true" : "false");
   return frames ? 0 : 1;
}
