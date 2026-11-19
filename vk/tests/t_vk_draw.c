/* SPDX-License-Identifier: MIT */
/* M9 test: Vulkan client draws a rotating triangle through the ICD (shader modules, graphics pipeline,
 * render pass, framebuffer, command buffer, queue submit, fence, host readback). Loader-free: dlopens the
 * ICD like t_queue --direct. One JSON line per case; exit 0 = all pass.
 *
 *   t_vk_draw --direct <icd.so> [--loop N] [--loop-reset] [--only triangle|two|depth|rot|idx16|idx32|scissor|viewport|multivp|clearatt|ring|pushfs|ubo|descupd|texture|depth16|depth32s8|stencil]
 *             [--spv-dir dir]
 * Pixel checks are skipped on the fake backend (MALI_BACKEND=fake): it renders nothing.
 * --loop N repeats the triangle case N times; --loop-reset reuses one command buffer via
 * vkResetCommandPool instead of allocating a fresh one each time.
 */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define W 256
#define H 256

static VkFormat g_depfmt = VK_FORMAT_D32_SFLOAT;
static VkFormat g_colfmt = VK_FORMAT_R8G8B8A8_UNORM;   /* colour attachment format (4 or 8 bytes per texel) */
static unsigned g_colbpp = 4;
static const float *g_clear_f;   /* clear the colour attachment to these four floats (SNORM / UNORM16 / float RAW targets) */
static int g_nofs;      /* pipeline without a fragment stage (depth/stencil-only draw, no rasterizer discard) */
static int g_cull;      /* vertex shader writes gl_CullDistance (MALI_CULLDIST=1 lowering) */
static int g_sparse;    /* vertex attributes at locations 0 and 2 (layout_skip): the skipped slot 1 is filled by the driver */
static int g_clearzs;   /* clearquad: the CmdClearAttachments after the draw also clears depth+stencil of the sub-rect */
static int g_nocol;     /* render pass with a depth attachment only (no colour attachment) */
static int g_dynstr;   /* dynamic VERTEX_INPUT_BINDING_STRIDE with a wrong static stride; the draw binds strides through CmdBindVertexBuffers2 */
static PFN_vkCmdBindVertexBuffers2 CmdBindVertexBuffers2_;
static int g_discard;   /* 1: rasterizerDiscardEnable with both stages, 2: vertex-only pipeline + discard */
static int g_clear_u;   /* clear the colour attachment to the integer value (200,100,50,7) instead of float zeros */
static int g_load;   /* two-pass LOAD test: store depth/stencil, and build s->rp2 = the same pass with loadOp LOAD */
static int g_fail, g_fake, g_loop = 1, g_reset;
/* R4 fixed-function state of the next scene_create()/scene_draw() (all zero = the old tests' state) */
static struct {
   int on;                      /* use fxz.vert (z = rot.w) and the fields below */
   VkCullModeFlags cull;
   int cw;                      /* VK_FRONT_FACE_CLOCKWISE instead of COUNTER_CLOCKWISE */
   int clamp, bias;
   int dclip;                   /* VK_EXT_depth_clip_enable: 0 = not chained, 1 = depthClipEnable FALSE, 2 = TRUE */
   float bias_c, bias_s, bias_clamp;
   int blend;
   VkBlendFactor sf, df;
   VkBlendOp op;
   uint32_t wmask;
   float bc[4];
   int poly;                    /* VkPolygonMode (0 fill, 1 line, 2 point), applied with or without .on */
   int dynpoly;                 /* poly is set with vkCmdSetPolygonModeEXT (pipeline polygonMode FILL + VK_DYNAMIC_STATE_POLYGON_MODE_EXT) */
   int dual;                    /* dual.frag (two outputs: o0 = varying colour, o1 = 0.5) instead of var.frag */
   int dyn;                     /* cull/front face/bias/blend constants are set dynamically (the static pipeline values are wrong on purpose) */
} g_fx;
/* stencil test of the next scene (depth test is off then); values are Vulkan's */
struct st_state {
   int on;
   VkCompareOp cmp_f, cmp_b;
   VkStencilOp fail, dfail, pass;
   uint32_t cmask, wmask, ref, clear;
   int cw;                      /* front face = clockwise */
   int dyn;                     /* compare mask / write mask / reference are dynamic (the static values are wrong on purpose) */
};
static struct st_state g_st;
static VkPhysicalDeviceFeatures g_feat;
static double g_t_rec, g_t_sub;   /* scene_draw: record / QueueSubmit microseconds, summed */
static int g_share;   /* share test: 2 = alternate the scissor halves per draw, 3 = dynamic-UBO offset from g_share_off[] and firstInstance from g_share_fi[] */
static uint32_t g_share_off[8], g_share_fi[8];
static int g_dirty;   /* share_dirty: 1 viewport / 2 cull mode changes between draws, 3 = the same pipeline rebound before every draw
                        (push constants: the caller passes different rot[] values) */
static int g_inst;   /* share_inst: per-instance attribute pipeline; 1 = firstInstance / vertex-buffer-offset changes between draws, 2 = four identical draws */
static int g_dyn;   /* many_draws: dynamic-UBO descriptor offset per draw (the mdbench triangle shape) */
static PFN_vkCmdSetPolygonModeEXT CmdSetPolygonModeEXT_;
static int g_have_eds3;      /* VK_EXT_extended_dynamic_state3 advertised, enabled, extendedDynamicState3PolygonMode set */
static int g_have_dclip;   /* VK_EXT_depth_clip_enable advertised and enabled */
static VkPipelineRasterizationDepthClipStateCreateInfoEXT g_dce = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT};
static VkSampleCountFlags g_fbcounts;   /* limits.framebufferColorSampleCounts */
static const char *g_only, *g_spv = "shader";
static int g_msaa;   /* 1: 4x MSAA colour + resolve attachment, 2: additionally sampleShadingEnable (needs MALI_EXPERIMENTAL_MSAA=1 on the driver) */

enum { MODE_TRI, MODE_IDX16, MODE_IDX32, MODE_CLEARATT, MODE_CLEARQUAD, MODE_SCISSOR, MODE_VPORT, MODE_MVP, MODE_PUSHFS, MODE_TEX, MODE_UBO, MODE_DESCUPD, MODE_MDI };

#define CASE_ON(n) ((!g_only || !strcmp(g_only, n)) && !case_skipped(n))
/* MALI_SKIP_CASES="a,b": cases to leave out (farm --env route: lets one failing case be isolated without a phone shell) */
static int
case_skipped(const char *n)
{
   const char *e = getenv("MALI_SKIP_CASES");
   size_t l = strlen(n);
   for (; e && *e; e++)
      if (!strncmp(e, n, l) && (e[l] == ',' || !e[l]) && (e == getenv("MALI_SKIP_CASES") || e[-1] == ','))
         return 1;
   return 0;
}
#define CHECK(c, ...)                                                                              \
   do {                                                                                            \
      if (!(c)) {                                                                                  \
         printf("{\"fail\":\"%s:%d\",\"expr\":\"%s\",\"detail\":\"", __FILE__, __LINE__, #c);     \
         printf(__VA_ARGS__);                                                                      \
         printf("\"}\n");                                                                          \
         g_fail = 1;                                                                               \
         return 0;                                                                                 \
      }                                                                                            \
   } while (0)

#define DEV_FNS(X)                                                                                 \
   X(DestroyDevice) X(GetDeviceQueue) X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory)  \
   X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(QueueSubmit) X(DeviceWaitIdle)  \
   X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory)                  \
   X(CreateImageView) X(DestroyImageView) X(GetImageSubresourceLayout) X(CreateBuffer)             \
   X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(CreateShaderModule)       \
   X(DestroyShaderModule) X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateRenderPass)      \
   X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) X(CreateGraphicsPipelines)       \
   X(DestroyPipeline) X(CreateCommandPool) X(DestroyCommandPool) X(ResetCommandPool)                \
   X(AllocateCommandBuffers) X(FreeCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer)        \
   X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdBindPipeline) X(CmdSetViewport) X(CmdSetScissor)  \
   X(CmdBindVertexBuffers) X(CmdBindVertexBuffers2) X(CmdPushConstants) X(CmdDraw) X(CmdDrawIndexed) X(CmdBindIndexBuffer) X(CmdDrawIndexedIndirect)  \
   X(CmdClearAttachments) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout)  \
   X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateSampler) \
   X(DestroySampler) X(CmdBindDescriptorSets) X(CreateQueryPool) X(DestroyQueryPool)       \
   X(CmdSetStencilReference) X(CmdSetStencilCompareMask) X(CmdSetStencilWriteMask) X(CmdSetCullMode) X(CmdSetFrontFace) X(CmdSetDepthBias) X(CmdSetBlendConstants) X(CmdResetQueryPool) X(CmdBeginQuery) X(CmdEndQuery) X(GetQueryPoolResults)

#define DECL(n) static PFN_vk##n n;
DEV_FNS(DECL)
static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memprops;

static double
now_us(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static int
memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want)
         return (int)i;
   return -1;
}

static VkShaderModule
load_spv(const char *name)
{
   char path[512];
   snprintf(path, sizeof path, "%s/%s", g_spv, name);
   FILE *f = fopen(path, "rb");
   if (!f) { printf("{\"fail\":\"open %s\"}\n", path); return VK_NULL_HANDLE; }
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint32_t *buf = malloc(n);
   if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return VK_NULL_HANDLE; }
   fclose(f);
   VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = buf};
   VkShaderModule m = VK_NULL_HANDLE;
   if (CreateShaderModule(dev, &ci, NULL, &m) != VK_SUCCESS)
      m = VK_NULL_HANDLE;
   free(buf);
   return m;
}

/* ------------------------------------------------------------------ scene */
struct scene {
   int depth;
   VkShaderModule vs, fs;
   VkPipelineLayout layout;
   VkRenderPass rp, rp2;
   int second;   /* scene_draw uses rp2 (LOAD) */
   VkPipeline pipe;
   VkImage col, dep;
   VkDeviceMemory colmem, depmem, vbmem;
   VkImageView colv, depv;
   VkFramebuffer fb;
   VkBuffer vb[2];
   VkCommandPool pool;
   VkCommandBuffer cb; /* reused with --loop-reset */
   uint8_t *px;
   uint32_t pitch;
   VkBuffer ib;
   VkDeviceMemory ibmem;
   int mode;     /* MODE_* */
   VkDescriptorSetLayout dsl;
   VkDescriptorPool dpool;
   VkDescriptorSet dset[2];
   VkBuffer ubo[3];          /* uniform buffers (colours) */
   VkDeviceMemory ubomem;
   uint8_t *ubop;            /* mapped, 256 bytes per buffer */
   VkImage tim;
   VkDeviceMemory timem;
   VkImageView tiv;
   VkSampler smp;
   VkImage msi;              /* g_msaa: the 4-sample colour image (col is its single-sample resolve target) */
   VkDeviceMemory msmem;
   VkImageView msv;
};

static int
scene_create(struct scene *s, int depth)
{
   /* s->mode is an INPUT: callers must initialise the struct. An uninitialised stack `struct scene` made mode random per process (low half of
    * an ASLR pointer): >= MODE_PUSHFS drew the full-screen indexed quad instead of the triangle (coverage 65536, ~50 % of runs on the CMF). */
   int mode = s->mode;
   memset(s, 0, sizeof *s);
   s->mode = mode;
   s->depth = depth;
   const char *vsn = g_inst ? "inst_attr.vert.spv" : g_cull ? "cull.vert.spv" : g_sparse ? "rot_sparse.vert.spv" : g_fx.on ? "fxz.vert.spv" : "rot.vert.spv", *fsn = "var.frag.spv";
   if (g_fx.dual) fsn = "dual.frag.spv";
   if (mode == MODE_TEX) { vsn = "tex.vert.spv"; fsn = "tex.frag.spv"; }
   if (mode == MODE_UBO || mode == MODE_DESCUPD) fsn = "ubo.frag.spv";
   if (mode == MODE_PUSHFS) fsn = "pc.frag.spv";
   s->vs = load_spv(vsn);
   s->fs = load_spv(fsn);
   CHECK(s->vs && s->fs, "shader modules %s %s", vsn, fsn);

   VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = 16};
   if (mode == MODE_PUSHFS) { pcr.stageFlags |= VK_SHADER_STAGE_FRAGMENT_BIT; pcr.size = 32; }
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
   if (mode == MODE_TEX || mode == MODE_UBO || mode == MODE_DESCUPD) {
      VkDescriptorSetLayoutBinding lb = {.binding = 0, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
         .descriptorType = mode == MODE_TEX ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : g_dyn ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
      VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                             .bindingCount = 1, .pBindings = &lb};
      CHECK(CreateDescriptorSetLayout(dev, &dli, NULL, &s->dsl) == VK_SUCCESS, "dsl");
      pli.setLayoutCount = 1;
      pli.pSetLayouts = &s->dsl;
      VkDescriptorPoolSize ps = {lb.descriptorType, 2};
      VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2,
                                        .poolSizeCount = 1, .pPoolSizes = &ps};
      CHECK(CreateDescriptorPool(dev, &dpi, NULL, &s->dpool) == VK_SUCCESS, "dpool");
      VkDescriptorSetLayout l2[2] = {s->dsl, s->dsl};
      VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = s->dpool,
                                         .descriptorSetCount = 2, .pSetLayouts = l2};
      CHECK(AllocateDescriptorSets(dev, &dai, s->dset) == VK_SUCCESS, "alloc sets");
   }
   CHECK(CreatePipelineLayout(dev, &pli, NULL, &s->layout) == VK_SUCCESS, "pipeline layout");

   /* colour image: linear RGBA8, host visible so the test can read it back */
   VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                           .format = g_colfmt, .extent = {W, H, 1}, .mipLevels = 1,
                           .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR,
                           .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   CHECK(CreateImage(dev, &ii, NULL, &s->col) == VK_SUCCESS, "colour image");
   VkMemoryRequirements mr;
   GetImageMemoryRequirements(dev, s->col, &mr);
   int mt = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   CHECK(mt >= 0, "no host visible type");
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                              .memoryTypeIndex = mt};
   CHECK(AllocateMemory(dev, &ai, NULL, &s->colmem) == VK_SUCCESS, "colour mem");
   CHECK(BindImageMemory(dev, s->col, s->colmem, 0) == VK_SUCCESS, "bind colour");
   CHECK(MapMemory(dev, s->colmem, 0, VK_WHOLE_SIZE, 0, (void **)&s->px) == VK_SUCCESS, "map colour");
   VkImageSubresource sr = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
   VkSubresourceLayout sl;
   GetImageSubresourceLayout(dev, s->col, &sr, &sl);
   s->pitch = (uint32_t)sl.rowPitch;
   s->px += sl.offset;
   VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = s->col,
                               .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = g_colfmt,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   CHECK(CreateImageView(dev, &vi, NULL, &s->colv) == VK_SUCCESS, "colour view");

   VkImageView atts[2] = {s->colv, VK_NULL_HANDLE};
   if (g_msaa) {
      VkImageCreateInfo mi = ii;
      mi.samples = VK_SAMPLE_COUNT_4_BIT;
      mi.tiling = VK_IMAGE_TILING_OPTIMAL;
      mi.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      CHECK(CreateImage(dev, &mi, NULL, &s->msi) == VK_SUCCESS, "4-sample colour image");
      GetImageMemoryRequirements(dev, s->msi, &mr);
      VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                 .memoryTypeIndex = memtype(mr.memoryTypeBits, 0)};
      CHECK(AllocateMemory(dev, &ma, NULL, &s->msmem) == VK_SUCCESS, "msaa mem");
      CHECK(BindImageMemory(dev, s->msi, s->msmem, 0) == VK_SUCCESS, "bind msaa");
      VkImageViewCreateInfo mv = vi;
      mv.image = s->msi;
      CHECK(CreateImageView(dev, &mv, NULL, &s->msv) == VK_SUCCESS, "msaa view");
      atts[0] = s->msv;      /* attachment 0 = MSAA, attachment 1 = the resolve image (colv) */
      atts[1] = s->colv;
   }
   if (depth) {
      VkImageCreateInfo di = ii;
      di.format = g_depfmt;
      di.tiling = VK_IMAGE_TILING_OPTIMAL;
      di.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
      CHECK(CreateImage(dev, &di, NULL, &s->dep) == VK_SUCCESS, "depth image");
      GetImageMemoryRequirements(dev, s->dep, &mr);
      mt = memtype(mr.memoryTypeBits, 0);
      VkMemoryAllocateInfo da = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                 .memoryTypeIndex = mt};
      CHECK(AllocateMemory(dev, &da, NULL, &s->depmem) == VK_SUCCESS, "depth mem");
      CHECK(BindImageMemory(dev, s->dep, s->depmem, 0) == VK_SUCCESS, "bind depth");
      VkImageViewCreateInfo dv = vi;
      dv.image = s->dep;
      dv.format = g_depfmt;
      dv.subresourceRange.aspectMask = g_depfmt == VK_FORMAT_D32_SFLOAT_S8_UINT || g_depfmt == VK_FORMAT_D24_UNORM_S8_UINT ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) :
                                        g_depfmt == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
      CHECK(CreateImageView(dev, &dv, NULL, &s->depv) == VK_SUCCESS, "depth view");
      atts[1] = s->depv;
   }

   VkAttachmentDescription ad[2] = {
      {.format = g_colfmt, .samples = VK_SAMPLE_COUNT_1_BIT,
       .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
       .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
       .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_GENERAL},
      {.format = g_depfmt, .samples = VK_SAMPLE_COUNT_1_BIT,
       .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
       .stencilLoadOp = g_st.on ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
       .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
   if (g_msaa) {
      ad[0].samples = VK_SAMPLE_COUNT_4_BIT;
      ad[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      ad[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      ad[1] = ad[0];
      ad[1].samples = VK_SAMPLE_COUNT_1_BIT;
      ad[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      ad[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      ad[1].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
   }
   if (g_nocol)
      ad[0] = ad[1];   /* the depth attachment becomes attachment 0; the colour image is created but not attached */
   VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkAttachmentReference rref = {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkAttachmentReference dref = {g_nocol ? 0 : 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = g_nocol ? 0 : 1,
                              .pColorAttachments = g_nocol ? NULL : &cref, .pResolveAttachments = g_msaa ? &rref : NULL,
                              .pDepthStencilAttachment = depth ? &dref : NULL};
   VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = g_nocol ? 1 : depth || g_msaa ? 2 : 1,
                                 .pAttachments = ad, .subpassCount = 1, .pSubpasses = &sp};
   if (g_load) {
      ad[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      if (g_st.on) ad[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
   }
   CHECK(CreateRenderPass(dev, &rpi, NULL, &s->rp) == VK_SUCCESS, "render pass");
   if (g_load) {
      /* the second pass over the same attachments: LOAD everything the first one stored */
      ad[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      ad[0].initialLayout = VK_IMAGE_LAYOUT_GENERAL;
      ad[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      ad[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      if (g_st.on) ad[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      CHECK(CreateRenderPass(dev, &rpi, NULL, &s->rp2) == VK_SUCCESS, "render pass (LOAD)");
   }
   if (g_nocol)
      atts[0] = atts[1];
   VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = s->rp,
                                  .attachmentCount = g_nocol ? 1 : depth || g_msaa ? 2 : 1, .pAttachments = atts, .width = W, .height = H,
                                  .layers = 1};
   CHECK(CreateFramebuffer(dev, &fbi, NULL, &s->fb) == VK_SUCCESS, "framebuffer");

   /* vertex buffers: binding 0 position, binding 1 colour */
   /* vertices 0..2 triangle; 3..6 a quad (-.5..+.5) drawn indexed with vertexOffset 3 */
   static const float pos[44] = {-.5f, -.5f, 0, 1, .5f, -.5f, 0, 1, 0, .5f, 0, 1,
                                 -.5f, -.5f, 0, 1, .5f, -.5f, 0, 1, -.5f, .5f, 0, 1, .5f, .5f, 0, 1,
                                 -1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, 1, 0, 1};
   static const float colr[44] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1,
                                  1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1,
                                  0, 0, 0, 1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 0, 1};
   VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256,
                            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
   for (int i = 0; i < 2; i++)
      CHECK(CreateBuffer(dev, &bi, NULL, &s->vb[i]) == VK_SUCCESS, "vertex buffer");
   GetBufferMemoryRequirements(dev, s->vb[0], &mr);
   VkDeviceSize stride = (mr.size + mr.alignment - 1) & ~(mr.alignment - 1);
   VkMemoryAllocateInfo va = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = stride * 2,
                              .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
   CHECK(AllocateMemory(dev, &va, NULL, &s->vbmem) == VK_SUCCESS, "vb mem");
   uint8_t *vp;
   CHECK(MapMemory(dev, s->vbmem, 0, VK_WHOLE_SIZE, 0, (void **)&vp) == VK_SUCCESS, "map vb");
   memcpy(vp, pos, sizeof pos);
   memcpy(vp + stride, colr, sizeof colr);
   UnmapMemory(dev, s->vbmem);
   CHECK(BindBufferMemory(dev, s->vb[0], s->vbmem, 0) == VK_SUCCESS, "bind vb0");
   CHECK(BindBufferMemory(dev, s->vb[1], s->vbmem, stride) == VK_SUCCESS, "bind vb1");

   /* index buffer: u16 {0,1,2,2,1,3} at byte 0, u32 same at byte 16 */
   VkBufferCreateInfo ibi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 128,
                             .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT};
   CHECK(CreateBuffer(dev, &ibi, NULL, &s->ib) == VK_SUCCESS, "index buffer");
   GetBufferMemoryRequirements(dev, s->ib, &mr);
   VkMemoryAllocateInfo ia = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                              .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
   CHECK(AllocateMemory(dev, &ia, NULL, &s->ibmem) == VK_SUCCESS, "ib mem");
   uint8_t *ip;
   CHECK(MapMemory(dev, s->ibmem, 0, VK_WHOLE_SIZE, 0, (void **)&ip) == VK_SUCCESS, "map ib");
   static const uint16_t i16[6] = {0, 1, 2, 2, 1, 3};
   uint32_t i32[6];
   for (int i = 0; i < 6; i++) i32[i] = i16[i];
   memcpy(ip, i16, sizeof i16);
   memcpy(ip + 16, i32, sizeof i32);
   /* R4: two DrawIndexedIndirect commands at byte 64 (stride 20): triangle 1 {0,1,2}, triangle 2 {2,1,3} */
   static const uint32_t ind[10] = {3, 1, 0, 3, 0, 3, 1, 3, 3, 0};
   memcpy(ip + 64, ind, sizeof ind);
   UnmapMemory(dev, s->ibmem);
   CHECK(BindBufferMemory(dev, s->ib, s->ibmem, 0) == VK_SUCCESS, "bind ib");

   if (mode == MODE_UBO || mode == MODE_DESCUPD) {
      /* three 256-byte uniform buffers in one allocation; colours written by the test */
      VkBufferCreateInfo ui = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256,
                               .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
      VkDeviceSize step = 0;
      for (int i = 0; i < 3; i++) CHECK(CreateBuffer(dev, &ui, NULL, &s->ubo[i]) == VK_SUCCESS, "ubo");
      GetBufferMemoryRequirements(dev, s->ubo[0], &mr);
      step = (256 + mr.alignment - 1) & ~(mr.alignment - 1);
      VkMemoryAllocateInfo ua = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = step * 3,
                                 .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
      CHECK(AllocateMemory(dev, &ua, NULL, &s->ubomem) == VK_SUCCESS, "ubo mem");
      CHECK(MapMemory(dev, s->ubomem, 0, VK_WHOLE_SIZE, 0, (void **)&s->ubop) == VK_SUCCESS, "map ubo");
      static const float colA[4] = {0.25f, 0.5f, 0.75f, 1.0f}, colB[4] = {1.0f, 0.0f, 0.5f, 1.0f}, colC[4] = {0.0f, 1.0f, 0.0f, 1.0f};
      memcpy(s->ubop, colA, 16);
      memcpy(s->ubop + step, colB, 16);
      memcpy(s->ubop + 2 * step, colC, 16);
      for (int i = 0; i < 3; i++) CHECK(BindBufferMemory(dev, s->ubo[i], s->ubomem, step * i) == VK_SUCCESS, "bind ubo");
      /* set 0 = colour A, set 1 = colour B */
      for (int i = 0; i < 2; i++) {
         VkDescriptorBufferInfo bi2 = {s->ubo[i], 0, 16};
         VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->dset[i], .dstBinding = 0,
                                   .descriptorCount = 1, .descriptorType = g_dyn ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi2};
         UpdateDescriptorSets(dev, 1, &w, 0, NULL);
      }
   }
   if (mode == MODE_TEX) {
      /* 8x8 RGBA8 linear texture, texel (i,j) = (i*32, j*32, 128, 255) */
      VkImageCreateInfo ti = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                              .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {8, 8, 1}, .mipLevels = 1, .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR,
                              .usage = VK_IMAGE_USAGE_SAMPLED_BIT, .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED};
      CHECK(CreateImage(dev, &ti, NULL, &s->tim) == VK_SUCCESS, "texture image");
      GetImageMemoryRequirements(dev, s->tim, &mr);
      VkMemoryAllocateInfo ta = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                 .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
      CHECK(AllocateMemory(dev, &ta, NULL, &s->timem) == VK_SUCCESS, "tex mem");
      CHECK(BindImageMemory(dev, s->tim, s->timem, 0) == VK_SUCCESS, "bind tex");
      uint8_t *tp;
      CHECK(MapMemory(dev, s->timem, 0, VK_WHOLE_SIZE, 0, (void **)&tp) == VK_SUCCESS, "map tex");
      VkSubresourceLayout tl;
      GetImageSubresourceLayout(dev, s->tim, &sr, &tl);
      for (int j = 0; j < 8; j++)
         for (int i = 0; i < 8; i++) {
            uint8_t *t = tp + tl.offset + j * tl.rowPitch + i * 4;
            t[0] = i * 32; t[1] = j * 32; t[2] = 128; t[3] = 255;
         }
      UnmapMemory(dev, s->timem);
      VkImageViewCreateInfo tv = vi;
      tv.image = s->tim;
      CHECK(CreateImageView(dev, &tv, NULL, &s->tiv) == VK_SUCCESS, "tex view");
      VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                                .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
      CHECK(CreateSampler(dev, &si, NULL, &s->smp) == VK_SUCCESS, "sampler");
      VkDescriptorImageInfo ii2 = {s->smp, s->tiv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->dset[0], .dstBinding = 0,
                                .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                .pImageInfo = &ii2};
      UpdateDescriptorSets(dev, 1, &w, 0, NULL);
   }

   VkPipelineShaderStageCreateInfo st[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
       .module = s->vs, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .module = s->fs, .pName = "main"}};
   VkVertexInputBindingDescription bd[2] = {{0, 16, VK_VERTEX_INPUT_RATE_VERTEX}, {1, 16, VK_VERTEX_INPUT_RATE_VERTEX}};
   VkVertexInputAttributeDescription ad2[2] = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0}};
   if (g_sparse) ad2[1].location = 2;
   if (g_dynstr) bd[0].stride = bd[1].stride = 4;
   if (g_inst) bd[0].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
   VkPipelineVertexInputStateCreateInfo vis = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = g_inst ? 1 : 2, .pVertexBindingDescriptions = bd,
                                               .vertexAttributeDescriptionCount = g_inst ? 1 : 2, .pVertexAttributeDescriptions = ad2};
   VkPipelineInputAssemblyStateCreateInfo ias = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = s->mode == MODE_MVP ? 4 : 1, .scissorCount = s->mode == MODE_MVP ? 4 : 1};
   VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                .polygonMode = g_fx.dynpoly ? VK_POLYGON_MODE_FILL : (VkPolygonMode)g_fx.poly, .cullMode = VK_CULL_MODE_NONE,
                                                .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f, .rasterizerDiscardEnable = g_discard != 0};
   VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                              .rasterizationSamples = g_msaa ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT,
                                              .sampleShadingEnable = g_msaa == 2, .minSampleShading = g_msaa == 2 ? 1.0f : 0.0f};
   VkPipelineDepthStencilStateCreateInfo dss = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
                                                .depthCompareOp = VK_COMPARE_OP_LESS, .maxDepthBounds = 1.0f};
   if (g_st.on) {
      dss.depthTestEnable = dss.depthWriteEnable = VK_FALSE;
      dss.stencilTestEnable = VK_TRUE;
      VkStencilOpState f = {.failOp = g_st.fail, .passOp = g_st.pass, .depthFailOp = g_st.dfail, .compareOp = g_st.cmp_f,
                            .compareMask = g_st.dyn ? 0x0f : g_st.cmask, .writeMask = g_st.dyn ? 0 : g_st.wmask,
                            .reference = g_st.dyn ? 9 : g_st.ref};
      dss.front = f;
      dss.back = f;
      dss.back.compareOp = g_st.cmp_b;
      rs.frontFace = g_st.cw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
   }
   VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
   if (g_fx.on) {
      rs.cullMode = g_fx.dyn ? VK_CULL_MODE_NONE : g_fx.cull;
      rs.frontFace = (g_fx.cw != 0) != (g_fx.dyn != 0) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
      rs.depthClampEnable = g_fx.clamp;
      if (g_fx.dclip) { g_dce.depthClipEnable = g_fx.dclip == 2; rs.pNext = &g_dce; }
      rs.depthBiasEnable = g_fx.bias;
      if (!g_fx.dyn) { rs.depthBiasConstantFactor = g_fx.bias_c; rs.depthBiasSlopeFactor = g_fx.bias_s; rs.depthBiasClamp = g_fx.bias_clamp; }
      cba.blendEnable = g_fx.blend;
      cba.srcColorBlendFactor = cba.srcAlphaBlendFactor = g_fx.sf;
      cba.dstColorBlendFactor = cba.dstAlphaBlendFactor = g_fx.df;
      cba.colorBlendOp = cba.alphaBlendOp = g_fx.op;
      cba.colorWriteMask = g_fx.wmask;
   }
   VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = g_nocol ? 0 : 1, .pAttachments = &cba};
   VkDynamicState dyn[11] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE,
                            VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
   VkPipelineDynamicStateCreateInfo dys = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = g_fx.on && g_fx.dyn ? 6 : 2, .pDynamicStates = dyn};
   if (g_dynstr) dyn[dys.dynamicStateCount++] = VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE;
   if (g_fx.dynpoly) dyn[dys.dynamicStateCount++] = VK_DYNAMIC_STATE_POLYGON_MODE_EXT;
   if (g_st.on && g_st.dyn) {   /* the three stencil states follow the others */
      for (int k = 0; k < 3; k++) dyn[dys.dynamicStateCount + k] = dyn[6 + k];
      dys.dynamicStateCount += 3;
   }
   if (g_fx.on) cbs.blendConstants[0] = g_fx.dyn ? 0.9f : g_fx.bc[0], cbs.blendConstants[1] = g_fx.dyn ? 0.9f : g_fx.bc[1],
                cbs.blendConstants[2] = g_fx.dyn ? 0.9f : g_fx.bc[2], cbs.blendConstants[3] = g_fx.dyn ? 0.9f : g_fx.bc[3];
   VkGraphicsPipelineCreateInfo gp = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = g_discard == 2 || g_nofs ? 1 : 2,
                                      .pStages = st, .pVertexInputState = &vis, .pInputAssemblyState = &ias,
                                      .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                      .pDepthStencilState = depth ? &dss : NULL, .pColorBlendState = &cbs,
                                      .pDynamicState = &dys, .layout = s->layout, .renderPass = s->rp};
   VkResult r = CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &s->pipe);
   CHECK(r == VK_SUCCESS, "graphics pipeline = %d", r);

   VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   CHECK(CreateCommandPool(dev, &pci, NULL, &s->pool) == VK_SUCCESS, "pool");
   return 1;
}

static void
scene_destroy(struct scene *s)
{
   DeviceWaitIdle(dev);
   if (s->pool) DestroyCommandPool(dev, s->pool, NULL);
   if (s->pipe) DestroyPipeline(dev, s->pipe, NULL);
   if (s->fb) DestroyFramebuffer(dev, s->fb, NULL);
   if (s->rp) DestroyRenderPass(dev, s->rp, NULL);
   if (s->rp2) DestroyRenderPass(dev, s->rp2, NULL);
   for (int i = 0; i < 2; i++) if (s->vb[i]) DestroyBuffer(dev, s->vb[i], NULL);
   if (s->smp) DestroySampler(dev, s->smp, NULL);
   if (s->tiv) DestroyImageView(dev, s->tiv, NULL);
   if (s->tim) DestroyImage(dev, s->tim, NULL);
   if (s->timem) FreeMemory(dev, s->timem, NULL);
   for (int i = 0; i < 3; i++) if (s->ubo[i]) DestroyBuffer(dev, s->ubo[i], NULL);
   if (s->ubomem) FreeMemory(dev, s->ubomem, NULL);
   if (s->dpool) DestroyDescriptorPool(dev, s->dpool, NULL);
   if (s->dsl) DestroyDescriptorSetLayout(dev, s->dsl, NULL);
   if (s->ib) DestroyBuffer(dev, s->ib, NULL);
   if (s->ibmem) FreeMemory(dev, s->ibmem, NULL);
   if (s->vbmem) FreeMemory(dev, s->vbmem, NULL);
   if (s->msv) DestroyImageView(dev, s->msv, NULL);
   if (s->msi) DestroyImage(dev, s->msi, NULL);
   if (s->msmem) FreeMemory(dev, s->msmem, NULL);
   if (s->colv) DestroyImageView(dev, s->colv, NULL);
   if (s->depv) DestroyImageView(dev, s->depv, NULL);
   if (s->col) DestroyImage(dev, s->col, NULL);
   if (s->dep) DestroyImage(dev, s->dep, NULL);
   if (s->colmem) FreeMemory(dev, s->colmem, NULL);
   if (s->depmem) FreeMemory(dev, s->depmem, NULL);
   if (s->layout) DestroyPipelineLayout(dev, s->layout, NULL);
   if (s->vs) DestroyShaderModule(dev, s->vs, NULL);
   if (s->fs) DestroyShaderModule(dev, s->fs, NULL);
}

/* draw n triangles (rot[i] = cos, sin, ox, oy), submit, wait */
static int
scene_draw(struct scene *s, int n, const float (*rot)[4], VkFence fence)
{
   VkCommandBuffer cb = s->cb;
   double t_rec0 = now_us();
   if (!cb || !g_reset) {
      VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s->pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
      CHECK(AllocateCommandBuffers(dev, &ai, &cb) == VK_SUCCESS, "alloc cb");
      s->cb = cb;
   } else {
      CHECK(ResetCommandPool(dev, s->pool, 0) == VK_SUCCESS, "reset pool");
   }
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   CHECK(BeginCommandBuffer(cb, &bi) == VK_SUCCESS, "begin");
   VkClearValue cv[2] = {{.color = {{0, 0, 0, 0}}}, {.depthStencil = {1.0f, g_st.clear}}};
   if (g_clear_f)
      memcpy(cv[0].color.float32, g_clear_f, 16);
   if (g_nocol)
      cv[0] = cv[1];
   if (g_clear_u)
      cv[0].color.uint32[0] = 200, cv[0].color.uint32[1] = 100, cv[0].color.uint32[2] = 50, cv[0].color.uint32[3] = 7;
   VkRenderPassBeginInfo rb = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = s->second ? s->rp2 : s->rp,
                               .framebuffer = s->fb, .renderArea = {{0, 0}, {W, H}},
                               .clearValueCount = g_nocol ? 1 : s->depth ? 2 : 1, .pClearValues = cv};
   CmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);
   VkViewport vp = {0, 0, W, H, 0.0f, 1.0f};
   VkRect2D sc = {{0, 0}, {W, H}};
   if (s->mode == MODE_SCISSOR) sc.extent.width = W / 2;   /* left half only */
   if (s->mode == MODE_VPORT || s->mode == MODE_MVP) vp = (VkViewport){0, 0, W / 2, H, 0.0f, 1.0f};   /* NDC quad maps into the left half */
   if (s->mode == MODE_CLEARATT) {
      VkClearAttachment ca = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0,
                              .clearValue = {.color = {{0, 0, 1, 1}}}};
      VkClearRect cr = {.rect = {{0, 0}, {W, H}}, .baseArrayLayer = 0, .layerCount = 1};
      CmdClearAttachments(cb, 1, &ca, 1, &cr);
   }
   if (s->mode == MODE_MVP) {
      /* multiViewport: 4 viewports/scissors; only index 0 is reachable without a geometry shader, so 1..3 are junk that must be ignored.
       * Set in two calls (first 2..3, then 0..1) to cover firstViewport != 0. */
      VkViewport vj[2] = {{0, 0, 3, 3, 0.0f, 1.0f}, {W / 2, 0, W / 2, H, 0.0f, 1.0f}};
      VkRect2D sj[2] = {{{0, 0}, {1, 1}}, {{0, 0}, {W, H}}};
      VkViewport v01[2] = {vp, {0, 0, 5, 5, 0.0f, 1.0f}};
      VkRect2D s01[2] = {sc, {{0, 0}, {2, 2}}};
      CmdSetViewport(cb, 2, 2, vj);
      CmdSetScissor(cb, 2, 2, sj);
      CmdSetViewport(cb, 0, 2, v01);
      CmdSetScissor(cb, 0, 2, s01);
   } else {
      CmdSetViewport(cb, 0, 1, &vp);
      CmdSetScissor(cb, 0, 1, &sc);
   }
   if (g_fx.on && g_fx.dyn) {
      CmdSetCullMode(cb, g_fx.cull);
      CmdSetFrontFace(cb, g_fx.cw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
      CmdSetDepthBias(cb, g_fx.bias_c, g_fx.bias_clamp, g_fx.bias_s);
      CmdSetBlendConstants(cb, g_fx.bc);
   }
   if (g_fx.dynpoly) CmdSetPolygonModeEXT_(cb, (VkPolygonMode)g_fx.poly);
   VkBuffer vbs[2] = {s->vb[0], s->vb[1]};
   VkDeviceSize offs[2] = {0, 0};
   if (g_st.on && g_st.dyn) {
      CmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, g_st.cmask);
      CmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, g_st.wmask);
      CmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, g_st.ref);
   }
   if (g_inst) {
      /* instance k of binding 0 = quad corner k (vertex 3 + k of the position buffer): firstInstance k draws the triangle at corner k */
      VkDeviceSize base = 3 * 16;
      CmdBindVertexBuffers(cb, 0, 1, vbs, &base);
      CmdDraw(cb, 3, 1, 0, 0);                              /* corner 0 */
      if (g_inst == 1) {
         CmdDraw(cb, 3, 1, 0, 1);                           /* differs only in firstInstance: corner 1 */
         VkDeviceSize b2 = base + 2 * 16;
         CmdBindVertexBuffers(cb, 0, 1, vbs, &b2);
         CmdDraw(cb, 3, 1, 0, 1);                           /* differs only in the vertex buffer offset: corner 3 */
         CmdDraw(cb, 3, 1, 0, 0);                           /* differs only in firstInstance: corner 2 */
      } else {
         CmdDraw(cb, 3, 1, 0, 0);
         CmdDraw(cb, 3, 1, 0, 0);
         CmdDraw(cb, 3, 1, 0, 0);
      }
      n = 0;
   } else if (g_dynstr) {
      /* two draws that differ only in the dynamic stride: stride 0 (every vertex = vertex 0: degenerate, no pixels), then 16 (the triangle) */
      VkDeviceSize sz[2] = {VK_WHOLE_SIZE, VK_WHOLE_SIZE}, z[2] = {0, 0}, st16[2] = {16, 16};
      CmdBindVertexBuffers2_(cb, 0, 2, vbs, offs, sz, z);
      CmdPushConstants(cb, s->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, rot[0]);
      CmdDraw(cb, 3, 1, 0, 0);
      CmdBindVertexBuffers2_(cb, 0, 2, vbs, offs, sz, st16);
      if (g_dynstr == 2) {
         /* a plain vkCmdBindVertexBuffers after the 2-form (new offsets/sizes only) must keep the stride 16 set above: if it reset the
          * stride to 0 the draw below would be degenerate and the triangle missing */
         CmdBindVertexBuffers(cb, 0, 2, vbs, offs);
      }
      CmdPushConstants(cb, s->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, rot[0]);
      CmdDraw(cb, 3, 1, 0, 0);
      n = 0;
   } else
   CmdBindVertexBuffers(cb, 0, 2, vbs, offs);
   /* the core 1.3 two-form (what zink always calls) must land on the same binding; strides NULL = the pipeline's. A draw that follows is the test. */
   { VkDeviceSize sizes[2] = {VK_WHOLE_SIZE, VK_WHOLE_SIZE}; CmdBindVertexBuffers2(cb, 0, 2, vbs, offs, sizes, NULL); }
   if (s->mode == MODE_IDX16) CmdBindIndexBuffer(cb, s->ib, 0, VK_INDEX_TYPE_UINT16);
   if (s->mode >= MODE_PUSHFS || s->mode == MODE_IDX32 || s->mode == MODE_MDI || s->mode == MODE_SCISSOR || s->mode == MODE_VPORT || s->mode == MODE_MVP) CmdBindIndexBuffer(cb, s->ib, 16, VK_INDEX_TYPE_UINT32);
   if ((s->mode == MODE_TEX || s->mode == MODE_UBO) && !g_dyn)
      CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->layout, 0, 1, &s->dset[0], 0, NULL);
   for (int i = 0; i < n; i++) {
      if (g_dirty == 1) {
         VkViewport vi = {(i & 1) ? W / 2 : 0, 0, W / 2, H, 0.0f, 1.0f};
         CmdSetViewport(cb, 0, 1, &vi);
      } else if (g_dirty == 2)
         CmdSetCullMode(cb, (i & 1) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
      else if (g_dirty == 3)
         CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);
      else if (g_dirty == 4) {
         VkRect2D si = {{(i & 1) ? W / 2 : 0, 0}, {W / 2, H}};
         CmdSetScissor(cb, 0, 1, &si);
      }
      if (g_share == 2) {
         VkRect2D sci = {{(i & 1) ? W / 2 : 0, 0}, {W / 2, H}};
         CmdSetScissor(cb, 0, 1, &sci);
      }
      if (s->mode == MODE_PUSHFS) {
         float pc[8] = {rot[i][0], rot[i][1], rot[i][2], rot[i][3], 0.75f, 0.25f, 0.5f, 1.0f};
         CmdPushConstants(cb, s->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
      } else {
         CmdPushConstants(cb, s->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, rot[i]);
      }
      if (g_dyn) {
         uint32_t off = (g_share == 3 ? g_share_off[i] : (uint32_t)(i % 3)) * 256;   /* three colour blocks, 256 B apart */
         CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->layout, 0, 1, &s->dset[0], 1, &off);
      }
      if (s->mode == MODE_DESCUPD)
         CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->layout, 0, 1, &s->dset[i & 1], 0, NULL);
      if (s->mode == MODE_MDI)
         CmdDrawIndexedIndirect(cb, s->ib, 64, 2, 20);   /* multiDrawIndirect: drawCount 2 */
      else if (s->mode >= MODE_PUSHFS)
         CmdDrawIndexed(cb, 6, 1, 0, 7, g_share == 3 ? g_share_fi[i] : 0);
      else if (s->mode == MODE_IDX16 || s->mode == MODE_IDX32 || s->mode == MODE_SCISSOR || s->mode == MODE_VPORT || s->mode == MODE_MVP)
         CmdDrawIndexed(cb, 6, 1, 0, 3, 0);
      else {
         CmdDraw(cb, 3, 1, 0, 0);
         /* polygonMode POINT: a 2-vertex triangle-list draw assembles no triangle and is rewritten to a different primitive than the draw above;
          * with MALI_DIRTY_CHECK (check.sh) a reuse of the previous descriptors across that change aborts in build_draw_descs */
         if (g_fx.poly == 2) CmdDraw(cb, 2, 1, 0, 0);
      }
   }
   if (s->mode == MODE_CLEARQUAD) {   /* CmdClearAttachments AFTER a draw, sub-rectangle: a constant-colour quad (the triangle must survive outside the rect) */
      VkClearAttachment ca = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0, .clearValue = {.color = {{0, 0, 1, 1}}}};
      VkClearRect cr = {.rect = {{100, 100}, {60, 60}}, .baseArrayLayer = 0, .layerCount = 1};
      VkClearAttachment cas[2] = {ca, {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .clearValue = {.depthStencil = {1.0f, 0}}}};
      CmdClearAttachments(cb, g_clearzs && s->depth ? 2 : 1, cas, 1, &cr);
   }
   CmdEndRenderPass(cb);
   CHECK(EndCommandBuffer(cb) == VK_SUCCESS, "end");
   g_t_rec += now_us() - t_rec0;
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   double t_sub0 = now_us();
   VkResult r = QueueSubmit(queue, 1, &si, fence);
   g_t_sub += now_us() - t_sub0;
   CHECK(r == VK_SUCCESS, "submit = %d", r);
   r = WaitForFences(dev, 1, &fence, VK_TRUE, 20000000000ull);
   CHECK(r == VK_SUCCESS, "wait = %d", r);
   ResetFences(dev, 1, &fence);
   if (!g_reset)
      FreeCommandBuffers(dev, s->pool, 1, &cb), s->cb = NULL;
   return 1;
}

static uint32_t
count_cov(struct scene *s)
{
   uint32_t n = 0;
   for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++)
         n += g_colbpp == 8 ? (s->px[y * s->pitch + x * 8 + 6] | s->px[y * s->pitch + x * 8 + 7]) != 0 : s->px[y * s->pitch + x * 4 + 3] != 0;
   return n;
}

static int
report(const char *name, struct scene *s, double expect, double tolpct, int exact_colour)
{
   if (g_fake) {
      printf("{\"test\":\"%s\",\"skipped\":\"fake backend\",\"ok\":true}\n", name);
      return 1;
   }
   uint32_t cov = count_cov(s);
   double err = fabs((double)cov - expect) / expect * 100.0;
   CHECK(err <= tolpct, "%s coverage %u expected %.0f (%.2f%%)", name, cov, expect, err);
   if (exact_colour) {
      /* centroid of (64,64) red, (192,64) green, (128,192) blue = (128,106): ~equal thirds */
      const uint8_t *p = s->px + 106 * s->pitch + 128 * 4;
      CHECK(abs(p[0] - 85) < 12 && abs(p[1] - 85) < 12 && abs(p[2] - 85) < 12 && p[3] == 255,
            "centroid colour %u,%u,%u,%u", p[0], p[1], p[2], p[3]);
      const uint8_t *q = s->px + 70 * s->pitch + 74 * 4; /* near the red vertex */
      CHECK(q[0] > 200 && q[1] < 60 && q[2] < 60, "red corner %u,%u,%u", q[0], q[1], q[2]);
      const uint8_t *g = s->px + 70 * s->pitch + 182 * 4;
      CHECK(g[1] > 200 && g[0] < 60 && g[2] < 60, "green corner %u,%u,%u", g[0], g[1], g[2]);
      const uint8_t *b = s->px + 182 * s->pitch + 128 * 4;
      CHECK(b[2] > 200 && b[0] < 60 && b[1] < 60, "blue corner %u,%u,%u", b[0], b[1], b[2]);
   }
   printf("{\"test\":\"%s\",\"coverage\":%u,\"expect\":%.0f,\"err_pct\":%.3f,\"ok\":true}\n", name, cov, expect, err);
   return 1;
}

static const float ID[4] = {1, 0, 0, 0};

static int
test_triangle(void)
{
   struct scene s = {.mode = MODE_TRI};   /* scene_create() takes s->mode as an input */
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   if (!scene_create(&s, 0)) return 0;
   double t0 = now_us();
   for (int i = 0; i < g_loop; i++) {
      /* rotate by i degrees around the centre: area is invariant */
      float a = (float)(i % 360) * 3.14159265f / 180.0f;
      float rot[1][4] = {{cosf(a), sinf(a), 0, 0}};
      if (!scene_draw(&s, 1, rot, f)) return 0;
      if (!g_fake) {
         uint32_t cov = count_cov(&s);
         double err = fabs((double)cov - 8192.0) / 8192.0 * 100.0;
         CHECK(err <= 1.0, "iteration %d coverage %u (%.2f%%)", i, cov, err);
      }
   }
   double dt = (now_us() - t0) / g_loop;
   /* identity rotation for the colour check */
   float rot[1][4] = {{ID[0], ID[1], ID[2], ID[3]}};
   if (!scene_draw(&s, 1, rot, f)) return 0;
   int ok = report("triangle", &s, 8192.0, 1.0, 1);
   printf("{\"test\":\"triangle_loop\",\"n\":%d,\"mean_us\":%.1f,\"loop_reset\":%d,\"ok\":%s}\n", g_loop, dt, g_reset,
          ok ? "true" : "false");
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

/* The counter file is per process: a fixed /tmp name was shared by every test run on the machine (other worktrees, parallel check.sh) and
 * made share_inst / share_dirty fail at random with wrapped or foreign byte totals. */
static char g_alloc_stats[64];
static void
alloc_stats_rm(void)
{
   if (g_alloc_stats[0]) unlink(g_alloc_stats);
}
static void
alloc_stats_start(void)   /* fresh counter file; the library writes the running total of hw_alloc bytes to $MALI_ALLOC_STATS at every submit */
{
   if (!g_alloc_stats[0]) {
      snprintf(g_alloc_stats, sizeof g_alloc_stats, "/tmp/mali_alloc_stats.%d.txt", (int)getpid());
      atexit(alloc_stats_rm);
   }
   unlink(g_alloc_stats);
   setenv("MALI_ALLOC_STATS", g_alloc_stats, 1);
}

/* arena bytes asked of hw_alloc so far (see alloc_stats_start) */
static unsigned long long
alloc_bytes(void)
{
   FILE *fp = fopen(g_alloc_stats, "r");
   unsigned long long v = 0;
   if (fp) {
      if (fscanf(fp, "%llu", &v) != 1) v = 0;
      fclose(fp);
   }
   return v;
}

/* ---- many draws in ONE frame (10k-50k): arena / CS / heap growth must not run out (was DEVICE_LOST above ~7k draws) ---- */
static int
test_many(void)
{
   int counts[] = {3000, 10000, 50000};
   if (getenv("MALI_MANY_COUNTS")) {   /* e.g. "3000": only the 3k frame (host profiling) */
      memset(counts, 0, sizeof counts);
      counts[0] = atoi(getenv("MALI_MANY_COUNTS"));
   }
   alloc_stats_start();
   const int nrep = getenv("MALI_MANY_REPS") ? atoi(getenv("MALI_MANY_REPS")) : 3;
   /* MALI_MANY_DYN=0: plain push-constant triangles; default: dynamic-UBO offset + DrawIndexed per draw (the bench2 triangle shape) */
   g_dyn = !getenv("MALI_MANY_DYN") || atoi(getenv("MALI_MANY_DYN"));
   struct scene s = {.mode = g_dyn ? MODE_UBO : MODE_TRI};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   if (!scene_create(&s, 0)) return 0;
   float (*rot)[4] = malloc(50000 * sizeof *rot);
   for (int i = 0; i < 50000; i++) memcpy(rot[i], ID, sizeof rot[i]);
   int ok = 1;
   for (unsigned c = 0; ok && c < sizeof counts / sizeof *counts && counts[c]; c++) {
      g_t_rec = g_t_sub = 0;
      unsigned long long ab0 = alloc_bytes();   /* total at the previous submit: the first rep of a size counts from there */
      double t0 = now_us();
      for (int rep = 0; ok && rep < nrep; rep++)
         ok &= scene_draw(&s, counts[c], (const float (*)[4])rot, f);
      printf("{\"test\":\"many_draws\",\"draws\":%d,\"dyn_ubo\":%d,\"us_per_draw\":%.3f,\"record_us\":%.3f,\"submit_us\":%.3f,\"alloc_B_per_draw\":%.0f,\"ok\":%s}\n", counts[c], g_dyn,
             (now_us() - t0) / nrep / counts[c], g_t_rec / nrep / counts[c], g_t_sub / nrep / counts[c],
             (double)(alloc_bytes() - ab0) / nrep / counts[c], ok ? "true" : "false");
   }
   if (ok && !g_dyn) ok = report("many_draws", &s, 8192.0, 1.0, 1);
   g_dyn = 0;
   free(rot);
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}


static uint32_t px_at(struct scene *s, int x, int y);

/* descriptor sharing between consecutive draws: draws that differ in exactly one thing (push constant / viewport-scissor) must each get
 * their own data. Pixels on hardware only. */
static int
test_share(void)
{
   struct scene s = {.mode = MODE_TRI};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   if (!scene_create(&s, 0)) return 0;
   int ok = 1;
   /* four quarter-size triangles at (+-.5, +-.5); consecutive duplicates (identical state) are mixed with changes */
   const float q[4][4] = {{.5f, 0, -.5f, -.5f}, {.5f, 0, .5f, -.5f}, {.5f, 0, -.5f, .5f}, {.5f, 0, .5f, .5f}};
   float rot[8][4];
   const int order[8] = {0, 0, 1, 2, 2, 2, 3, 1};
   for (int i = 0; i < 8; i++) memcpy(rot[i], q[order[i]], sizeof rot[i]);
   ok &= scene_draw(&s, 8, (const float (*)[4])rot, f);
   if (ok && !g_fake) {
      for (int k = 0; k < 4; k++) {
         int cx = (int)((q[k][2] * .5f + .5f) * W), cy = (int)((q[k][3] * .5f + .5f) * H);
         CHECK(px_at(&s, cx, cy) != 0, "share: triangle %d (push constant) missing at %d,%d", k, cx, cy);
      }
      CHECK(px_at(&s, W / 2, H / 2) == 0 || g_colbpp == 8, "share: stray pixel at the centre");
      ok &= report("share_push", &s, 8192.0, 2.0, 0);
   } else if (ok) printf("{\"test\":\"share_push\",\"skipped\":\"fake backend\",\"ok\":true}\n");
   /* identical draw and push constants, scissor alternates left/right half: both halves must be drawn */
   g_share = 2;
   float id[4][4];
   for (int i = 0; i < 4; i++) memcpy(id[i], ID, sizeof id[i]);
   ok &= scene_draw(&s, 4, (const float (*)[4])id, f);
   g_share = 0;
   if (ok) ok &= report("share_scissor", &s, 8192.0, 1.0, 0);
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

/* ---- stencil: coverage / pixel checks against the stencil test (pixels only on hardware) ---- */
static uint32_t
px_at(struct scene *s, int x, int y)
{
   uint32_t v;
   memcpy(&v, s->px + y * s->pitch + x * g_colbpp, 4);
   return v;
}

struct st_case {
   const char *name;
   VkCompareOp cmp_f, cmp_b;
   VkStencilOp fail, pass;
   uint32_t cmask, wmask, ref, clear;
   int cw, dyn;
   double expect;               /* coverage of one triangle (8192 px) or 0 */
};

static int
test_st_cov(const struct st_case *c, uint32_t *covout)
{
   struct scene s = {.mode = MODE_TRI};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   g_st = (struct st_state){.on = 1, .cmp_f = c->cmp_f, .cmp_b = c->cmp_b, .fail = c->fail, .dfail = VK_STENCIL_OP_KEEP,
                         .pass = c->pass, .cmask = c->cmask, .wmask = c->wmask, .ref = c->ref, .clear = c->clear,
                         .cw = c->cw, .dyn = c->dyn};
   int ok = scene_create(&s, 1);
   float rot[1][4] = {{1, 0, 0, 0}};
   if (ok) ok = scene_draw(&s, 1, rot, f);
   g_st = (struct st_state){0};
   if (ok) {
      if (g_fake) {
         printf("{\"test\":\"%s\",\"skipped\":\"fake backend\",\"ok\":true}\n", c->name);
      } else {
         uint32_t cov = count_cov(&s);
         if (covout) { *covout = cov; scene_destroy(&s); DestroyFence(dev, f, NULL); return 1; }
         ok = c->expect == 0 ? cov == 0 : fabs((double)cov - c->expect) / c->expect * 100.0 <= 2.5;
         CHECK(ok, "%s coverage %u expected %.0f", c->name, cov, c->expect);
         printf("{\"test\":\"%s\",\"coverage\":%u,\"ok\":%s}\n", c->name, cov, ok ? "true" : "false");
      }
   }
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

/* centre pixel of: draw A only, B only, A then B under the stencil state `st`.
 * wmask != 0 -> the INCR_CLAMP pass op marks A, B is rejected where A overlaps (centre == A);
 * wmask == 0 -> nothing is written, B overwrites (centre == B). */
static int
test_st_two(const char *name, int dyn, uint32_t wmask)
{
   struct scene s = {.mode = MODE_TRI};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   g_st = (struct st_state){.on = 1, .cmp_f = VK_COMPARE_OP_EQUAL, .cmp_b = VK_COMPARE_OP_EQUAL, .fail = VK_STENCIL_OP_KEEP,
                         .pass = VK_STENCIL_OP_INCREMENT_AND_CLAMP, .cmask = 0xff, .wmask = wmask, .ref = 0, .clear = 0, .dyn = dyn};
   int ok = scene_create(&s, 1);
   float one[2][4] = {{1, 0, 0, 0}, {-1, 0, 0, 0}};
   uint32_t a = 0, b = 0, ab = 0;
   if (ok) ok = scene_draw(&s, 1, &one[0], f);
   if (ok) a = px_at(&s, 128, 128);
   if (ok) ok = scene_draw(&s, 1, &one[1], f);
   if (ok) b = px_at(&s, 128, 128);
   if (ok) ok = scene_draw(&s, 2, one, f);
   if (ok) ab = px_at(&s, 128, 128);
   g_st = (struct st_state){0};
   if (ok) {
      if (g_fake) {
         printf("{\"test\":\"%s\",\"skipped\":\"fake backend\",\"ok\":true}\n", name);
      } else {
         CHECK(a != b, "%s: centre A %08x == B %08x (test is blind)", name, a, b);
         uint32_t want = wmask ? a : b;
         ok = ok && ab == want;
         CHECK(ab == want, "%s: centre %08x expected %08x (A %08x B %08x)", name, ab, want, a, b);
         printf("{\"test\":\"%s\",\"centre\":\"%08x\",\"want\":\"%08x\",\"ok\":%s}\n", name, ab, want, ok ? "true" : "false");
      }
   }
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

static int
test_stencil(VkFormat fmt)
{
   VkFormat save = g_depfmt;
   g_depfmt = fmt;
   const VkCompareOp EQ = VK_COMPARE_OP_EQUAL, NV = VK_COMPARE_OP_NEVER, AL = VK_COMPARE_OP_ALWAYS;
   const VkStencilOp KP = VK_STENCIL_OP_KEEP;
   const struct st_case cases[] = {
      {"stencil_never", NV, NV, KP, KP, 0xff, 0xff, 0, 0, 0, 0, 0},
      {"stencil_always", AL, AL, KP, KP, 0xff, 0xff, 0, 0, 0, 0, 8192},
      {"stencil_eq_pass", EQ, EQ, KP, KP, 0xff, 0xff, 5, 5, 0, 0, 8192},
      {"stencil_eq_fail", EQ, EQ, KP, KP, 0xff, 0xff, 4, 5, 0, 0, 0},
      {"stencil_cmask", EQ, EQ, KP, KP, 0x0f, 0xff, 0x05, 0x15, 0, 0, 8192},     /* (0x15 & 0xf) == (0x05 & 0xf) */
      {"stencil_cmask_fail", EQ, EQ, KP, KP, 0xff, 0xff, 0x05, 0x15, 0, 0, 0},
      {"stencil_dyn_ref", EQ, EQ, KP, KP, 0xff, 0xff, 5, 5, 0, 1, 8192},         /* static ref is 9, mask 0x0f */
      {"stencil_dyn_cmask", EQ, EQ, KP, KP, 0x0f, 0xff, 0x05, 0x15, 0, 1, 8192},
   };
   int ok = 1;
   for (unsigned i = 0; ok && i < sizeof cases / sizeof *cases; i++) {
      char nm[64];
      struct st_case c = cases[i];
      snprintf(nm, sizeof nm, "%s%s", c.name, fmt == VK_FORMAT_S8_UINT ? "_s8" : fmt == VK_FORMAT_D32_SFLOAT_S8_UINT ? "_d32s8" : "_d24s8");
      c.name = nm;
      ok = test_st_cov(&c, NULL);
   }
   /* back faces use the back state: front NEVER / back ALWAYS shows the triangle for exactly one winding */
   if (ok) {
      struct st_case fb = {"stencil_face", NV, AL, KP, KP, 0xff, 0xff, 0, 0, 0, 0, -1}, r0, r1;
      r0 = fb; r1 = fb;
      r1.cw = 1;
      r0.expect = 0; r1.expect = 8192;
      char nm0[64], nm1[64];
      const char *sfx = fmt == VK_FORMAT_S8_UINT ? "_s8" : fmt == VK_FORMAT_D32_SFLOAT_S8_UINT ? "_d32s8" : "_d24s8";
      snprintf(nm0, sizeof nm0, "stencil_face_ccw%s", sfx);
      snprintf(nm1, sizeof nm1, "stencil_face_cw%s", sfx);
      r0.name = nm0; r1.name = nm1;
      /* which winding is the back face depends on the vertex order: exactly one of the two must show the triangle */
      uint32_t c0 = 0, c1 = 0;
      ok = test_st_cov(&r0, &c0) && test_st_cov(&r1, &c1);
      if (ok && !g_fake) {
         ok = (c0 == 0 && c1 > 8192 * 0.975 && c1 < 8192 * 1.025) || (c1 == 0 && c0 > 8192 * 0.975 && c0 < 8192 * 1.025);
         CHECK(ok, "%s: front-NEVER/back-ALWAYS coverage ccw %u cw %u (want one 0, one ~8192)", nm0, c0, c1);
         printf("{\"test\":\"stencil_face%s\",\"cov_ccw\":%u,\"cov_cw\":%u,\"ok\":%s}\n", sfx, c0, c1, ok ? "true" : "false");
      }
   }
   if (ok) {
      char nm[64];
      const char *sfx = fmt == VK_FORMAT_S8_UINT ? "_s8" : fmt == VK_FORMAT_D32_SFLOAT_S8_UINT ? "_d32s8" : "_d24s8";
      snprintf(nm, sizeof nm, "stencil_incr%s", sfx);
      ok = test_st_two(nm, 0, 0xff);
      if (ok) { snprintf(nm, sizeof nm, "stencil_wmask0%s", sfx); ok = test_st_two(nm, 0, 0); }
      if (ok) { snprintf(nm, sizeof nm, "stencil_dyn_wmask%s", sfx); ok = test_st_two(nm, 1, 0xff); }
      if (ok) { snprintf(nm, sizeof nm, "stencil_dyn_wmask0%s", sfx); ok = test_st_two(nm, 1, 0); }
   }
   g_depfmt = save;
   return ok;
}

/* loadOp LOAD: pass 1 (CLEAR) draws A, pass 2 (LOAD) draws B; the result must equal one pass drawing A then B.
 * kind: 0 depth (g_depfmt), 1 stencil (D32S8, INCR_AND_CLAMP/EQUAL 0, so B is rejected where A overlaps), 2 colour (g_colfmt, no depth). */
static int
test_load(const char *name, int kind)
{
   struct scene s = {.mode = MODE_TRI};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   VkFormat save = g_depfmt;
   if (kind == 1 || kind == 3 || kind == 4) {
      g_depfmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
      g_st = (struct st_state){.on = 1, .cmp_f = VK_COMPARE_OP_EQUAL, .cmp_b = VK_COMPARE_OP_EQUAL, .fail = VK_STENCIL_OP_KEEP,
                               .pass = kind == 3 ? VK_STENCIL_OP_INVERT : kind == 4 ? VK_STENCIL_OP_DECREMENT_AND_WRAP : VK_STENCIL_OP_INCREMENT_AND_CLAMP, .cmask = 0xff, .wmask = 0xff, .ref = 0, .clear = 0};
   }
   g_load = 1;
   int ok = scene_create(&s, kind != 2);
   g_load = 0;
   g_st = (struct st_state){0};
   g_depfmt = save;
   float AB[2][4] = {{1, 0, 0, 0}, {-1, 0, 0, 0}};
   if (kind == 2) { AB[0][2] = -.5f; AB[1][0] = 1; AB[1][2] = .5f; }
   uint32_t a = 0, b = 0, ref = 0, got = 0, cref = 0, cgot = 0;
   if (ok) ok = scene_draw(&s, 2, AB, f);               /* reference: both in one pass (the stencil state lives in s) */
   static const int PX[6][2] = {{128, 128}, {128, 90}, {128, 170}, {90, 150}, {166, 110}, {128, 40}};
   uint32_t pref[6] = {0}, pgot[6] = {0}, p1[6] = {0};
   uint32_t cov1 = 0;
   if (ok) { ref = px_at(&s, 128, 128); cref = count_cov(&s); for (int i = 0; i < 6; i++) pref[i] = px_at(&s, PX[i][0], PX[i][1]); }
   if (ok) ok = scene_draw(&s, 1, &AB[0], f);
   if (ok) a = px_at(&s, 128, 128);
   if (ok) ok = scene_draw(&s, 1, &AB[1], f);
   if (ok) b = px_at(&s, 128, 128);
   if (ok) ok = scene_draw(&s, 1, &AB[0], f);          /* pass 1: clear, A, store */
   if (ok) { cov1 = count_cov(&s); for (int i = 0; i < 6; i++) p1[i] = px_at(&s, PX[i][0], PX[i][1]); }
   s.second = 1;
   if (ok) ok = scene_draw(&s, 1, &AB[1], f);          /* pass 2: LOAD colour+depth(+stencil), B */
   s.second = 0;
   if (ok) { got = px_at(&s, 128, 128); cgot = count_cov(&s); for (int i = 0; i < 6; i++) pgot[i] = px_at(&s, PX[i][0], PX[i][1]); }
   if (ok) {
      if (g_fake) {
         printf("{\"test\":\"%s\",\"skipped\":\"fake backend\",\"ok\":true}\n", name);
      } else {
         if (kind != 2) CHECK(a != b, "%s: centre A %08x == B %08x (test is blind)", name, a, b);
         ok = got == ref && cgot == cref;
         int same = 1;
         for (int i = 0; i < 6; i++) same &= pgot[i] == pref[i];
         ok = ok && same;
         if (!ok) {
            /* which failure: pass 2 drew nothing (cov == pass 1), preload did not run (B overwrote A where it must be rejected: cov > ref), or B was rejected too much (cov < ref) */
            const char *why = cgot == cov1 ? "pass2 wrote nothing (colour marker unchanged)" : cgot > cref ? "preload did not take effect: B drawn over A (depth/stencil came back as cleared)" : cgot < cref ? "B over-rejected: depth/stencil came back wrong (too near / non-zero)" : "coverage equal, pixel values differ";
            char pxs[400]; int n = 0;
            for (int i = 0; i < 6; i++)
               n += snprintf(pxs + n, sizeof pxs - n, " (%d,%d)%08x/%08x/%08x", PX[i][0], PX[i][1], p1[i], pref[i], pgot[i]);
            printf("{\"fail\":\"%s: %s; cov pass1 %u ref %u got %u; px pass1/want/got:%s\"}\n", name, why, cov1, cref, cgot, pxs);
         }
         CHECK(ok, "%s: two-pass centre %08x cov %u vs one-pass %08x cov %u (A %08x B %08x)", name, got, cgot, ref, cref, a, b);
         printf("{\"test\":\"%s\",\"centre\":\"%08x\",\"want\":\"%08x\",\"cov\":%u,\"ok\":%s}\n", name, got, ref, cgot, ok ? "true" : "false");
      }
   }
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

/* RAW (non-blendable) render target: integer (RGBA8UI, RG16UI, R8UI, A8B8G8R8/A2B10G10R10 integer: clear to the raw words) or SNORM / UNORM16 / float (cf: clear to the floats); the render pass clears, one triangle is drawn; the corner pixels must hold the words */
static int
test_int_clear_f(const char *name, VkFormat fmt, unsigned bpp, uint32_t want0, const float *cf)
{
   struct scene s = {.mode = MODE_TRI};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   g_colfmt = fmt;
   g_colbpp = bpp;
   g_clear_u = !cf;
   g_clear_f = cf;
   int ok = scene_create(&s, 0);
   float one[1][4] = {{1, 0, 0, 0}};
   if (ok) ok = scene_draw(&s, 1, one, f);   /* a triangle in the middle; the corners keep the clear value */
   g_clear_u = 0;
   g_clear_f = NULL;
   uint32_t got = 0, mask = bpp >= 4 ? ~0u : (1u << (8 * bpp)) - 1;
   if (ok && !g_fake) {
      got = px_at(&s, 5, 5) & mask;
      ok = got == want0 && (px_at(&s, 250, 250) & mask) == want0;
   }
   printf("{\"test\":\"%s\",\"px\":\"%08x\",\"want\":\"%08x\",\"ok\":%s%s}\n", name, got, want0, ok ? "true" : "false", g_fake ? ",\"skipped\":\"fake backend\"" : "");
   CHECK(ok, "%s: pixel %08x want %08x", name, got, want0);
   g_colfmt = VK_FORMAT_R8G8B8A8_UNORM;
   g_colbpp = 4;
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

/* rasterizer discard: draws produce nothing (both stages present, or a vertex-only pipeline); the next pipeline draws normally */
static int
test_discard(const char *name, int mode)
{
   struct scene s = {.mode = MODE_TRI};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   g_discard = mode;
   int ok = scene_create(&s, 0);
   g_discard = 0;
   float one[1][4] = {{1, 0, 0, 0}};
   if (ok) ok = scene_draw(&s, 1, one, f);
   unsigned cov = 0;
   if (ok && !g_fake) {
      cov = count_cov(&s);
      ok = cov == 0;
   }
   printf("{\"test\":\"%s\",\"coverage\":%u,\"ok\":%s%s}\n", name, cov, ok ? "true" : "false", g_fake ? ",\"skipped\":\"fake backend\"" : "");
   CHECK(ok, "%s: coverage %u, want 0", name, cov);
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

/* dynamic vertex binding stride (rebind=1: a plain BindVertexBuffers follows the stride-16 2-form bind): static stride 4 is wrong; draw 1 binds stride 0 (nothing), draw 2 stride 16 (the triangle) */
static int
test_dynstride(const char *name, int rebind)
{
   struct scene s = {.mode = MODE_TRI};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   g_dynstr = rebind ? 2 : 1;
   int ok = scene_create(&s, 0);
   float one[1][4] = {{1, 0, 0, 0}};
   if (ok) ok = scene_draw(&s, 1, one, f);
   g_dynstr = 0;
   unsigned cov = 0;
   if (ok && !g_fake) {
      cov = count_cov(&s);
      ok = cov > 1000;
   }
   printf("{\"test\":\"%s\",\"coverage\":%u,\"ok\":%s%s}\n", name, cov, ok ? "true" : "false", g_fake ? ",\"skipped\":\"fake backend\"" : "");
   CHECK(ok, "%s: coverage %u, want the triangle", name, cov);
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

/* depth-only render pass (no colour attachment): the draw must complete */
static int
test_nocol(const char *name, int stencil)
{
   g_nofs = !strncmp(name, "nofs_", 5);   /* nofs_*: FS-less pipeline; the draw must still be emitted (golden stream), CTS .nocolor reads it back */
   struct scene s = {.mode = MODE_TRI};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   VkFormat save = g_depfmt;
   if (stencil) {
      g_depfmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
      g_st = (struct st_state){.on = 1, .cmp_f = VK_COMPARE_OP_ALWAYS, .cmp_b = VK_COMPARE_OP_ALWAYS, .fail = VK_STENCIL_OP_KEEP,
                               .pass = VK_STENCIL_OP_INCREMENT_AND_CLAMP, .cmask = 0xff, .wmask = 0xff, .ref = 0, .clear = 0};
   }
   g_nocol = 1;
   int ok = scene_create(&s, 1);
   float one[1][4] = {{1, 0, 0, 0}};
   if (ok) ok = scene_draw(&s, 1, one, f);
   g_nocol = 0; g_nofs = 0;
   g_st = (struct st_state){0};
   g_depfmt = save;
   printf("{\"test\":\"%s\",\"ok\":%s}\n", name, ok ? "true" : "false");   /* completion (no hang, no error) is the check; dEQP depth/stencil .nocolor reads the result back */
   CHECK(ok, "%s: depth-only pass failed", name);
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

static int
test_int_clear(const char *name, VkFormat fmt, unsigned bpp, uint32_t want0)
{
   return test_int_clear_f(name, fmt, bpp, want0, NULL);
}

static int
test_two(int depth)
{
   struct scene s = {.mode = MODE_TRI};   /* scene_create() takes s->mode as an input */
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   if (!scene_create(&s, depth)) return 0;
   int ok;
   if (!depth) {
      /* two touching, non-overlapping triangles (offsets -0.5 / +0.5 in NDC) */
      float rot[2][4] = {{1, 0, -.5f, 0}, {1, 0, .5f, 0}};
      if (!scene_draw(&s, 2, rot, f)) return 0;
      ok = report("two", &s, 16384.0, 1.5, 0);
   } else {
      /* same z, LESS test: the second (180 degree flipped, overlapping) draw is rejected where it
       * overlaps the first, but equal depth fails LESS everywhere it overlaps; it still shows
       * outside. Union area of triangle and its point reflection = 8192 + (8192 - overlap). */
      float rot[2][4] = {{1, 0, 0, 0}, {-1, 0, 0, 0}};
      if (!scene_draw(&s, 2, rot, f)) return 0;
      /* overlap of the triangle with its reflection through the centre of the bounding point (0,0):
       * computed numerically below on the host. */
      double ov = 0;
      {
         /* A=(-.5,-.5),(.5,-.5),(0,.5); B = -A. count px centres inside both */
         for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
               double px = (x + .5) / W * 2 - 1, py = (y + .5) / H * 2 - 1;
               int inA = py >= -.5 && py <= .5 && fabs(px) <= (py + .5) * .5;
               int inB = py >= -.5 && py <= .5 && fabs(px) <= (.5 - py) * .5;
               ov += inA && inB;
            }
      }
      /* with LESS and equal z, B appears only outside A; without depth it overwrites: same union.
       * Distinguish by colour: B's vertex colours differ from A at shared pixels, so compare the
       * centre pixel against what A alone would give (A wins when depth rejects B). */
      ok = report(g_depfmt == VK_FORMAT_D16_UNORM ? "depth16" : g_depfmt == VK_FORMAT_D32_SFLOAT_S8_UINT ? "depth32s8" : "depth", &s, 16384.0 - ov, 2.5, 0);
      if (ok && !g_fake) {
         const uint8_t *p = s.px + 128 * s.pitch + 128 * 4; /* inside both */
         /* B centre colour is mix with bottom-left... just require non-zero and report it */
         printf("{\"test\":\"depth_centre\",\"rgba\":[%u,%u,%u,%u],\"overlap_px\":%.0f}\n", p[0], p[1], p[2], p[3], ov);
      }
   }
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

/* mode cases: expected coverage in px, tolerance %, optional centre/corner probes */
/* MSAA (MALI_EXPERIMENTAL_MSAA=1): the triangle is rendered into a 4-sample image and resolved (hardware Average writeback) into col.
 * Pixel checks on the resolved image: the interior keeps the colour, edge pixels have coverage 1/4..3/4 (alpha ~64/128/191), and
 * the coverage-weighted area is the triangle area. mode 2 additionally enables sampleRateShading (minSampleShading 1.0). */
static int
test_msaa(const char *name, int mode)
{
   const char *gate = getenv("MALI_EXPERIMENTAL_MSAA");   /* 4x pipelines are gated; sample counts are advertised regardless (as on main) */
   if (!(gate && gate[0] == '1') || !(g_fbcounts & VK_SAMPLE_COUNT_4_BIT) || (mode == 2 && !g_feat.sampleRateShading)) {
      printf("{\"test\":\"%s\",\"skipped\":\"4x MSAA%s not advertised (MALI_EXPERIMENTAL_MSAA=1)\",\"ok\":true}\n", name,
             mode == 2 ? " / sampleRateShading" : "");
      return 1;
   }
   g_msaa = mode;
   struct scene s = {.mode = MODE_TRI};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   int made = scene_create(&s, 0);
   g_msaa = 0;
   if (!made) return 0;
   float rot[1][4] = {{ID[0], ID[1], ID[2], ID[3]}};
   if (!scene_draw(&s, 1, rot, f)) return 0;
   int ok = 1;
   if (!g_fake) {
      double wsum = 0;
      unsigned partial = 0, quarter = 0;
      for (int y = 0; y < H; y++)
         for (int x = 0; x < W; x++) {
            unsigned a = s.px[y * s.pitch + x * 4 + 3];
            wsum += a / 255.0;
            if (a > 0 && a < 255) {
               partial++;
               int k = (int)(a * 4 / 255.0 + 0.5);
               if (abs((int)a - k * 255 / 4) <= 10) quarter++;
            }
         }
      double err = fabs(wsum - 8192.0) / 8192.0 * 100.0;
      CHECK(err <= 1.5, "%s coverage-weighted area %.0f expected 8192 (%.2f%%)", name, wsum, err);
      const uint8_t *p = s.px + 106 * s.pitch + 128 * 4;   /* centroid: ~equal thirds */
      CHECK(abs(p[0] - 85) < 12 && abs(p[1] - 85) < 12 && abs(p[2] - 85) < 12 && p[3] == 255, "%s interior %u,%u,%u,%u", name, p[0], p[1], p[2], p[3]);
      CHECK(partial > 100, "%s only %u edge pixels with partial alpha (resolve did not average samples)", name, partial);
      CHECK(quarter * 10 >= partial * 9, "%s edge alpha not in quarter steps: %u/%u", name, quarter, partial);
      const uint8_t *o = s.px + 8 * s.pitch + 8 * 4;   /* outside: cleared transparent */
      CHECK(o[3] == 0, "%s outside pixel alpha %u", name, o[3]);
      printf("{\"test\":\"%s\",\"area\":%.0f,\"edge_px\":%u,\"quarter_px\":%u,\"ok\":true}\n", name, wsum, partial, quarter);
   } else {
      printf("{\"test\":\"%s\",\"skipped\":\"fake backend\",\"ok\":true}\n", name);
   }
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

static int
test_mode(const char *name, int mode, double expect, double tol)
{
   struct scene s = {.mode = MODE_TRI};   /* scene_create() takes s->mode as an input */
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   if (!scene_create(&s, g_clearzs)) return 0;
   s.mode = mode;
   float rot[1][4] = {{1, 0, 0, 0}};
   int ok = 1;
   for (int i = 0; ok && i < g_loop; i++) {
      if (!scene_draw(&s, 1, rot, f)) return 0;
      if (!g_fake) {
         if (mode == MODE_CLEARATT) {
            /* blue background where the triangle is not: alpha 255 everywhere */
            uint32_t a = 0;
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) a += s.px[y * s.pitch + x * 4 + 3] == 255;
            CHECK(a == W * H, "clearattachments: %u opaque px", a);
            const uint8_t *c = s.px + 8 * s.pitch + 8 * 4;
            CHECK(c[2] == 255 && c[0] == 0 && c[1] == 0, "corner %u,%u,%u", c[0], c[1], c[2]);
            const uint8_t *t = s.px + 128 * s.pitch + 128 * 4;   /* triangle interior wins over the clear */
            CHECK(t[2] < 250 || t[0] > 5 || t[1] > 5, "triangle not drawn over clear");
         } else if (mode == MODE_CLEARQUAD) {
            /* the rect [100,160)^2 is exactly the clear colour (the triangle's pixels inside it are overwritten); nothing outside it is */
            uint32_t in = 0, out_blue = 0, out_cov = 0;
            for (int y = 0; y < H; y++)
               for (int x = 0; x < W; x++) {
                  const uint8_t *c = s.px + y * s.pitch + x * 4;
                  int blue = c[0] == 0 && c[1] == 0 && c[2] == 255 && c[3] == 255;
                  if (x >= 100 && x < 160 && y >= 100 && y < 160) in += blue;
                  else out_blue += blue, out_cov += c[3] != 0;
               }
            CHECK(in == 60 * 60, "clear quad: %u of 3600 px in the rect are the clear colour", in);
            CHECK(out_blue == 0, "clear quad leaked %u px outside the scissor", out_blue);
            CHECK(out_cov > 0, "triangle outside the rect vanished");
         } else {
            uint32_t cov = count_cov(&s);
            double err = fabs((double)cov - expect) / expect * 100.0;
            CHECK(err <= tol, "%s iteration %d coverage %u expect %.0f (%.2f%%)", name, i, cov, expect, err);
            if (mode == MODE_SCISSOR || mode == MODE_VPORT || mode == MODE_MVP) {   /* nothing right of the half line */
               uint32_t right = 0;
               for (int y = 0; y < H; y++) for (int x = W / 2; x < W; x++) right += s.px[y * s.pitch + x * 4 + 3] != 0;
               CHECK(right == 0, "%u px right of the half line", right);
            }
         }
      }
   }
   if (!g_fake)
      printf("{\"test\":\"%s\",\"n\":%d,\"expect\":%.0f,\"ok\":true}\n", name, g_loop, expect);
   else
      printf("{\"test\":\"%s\",\"skipped\":\"fake backend\",\"ok\":true}\n", name);
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}


/* fillModeNonSolid: polygonMode LINE / POINT. Filled coverage of the quad modes is 16384 (idx) / 8192 (triangle); LINE must leave the
 * interior empty and cover only the edges (lo..hi pixels, triangle perimeter ~ 440 + diagonal for the quad), POINT only the vertices. */
static int
test_poly(const char *name, int mode, int poly, uint32_t lo, uint32_t hi, int cx, int cy)
{
   struct scene s = {.mode = mode};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   if (g_fake) { printf("{\"test\":\"%s\",\"skipped\":\"fake backend\",\"ok\":true}\n", name); return 1; }
   if (!g_feat.fillModeNonSolid) { printf("{\"test\":\"%s\",\"skipped\":\"fillModeNonSolid not advertised\",\"ok\":true}\n", name); return 1; }
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   memset(&g_fx, 0, sizeof g_fx);
   g_fx.poly = poly;
   if (!scene_create(&s, 0)) return 0;
   s.mode = mode;
   float rot[1][4] = {{1, 0, 0, 0}};
   int ok = scene_draw(&s, 1, rot, f);
   uint32_t cov = ok ? count_cov(&s) : 0;
   const uint8_t *c = s.px + cy * s.pitch + cx * 4;
   int good = ok && cov >= lo && cov <= hi && c[3] == 0;
   if (!good) { printf("{\"fail\":\"%s: coverage %u expected %u..%u, interior alpha %u (want 0)\"}\n", name, cov, lo, hi, c[3]); g_fail++; }
   else printf("{\"test\":\"%s\",\"cov\":%u,\"ok\":true}\n", name, cov);
   memset(&g_fx, 0, sizeof g_fx);
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return good;
}

/* VK_EXT_extended_dynamic_state3 polygonMode: same scenes as test_poly, but the pipeline is created FILL with POLYGON_MODE_EXT dynamic and the mode
 * comes from vkCmdSetPolygonModeEXT. On the fake backend nothing renders: the draw must still build and its decoded stream (golden) differs from FILL. */
static int
test_poly_dyn(const char *name, int mode, int poly, uint32_t lo, uint32_t hi, int cx, int cy)
{
   struct scene s = {.mode = mode};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   if (!g_have_eds3) { printf("{\"test\":\"%s\",\"skipped\":\"extendedDynamicState3PolygonMode not advertised\",\"ok\":true}\n", name); return 1; }
   if (!g_feat.fillModeNonSolid) { printf("{\"test\":\"%s\",\"skipped\":\"fillModeNonSolid not advertised\",\"ok\":true}\n", name); return 1; }
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   memset(&g_fx, 0, sizeof g_fx);
   g_fx.poly = poly;
   g_fx.dynpoly = 1;
   if (!scene_create(&s, 0)) return 0;
   s.mode = mode;
   float rot[1][4] = {{1, 0, 0, 0}};
   int ok = scene_draw(&s, 1, rot, f);
   int good = ok;
   if (ok && !g_fake) {
      uint32_t cov = count_cov(&s);
      const uint8_t *c = s.px + cy * s.pitch + cx * 4;
      good = cov >= lo && cov <= hi && c[3] == 0;
      if (!good) { printf("{\"fail\":\"%s: coverage %u expected %u..%u, interior alpha %u (want 0)\"}\n", name, cov, lo, hi, c[3]); g_fail++; }
      else printf("{\"test\":\"%s\",\"cov\":%u,\"ok\":true}\n", name, cov);
   } else if (ok)
      printf("{\"test\":\"%s\",\"ok\":true,\"skipped\":\"pixels: fake backend\"}\n", name);
   memset(&g_fx, 0, sizeof g_fx);
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return good;
}

/* R4: fixed-function state. Triangle (-.5,-.5) (.5,-.5) (0,.5) in Vulkan y-down NDC is clockwise on screen. */
enum { EXP_COV, EXP_SAME, EXP_DOUBLE, EXP_HALF, EXP_ZERO, EXP_ADDSQ, EXP_RONLY, EXP_X15 };
struct fxcase {
   const char *name;
   int depth, n;                  /* depth attachment, draws */
   float z[2];
   int exp;
   double cov;                    /* EXP_COV: expected pixels */
   int need;                      /* 1 = depthClamp, 2 = depthBiasClamp feature needed */
   void (*set)(void);
};

static void s_none(void) {}
static void s_back_cw(void) { g_fx.cull = VK_CULL_MODE_BACK_BIT; g_fx.cw = 1; }
static void s_back_ccw(void) { g_fx.cull = VK_CULL_MODE_BACK_BIT; }
static void s_front_cw(void) { g_fx.cull = VK_CULL_MODE_FRONT_BIT; g_fx.cw = 1; }
static void s_front_ccw(void) { g_fx.cull = VK_CULL_MODE_FRONT_BIT; }
static void s_dyn_back_ccw(void) { s_back_ccw(); g_fx.dyn = 1; }
static void s_dyn_front_ccw(void) { s_front_ccw(); g_fx.dyn = 1; }
static void blend(VkBlendFactor sf, VkBlendFactor df, VkBlendOp op) { g_fx.blend = 1; g_fx.sf = sf; g_fx.df = df; g_fx.op = op; }
static void s_add11(void) { blend(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD); }
static void s_rsub11(void) { blend(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_REVERSE_SUBTRACT); }
static void s_const_half(void) { blend(VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD); g_fx.bc[0] = g_fx.bc[1] = g_fx.bc[2] = g_fx.bc[3] = 0.5f; }
static void s_dyn_const_half(void) { s_const_half(); g_fx.dyn = 1; }
static void s_one_dst(void) { blend(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_OP_ADD); }
static void s_one_src(void) { blend(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_OP_ADD); }
static void s_dst_src(void) { blend(VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_OP_ADD); }
/* dualSrcBlend: dual.frag writes o0 = varying colour, o1 = (0.5, 0.5, 0.5, 0.5) */
static void s_d_src1c(void) { g_fx.dual = 1; blend(VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD); }
static void s_d_inv_src1a(void) { g_fx.dual = 1; blend(VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD); }
static void s_d_one_src1c(void) { g_fx.dual = 1; blend(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_OP_ADD); }
static void s_mask_r(void) { g_fx.wmask = 1; }
static void s_src_alpha(void) { blend(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD); }
static void s_bias_off(void) { g_fx.bias = 1; }   /* bias enabled, factors 0: z = 1.0 fails LESS against the 1.0 clear */
static void s_bias_on(void) { g_fx.bias = 1; g_fx.bias_c = -1e5f; }
static void s_dyn_bias(void) { s_bias_on(); g_fx.dyn = 1; }
static void s_clamp(void) { g_fx.clamp = 1; }
static void s_clamp_noclip(void) { g_fx.clamp = 1; g_fx.dclip = 1; }   /* clamp + depthClipEnable FALSE: same as plain clamp */
static void s_clamp_clip(void) { g_fx.clamp = 1; g_fx.dclip = 2; }     /* clamp + depthClipEnable TRUE: z 1.5 is clipped */
static void s_bias_pos(void) { g_fx.bias = 1; g_fx.bias_c = 1e7f; }   /* z 0.5 + huge bias: fails LESS against the 1.0 clear */
static void s_bias_pos_clamp(void) { s_bias_pos(); g_fx.bias_clamp = 0.25f; }   /* clamped to +0.25: z 0.75 passes */
static void s_bias_neg_clamp(void) { g_fx.bias = 1; g_fx.bias_c = -1e7f; g_fx.bias_clamp = -0.25f; }   /* z 1.0 - 0.25 passes */
static void s_dyn_bias_pos_clamp(void) { s_bias_pos_clamp(); g_fx.dyn = 1; }

static const struct fxcase g_fxcases[] = {
   {"fx_cull_none", 0, 1, {0.5f}, EXP_COV, 8192, 0, s_none},
   {"fx_cull_back_cw", 0, 1, {0.5f}, EXP_COV, 8192, 0, s_back_cw},
   {"fx_cull_back_ccw", 0, 1, {0.5f}, EXP_COV, 0, 0, s_back_ccw},
   {"fx_cull_front_cw", 0, 1, {0.5f}, EXP_COV, 0, 0, s_front_cw},
   {"fx_cull_front_ccw", 0, 1, {0.5f}, EXP_COV, 8192, 0, s_front_ccw},
   {"fx_dyn_cull_back_ccw", 0, 1, {0.5f}, EXP_COV, 0, 0, s_dyn_back_ccw},
   {"fx_dyn_cull_front_ccw", 0, 1, {0.5f}, EXP_COV, 8192, 0, s_dyn_front_ccw},
   {"fx_blend_add11", 0, 2, {0.5f, 0.5f}, EXP_DOUBLE, 0, 0, s_add11},
   {"fx_blend_rsub11", 0, 2, {0.5f, 0.5f}, EXP_ZERO, 0, 0, s_rsub11},
   {"fx_blend_const", 0, 1, {0.5f}, EXP_HALF, 0, 0, s_const_half},
   {"fx_dyn_blend_const", 0, 1, {0.5f}, EXP_HALF, 0, 0, s_dyn_const_half},
   {"fx_blend_one_dstcolor", 0, 2, {0.5f, 0.5f}, EXP_ADDSQ, 0, 0, s_one_dst},
   {"fx_blend_one_srccolor", 0, 2, {0.5f, 0.5f}, EXP_ADDSQ, 0, 0, s_one_src},
   {"fx_blend_2srcdst", 0, 2, {0.5f, 0.5f}, EXP_ZERO, 0, 0, s_dst_src},
   {"fx_blend_srcalpha", 0, 2, {0.5f, 0.5f}, EXP_SAME, 0, 0, s_src_alpha},
   {"fx_dual_src1_color", 0, 1, {0.5f}, EXP_HALF, 0, 4, s_d_src1c},
   {"fx_dual_inv_src1_alpha", 0, 1, {0.5f}, EXP_HALF, 0, 4, s_d_inv_src1a},
   {"fx_dual_one_src1_color", 0, 2, {0.5f, 0.5f}, EXP_X15, 0, 4, s_d_one_src1c},
   {"fx_write_mask_r", 0, 1, {0.5f}, EXP_RONLY, 0, 0, s_mask_r},
   {"fx_bias_off", 1, 1, {1.0f}, EXP_COV, 0, 0, s_bias_off},
   {"fx_bias_on", 1, 1, {1.0f}, EXP_COV, 8192, 0, s_bias_on},
   {"fx_dyn_bias", 1, 1, {1.0f}, EXP_COV, 8192, 0, s_dyn_bias},
   {"fx_bias_pos_noclamp", 1, 1, {0.5f}, EXP_COV, 0, 0, s_bias_pos},
   {"fx_bias_pos_clamp", 1, 1, {0.5f}, EXP_COV, 8192, 2, s_bias_pos_clamp},
   {"fx_bias_neg_clamp", 1, 1, {1.0f}, EXP_COV, 8192, 2, s_bias_neg_clamp},
   {"fx_dyn_bias_pos_clamp", 1, 1, {0.5f}, EXP_COV, 8192, 2, s_dyn_bias_pos_clamp},
   {"fx_clip_off_range", 0, 1, {1.5f}, EXP_COV, 0, 0, s_none},
   {"fx_depth_clamp", 0, 1, {1.5f}, EXP_COV, 8192, 1, s_clamp},
   {"fx_dclip_clamp_noclip", 0, 1, {1.5f}, EXP_COV, 8192, 5, s_clamp_noclip},   /* needs VK_EXT_depth_clip_enable */
   {"fx_dclip_clamp_clip", 0, 1, {1.5f}, EXP_COV, 0, 5, s_clamp_clip},
};

static int
fx_run(const struct fxcase *c, uint8_t px[4], uint32_t *cov)
{
   struct scene s;
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   memset(&g_fx, 0, sizeof g_fx);
   g_fx.on = 1;
   g_fx.wmask = 0xf;
   c->set();
   memset(&s, 0, sizeof s);
   s.mode = MODE_TRI;
   if (!scene_create(&s, c->depth)) return 0;
   float rot[2][4] = {{1, 0, 0, c->z[0]}, {1, 0, 0, c->z[1]}};
   int ok = scene_draw(&s, c->n, rot, f);
   if (ok) {
      *cov = count_cov(&s);
      memcpy(px, s.px + 106 * s.pitch + 128 * 4, 4);
   }
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   memset(&g_fx, 0, sizeof g_fx);
   return ok;
}

static int
test_fx(void)
{
   int ok = 1;
   struct fxcase ref = {"ref", 0, 1, {0.5f}, EXP_SAME, 0, 0, s_none};
   uint8_t p[4] = {0}, q[4];
   uint32_t cov = 0;
   if (g_fake) {   /* the fake backend renders nothing, but every fx pipeline (shader + blend compile, per GPU class) must still build */
      for (unsigned i = 0; i < sizeof g_fxcases / sizeof *g_fxcases; i++) {
         const struct fxcase *c = &g_fxcases[i];
         struct scene s;
         if (g_only && strcmp(g_only, "fx") && strcmp(g_only, c->name)) continue;
         memset(&g_fx, 0, sizeof g_fx);
         g_fx.on = 1;
         g_fx.wmask = 0xf;
         c->set();
         memset(&s, 0, sizeof s);
         s.mode = MODE_TRI;
         printf("{\"test\":\"%s\",\"pipeline\":\"%s\"}\n", c->name, "creating");
         if (!scene_create(&s, c->depth)) return 0;
         scene_destroy(&s);
         memset(&g_fx, 0, sizeof g_fx);
      }
      printf("{\"test\":\"fx\",\"skipped\":\"fake backend: pipelines only\",\"ok\":true}\n");
      return 1;
   }
   if (!fx_run(&ref, p, &cov)) return 0;
   CHECK(abs(p[0] - 85) < 12 && p[3] == 255, "fx reference pixel %u,%u,%u,%u", p[0], p[1], p[2], p[3]);
   for (unsigned i = 0; ok && i < sizeof g_fxcases / sizeof *g_fxcases; i++) {
      const struct fxcase *c = &g_fxcases[i];
      if (g_only && strcmp(g_only, "fx") && strcmp(g_only, c->name)) continue;
      if ((c->need & 1) && !g_feat.depthClamp) { printf("{\"test\":\"%s\",\"skipped\":\"depthClamp not advertised\",\"ok\":true}\n", c->name); continue; }
      if ((c->need & 4) && !g_have_dclip) { printf("{\"test\":\"%s\",\"skipped\":\"VK_EXT_depth_clip_enable not advertised\",\"ok\":true}\n", c->name); continue; }
      if ((c->need & 2) && !g_feat.depthBiasClamp) { printf("{\"test\":\"%s\",\"skipped\":\"depthBiasClamp not advertised\",\"ok\":true}\n", c->name); continue; }
      if ((c->need & 4) && !g_feat.dualSrcBlend) { printf("{\"test\":\"%s\",\"skipped\":\"dualSrcBlend not advertised\",\"ok\":true}\n", c->name); continue; }
      if (!fx_run(c, q, &cov)) return 0;
      int good = 1;
      int e[4] = {p[0], p[1], p[2], p[3]};
      switch (c->exp) {
      case EXP_COV: good = fabs((double)cov - c->cov) <= c->cov * 0.02 + 8; break;
      case EXP_SAME: e[3] = 255; break;
      case EXP_DOUBLE: for (int k = 0; k < 3; k++) e[k] = e[k] * 2 > 255 ? 255 : e[k] * 2; e[3] = 255; break;
      case EXP_HALF: for (int k = 0; k < 3; k++) e[k] /= 2; e[3] = 127; break;
      case EXP_ZERO: e[0] = e[1] = e[2] = e[3] = 0; break;
      case EXP_ADDSQ: for (int k = 0; k < 3; k++) e[k] = e[k] + e[k] * e[k] / 255; e[3] = 255; break;
      case EXP_X15: for (int k = 0; k < 3; k++) e[k] = e[k] + e[k] / 2; e[3] = 255; break;
      case EXP_RONLY: e[1] = e[2] = e[3] = 0; break;
      }
      if (c->exp != EXP_COV)
         for (int k = 0; k < 4; k++) good &= abs(q[k] - e[k]) <= 4;
      if (!good) {
         printf("{\"fail\":\"%s: pixel %u,%u,%u,%u expected %d,%d,%d,%d (coverage %u expected %.0f)\"}\n", c->name, q[0], q[1], q[2], q[3], e[0], e[1], e[2], e[3], cov, c->cov);
         g_fail++;
         ok = 0;
      } else {
         printf("{\"test\":\"%s\",\"cov\":%u,\"px\":[%u,%u,%u,%u],\"ok\":true}\n", c->name, cov, q[0], q[1], q[2], q[3]);
      }
   }
   return ok;
}

/* ring of 3 frames: three command buffers in one vkQueueSubmit, then three separate submits with a fence ring */
/* M13 occlusion: query 0 brackets the triangle draw, query 1 brackets nothing, query 2 a second draw (shifted triangle);
 * samples-passed must equal the covered-pixel count (1 sample/pixel) and 0 for the empty query. */
static int
test_occ(void)
{
   struct scene s = {.mode = MODE_TRI};   /* scene_create() takes s->mode as an input */
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   if (!scene_create(&s, 0)) return 0;
   s.mode = MODE_TRI;
   VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = 3};
   VkQueryPool qp;
   CHECK(CreateQueryPool(dev, &qi, NULL, &qp) == VK_SUCCESS, "CreateQueryPool");
   for (int i = 0; i < g_loop; i++) {
      VkCommandBuffer cb;
      VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s.pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
      CHECK(AllocateCommandBuffers(dev, &ai, &cb) == VK_SUCCESS, "alloc");
      VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                     .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      CHECK(BeginCommandBuffer(cb, &bi) == VK_SUCCESS, "begin");
      CmdResetQueryPool(cb, qp, 0, 3);
      VkClearValue cv = {.color = {{0, 0, 0, 0}}};
      VkRenderPassBeginInfo rb = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = s.rp,
                                  .framebuffer = s.fb, .renderArea = {{0, 0}, {W, H}}, .clearValueCount = 1, .pClearValues = &cv};
      CmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
      CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipe);
      VkViewport vp = {0, 0, W, H, 0.0f, 1.0f};
      VkRect2D sc = {{0, 0}, {W, H}};
      CmdSetViewport(cb, 0, 1, &vp);
      CmdSetScissor(cb, 0, 1, &sc);
      VkBuffer vbs[2] = {s.vb[0], s.vb[1]};
      VkDeviceSize offs[2] = {0, 0};
      CmdBindVertexBuffers(cb, 0, 2, vbs, offs);
      float rot[4] = {1, 0, 0, 0};
      CmdPushConstants(cb, s.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, rot);
      CmdBeginQuery(cb, qp, 0, VK_QUERY_CONTROL_PRECISE_BIT);
      CmdDraw(cb, 3, 1, 0, 0);
      CmdEndQuery(cb, qp, 0);
      CmdBeginQuery(cb, qp, 1, VK_QUERY_CONTROL_PRECISE_BIT);
      CmdEndQuery(cb, qp, 1);
      CmdBeginQuery(cb, qp, 2, VK_QUERY_CONTROL_PRECISE_BIT);
      CmdDraw(cb, 3, 1, 0, 0);
      CmdEndQuery(cb, qp, 2);
      CmdEndRenderPass(cb);
      CHECK(EndCommandBuffer(cb) == VK_SUCCESS, "end");
      VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
      CHECK(QueueSubmit(queue, 1, &si, f) == VK_SUCCESS, "submit");
      CHECK(WaitForFences(dev, 1, &f, VK_TRUE, 20000000000ull) == VK_SUCCESS, "wait");
      ResetFences(dev, 1, &f);
      FreeCommandBuffers(dev, s.pool, 1, &cb);
      if (g_fake) continue;
      uint64_t v[3] = {~0ull, ~0ull, ~0ull};
      VkResult r = GetQueryPoolResults(dev, qp, 0, 3, sizeof v, v, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
      CHECK(r == VK_SUCCESS, "GetQueryPoolResults = %d", r);
      uint32_t cov = count_cov(&s);
      CHECK(cov > 1000, "coverage %u", cov);
      CHECK(v[1] == 0, "empty query = %llu", (unsigned long long)v[1]);
      CHECK(v[0] == cov, "iteration %d query 0 = %llu, coverage %u", i, (unsigned long long)v[0], cov);
      CHECK(v[2] == cov, "iteration %d query 2 = %llu, coverage %u", i, (unsigned long long)v[2], cov);
   }
   printf("{\"test\":\"occlusion\",\"n\":%d,\"ok\":true%s}\n", g_loop, g_fake ? ",\"skipped\":\"results: fake backend\"" : "");
   DestroyQueryPool(dev, qp, NULL);
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return 1;
}

static int
test_ring(void)
{
   struct scene s = {.mode = MODE_TRI};   /* scene_create() takes s->mode as an input */
   VkFence f[3];
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   for (int i = 0; i < 3; i++) CHECK(CreateFence(dev, &fi, NULL, &f[i]) == VK_SUCCESS, "fence");
   if (!scene_create(&s, 0)) return 0;
   s.mode = MODE_TRI;
   int ok = 1;
   int saved = g_reset;
   g_reset = 0;
   for (int i = 0; ok && i < g_loop; i++) {
      /* record three frames, submit each with its ring fence, wait the oldest as the ring wraps */
      VkCommandBuffer cb[3];
      VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s.pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 3};
      CHECK(AllocateCommandBuffers(dev, &ai, cb) == VK_SUCCESS, "alloc ring");
      for (int k = 0; k < 3; k++) {
         VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
         CHECK(BeginCommandBuffer(cb[k], &bi) == VK_SUCCESS, "begin");
         VkClearValue cv = {.color = {{0, 0, 0, 0}}};
         VkRenderPassBeginInfo rb = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = s.rp,
                                     .framebuffer = s.fb, .renderArea = {{0, 0}, {W, H}}, .clearValueCount = 1, .pClearValues = &cv};
         CmdBeginRenderPass(cb[k], &rb, VK_SUBPASS_CONTENTS_INLINE);
         CmdBindPipeline(cb[k], VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipe);
         VkViewport vp = {0, 0, W, H, 0, 1};
         VkRect2D sc = {{0, 0}, {W, H}};
         CmdSetViewport(cb[k], 0, 1, &vp);
         CmdSetScissor(cb[k], 0, 1, &sc);
         VkBuffer vbs[2] = {s.vb[0], s.vb[1]};
         VkDeviceSize offs[2] = {0, 0};
         CmdBindVertexBuffers(cb[k], 0, 2, vbs, offs);
         float rot[4] = {1, 0, (k - 1) * 0.25f, 0};
         CmdPushConstants(cb[k], s.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, rot);
         CmdDraw(cb[k], 3, 1, 0, 0);
         CmdEndRenderPass(cb[k]);
         CHECK(EndCommandBuffer(cb[k]) == VK_SUCCESS, "end");
      }
      for (int k = 0; k < 3; k++) {
         VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb[k]};
         CHECK(QueueSubmit(queue, 1, &si, f[k]) == VK_SUCCESS, "submit ring %d", k);
      }
      for (int k = 0; k < 3; k++) {
         CHECK(WaitForFences(dev, 1, &f[k], VK_TRUE, 20000000000ull) == VK_SUCCESS, "wait ring %d", k);
         ResetFences(dev, 1, &f[k]);
      }
      FreeCommandBuffers(dev, s.pool, 3, cb);
      if (!g_fake) {   /* last frame wins: triangle shifted by +0.25 NDC (clear before it) */
         uint32_t cov = count_cov(&s);
         double err = fabs((double)cov - 8192.0) / 8192.0 * 100.0;
         CHECK(err <= 1.0, "ring iteration %d coverage %u (%.2f%%)", i, cov, err);
      }
   }
   g_reset = saved;
   printf("{\"test\":\"ring3\",\"n\":%d,\"ok\":true%s}\n", g_loop, g_fake ? ",\"skipped\":\"pixels: fake backend\"" : "");
   scene_destroy(&s);
   for (int i = 0; i < 3; i++) DestroyFence(dev, f[i], NULL);
   return ok;
}

static int
near3(const uint8_t *p, int r, int g, int b, int tol)
{
   return abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol && p[3] == 255;
}

/* consecutive draws that differ only in the dynamic-UBO offset (and, later, in firstInstance): each must get its own dynamic table, and the
 * reused static driver set must not leak one draw's data into the next. Pixels on hardware only. Four quarter triangles, last draw wins. */
static int
test_share_dyn(void)
{
   g_dyn = 1;
   g_share = 3;
   struct scene s = {.mode = MODE_UBO};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   int ok = scene_create(&s, 0);
   const float q[4][4] = {{.5f, 0, -.5f, -.5f}, {.5f, 0, .5f, -.5f}, {.5f, 0, -.5f, .5f}, {.5f, 0, .5f, .5f}};
   const int pos[8] = {0, 0, 1, 1, 2, 2, 3, 3};
   const uint32_t off[8] = {0, 1, 1, 2, 2, 2, 0, 1};   /* colour blocks A, B, C */
   const uint32_t fi_[8] = {0, 0, 0, 0, 0, 0, 1, 1};
   float rot[8][4];
   for (int i = 0; i < 8; i++) {
      memcpy(rot[i], q[pos[i]], sizeof rot[i]);
      g_share_off[i] = off[i];
      g_share_fi[i] = fi_[i];
   }
   if (ok) ok = scene_draw(&s, 8, (const float (*)[4])rot, f);
   if (ok && !g_fake) {
      static const int want[4][3] = {{255, 0, 128}, {0, 255, 0}, {0, 255, 0}, {255, 0, 128}};   /* last colour drawn at each position */
      for (int k = 0; k < 4; k++) {
         int cx = (int)((q[k][2] * .5f + .5f) * W), cy = (int)((q[k][3] * .5f + .5f) * H);
         const uint8_t *c = s.px + cy * s.pitch + cx * g_colbpp;
         CHECK(near3(c, want[k][0], want[k][1], want[k][2], 2), "share_dyn: triangle %d colour %u,%u,%u,%u", k, c[0], c[1], c[2], c[3]);
      }
      printf("{\"test\":\"share_dyn\",\"ok\":true}\n");
   } else if (ok) printf("{\"test\":\"share_dyn\",\"skipped\":\"fake backend\",\"ok\":true}\n");
   g_share = 0;
   g_dyn = 0;
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return ok;
}

/* firstInstance is baked into the per-instance attribute descriptors (build_draw_descs): a draw that differs only in firstInstance, or only
 * in the vertex-buffer offset, must not reuse the previous draw's driver set. Host: the frame with changes allocates more than the frame with
 * four identical draws (the fake backend counts the bytes); hardware: all four corner triangles are drawn. */
static int
test_share_inst(void)
{
   alloc_stats_start();
   float dummy[4][4] = {{0}};
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   unsigned long long ab[3] = {0};
   int ok = 1;
   struct scene s;
   for (int pass = 0; ok && pass < 3; pass++) {
      g_inst = pass == 1 ? 1 : 2;   /* 2, 1, 2: the second identical frame removes the first-submit warm-up */
      s = (struct scene){.mode = MODE_TRI};
      ok = scene_create(&s, 0) && scene_draw(&s, 4, dummy, f);
      ab[pass] = alloc_bytes();
      if (ok && pass == 1 && !g_fake) {
         for (int k = 0; k < 4; k++) {
            int cx = (int)(((k & 1 ? .5f : -.5f) * .5f + .5f) * W) - 6, cy = (int)(((k & 2 ? .5f : -.5f) * .5f + .5f) * H) - 6;   /* inside the triangle: offset + (-0.1,-0.1) */
            CHECK(px_at(&s, cx, cy) != 0, "share_inst: corner %d triangle missing at %d,%d", k, cx, cy);
         }
         ok &= report("share_inst", &s, 4 * 2048.0, 3.0, 0);
      }
      scene_destroy(&s);
   }
   g_inst = 0;
   DestroyFence(dev, f, NULL);
   if (!ok) return 0;
   if (!g_fake) {   /* the byte counter is the fake-backend host assertion (the phone library has no /tmp stats file) */
      printf("{\"test\":\"share_inst_pixels\",\"ok\":true}\n");
      return 1;
   }
   unsigned long long same = ab[2] - ab[1], chg = ab[1] - ab[0];
   /* same-state frame 0 -> 2 differ by nothing; the changing frame must allocate at least 3 driver sets (3 x 17 x 32 B) more than an identical one */
   printf("{\"test\":\"share_inst\",\"alloc_identical\":%llu,\"alloc_changed\":%llu,\"ok\":%s}\n", same, chg, chg >= same + 3 * 17 * 32 ? "true" : "false");
   CHECK(chg >= same + 3 * 17 * 32, "share_inst: draws that differ in firstInstance / vertex buffer reused the driver set (%llu vs %llu bytes)", chg, same);
   return 1;
}

/* share_dirty (fake backend byte counter): each state group's dirty bit must make the draw rebuild what depends on it, and rebinding an equal
 * pipeline must not. Frames of 4 draws: push constants, viewport, cull mode change between draws (more bytes than the identical frame) and the
 * same pipeline rebound (no more bytes). MALI_DIRTY_CHECK (check.sh) additionally aborts when a mask reuses what really changed. */
static int
test_share_dirty(void)
{
   if (!g_fake) {
      printf("{\"test\":\"share_dirty\",\"skipped\":\"fake backend byte counter\",\"ok\":true}\n");
      return 1;
   }
   alloc_stats_start();
   float same_rot[4][4] = {{0}}, var_rot[4][4] = {{0}};
   for (int i = 0; i < 4; i++) var_rot[i][0] = 0.1f * i;
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   static const char *names[5] = {"push", "viewport", "cullmode", "rebind_pipeline", "scissor"};
   int ok = 1;
   for (int k = 0; ok && k < 5; k++) {
      unsigned long long ab[3];
      for (int pass = 0; ok && pass < 3; pass++) {
         g_dirty = pass == 2 ? (k == 0 ? 0 : k) : (k == 0 ? 0 : 0);
         if (k == 3) g_dirty = pass == 2 ? 3 : 0;
         else if (k == 4) g_dirty = pass == 2 ? 4 : 0;
         else if (k == 1) g_dirty = pass == 2 ? 1 : 0;
         else if (k == 2) g_dirty = pass == 2 ? 2 : 0;
         struct scene s = {.mode = MODE_TRI};
         ok = scene_create(&s, 0) && scene_draw(&s, 4, k == 0 && pass == 2 ? var_rot : same_rot, f);
         ab[pass] = alloc_bytes();
         scene_destroy(&s);
      }
      g_dirty = 0;
      if (!ok) break;
      unsigned long long same = ab[1] - ab[0], chg = ab[2] - ab[1];
      int want_more = k < 3;   /* rebinding an equal pipeline and the scissor (a register) must not rebuild any table */
      int good = want_more ? chg >= same + 3 * 64 : chg <= same;
      printf("{\"test\":\"share_dirty\",\"bit\":\"%s\",\"alloc_identical\":%llu,\"alloc_changed\":%llu,\"ok\":%s}\n", names[k], same, chg, good ? "true" : "false");
      CHECK(good, "share_dirty %s: %llu bytes for the changing frame vs %llu for the identical one", names[k], chg, same);
   }
   DestroyFence(dev, f, NULL);
   return ok;
}

/* full-screen quad cases: push-constant-only FS colour, sampled texture, uniform buffer, descriptor updates between draws */
static int
test_quad(const char *name, int mode)
{
   struct scene s = {.mode = MODE_TRI};   /* scene_create() takes s->mode as an input */
   VkFence f;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   s.mode = mode;
   if (!scene_create(&s, 0)) return 0;
   float one[1][4] = {{1, 0, 0, 0}};
   float two[2][4] = {{.5f, 0, -.5f, 0}, {.5f, 0, .5f, 0}};
   for (int i = 0; i < g_loop; i++) {
      if (mode == MODE_DESCUPD) {   /* rewrite set 1 -> buffer B or C between submissions, then two draws with two sets */
         VkDescriptorBufferInfo bi = {s.ubo[1 + (i & 1)], 0, 16};
         VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s.dset[1], .dstBinding = 0,
                                   .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi};
         UpdateDescriptorSets(dev, 1, &w, 0, NULL);
      }
      if (!scene_draw(&s, mode == MODE_DESCUPD ? 2 : 1, mode == MODE_DESCUPD ? two : one, f)) return 0;
      if (g_fake) continue;
      uint32_t cov = count_cov(&s);
      double expect = mode == MODE_DESCUPD ? 32768.0 : 65536.0;
      CHECK(cov == (uint32_t)expect, "%s iteration %d coverage %u expect %.0f", name, i, cov, expect);
      const uint8_t *c = s.px + 128 * s.pitch + 128 * 4;
      if (mode == MODE_PUSHFS) CHECK(near3(c, 191, 64, 128, 2), "pc colour %u,%u,%u,%u", c[0], c[1], c[2], c[3]);
      if (mode == MODE_UBO) CHECK(near3(c, 64, 128, 191, 2), "ubo colour %u,%u,%u,%u", c[0], c[1], c[2], c[3]);
      if (mode == MODE_DESCUPD) {
         const uint8_t *l = s.px + 128 * s.pitch + 64 * 4, *r = s.px + 128 * s.pitch + 192 * 4;
         CHECK(near3(l, 64, 128, 191, 2), "left (set 0) %u,%u,%u,%u", l[0], l[1], l[2], l[3]);
         if (i & 1) CHECK(near3(r, 0, 255, 0, 2), "right (set 1 = C) %u,%u,%u,%u", r[0], r[1], r[2], r[3]);
         else CHECK(near3(r, 255, 0, 128, 2), "right (set 1 = B) %u,%u,%u,%u", r[0], r[1], r[2], r[3]);
      }
      if (mode == MODE_TEX) {
         unsigned bad = 0, checked = 0;
         for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
               if ((x & 31) == 0 || (x & 31) == 31 || (y & 31) == 0 || (y & 31) == 31) continue;   /* texel borders: rounding */
               const uint8_t *p = s.px + y * s.pitch + x * 4;
               checked++;
               bad += !near3(p, (x / 32) * 32, (y / 32) * 32, 128, 1);
            }
         CHECK(bad == 0, "texture: %u of %u px differ from the texel colour (first texel px %u,%u,%u,%u)", bad, checked,
               s.px[16 * 4], s.px[16 * 4 + 1], s.px[16 * 4 + 2], s.px[16 * 4 + 3]);
      }
   }
   printf("{\"test\":\"%s\",\"n\":%d,\"ok\":true%s}\n", name, g_loop, g_fake ? ",\"skipped\":\"pixels: fake backend\"" : "");
   scene_destroy(&s);
   DestroyFence(dev, f, NULL);
   return 1;
}

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--loop") && i + 1 < argc) g_loop = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--loop-reset")) g_reset = 1;
      else if (!strcmp(argv[i], "--only") && i + 1 < argc) g_only = argv[++i];
      else if (!strcmp(argv[i], "--spv-dir") && i + 1 < argc) g_spv = argv[++i];
      else { fprintf(stderr, "usage: t_vk_draw --direct icd.so [--loop N] [--loop-reset] [--only case]\n"); return 2; }
   }
   if (!icd) { fprintf(stderr, "--direct required\n"); return 2; }
   const char *be = getenv("MALI_BACKEND");
   g_fake = be && !strcmp(be, "fake");

   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("{\"fail\":\"dlopen: %s\"}\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   VkResult (*negotiate)(uint32_t *) = (VkResult (*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   uint32_t iv = 5;
   if (!gipa || !negotiate || negotiate(&iv)) { printf("{\"fail\":\"icd entry\"}\n"); return 1; }

   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   const char *iext[] = {"VK_KHR_get_physical_device_properties2", "VK_KHR_external_memory_capabilities",
                         "VK_KHR_external_semaphore_capabilities", "VK_KHR_external_fence_capabilities"};
   ici.enabledExtensionCount = 4;
   ici.ppEnabledExtensionNames = iext;
   VkInstance inst;
   if (CreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("{\"fail\":\"CreateInstance\"}\n"); return 1; }
   PFN_vkEnumeratePhysicalDevices EPD = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceMemoryProperties GMP = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
   PFN_vkGetPhysicalDeviceProperties GPP = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
   PFN_vkGetPhysicalDeviceFeatures GPF = (PFN_vkGetPhysicalDeviceFeatures)gipa(inst, "vkGetPhysicalDeviceFeatures");
   PFN_vkCreateDevice CreateDevice = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
   PFN_vkGetDeviceProcAddr GDPA = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
   PFN_vkDestroyInstance DestroyInstance = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
   VkPhysicalDevice pd;
   uint32_t cnt = 1;
   VkResult r = EPD(inst, &cnt, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || cnt < 1) { printf("{\"fail\":\"no device\"}\n"); return 1; }
   VkPhysicalDeviceProperties pp;
   GPP(pd, &pp);
   GMP(pd, &memprops);
   g_fbcounts = pp.limits.framebufferColorSampleCounts;
   printf("{\"device\":\"%s\"}\n", pp.deviceName);

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
   GPF(pd, &g_feat);
   VkPhysicalDeviceFeatures en = {.depthClamp = g_feat.depthClamp, .independentBlend = g_feat.independentBlend, .fillModeNonSolid = g_feat.fillModeNonSolid, .depthBiasClamp = g_feat.depthBiasClamp, .multiViewport = g_feat.multiViewport, .dualSrcBlend = g_feat.dualSrcBlend, .sampleRateShading = g_feat.sampleRateShading};
   di.pEnabledFeatures = &en;
   /* VK_EXT_depth_clip_enable: enabled when advertised (feature struct chained without a Features2) */
   VkPhysicalDeviceDepthClipEnableFeaturesEXT dce_f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
   const char *dce_ext[] = {"VK_EXT_depth_clip_enable", "VK_EXT_extended_dynamic_state3"};
   const char *en_ext[2];
   VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3_f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
   {
      PFN_vkGetPhysicalDeviceFeatures2 GPF2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(inst, "vkGetPhysicalDeviceFeatures2");
      PFN_vkEnumerateDeviceExtensionProperties EDE = (PFN_vkEnumerateDeviceExtensionProperties)gipa(inst, "vkEnumerateDeviceExtensionProperties");
      uint32_t ne = 0;
      EDE(pd, NULL, &ne, NULL);
      VkExtensionProperties eps[128];
      if (ne > 128) ne = 128;
      EDE(pd, NULL, &ne, eps);
      int has = 0, has3 = 0;
      for (uint32_t i = 0; i < ne; i++) {
         has |= !strcmp(eps[i].extensionName, dce_ext[0]);
         has3 |= !strcmp(eps[i].extensionName, dce_ext[1]);
      }
      dce_f.pNext = &eds3_f;
      VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &dce_f};
      GPF2(pd, &f2);
      void *chain = NULL;
      if (has3 && eds3_f.extendedDynamicState3PolygonMode) {
         g_have_eds3 = 1;
         /* enable polygonMode only: the other bits are not what this test exercises */
         VkPhysicalDeviceExtendedDynamicState3FeaturesEXT e3 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT,
                                                               .extendedDynamicState3PolygonMode = VK_TRUE};
         eds3_f = e3;
         eds3_f.pNext = NULL;
         en_ext[di.enabledExtensionCount++] = dce_ext[1];
         chain = &eds3_f;
      }
      if (has && dce_f.depthClipEnable) {
         g_have_dclip = 1;
         dce_f.pNext = chain;
         chain = &dce_f;
         en_ext[di.enabledExtensionCount++] = dce_ext[0];
      }
      di.pNext = chain;
      di.ppEnabledExtensionNames = en_ext;
   }
   if (CreateDevice(pd, &di, NULL, &dev) != VK_SUCCESS) { printf("{\"fail\":\"CreateDevice\"}\n"); return 1; }
#define LOAD(n) n = (PFN_vk##n)GDPA(dev, "vk" #n); if (!n) { printf("{\"fail\":\"missing vk" #n "\"}\n"); return 1; }
   DEV_FNS(LOAD)
   CmdBindVertexBuffers2_ = (PFN_vkCmdBindVertexBuffers2)GDPA(dev, "vkCmdBindVertexBuffers2");
   CmdSetPolygonModeEXT_ = (PFN_vkCmdSetPolygonModeEXT)GDPA(dev, "vkCmdSetPolygonModeEXT");
   if (g_have_eds3 && !CmdSetPolygonModeEXT_) { printf("{\"fail\":\"missing vkCmdSetPolygonModeEXT\"}\n"); return 1; }
   GetDeviceQueue(dev, 0, 0, &queue);

   int ok = 1;
   if (CASE_ON("triangle") || CASE_ON("rot")) ok &= test_triangle();
   if (ok && CASE_ON("sparse_loc")) { g_sparse = 1; ok &= test_triangle(); g_sparse = 0; }   /* same triangle as "triangle" through attribute locations 0 and 2 */
   if (ok && CASE_ON("cull_dist") && !(getenv("MALI_CULLDIST") && !strcmp(getenv("MALI_CULLDIST"), "1")))
      printf("{\"test\":\"cull_dist\",\"skipped\":\"needs MALI_CULLDIST=1 (the compiler lowers only for the value 1)\",\"ok\":true}\n");
   else if (ok && CASE_ON("cull_dist")) { g_cull = 1; ok &= test_triangle(); g_cull = 0; }   /* gl_CullDistance VS (default off: only with MALI_CULLDIST=1) */
   if (ok && CASE_ON("two")) ok &= test_two(0);
   if (ok && CASE_ON("depth")) ok &= test_two(1);
   /* D16 / D32S8 depth attachments (the depth test result must match D32F) */
   if (ok && CASE_ON("load_depth")) ok &= test_load("load_depth", 0);
   if (ok && CASE_ON("nocol_depth")) ok &= test_nocol("nocol_depth", 0);
   if (ok && CASE_ON("nofs_depth")) ok &= test_nocol("nofs_depth", 0);
   if (ok && CASE_ON("nofs_stencil")) ok &= test_nocol("nofs_stencil", 1);
   if (ok && CASE_ON("nocol_stencil")) ok &= test_nocol("nocol_stencil", 1);
   if (ok && CASE_ON("dyn_stride")) ok &= test_dynstride("dyn_stride", 0);
   if (ok && CASE_ON("dyn_stride_rebind")) ok &= test_dynstride("dyn_stride_rebind", 1);   /* plain BindVertexBuffers after BindVertexBuffers2 keeps the stride */
   if (ok && CASE_ON("discard_both")) ok &= test_discard("discard_both", 1);
   if (ok && CASE_ON("discard_vs_only")) ok &= test_discard("discard_vs_only", 2);
   if (ok && CASE_ON("int_clear_rgba8ui")) ok &= test_int_clear("int_clear_rgba8ui", VK_FORMAT_R8G8B8A8_UINT, 4, 0x073264c8u);
   if (ok && CASE_ON("int_clear_rg16ui")) ok &= test_int_clear("int_clear_rg16ui", VK_FORMAT_R16G16_UINT, 4, 0x006400c8u);
   if (ok && CASE_ON("int_clear_r8ui")) ok &= test_int_clear("int_clear_r8ui", VK_FORMAT_R8_UINT, 1, 0xc8u);
   {
      static const float sn1[4] = { 1, -1, 0, 1 }, f15[4] = { 1.5f, 1.5f, 1.5f, 1.5f };
      if (ok && CASE_ON("rt_clear_r8snorm")) ok &= test_int_clear_f("rt_clear_r8snorm", VK_FORMAT_R8_SNORM, 1, 0x7fu, sn1);
      if (ok && CASE_ON("rt_clear_rgba8snorm")) ok &= test_int_clear_f("rt_clear_rgba8snorm", VK_FORMAT_R8G8B8A8_SNORM, 4, 0x7f00817fu, sn1);
      if (ok && CASE_ON("rt_clear_a8b8g8r8snorm")) ok &= test_int_clear_f("rt_clear_a8b8g8r8snorm", VK_FORMAT_A8B8G8R8_SNORM_PACK32, 4, 0x7f00817fu, sn1);
      if (ok && CASE_ON("rt_clear_r16unorm")) ok &= test_int_clear_f("rt_clear_r16unorm", VK_FORMAT_R16_UNORM, 2, 0xffffu, sn1);
      if (ok && CASE_ON("rt_clear_rg16snorm")) ok &= test_int_clear_f("rt_clear_rg16snorm", VK_FORMAT_R16G16_SNORM, 4, 0x80017fffu, sn1);
      if (ok && CASE_ON("rt_clear_r32f")) ok &= test_int_clear_f("rt_clear_r32f", VK_FORMAT_R32_SFLOAT, 4, 0x3fc00000u, f15);
      if (ok && CASE_ON("rt_clear_rgba32f")) ok &= test_int_clear_f("rt_clear_rgba32f", VK_FORMAT_R32G32B32A32_SFLOAT, 16, 0x3fc00000u, f15);
      if (ok && CASE_ON("rt_clear_a8b8g8r8ui")) ok &= test_int_clear("rt_clear_a8b8g8r8ui", VK_FORMAT_A8B8G8R8_UINT_PACK32, 4, 0x073264c8u);
      if (ok && CASE_ON("rt_clear_a2b10g10r10ui")) ok &= test_int_clear("rt_clear_a2b10g10r10ui", VK_FORMAT_A2B10G10R10_UINT_PACK32, 4, 0xc32190c8u);
   }
   if (ok && CASE_ON("load_depth16")) { g_depfmt = VK_FORMAT_D16_UNORM; ok &= test_load("load_depth16", 0); g_depfmt = VK_FORMAT_D32_SFLOAT; }
   if (ok && CASE_ON("load_stencil")) ok &= test_load("load_stencil", 1);
   if (ok && CASE_ON("st_op_invert")) ok &= test_load("st_op_invert", 3);
   if (ok && CASE_ON("st_op_decwrap")) ok &= test_load("st_op_decwrap", 4);
   if (ok && CASE_ON("load_colour_f16")) { g_colfmt = VK_FORMAT_R16G16B16A16_SFLOAT; g_colbpp = 8; ok &= test_load("load_colour_f16", 2); g_colfmt = VK_FORMAT_R8G8B8A8_UNORM; g_colbpp = 4; }
   if (ok && CASE_ON("load_colour_rgba8ui")) { g_colfmt = VK_FORMAT_R8G8B8A8_UINT; g_colbpp = 4; ok &= test_load("load_colour_rgba8ui", 2); g_colfmt = VK_FORMAT_R8G8B8A8_UNORM; g_colbpp = 4; }   /* RAW-format preload (kinds 12..16) */
   if (ok && CASE_ON("load_colour_rg16sint")) { g_colfmt = VK_FORMAT_R16G16_SINT; g_colbpp = 4; g_clear_u = 1; ok &= test_load("load_colour_rg16sint", 2); g_clear_u = 0; g_colfmt = VK_FORMAT_R8G8B8A8_UNORM; g_colbpp = 4; }   /* non-zero clear: a LOAD that writes zeros must fail */
   if (ok && CASE_ON("load_colour_rgba8snorm")) { g_colfmt = VK_FORMAT_R8G8B8A8_SNORM; g_colbpp = 4; ok &= test_load("load_colour_rgba8snorm", 2); g_colfmt = VK_FORMAT_R8G8B8A8_UNORM; g_colbpp = 4; }   /* RAW-format preload (kinds 12..16) */
   if (ok && CASE_ON("load_colour_rgba16unorm")) { g_colfmt = VK_FORMAT_R16G16B16A16_UNORM; g_colbpp = 8; ok &= test_load("load_colour_rgba16unorm", 2); g_colfmt = VK_FORMAT_R8G8B8A8_UNORM; g_colbpp = 4; }   /* RAW-format preload (kinds 12..16) */
   if (ok && CASE_ON("load_colour_r32f")) { g_colfmt = VK_FORMAT_R32_SFLOAT; g_colbpp = 4; ok &= test_load("load_colour_r32f", 2); g_colfmt = VK_FORMAT_R8G8B8A8_UNORM; g_colbpp = 4; }   /* RAW-format preload (kinds 12..16) */
   if (ok && CASE_ON("load_colour_rgba16uint")) { g_colfmt = VK_FORMAT_R16G16B16A16_UINT; g_colbpp = 8; ok &= test_load("load_colour_rgba16uint", 2); g_colfmt = VK_FORMAT_R8G8B8A8_UNORM; g_colbpp = 4; }   /* RAW-format preload (kinds 12..16) */
   if (ok && CASE_ON("load_colour_a2b10")) { g_colfmt = VK_FORMAT_A2B10G10R10_UNORM_PACK32; ok &= test_load("load_colour_a2b10", 2); g_colfmt = VK_FORMAT_R8G8B8A8_UNORM; }
   if (ok && CASE_ON("load_colour_a1r5")) { g_colfmt = VK_FORMAT_A1R5G5B5_UNORM_PACK16; g_colbpp = 2; ok &= test_load("load_colour_a1r5", 2); g_colfmt = VK_FORMAT_R8G8B8A8_UNORM; g_colbpp = 4; }
   if (ok && CASE_ON("depth16")) { g_depfmt = VK_FORMAT_D16_UNORM; ok &= test_two(1); g_depfmt = VK_FORMAT_D32_SFLOAT; }
   if (ok && CASE_ON("depth32s8")) { g_depfmt = VK_FORMAT_D32_SFLOAT_S8_UINT; ok &= test_two(1); g_depfmt = VK_FORMAT_D32_SFLOAT; }
   if (ok && CASE_ON("stencil")) {
      ok &= test_stencil(VK_FORMAT_S8_UINT);
      ok &= test_stencil(VK_FORMAT_D32_SFLOAT_S8_UINT);
      ok &= test_stencil(VK_FORMAT_D24_UNORM_S8_UINT);
   }
   if (ok && CASE_ON("poly")) {
      ok &= test_poly("poly_line_tri", MODE_TRI, 1, 250, 1500, 128, 106);
      ok &= test_poly("poly_point_tri", MODE_TRI, 2, 1, 40, 128, 106);
      ok &= test_poly("poly_line_idx16", MODE_IDX16, 1, 250, 2500, 100, 130);
      ok &= test_poly("poly_line_idx32", MODE_IDX32, 1, 250, 2500, 100, 130);
      ok &= test_poly("poly_line_mdi", MODE_MDI, 1, 250, 5000, 100, 130);
      ok &= test_poly_dyn("poly_dyn_line_tri", MODE_TRI, 1, 250, 1500, 128, 106);
      ok &= test_poly_dyn("poly_dyn_point_tri", MODE_TRI, 2, 1, 40, 128, 106);
      ok &= test_poly_dyn("poly_dyn_line_idx16", MODE_IDX16, 1, 250, 2500, 100, 130);
   }
   if (ok && CASE_ON("idx16")) ok &= test_mode("idx16", MODE_IDX16, 16384.0, 0.5);
   if (ok && CASE_ON("idx32")) ok &= test_mode("idx32", MODE_IDX32, 16384.0, 0.5);
   if (ok && CASE_ON("mdi")) ok &= test_mode("mdi", MODE_MDI, 16384.0, 0.5);
   if (ok && CASE_ON("scissor")) ok &= test_mode("scissor", MODE_SCISSOR, 8192.0, 0.5);
   if (ok && CASE_ON("viewport")) ok &= test_mode("viewport", MODE_VPORT, 8192.0, 0.5);
   if (ok && CASE_ON("multivp")) {
      if (g_feat.multiViewport) ok &= test_mode("multivp", MODE_MVP, 8192.0, 0.5);
      else printf("{\"test\":\"multivp\",\"skipped\":\"multiViewport not advertised\",\"ok\":true}\n");
   }
   if (ok && CASE_ON("clearatt")) ok &= test_mode("clearatt", MODE_CLEARATT, 0, 0);
   if (ok && CASE_ON("clearquad")) ok &= test_mode("clearquad", MODE_CLEARQUAD, 0, 0);
   if (ok && CASE_ON("clearquad_zs")) { g_clearzs = 1; ok &= test_mode("clearquad_zs", MODE_CLEARQUAD, 0, 0); g_clearzs = 0; }
   if (ok && (CASE_ON("fx") || (g_only && !strncmp(g_only, "fx_", 3)))) ok &= test_fx();
   if (ok && CASE_ON("msaa_resolve4")) ok &= test_msaa("msaa_resolve4", 1);
   if (ok && CASE_ON("msaa_sample_shading")) ok &= test_msaa("msaa_sample_shading", 2);
   if (ok && g_only && CASE_ON("many_draws")) ok &= test_many();   /* explicit only: 50k-draw frames are too big for the golden dumps */
   if (ok && CASE_ON("share")) ok &= test_share();
   if (ok && CASE_ON("share")) ok &= test_share_dyn();
   if (ok && CASE_ON("share")) ok &= test_share_inst();
   if (ok && CASE_ON("share")) ok &= test_share_dirty();
   if (ok && CASE_ON("ring")) ok &= test_ring();
   if (ok && CASE_ON("occlusion")) ok &= test_occ();
   if (ok && CASE_ON("pushfs")) ok &= test_quad("pushfs", MODE_PUSHFS);
   if (ok && CASE_ON("ubo")) ok &= test_quad("ubo", MODE_UBO);
   if (ok && CASE_ON("descupd")) ok &= test_quad("descupd", MODE_DESCUPD);
   if (ok && CASE_ON("texture")) ok &= test_quad("texture", MODE_TEX);

   DeviceWaitIdle(dev);
   DestroyDevice(dev, NULL);
   DestroyInstance(inst, NULL);
   dlclose(lib);
   printf("{\"pass\":%s}\n", (ok && !g_fail) ? "true" : "false");
   return (ok && !g_fail) ? 0 : 1;
}
