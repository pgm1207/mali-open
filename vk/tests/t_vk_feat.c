/* SPDX-License-Identifier: MIT */
/* R4-B2: behaviour tests for the DXVK-required core features, one test per feature (the honesty rule: a feature is advertised only
 * with a passing test here or in t_vk_draw). Loader-free like t_vk_draw. One JSON line per case; exit 0 = all pass.
 *
 *   t_vk_feat --direct <icd.so> [--spv-dir dir] [--only firstinst|provoking|idx32|aniso|mirror|cubearray|arrtex|inlineub|push256|wide|linerast|tf_reject|tf_write|tf_default|tf_inactive|tf_reset|tf_multi|tf_overflow|storfmt|storfmt_nf|ycbcr|imageless|spec|maint5|cbc|divisor|bda|didx|didxbuf|nulld|scalar|ubostd|zinit|demote|subgroup|sgctl|int16|int8|int64|intdot|clip|clip8|samplerate|dparams|gather|gatherlim|fsstore|memmodel]
 *
 * A case whose feature is not advertised prints "skipped" (so it can be run against the driver before the bit is flipped:
 * the flip is made, the test run, and reverted if it fails). Pixels are checked only on hardware (fake backend renders nothing).
 */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define W 256
#define H 256

static int g_fail, g_fake;
static const char *g_only, *g_spv = "shader";
static int g_ycbcr, g_imageless;
static int g_tf;   /* VK_EXT_transform_feedback advertised (default on; MALI_EXPERIMENTAL_TF=0 opts out) */
static const VkSpecializationInfo *g_spec_fs;   /* make_pipe: specialization info of the fragment stage (test spec) */
static VkPhysicalDevice g_pd;
static VkPhysicalDeviceFeatures g_feat;
static VkPhysicalDeviceProperties g_props;
static PFN_vkGetImageSubresourceLayout g_gsl;
static PFN_vkResetFences g_rf;
static PFN_vkResetCommandBuffer g_rcb;
static PFN_vkEnumerateDeviceExtensionProperties g_edep;
static VkBool32 g_mirror, g_bda, g_didx;
static int g_modless;  /* VK_KHR_maintenance5 (test maint5): stages carry a chained VkShaderModuleCreateInfo, module == VK_NULL_HANDLE */
static int g_ib2;      /* test maint5: vkCmdBindIndexBuffer2KHR instead of vkCmdBindIndexBuffer */
static int g_cbc, g_cbc_nofmt; static uint32_t g_cbc_max;   /* EXT_custom_border_color: features and maxCustomBorderColorSamplers */
static int g_m5;       /* maintenance5 feature advertised */
static int g_div, g_div0; static uint32_t g_div_max;   /* EXT/KHR_vertex_attribute_divisor: instance-rate divisor, zero divisor, maxVertexAttribDivisor */
static const VkPipelineVertexInputStateCreateInfo *g_vi;   /* make_pipe: vertex input state override (test divisor) */
static PFN_vkCmdBindIndexBuffer2KHR g_cbib2;
static int test_idx32(void);
static int test_zinit(void);
#define BIND_IB(cb, b, off, ty) do { if (g_ib2) g_cbib2(cb, b, off, VK_WHOLE_SIZE, ty); else CmdBindIndexBuffer(cb, b, off, ty); } while (0)
static int g_sgctl;   /* compute pipelines: add a required subgroup size 16 + REQUIRE_FULL_SUBGROUPS to the stage (test sgctl) */
static VkPrimitiveTopology g_topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
static float g_lw = 1.0f;
static VkBool32 g_sample_shading;   /* make_pipe: sampleShadingEnable (minSampleShading 1.0) */
static const void *g_rs_next;   /* pNext of the rasterization state (line-state struct) for make_pipe */
static int g_expect_xfb_fail;   /* make_pipe: a transform-feedback shader must be refused, with a message naming TF */
static VkBool32 g_lr_rect, g_lr_bres, g_lr_smooth, g_lr_stip_rect, g_lr_stip_bres, g_lr_stip_smooth;   /* VK_EXT_line_rasterization features */

#define CASE_ON(n) (!g_only || !strcmp(g_only, n))
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

#define DEV_FNS(X)                                                                                                    \
   X(DestroyDevice) X(GetDeviceQueue) X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory) X(CreateFence)    \
   X(DestroyFence) X(WaitForFences) X(QueueSubmit) X(DeviceWaitIdle) X(CreateImage) X(DestroyImage)                  \
   X(GetImageMemoryRequirements) X(BindImageMemory) X(CreateImageView) X(DestroyImageView) X(CreateBuffer)          \
   X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(CreateShaderModule) X(DestroyShaderModule) \
   X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer)   \
   X(DestroyFramebuffer) X(CreateGraphicsPipelines) X(DestroyPipeline) X(CreateCommandPool) X(DestroyCommandPool)   \
   X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdBeginRenderPass) X(CmdEndRenderPass)    \
   X(CmdBindPipeline) X(CmdSetViewport) X(CmdSetScissor) X(CmdPushConstants) X(CmdDraw) X(CmdDrawIndexed)            \
   X(CmdBindIndexBuffer) X(CmdDrawIndirect) X(CmdDrawIndexedIndirect) X(CreateDescriptorSetLayout)                  \
   X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets)         \
   X(UpdateDescriptorSets) X(CreateSampler) X(DestroySampler) X(CmdBindDescriptorSets) X(CmdCopyBufferToImage)      \
   X(CmdPipelineBarrier) X(CreateComputePipelines) X(CmdDispatch) X(CmdCopyImageToBuffer) X(GetBufferDeviceAddress) \
   X(CreateSamplerYcbcrConversion) X(DestroySamplerYcbcrConversion) X(CreateRenderPass2) X(CmdBeginRenderPass2) X(CmdEndRenderPass2) \
   X(CmdBindVertexBuffers) X(InvalidateMappedMemoryRanges)

#define DECL(n) static PFN_vk##n n;
DEV_FNS(DECL)
static PFN_vkCmdBindTransformFeedbackBuffersEXT CmdBindTransformFeedbackBuffersEXT;
static PFN_vkCmdBeginTransformFeedbackEXT CmdBeginTransformFeedbackEXT;
static PFN_vkCmdEndTransformFeedbackEXT CmdEndTransformFeedbackEXT;
static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memprops;

static int
memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return (int)i;
   return -1;
}

static uint32_t *g_ml_last; static size_t g_ml_lastsz;
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
   if (g_modless) {   /* no module is created: the caller chains the code to the stage; g_code[] owns the words */
      g_ml_last = buf; g_ml_lastsz = (size_t)n;
      return (VkShaderModule)(uintptr_t)1;   /* "loaded" marker only: never passed to the driver */
   }
   VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = buf};
   VkShaderModule m = VK_NULL_HANDLE;
   if (CreateShaderModule(dev, &ci, NULL, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
   free(buf);
   return m;
}

/* host visible buffer with mapped pointer */
struct hbuf { VkBuffer b; VkDeviceMemory m; uint8_t *p; };

static int
hbuf_create(struct hbuf *h, VkDeviceSize size, VkBufferUsageFlags usage)
{
   VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
   CHECK(CreateBuffer(dev, &bi, NULL, &h->b) == VK_SUCCESS, "buffer");
   VkMemoryRequirements mr;
   GetBufferMemoryRequirements(dev, h->b, &mr);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                              .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
   CHECK(AllocateMemory(dev, &ai, NULL, &h->m) == VK_SUCCESS, "buffer memory");
   CHECK(BindBufferMemory(dev, h->b, h->m, 0) == VK_SUCCESS, "bind buffer");
   CHECK(MapMemory(dev, h->m, 0, VK_WHOLE_SIZE, 0, (void **)&h->p) == VK_SUCCESS, "map buffer");
   return 1;
}

static void
hbuf_destroy(struct hbuf *h)
{
   if (h->b) DestroyBuffer(dev, h->b, NULL);
   if (h->m) FreeMemory(dev, h->m, NULL);
}

/* Render target: linear RGBA8 256x256, host readable. */
struct tgt {
   VkImage img; VkDeviceMemory mem; VkImageView view; VkRenderPass rp; VkFramebuffer fb;
   uint8_t *px; uint32_t pitch;
   VkCommandPool pool; VkFence fence;
};

static int
tgt_create(struct tgt *t)
{
   memset(t, 0, sizeof *t);
   VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                           .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1,
                           .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR,
                           .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   CHECK(CreateImage(dev, &ii, NULL, &t->img) == VK_SUCCESS, "colour image");
   VkMemoryRequirements mr;
   GetImageMemoryRequirements(dev, t->img, &mr);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                              .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
   CHECK(AllocateMemory(dev, &ai, NULL, &t->mem) == VK_SUCCESS, "colour mem");
   CHECK(BindImageMemory(dev, t->img, t->mem, 0) == VK_SUCCESS, "bind colour");
   CHECK(MapMemory(dev, t->mem, 0, VK_WHOLE_SIZE, 0, (void **)&t->px) == VK_SUCCESS, "map colour");
   VkImageSubresource sr = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
   VkSubresourceLayout sl;
   g_gsl(dev, t->img, &sr, &sl);
   t->pitch = (uint32_t)sl.rowPitch;
   t->px += sl.offset;
   VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->img,
                               .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   CHECK(CreateImageView(dev, &vi, NULL, &t->view) == VK_SUCCESS, "colour view");
   VkAttachmentDescription ad = {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
                                 .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                 .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
   VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &cref};
   VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad,
                                 .subpassCount = 1, .pSubpasses = &sp};
   CHECK(CreateRenderPass(dev, &rpi, NULL, &t->rp) == VK_SUCCESS, "render pass");
   VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = t->rp, .attachmentCount = 1,
                                  .pAttachments = &t->view, .width = W, .height = H, .layers = 1};
   CHECK(CreateFramebuffer(dev, &fbi, NULL, &t->fb) == VK_SUCCESS, "framebuffer");
   VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   CHECK(CreateCommandPool(dev, &pci, NULL, &t->pool) == VK_SUCCESS, "pool");
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &t->fence) == VK_SUCCESS, "fence");
   return 1;
}

static void
tgt_destroy(struct tgt *t)
{
   DeviceWaitIdle(dev);
   if (t->fence) DestroyFence(dev, t->fence, NULL);
   if (t->pool) DestroyCommandPool(dev, t->pool, NULL);
   if (t->fb) DestroyFramebuffer(dev, t->fb, NULL);
   if (t->rp) DestroyRenderPass(dev, t->rp, NULL);
   if (t->view) DestroyImageView(dev, t->view, NULL);
   if (t->img) DestroyImage(dev, t->img, NULL);
   if (t->mem) FreeMemory(dev, t->mem, NULL);
}

/* allocate a primary command buffer, begin */
static VkCommandBuffer
cb_begin(struct tgt *t)
{
   VkCommandBuffer cb;
   VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = t->pool,
                                     .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   if (AllocateCommandBuffers(dev, &ai, &cb) != VK_SUCCESS) return NULL;
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   if (BeginCommandBuffer(cb, &bi) != VK_SUCCESS) return NULL;
   return cb;
}

static int
cb_submit(struct tgt *t, VkCommandBuffer cb)
{
   CHECK(EndCommandBuffer(cb) == VK_SUCCESS, "end");
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   VkResult r = QueueSubmit(queue, 1, &si, t->fence);
   CHECK(r == VK_SUCCESS, "submit = %d", r);
   r = WaitForFences(dev, 1, &t->fence, VK_TRUE, 20000000000ull);
   CHECK(r == VK_SUCCESS, "wait = %d", r);
   g_rf(dev, 1, &t->fence);
   return 1;
}

/* graphics pipeline: VS+FS, triangle list, no vertex input, optional set layout / push constant size, no depth, opaque */
static int
make_pipe(struct tgt *t, const char *vs, const char *fs, VkDescriptorSetLayout dsl, uint32_t pc_size, VkPipelineLayout *lay, VkPipeline *pipe)
{
   VkShaderModule v = load_spv(vs);
   uint32_t *vcode = g_ml_last; size_t vsz = g_ml_lastsz;
   VkShaderModule f = load_spv(fs);
   uint32_t *fcode = g_ml_last; size_t fsz = g_ml_lastsz;
   CHECK(v && f, "shader modules %s %s", vs, fs);
   VkShaderModuleCreateInfo vmci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = vsz, .pCode = vcode};
   VkShaderModuleCreateInfo fmci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = fsz, .pCode = fcode};
   VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = pc_size};
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = dsl ? 1 : 0, .pSetLayouts = &dsl,
                                     .pushConstantRangeCount = pc_size ? 1 : 0, .pPushConstantRanges = &pcr};
   CHECK(CreatePipelineLayout(dev, &pli, NULL, lay) == VK_SUCCESS, "pipeline layout");
   VkPipelineShaderStageCreateInfo st[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .pNext = g_modless ? &vmci : NULL, .module = g_modless ? VK_NULL_HANDLE : v, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .pNext = g_modless ? &fmci : NULL, .module = g_modless ? VK_NULL_HANDLE : f, .pName = "main",
       .pSpecializationInfo = g_spec_fs}};
   VkPipelineVertexInputStateCreateInfo vis = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   if (g_vi) vis = *g_vi;
   VkPipelineInputAssemblyStateCreateInfo ias = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = g_topo};
   VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
   VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = g_lw, .pNext = g_rs_next};
   VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                              .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
                                              .sampleShadingEnable = g_sample_shading, .minSampleShading = g_sample_shading ? 1.0f : 0.0f};
   VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
   VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &cba};
   VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
   VkPipelineDynamicStateCreateInfo dys = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
   VkGraphicsPipelineCreateInfo gp = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
                                      .pVertexInputState = &vis, .pInputAssemblyState = &ias, .pViewportState = &vps,
                                      .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cbs,
                                      .pDynamicState = &dys, .layout = *lay, .renderPass = t->rp};
   VkResult r;
   if (g_expect_xfb_fail) {
      /* Capture stderr: the refusal must name the reason, not just return an error code. */
      char msg[512] = "";
      fflush(stderr);
      int saved = dup(2);
      FILE *cap = tmpfile();
      CHECK(saved >= 0 && cap && dup2(fileno(cap), 2) >= 0, "stderr capture (tf)");
      r = CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, pipe);
      fflush(stderr);
      dup2(saved, 2);
      close(saved);
      rewind(cap);
      if (cap) { size_t n = fread(msg, 1, sizeof(msg) - 1, cap); msg[n] = 0; fclose(cap); }
      if (g_modless) { free(vcode); free(fcode); }
      else { DestroyShaderModule(dev, v, NULL); DestroyShaderModule(dev, f, NULL); }
      CHECK(r == VK_ERROR_FEATURE_NOT_PRESENT, "a transform-feedback pipeline must be refused (%d)", r);
      CHECK(strstr(msg, "transform feedback") != NULL, "the refusal must name transform feedback: %s", msg);
      return 1;
   }
   r = CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, pipe);
   if (g_modless) { free(vcode); free(fcode); }
   else { DestroyShaderModule(dev, v, NULL); DestroyShaderModule(dev, f, NULL); }
   CHECK(r == VK_SUCCESS, "graphics pipeline = %d", r);
   return 1;
}

static void
begin_pass(VkCommandBuffer cb, struct tgt *t, VkPipeline pipe)
{
   VkClearValue cv = {.color = {{0, 0, 0, 0}}};
   VkRenderPassBeginInfo rb = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = t->rp, .framebuffer = t->fb,
                               .renderArea = {{0, 0}, {W, H}}, .clearValueCount = 1, .pClearValues = &cv};
   CmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   VkViewport vp = {0, 0, W, H, 0.0f, 1.0f};
   VkRect2D sc = {{0, 0}, {W, H}};
   CmdSetViewport(cb, 0, 1, &vp);
   CmdSetScissor(cb, 0, 1, &sc);
}

static const uint8_t *
pix(struct tgt *t, int x, int y)
{
   return t->px + y * t->pitch + x * 4;
}

static uint32_t
coverage(struct tgt *t)
{
   uint32_t n = 0;
   for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) n += pix(t, x, y)[3] != 0;
   return n;
}

static int
near(const uint8_t *p, int r, int g, int b, int a, int tol)
{
   return abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol && abs(p[3] - a) <= tol;
}

/* ---------------------------------------------------------------- imageless framebuffer + renderpass2 (zink 26.0.8 requires both extension names) */
/* A clear-only pass built with vkCreateRenderPass2, an imageless framebuffer (attachment image info only) and the view supplied at
 * vkCmdBeginRenderPass2 through VkRenderPassAttachmentBeginInfo. The clear colour must land in the image (hardware only). */
static int
test_imageless(void)
{
   if (!g_imageless) { printf("{\"test\":\"imageless\",\"skipped\":\"imagelessFramebuffer not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   VkAttachmentDescription2 ad = {.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                  .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
   VkAttachmentReference2 cref = {.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
   VkSubpassDescription2 sp = {.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2, .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                               .colorAttachmentCount = 1, .pColorAttachments = &cref};
   VkRenderPassCreateInfo2 rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2, .attachmentCount = 1, .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sp};
   VkRenderPass rp2;
   CHECK(CreateRenderPass2(dev, &rpi, NULL, &rp2) == VK_SUCCESS, "vkCreateRenderPass2");
   VkFormat vf = VK_FORMAT_R8G8B8A8_UNORM;
   VkFramebufferAttachmentImageInfo ai = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                          .width = W, .height = H, .layerCount = 1, .viewFormatCount = 1, .pViewFormats = &vf};
   VkFramebufferAttachmentsCreateInfo fa = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO, .attachmentImageInfoCount = 1, .pAttachmentImageInfos = &ai};
   VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .pNext = &fa, .flags = VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT,
                                  .renderPass = rp2, .attachmentCount = 1, .width = W, .height = H, .layers = 1};
   VkFramebuffer fb;
   CHECK(CreateFramebuffer(dev, &fbi, NULL, &fb) == VK_SUCCESS, "imageless framebuffer");
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   VkClearValue cv = {.color = {{0.2f, 0.4f, 0.6f, 1.0f}}};
   VkRenderPassAttachmentBeginInfo ab = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO, .attachmentCount = 1, .pAttachments = &t.view};
   VkRenderPassBeginInfo rb = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .pNext = &ab, .renderPass = rp2, .framebuffer = fb,
                               .renderArea = {{0, 0}, {W, H}}, .clearValueCount = 1, .pClearValues = &cv};
   VkSubpassBeginInfo sb = {.sType = VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO, .contents = VK_SUBPASS_CONTENTS_INLINE};
   VkSubpassEndInfo se = {.sType = VK_STRUCTURE_TYPE_SUBPASS_END_INFO};
   CmdBeginRenderPass2(cb, &rb, &sb);
   CmdEndRenderPass2(cb, &se);
   if (!cb_submit(&t, cb)) return 0;
   int ok = 1;
   if (!g_fake) {
      const uint8_t *p = pix(&t, 100, 100);
      ok = near(p, 51, 102, 153, 255, 3);
      CHECK(ok, "imageless clear pixel %u,%u,%u,%u expected 51,102,153,255", p[0], p[1], p[2], p[3]);
   }
   DestroyFramebuffer(dev, fb, NULL);
   DestroyRenderPass(dev, rp2, NULL);
   tgt_destroy(&t);
   printf("{\"test\":\"imageless\",\"ok\":%s%s}\n", ok ? "true" : "false", g_fake ? ",\"skipped\":\"pixels: fake backend (calls ran)\"" : "");
   return ok;
}

/* ---------------------------------------------------------------- firstinst / idx32 */
static int
red_at(struct tgt *t, int x, int y)
{
   return pix(t, x, y)[0] > 200 && pix(t, x, y)[3] == 255;
}

static int
test_firstinst(void)
{
   if (!g_feat.drawIndirectFirstInstance) { printf("{\"test\":\"firstinst\",\"skipped\":\"drawIndirectFirstInstance not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   VkPipelineLayout lay;
   VkPipeline pipe;
   if (!make_pipe(&t, "inst.vert.spv", "solid.frag.spv", VK_NULL_HANDLE, 0, &lay, &pipe)) return 0;
   struct hbuf ib;
   if (!hbuf_create(&ib, 256, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)) return 0;
   /* byte 0: u32 indices {0,1,2}; byte 16: DrawIndirect {3,1,0,firstInstance 0}, {3,1,0,2}; byte 48: {3,2,0,3} (instances 3,4);
    * byte 80: DrawIndexedIndirect {3,1,0,0,firstInstance 1} */
   static const uint32_t idx[3] = {0, 1, 2};
   static const uint32_t di[12] = {3, 1, 0, 0, 3, 1, 0, 2, 3, 2, 0, 3};
   static const uint32_t dx[5] = {3, 1, 0, 0, 1};
   memcpy(ib.p, idx, sizeof idx);
   memcpy(ib.p + 16, di, sizeof di);
   memcpy(ib.p + 80, dx, sizeof dx);
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   begin_pass(cb, &t, pipe);
   BIND_IB(cb, ib.b, 0, VK_INDEX_TYPE_UINT32);
   CmdDrawIndirect(cb, ib.b, 16, 1, 16);                 /* A: firstInstance 0 -> x -1..-0.5 */
   CmdDrawIndirect(cb, ib.b, 32, 1, 16);                 /* B: firstInstance 2 -> x 0..0.5 */
   CmdDrawIndirect(cb, ib.b, 48, 1, 16);                 /* D: instances 3,4 -> x 0.5..1 (4 is off screen) */
   CmdDrawIndexedIndirect(cb, ib.b, 80, 1, 20);          /* C: firstInstance 1 -> x -0.5..0 */
   CmdEndRenderPass(cb);
   if (!cb_submit(&t, cb)) return 0;
   if (!g_fake) {
      uint32_t cov = coverage(&t);
      CHECK(abs((int)cov - 8192) <= 8192 / 50, "coverage %u expected 8192", cov);
      /* centroids of A, C, B, D (y = 117 in image rows: NDC y -0.083) */
      static const int cx[4] = {21, 85, 149, 213};
      for (int i = 0; i < 4; i++)
         CHECK(red_at(&t, cx[i], 117), "triangle %d missing at (%d,117): %u,%u,%u,%u", i, cx[i], pix(&t, cx[i], 117)[0],
               pix(&t, cx[i], 117)[1], pix(&t, cx[i], 117)[2], pix(&t, cx[i], 117)[3]);
      printf("{\"test\":\"firstinst\",\"coverage\":%u,\"ok\":true}\n", cov);
   } else {
      printf("{\"test\":\"firstinst\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   }
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   hbuf_destroy(&ib);
   tgt_destroy(&t);
   return 1;
}

/* multiDrawIndirect at maxDrawIndirectCount with a padded stride (32 != sizeof(VkDrawIndirectCommand) = 16). Entries 0, 1 and limit-1 hold real draws
 * (firstInstance 0, 2, 3 -> columns 0, 2, 3 of the inst shader); every other entry is all-zero (no-op), and the 16 padding bytes of the used entries
 * are garbage that a stride-16 reader would execute. */
static int
test_mdilimit(void)
{
   if (!g_feat.multiDrawIndirect) { printf("{\"test\":\"mdilimit\",\"skipped\":\"multiDrawIndirect not advertised\",\"ok\":true}\n"); return 1; }
   uint32_t lim = g_props.limits.maxDrawIndirectCount;
   CHECK(lim >= 0xffff, "maxDrawIndirectCount %u < 65535 with multiDrawIndirect", lim);
   const uint32_t stride = 32;
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   VkPipelineLayout lay;
   VkPipeline pipe;
   if (!make_pipe(&t, "inst.vert.spv", "solid.frag.spv", VK_NULL_HANDLE, 0, &lay, &pipe)) return 0;
   struct hbuf ib;
   VkDeviceSize sz = 16 + (VkDeviceSize)stride * (lim - 1) + 16;
   if (!hbuf_create(&ib, sz, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)) return 0;
   memset(ib.p, 0, sz);
   static const uint32_t idx[3] = {0, 1, 2};
   memcpy(ib.p, idx, sizeof idx);
   static const uint32_t first[3] = {0, 2, 3};
   const uint32_t slot[3] = {0, 1, lim - 1};
   for (int i = 0; i < 3; i++) {
      uint32_t *e = (uint32_t *)(void *)(ib.p + 16 + (size_t)stride * slot[i]);
      e[0] = 3; e[1] = 1; e[2] = 0; e[3] = first[i];
      e[4] = 3; e[5] = 1; e[6] = 0; e[7] = 1;   /* padding: would be a visible triangle if the stride were 16 */
   }
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   begin_pass(cb, &t, pipe);
   BIND_IB(cb, ib.b, 0, VK_INDEX_TYPE_UINT32);
   CmdDrawIndirect(cb, ib.b, 16, lim, stride);
   CmdEndRenderPass(cb);
   if (!cb_submit(&t, cb)) return 0;
   if (!g_fake) {
      uint32_t cov = coverage(&t);
      CHECK(abs((int)cov - 3 * 2048) <= 3 * 2048 / 50, "coverage %u expected %u", cov, 3 * 2048);
      static const int cx[4] = {21, 85, 149, 213};
      for (int i = 0; i < 4; i++)
         CHECK(red_at(&t, cx[i], 117) == (i != 1), "column %d: %s", i, i == 1 ? "padding was executed (stride ignored)" : "triangle missing");
      printf("{\"test\":\"mdilimit\",\"coverage\":%u,\"limit\":%u,\"ok\":true}\n", cov, lim);
   } else printf("{\"test\":\"mdilimit\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   hbuf_destroy(&ib);
   tgt_destroy(&t);
   return 1;
}

/* ---------------------------------------------------------------- vertex attribute divisor */
/* One float attribute at instance rate with divisor d (shader div.vert: x offset = value). 6 instances; the vertex buffer holds the
 * offsets 0, 0.5, 1, 1.5 (triangle k at column k of four). Instance i reads element i / d (d = 0: element 0), so the triangles drawn are
 * the columns {i / d | i < 6, i / d < 4}: checked as present / absent at each column centroid. d = 1 is the plain instance-rate case, 2 and
 * 4 are the POT hardware divisor, 3 5 7 the NPOT one, 0 needs vertexAttributeInstanceRateZeroDivisor. */
static int
test_divisor(void)
{
   if (!g_div && !g_div0) { printf("{\"test\":\"divisor\",\"skipped\":\"vertexAttributeInstanceRateDivisor not advertised\",\"ok\":true}\n"); return 1; }
   CHECK(g_div_max >= 0xffff, "maxVertexAttribDivisor %u", g_div_max);
   /* {divisor, firstInstance, instances}. Second block: firstInstance != 0. Hardware (v10, 2026-11-18): the draw's INSTANCE_OFFSET is added to the
    * instance index BEFORE the divide, element = (firstInstance + i) / divisor - which IS Vulkan's rule (the instance index starts at firstInstance,
    * vktDrawInstancedTests.cpp:926-927 builds its reference the same way). supportsNonZeroFirstInstance stays false only because the one device run of
    * the firstInstance cases (job 20261118-142435-48673771) failed and has never been explained; the test pins the hardware so a rerun shows it. */
   static const uint32_t cs[][3] = {{1, 0, 6}, {2, 0, 6}, {3, 0, 6}, {4, 0, 6}, {5, 0, 6}, {7, 0, 6}, {0, 0, 6},
                                    {2, 1, 4}, {3, 1, 6}, {4, 2, 8}, {1, 1, 2}, {0, 1, 3}};
   struct hbuf vb;
   if (!hbuf_create(&vb, 64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return 0;
   static const float vals[4] = {0.0f, 0.5f, 1.0f, 1.5f};
   memset(vb.p, 0, 64);   /* instances past the four columns (d = 1 draws 6) read zeros = column 0, not leftover memory */
   memcpy(vb.p, vals, sizeof vals);
   for (unsigned c = 0; c < sizeof cs / sizeof *cs; c++) {
      const uint32_t d = cs[c][0], fi = cs[c][1], ni = cs[c][2];
      if (d > 1 && !g_div) { printf("{\"test\":\"divisor\",\"divisor\":%u,\"skipped\":\"divisor > 1 not advertised (firstInstance semantics, see mali_physical_device.c)\",\"ok\":true}\n", d); continue; }
      if (d == 0 && !g_div0) { printf("{\"test\":\"divisor\",\"divisor\":0,\"skipped\":\"zero divisor not advertised\",\"ok\":true}\n"); continue; }
      struct tgt t;
      if (!tgt_create(&t)) return 0;
      VkVertexInputBindingDescription bd = {.binding = 0, .stride = 4, .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE};
      VkVertexInputAttributeDescription ad = {.location = 0, .binding = 0, .format = VK_FORMAT_R32_SFLOAT, .offset = 0};
      VkVertexInputBindingDivisorDescriptionEXT dd = {.binding = 0, .divisor = d};
      VkPipelineVertexInputDivisorStateCreateInfoEXT dsci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
                                                             .vertexBindingDivisorCount = 1, .pVertexBindingDivisors = &dd};
      VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = &dsci,
                                                 .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bd,
                                                 .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &ad};
      VkPipelineLayout lay;
      VkPipeline pipe;
      g_vi = &vi;
      int mp = make_pipe(&t, "div.vert.spv", "solid.frag.spv", VK_NULL_HANDLE, 0, &lay, &pipe);
      g_vi = NULL;
      if (!mp) return 0;
      VkCommandBuffer cb = cb_begin(&t);
      CHECK(cb, "cmdbuf");
      begin_pass(cb, &t, pipe);
      VkDeviceSize off = 0;
      CmdBindVertexBuffers(cb, 0, 1, &vb.b, &off);
      CmdDraw(cb, 3, ni, 0, fi);
      CmdEndRenderPass(cb);
      if (!cb_submit(&t, cb)) return 0;
      if (!g_fake) {
         int want[4] = {0, 0, 0, 0};
         for (unsigned i = 0; i < ni; i++) { unsigned k = d == 0 ? 0 : d == 1 ? fi + i : (fi + i) / d;   /* d > 1: the hardware divides the offset instance index */ if (k < 4) want[k] = 1; }
         for (int k = 0; k < 4; k++)
            CHECK(red_at(&t, 21 + 64 * k, 117) == want[k], "divisor %u first %u: column %d %s, pixel %u,%u,%u,%u", d, fi, k, want[k] ? "missing" : "drawn",
                  pix(&t, 21 + 64 * k, 117)[0], pix(&t, 21 + 64 * k, 117)[1], pix(&t, 21 + 64 * k, 117)[2], pix(&t, 21 + 64 * k, 117)[3]);
         printf("{\"test\":\"divisor\",\"divisor\":%u,\"first_instance\":%u,\"columns\":\"%d%d%d%d\",\"ok\":true}\n", d, fi, want[0], want[1], want[2], want[3]);
      } else {
         printf("{\"test\":\"divisor\",\"divisor\":%u,\"first_instance\":%u,\"skipped\":\"pixels: fake backend\",\"ok\":true}\n", d, fi);
      }
      DestroyPipeline(dev, pipe, NULL);
      DestroyPipelineLayout(dev, lay, NULL);
      tgt_destroy(&t);
   }
   hbuf_destroy(&vb);
   return 1;
}

/* ---------------------------------------------------------------- inst_divisor: three instance/vertex rates in one draw */
/* Proves that VkVertexInputBindingDivisorDescriptionEXT reaches the attribute descriptors, with a control that is a different RATE in
 * the same draw: location 0 (binding 0) is instance rate with divisor 1, so instance k reads element k (red = 0 on instance 0, red = 255
 * on instance 1); location 1 (binding 1) has divisor 0, i.e. one element for every instance (green = 255 in both halves, 0 if the
 * divisor were ignored and the element advanced); location 2 (binding 2) is vertex rate, so its value differs between two pixels of the
 * same triangle (blue near 0 at the first vertex, near 255 at the third). One draw, one readback. */
static int
test_inst_divisor(void)
{
   if (!g_div || !g_div0) { printf("{\"test\":\"inst_divisor\",\"skipped\":\"vertexAttributeInstanceRateDivisor/ZeroDivisor not advertised\",\"ok\":true}\n"); return 1; }
   /* One buffer, three bindings: [0..31] the two divisor-1 elements (vec4), [32..39] the two divisor-0 elements, [40..51] the three
    * vertex-rate elements. Element 1 of the divisor-0 binding is 0.0 so a divisor that is ignored shows up as a black right half. */
   struct hbuf vb;
   if (!hbuf_create(&vb, 64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return 0;
   memset(vb.p, 0, 64);
   const float inst_el[2][4] = {{0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}};
   const float zero_el[2] = {1.0f, 0.0f};
   const float vtx_el[3] = {0.0f, 0.0f, 1.0f};
   memcpy(vb.p, inst_el, sizeof inst_el);
   memcpy(vb.p + 32, zero_el, sizeof zero_el);
   memcpy(vb.p + 40, vtx_el, sizeof vtx_el);

   struct tgt t;
   if (!tgt_create(&t)) return 0;
   VkVertexInputBindingDescription bd[3] = {{.binding = 0, .stride = 16, .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE},
                                            {.binding = 1, .stride = 4, .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE},
                                            {.binding = 2, .stride = 4, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX}};
   VkVertexInputAttributeDescription ad[3] = {{.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0},
                                              {.location = 1, .binding = 1, .format = VK_FORMAT_R32_SFLOAT, .offset = 0},
                                              {.location = 2, .binding = 2, .format = VK_FORMAT_R32_SFLOAT, .offset = 0}};
   VkVertexInputBindingDivisorDescriptionEXT dd[2] = {{.binding = 0, .divisor = 1}, {.binding = 1, .divisor = 0}};
   VkPipelineVertexInputDivisorStateCreateInfoEXT dsci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
                                                          .vertexBindingDivisorCount = 2, .pVertexBindingDivisors = dd};
   VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = &dsci,
                                              .vertexBindingDescriptionCount = 3, .pVertexBindingDescriptions = bd,
                                              .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = ad};
   VkPipelineLayout lay;
   VkPipeline pipe;
   g_vi = &vi;
   int mp = make_pipe(&t, "inst_div.vert.spv", "inst_div.frag.spv", VK_NULL_HANDLE, 0, &lay, &pipe);
   g_vi = NULL;
   if (!mp) return 0;
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   begin_pass(cb, &t, pipe);
   VkBuffer bufs[3] = {vb.b, vb.b, vb.b};   /* the three bindings are windows into one buffer */
   VkDeviceSize off[3] = {0, 32, 40};
   CmdBindVertexBuffers(cb, 0, 3, bufs, off);
   CmdDraw(cb, 3, 2, 0, 0);   /* two instances, same geometry: the left half is instance 0, the right half instance 1 */
   CmdEndRenderPass(cb);
   if (!cb_submit(&t, cb)) return 0;
   if (!g_fake) {
      const uint8_t *left = pix(&t, 50, 204);    /* instance 0, inside the triangle */
      const uint8_t *right = pix(&t, 204, 204);  /* instance 1, the same place in its half */
      const uint8_t *bot = pix(&t, 12, 242);     /* vertex 0 corner: vertex-rate value 0 */
      const uint8_t *top = pix(&t, 12, 31);      /* vertex 2 corner: vertex-rate value 1 */
      CHECK(left[0] < 20 && left[3] == 255, "instance 0: divisor-1 element 0 (red %u)", left[0]);
      CHECK(right[0] > 235, "instance 1: divisor-1 element 1 (red %u)", right[0]);
      CHECK(left[1] > 235 && right[1] > 235, "divisor 0 must read element 0 for both instances (green %u/%u)", left[1], right[1]);
      CHECK(bot[2] < 40 && top[2] > 200, "vertex-rate attribute must advance per vertex (blue %u bottom / %u top)", bot[2], top[2]);
      printf("{\"test\":\"inst_divisor\",\"divisor\":1,\"zero_divisor\":0,\"vertex_rate\":true,\"left\":\"%u,%u,%u\",\"right\":\"%u,%u,%u\""
             ",\"top_blue\":%u,\"bottom_blue\":%u,\"ok\":true}\n", left[0], left[1], left[2], right[0], right[1], right[2], top[2], bot[2]);
   } else {
      printf("{\"test\":\"inst_divisor\",\"divisor\":1,\"zero_divisor\":0,\"vertex_rate\":true,\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   }
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   tgt_destroy(&t);
   hbuf_destroy(&vb);
   return 1;
}

static int
test_idx32(void)
{
   if (!g_feat.fullDrawIndexUint32) { printf("{\"test\":\"idx32\",\"skipped\":\"fullDrawIndexUint32 not advertised\",\"ok\":true}\n"); return 1; }
   CHECK(g_props.limits.maxDrawIndexedIndexValue == 0xffffffffu, "maxDrawIndexedIndexValue = %u", g_props.limits.maxDrawIndexedIndexValue);
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   VkPipelineLayout lay;
   VkPipeline pipe;
   if (!make_pipe(&t, "inst.vert.spv", "solid.frag.spv", VK_NULL_HANDLE, 0, &lay, &pipe)) return 0;
   struct hbuf ib;
   if (!hbuf_create(&ib, 256, VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) return 0;
   uint32_t idx[6] = {0x01000000u, 0x01000001u, 0x01000002u, 0x7ffffff0u, 0x7ffffff1u, 0x7ffffff2u};
   memcpy(ib.p, idx, sizeof idx);
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   begin_pass(cb, &t, pipe);
   BIND_IB(cb, ib.b, 0, VK_INDEX_TYPE_UINT32);
   CmdDrawIndexed(cb, 3, 1, 0, -0x01000000, 0);          /* indices above 2^24: vertex 0..2, x -1..-0.5 */
   CmdDrawIndexed(cb, 3, 1, 3, -0x7ffffff0, 2);          /* indices near 2^31, firstInstance 2: x 0..0.5 */
   CmdEndRenderPass(cb);
   if (!cb_submit(&t, cb)) return 0;
   if (!g_fake) {
      uint32_t cov = coverage(&t);
      CHECK(abs((int)cov - 4096) <= 4096 / 50, "coverage %u expected 4096", cov);
      CHECK(red_at(&t, 21, 117), "first triangle missing");
      CHECK(red_at(&t, 149, 117), "second triangle missing");
      printf("{\"test\":\"idx32\",\"coverage\":%u,\"ok\":true}\n", cov);
   } else {
      printf("{\"test\":\"idx32\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   }
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   hbuf_destroy(&ib);
   tgt_destroy(&t);
   return 1;
}

/* ---------------------------------------------------------------- textured cases */
struct tex {
   VkImage img; VkDeviceMemory mem; VkImageView view; VkSampler smp;
   VkDescriptorSetLayout dsl; VkDescriptorPool pool; VkDescriptorSet set;
};

/* Create an optimal sampled image, upload `regions` from `data`, make a view / sampler / descriptor set (combined sampler, fragment). */
static int
tex_create(struct tgt *t, struct tex *x, const VkImageCreateInfo *ici, VkImageViewType vt, uint32_t layers, const void *data, size_t size,
           const VkBufferImageCopy *regions, uint32_t nreg, const VkSamplerCreateInfo *sci)
{
   memset(x, 0, sizeof *x);
   CHECK(CreateImage(dev, ici, NULL, &x->img) == VK_SUCCESS, "texture image");
   VkMemoryRequirements mr;
   GetImageMemoryRequirements(dev, x->img, &mr);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                              .memoryTypeIndex = memtype(mr.memoryTypeBits, 0)};
   CHECK(AllocateMemory(dev, &ai, NULL, &x->mem) == VK_SUCCESS, "texture memory");
   CHECK(BindImageMemory(dev, x->img, x->mem, 0) == VK_SUCCESS, "bind texture");
   struct hbuf st;
   if (!hbuf_create(&st, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return 0;
   memcpy(st.p, data, size);
   VkCommandBuffer cb = cb_begin(t);
   CHECK(cb, "cmdbuf");
   VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, ici->mipLevels, 0, ici->arrayLayers};
   VkImageMemoryBarrier b1 = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = x->img, .subresourceRange = rng};
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
   CmdCopyBufferToImage(cb, st.b, x->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nreg, regions);
   VkImageMemoryBarrier b2 = b1;
   b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
   b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
   if (!cb_submit(t, cb)) return 0;
   hbuf_destroy(&st);
   VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = x->img, .viewType = vt,
                               .format = ici->format, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, ici->mipLevels, 0, layers}};
   CHECK(CreateImageView(dev, &vi, NULL, &x->view) == VK_SUCCESS, "texture view");
   CHECK(CreateSampler(dev, sci, NULL, &x->smp) == VK_SUCCESS, "sampler");
   VkDescriptorSetLayoutBinding lb = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
                                      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &lb};
   CHECK(CreateDescriptorSetLayout(dev, &dli, NULL, &x->dsl) == VK_SUCCESS, "dsl");
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
   CHECK(CreateDescriptorPool(dev, &dpi, NULL, &x->pool) == VK_SUCCESS, "dpool");
   VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = x->pool,
                                      .descriptorSetCount = 1, .pSetLayouts = &x->dsl};
   CHECK(AllocateDescriptorSets(dev, &dai, &x->set) == VK_SUCCESS, "alloc set");
   VkDescriptorImageInfo ii = {x->smp, x->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
   VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = x->set, .dstBinding = 0, .descriptorCount = 1,
                             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
   UpdateDescriptorSets(dev, 1, &w, 0, NULL);
   return 1;
}

static void
tex_destroy(struct tex *x)
{
   if (x->pool) DestroyDescriptorPool(dev, x->pool, NULL);
   if (x->dsl) DestroyDescriptorSetLayout(dev, x->dsl, NULL);
   if (x->smp) DestroySampler(dev, x->smp, NULL);
   if (x->view) DestroyImageView(dev, x->view, NULL);
   if (x->img) DestroyImage(dev, x->img, NULL);
   if (x->mem) FreeMemory(dev, x->mem, NULL);
}

/* one full-screen quad draw sampling tex with fragment shader fs, push constants uv = pos01 * s.xy + s.zw */
static int
quad_draw(struct tgt *t, struct tex *x, const char *fs, const float s[4])
{
   VkPipelineLayout lay;
   VkPipeline pipe;
   if (!make_pipe(t, "quad.vert.spv", fs, x->dsl, 16, &lay, &pipe)) return 0;
   VkCommandBuffer cb = cb_begin(t);
   CHECK(cb, "cmdbuf");
   begin_pass(cb, t, pipe);
   CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, lay, 0, 1, &x->set, 0, NULL);
   CmdPushConstants(cb, lay, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, s);
   CmdDraw(cb, 6, 1, 0, 0);
   CmdEndRenderPass(cb);
   int ok = cb_submit(t, cb);
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   return ok;
}

static VkImageCreateInfo
img2d(VkFormat f, uint32_t w, uint32_t h, uint32_t mips, uint32_t layers, VkImageCreateFlags flags)
{
   return (VkImageCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                              .extent = {w, h, 1}, .mipLevels = mips, .arrayLayers = layers, .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
}

/* Anisotropy: 64x64 texture, 7 mips each a different flat colour (mip 0 red, mip 1 green). The quad's uv derivative is 0.25 texel/px in
 * u and 1.5 in v: without anisotropy the LOD follows the long axis (log2 1.5 = 0.58 -> mip 1, green); with maxAnisotropy 16 the LOD
 * follows long/ratio (1.5/6 = 0.25 -> clamped to 0 -> mip 0, red). */
static int
aniso_run(struct tgt *t, int enable, uint8_t out[4])
{
   static const uint8_t mc[7][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255},
                                    {0, 255, 255, 255}, {255, 0, 255, 255}, {255, 255, 255, 255}};
   uint8_t *data = malloc(64 * 64 * 4 * 2);
   VkBufferImageCopy reg[7];
   size_t off = 0;
   for (int l = 0; l < 7; l++) {
      uint32_t d = 64u >> l;
      for (uint32_t i = 0; i < d * d; i++) memcpy(data + off + i * 4, mc[l], 4);
      reg[l] = (VkBufferImageCopy){.bufferOffset = off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l, 0, 1}, .imageExtent = {d, d, 1}};
      off += d * d * 4;
   }
   VkImageCreateInfo ici = img2d(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 7, 1, 0);
   VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
                             .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                             .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                             .anisotropyEnable = enable, .maxAnisotropy = enable ? 16.0f : 1.0f, .maxLod = 7.0f};
   struct tex x;
   int ok = tex_create(t, &x, &ici, VK_IMAGE_VIEW_TYPE_2D, 1, data, off, reg, 7, &si);
   free(data);
   if (ok) {
      const float s[4] = {1.0f, 6.0f, 0, 0};
      ok = quad_draw(t, &x, "tex.frag.spv", s);
      if (ok && !g_fake) memcpy(out, pix(t, 128, 128), 4);
   }
   tex_destroy(&x);
   return ok;
}

static int
test_aniso(void)
{
   if (!g_feat.samplerAnisotropy) { printf("{\"test\":\"aniso\",\"skipped\":\"samplerAnisotropy not advertised\",\"ok\":true}\n"); return 1; }
   CHECK(g_props.limits.maxSamplerAnisotropy == 16.0f, "maxSamplerAnisotropy = %g", g_props.limits.maxSamplerAnisotropy);
   uint8_t a[4] = {0}, b[4] = {0};
   for (int e = 0; e < 2; e++) {
      struct tgt t;
      if (!tgt_create(&t)) return 0;
      int ok = aniso_run(&t, e, e ? b : a);
      tgt_destroy(&t);
      if (!ok) return 0;
   }
   if (g_fake) { printf("{\"test\":\"aniso\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n"); return 1; }
   CHECK(near(a, 0, 255, 0, 255, 8), "no anisotropy: %u,%u,%u,%u expected mip 1 (green)", a[0], a[1], a[2], a[3]);
   CHECK(near(b, 255, 0, 0, 255, 8), "anisotropy 16: %u,%u,%u,%u expected mip 0 (red)", b[0], b[1], b[2], b[3]);
   printf("{\"test\":\"aniso\",\"off\":[%u,%u,%u,%u],\"on\":[%u,%u,%u,%u],\"ok\":true}\n", a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
   return 1;
}

/* Mirror clamp to edge: 4x1 texture R,G,B,Y; u runs -1..1 across the quad, nearest filter: coordinate -> clamp(|u|, 0, 1). */
static int
test_mirror(void)
{
   if (!g_mirror) { printf("{\"test\":\"mirror\",\"skipped\":\"samplerMirrorClampToEdge not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   static const uint8_t texel[4][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}};
   VkBufferImageCopy reg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {4, 1, 1}};
   VkImageCreateInfo ici = img2d(VK_FORMAT_R8G8B8A8_UNORM, 4, 1, 1, 1, 0);
   VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                             .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
                             .addressModeV = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
                             .addressModeW = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, .maxLod = 0.25f};
   struct tex x;
   if (!tex_create(&t, &x, &ici, VK_IMAGE_VIEW_TYPE_2D, 1, texel, sizeof texel, &reg, 1, &si)) return 0;
   const float s[4] = {2.0f, 0.0f, -1.0f, 0.5f};
   if (!quad_draw(&t, &x, "tex.frag.spv", s)) return 0;
   if (!g_fake) {
      /* x -> u = (x+.5)/128 - 1 ; texel = floor(clamp(|u|,0,1) * 4) */
      /* x -> u = (x+.5)/128 - 1 ; texel = floor(clamp(|u|,0,1) * 4); skip positions near a texel boundary */
      int n = 0, mirrored = 0;
      for (int x = 4; x < W; x += 6) {
         double u = (x + 0.5) / 128.0 - 1.0, a = fabs(u) > 1.0 ? 1.0 : fabs(u), ft = a * 4.0;
         if (fabs(ft - floor(ft + 0.5)) < 0.15) continue;
         int tx = (int)ft > 3 ? 3 : (int)ft;
         const uint8_t *e = texel[tx];
         const uint8_t *p = pix(&t, x, 128);
         CHECK(near(p, e[0], e[1], e[2], e[3], 8), "x=%d (u=%.3f) got %u,%u,%u,%u expected texel %d", x, u, p[0], p[1], p[2], p[3], tx);
         n++;
         mirrored += u < 0;
      }
      CHECK(n > 20 && mirrored > 8, "probes %d mirrored %d", n, mirrored);
      printf("{\"test\":\"mirror\",\"probes\":%d,\"ok\":true}\n", n);
   } else {
      printf("{\"test\":\"mirror\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   }
   tex_destroy(&x);
   tgt_destroy(&t);
   return 1;
}

/* VK_EXT_custom_border_color: clamp-to-border sampler with a custom colour. 4x1 white texture, u runs -1..1 across the quad (as in test mirror), nearest
 * filter: u < 0 returns the border colour, u in (0,1) the texel. Float colour (0.25, 0.5, 0.75, 1) -> 64,128,191,255 in R8G8B8A8_UNORM, once with the
 * view format named in the create info and once with VK_FORMAT_UNDEFINED (customBorderColorWithoutFormat). The descriptor words come from
 * vk_sampler_state.border_color_value (the runtime fills it from VkSamplerCustomBorderColorCreateInfoEXT) -> sampler descriptor words 4..7. */
static int
cbc_one(VkFormat fmt)
{
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   static const uint8_t texel[4][4] = {{255, 255, 255, 255}, {255, 255, 255, 255}, {255, 255, 255, 255}, {255, 255, 255, 255}};
   VkBufferImageCopy reg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {4, 1, 1}};
   VkImageCreateInfo ici = img2d(VK_FORMAT_R8G8B8A8_UNORM, 4, 1, 1, 1, 0);
   VkSamplerCustomBorderColorCreateInfoEXT cb = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
                                                 .customBorderColor = {.float32 = {0.25f, 0.5f, 0.75f, 1.0f}}, .format = fmt};
   VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .pNext = &cb, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                             .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                             .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                             .borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT, .maxLod = 0.25f};
   struct tex x;
   if (!tex_create(&t, &x, &ici, VK_IMAGE_VIEW_TYPE_2D, 1, texel, sizeof texel, &reg, 1, &si)) return 0;
   const float s[4] = {2.0f, 0.0f, -1.0f, 0.5f};
   if (!quad_draw(&t, &x, "tex.frag.spv", s)) return 0;
   if (!g_fake) {
      const uint8_t *b = pix(&t, 10, 128), *in = pix(&t, 200, 128);
      CHECK(near(b, 64, 128, 191, 255, 6), "border (format %d): got %u,%u,%u,%u expected 64,128,191,255", (int)fmt, b[0], b[1], b[2], b[3]);
      CHECK(near(in, 255, 255, 255, 255, 6), "inside (format %d): got %u,%u,%u,%u expected white", (int)fmt, in[0], in[1], in[2], in[3]);
   }
   tex_destroy(&x);
   tgt_destroy(&t);
   return 1;
}

static int
test_cbc(void)
{
   if (!g_cbc) { printf("{\"test\":\"cbc\",\"skipped\":\"customBorderColors not advertised\",\"ok\":true}\n"); return 1; }
   CHECK(g_cbc_nofmt, "customBorderColorWithoutFormat must go with customBorderColors (one descriptor path)");
   CHECK(g_cbc_max >= 1, "maxCustomBorderColorSamplers = %u", g_cbc_max);
   int ok = cbc_one(VK_FORMAT_R8G8B8A8_UNORM) && cbc_one(VK_FORMAT_UNDEFINED);
   if (!ok) return 0;
   printf("{\"test\":\"cbc\",\"max\":%u,\"ok\":true%s}\n", g_cbc_max, g_fake ? ",\"skipped\":\"pixels: fake backend (creation and draw ran)\"" : "");
   return 1;
}

/* Cube array: 2 cubes (12 layers) of 4x4 flat faces, layer L colour (10+20L, 255-20L, 77). Four quadrants sample L = 0, 3, 6, 10. */
static int
test_cubearray(void)
{
   if (!g_feat.imageCubeArray) { printf("{\"test\":\"cubearray\",\"skipped\":\"imageCubeArray not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   uint8_t data[12 * 4 * 4 * 4];
   for (int l = 0; l < 12; l++)
      for (int i = 0; i < 16; i++) {
         uint8_t *p = data + (l * 16 + i) * 4;
         p[0] = 10 + 20 * l; p[1] = 255 - 20 * l; p[2] = 77; p[3] = 255;
      }
   VkBufferImageCopy reg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 12}, .imageExtent = {4, 4, 1}};
   VkImageCreateInfo ici = img2d(VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 12, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
   VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                             .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                             .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
   struct tex x;
   if (!tex_create(&t, &x, &ici, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 12, data, sizeof data, &reg, 1, &si)) return 0;
   const float s[4] = {1.0f, 1.0f, 0, 0};
   if (!quad_draw(&t, &x, "cubearr.frag.spv", s)) return 0;
   if (!g_fake) {
      static const struct { int x, y, l; } probe[] = {{64, 64, 0}, {192, 64, 3}, {64, 192, 6}, {192, 192, 10}};
      for (unsigned i = 0; i < 4; i++) {
         int l = probe[i].l;
         const uint8_t *p = pix(&t, probe[i].x, probe[i].y);
         CHECK(near(p, 10 + 20 * l, 255 - 20 * l, 77, 255, 6), "quadrant %u got %u,%u,%u,%u expected layer %d", i, p[0], p[1], p[2], p[3], l);
      }
      printf("{\"test\":\"cubearray\",\"ok\":true}\n");
   } else {
      printf("{\"test\":\"cubearray\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   }
   tex_destroy(&x);
   tgt_destroy(&t);
   return 1;
}

/* Array texture with mips (layer-major payload order): 8x8, 4 mips, 3 layers, every (layer, mip) its own flat colour
 * R = 40 + 60 L, G = 40 + 50 M, B = 100. Quadrants sample (L,M) = (1,0), (2,1), (1,2), (2,3). */
static int
test_arrtex(void)
{
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   uint8_t data[3 * (64 + 16 + 4 + 1) * 4];
   VkBufferImageCopy reg[12];
   size_t off = 0;
   unsigned n = 0;
   for (unsigned l = 0; l < 3; l++)
      for (unsigned m = 0; m < 4; m++) {
         unsigned dim = 8u >> m;
         reg[n++] = (VkBufferImageCopy){.bufferOffset = off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, l, 1}, .imageExtent = {dim, dim, 1}};
         for (unsigned i = 0; i < dim * dim; i++, off += 4) {
            data[off] = 40 + 60 * l; data[off + 1] = 40 + 50 * m; data[off + 2] = 100; data[off + 3] = 255;
         }
      }
   VkImageCreateInfo ici = img2d(VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 4, 3, 0);
   VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                             .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                             .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 3.0f};
   struct tex x;
   if (!tex_create(&t, &x, &ici, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 3, data, off, reg, 12, &si)) return 0;
   const float s[4] = {1.0f, 1.0f, 0, 0};
   if (!quad_draw(&t, &x, "arrtex.frag.spv", s)) return 0;
   if (!g_fake) {
      static const struct { int x, y, l, m; } probe[] = {{64, 64, 1, 0}, {192, 64, 2, 1}, {64, 192, 1, 2}, {192, 192, 2, 3}};
      for (unsigned i = 0; i < 4; i++) {
         const uint8_t *p = pix(&t, probe[i].x, probe[i].y);
         CHECK(near(p, 40 + 60 * probe[i].l, 40 + 50 * probe[i].m, 100, 255, 6), "quadrant %u got %u,%u,%u,%u expected layer %d mip %d", i, p[0],
               p[1], p[2], p[3], probe[i].l, probe[i].m);
      }
      printf("{\"test\":\"arrtex\",\"ok\":true}\n");
   } else {
      printf("{\"test\":\"arrtex\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   }
   tex_destroy(&x);
   tgt_destroy(&t);
   return 1;
}

/* wideLines: horizontal line list, width 5 -> about 5 rows covered at x=128, and width 1 -> about 1 */
static int
line_rows(float w)
{
   struct tgt t;
   if (!tgt_create(&t)) return -1;
   g_topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
   g_lw = w;
   VkPipelineLayout lay;
   VkPipeline pipe;
   int ok = make_pipe(&t, "line.vert.spv", "solid.frag.spv", VK_NULL_HANDLE, 0, &lay, &pipe);
   g_topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   g_lw = 1.0f;
   if (!ok) return -1;
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   begin_pass(cb, &t, pipe);
   CmdDraw(cb, 2, 1, 0, 0);
   CmdEndRenderPass(cb);
   if (!cb_submit(&t, cb)) return -1;
   int rows = 0;
   if (!g_fake)
      for (int y = 0; y < H; y++) rows += pix(&t, 128, y)[3] != 0;
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   tgt_destroy(&t);
   return rows;
}

/* VK_EXT_line_rasterization: the minimum honest set is rectangularLines (the hardware's default wide line is a rectangle); the other modes and
 * every stipple bit stay false. Checks the feature bits, then draws width-5 lines with the DEFAULT and RECTANGULAR modes and expects the same ~5 rows as
 * test_wide. Skips while the extension is not advertised. */
static int
test_linerast(void)
{
   if (!g_lr_rect) { printf("{\"test\":\"linerast\",\"skipped\":\"rectangularLines not advertised\",\"ok\":true}\n"); return 1; }
   CHECK(!g_lr_bres && !g_lr_smooth && !g_lr_stip_rect && !g_lr_stip_bres && !g_lr_stip_smooth,
         "only rectangularLines may be set: bresenham %u smooth %u stippled %u/%u/%u", g_lr_bres, g_lr_smooth, g_lr_stip_rect, g_lr_stip_bres, g_lr_stip_smooth);
   CHECK(g_feat.wideLines, "rectangular wide lines need wideLines");
   static const VkLineRasterizationModeEXT modes[] = {VK_LINE_RASTERIZATION_MODE_DEFAULT_EXT, VK_LINE_RASTERIZATION_MODE_RECTANGULAR_EXT};
   int rows[2];
   for (unsigned m = 0; m < 2; m++) {
      VkPipelineRasterizationLineStateCreateInfoEXT ls = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT,
                                                          .lineRasterizationMode = modes[m]};
      g_rs_next = &ls;
      rows[m] = line_rows(5.0f);
      g_rs_next = NULL;
      if (rows[m] < 0) return 0;
      if (!g_fake) CHECK(rows[m] >= 4 && rows[m] <= 6, "line mode %d width 5 covers %d rows", modes[m], rows[m]);
   }
   if (!g_fake) {
      CHECK(rows[0] == rows[1], "DEFAULT and RECTANGULAR must rasterize alike (%d vs %d)", rows[0], rows[1]);
      printf("{\"test\":\"linerast\",\"rows_default\":%d,\"rows_rect\":%d,\"ok\":true}\n", rows[0], rows[1]);
   } else {
      printf("{\"test\":\"linerast\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   }
   return 1;
}

/* VK_EXT_transform_feedback is plumbing-only (mali_tf.h): the XFB decorations are lowered to global stores by the
 * compiler, and the pipeline is then refused because no streamout job exists to execute it. The test proves both the
 * code and that the message names the reason. */
static int
test_tf_reject(void)
{
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   VkPipelineLayout lay = VK_NULL_HANDLE;
   VkPipeline pipe = VK_NULL_HANDLE;
   /* stage 3: the XFB outputs are lowered to global stores, so the pipeline compiles (the stores' base address is
    * still 0 until the per-buffer address is wired, which is what the buffer-content test will check). */
   g_expect_xfb_fail = 0;
   int ok = make_pipe(&t, "xfb.vert.spv", "solid.frag.spv", VK_NULL_HANDLE, 0, &lay, &pipe);
   CHECK(pipe != VK_NULL_HANDLE, "an XFB pipeline must compile now that the lowering is in");
   if (ok) printf("{\"test\":\"tf_reject\",\"pipeline\":\"compiled\",\"ok\":true}\n");
   return ok;
}

/* Stage 2 of VK_EXT_transform_feedback: the commands must record and validate (bind buffers, begin, end), and the
 * XFB buffer must be marked GPU-written so the fence wait syncs it. The streamout job does not exist yet, so the
 * *draw* is still what cannot happen (mali_compile.c names the missing nir_load_xfb_address lowering); this case
 * proves the recording path and that the failure is the lowering, not a missing command. */
static int
test_tf_record(void)
{
   if (!CmdBindTransformFeedbackBuffersEXT) {
      printf("{\"test\":\"tf_record\",\"skipped\":\"VK_EXT_transform_feedback not advertised\",\"ok\":true}\n");
      return 1;
   }
   VkBuffer xfb = VK_NULL_HANDLE;
   VkDeviceSize sz = 4 * 4 * 4;   /* 4 vertices x vec4 */
   /* Memory-backed through the harness helper: vkCmdBindTransformFeedbackBuffersEXT requires every buffer to be
    * completely and contiguously bound (CodeRabbit on #313, t_vk_feat.c:1026). */
   struct hbuf hb = {0};
   if (!hbuf_create(&hb, sz, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
      return 0;
   xfb = hb.b;
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cb");
   CmdBindTransformFeedbackBuffersEXT(cb, 0, 1, &xfb, (VkDeviceSize [1]){0}, (VkDeviceSize [1]){sz});
   CmdBeginTransformFeedbackEXT(cb, 0, 0, NULL, NULL);
   CmdEndTransformFeedbackEXT(cb, 0, 0, NULL, NULL);
   VkResult r = EndCommandBuffer(cb);
   CHECK(r == VK_SUCCESS, "begin/end/bind must record cleanly (%d)", (int)r);
   /* the draw: still refused, and the message must name the lowering, not a missing entrypoint */
   VkPipelineLayout lay = VK_NULL_HANDLE;
   VkPipeline pipe = VK_NULL_HANDLE;
   int ok = make_pipe(&t, "xfb.vert.spv", "solid.frag.spv", VK_NULL_HANDLE, 0, &lay, &pipe);
   if (!ok) return 0;
   printf("{\"test\":\"tf_record\",\"record\":\"ok\",\"pipeline\":\"%s\",\"pending\":\"nir_load_xfb_address lowering + streamout job\",\"ok\":true}\n",
          pipe ? "created" : "refused (named)");
   DestroyBuffer(dev, xfb, NULL);
   return 1;
}

/* Stage 4 of VK_EXT_transform_feedback: the lowered stores must land in the buffers passed to
 * vkCmdBindTransformFeedbackBuffersEXT, at the bind offset plus the declared xfb_offset. Two buffers, the second
 * bound at byte 16 and written through a 4-byte xfb_offset inside an 8-byte stride: the 0xcc words that survive
 * prove the offsets and the stride are honoured, not that everything landed at address 0. */
static int
test_tf_write(void)
{
   if (!CmdBindTransformFeedbackBuffersEXT) {
      printf("{\"test\":\"tf_write\",\"skipped\":\"VK_EXT_transform_feedback not advertised\",\"ok\":true}\n");
      return 1;
   }
   enum { NV = 3 };
   static const float pos[NV][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}};
   struct hbuf vb, xb[2];
   /* Buffer 1 is 40 B and the bind starts at 16, so its capture size is 24, not 16 + NV*8 (VUID-02363). */
   const VkDeviceSize xsz[2] = {NV * 16, 24}, xoff[2] = {0, 16};
   if (!hbuf_create(&vb, sizeof pos, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return 0;
   memcpy(vb.p, pos, sizeof pos);
   for (int i = 0; i < 2; i++) {
      if (!hbuf_create(&xb[i], xsz[i], VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
         return 0;
      memset(xb[i].p, 0xcc, xsz[i]);   /* a word the XFB stores must not touch */
   }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   VkVertexInputBindingDescription vbd = {0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
   VkVertexInputAttributeDescription vad = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
   VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                              .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vbd,
                                              .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &vad};
   VkPipelineLayout lay = VK_NULL_HANDLE;
   VkPipeline pipe = VK_NULL_HANDLE;
   g_vi = &vi;
   int ok = make_pipe(&t, "xfb2.vert.spv", "solid.frag.spv", VK_NULL_HANDLE, 0, &lay, &pipe);
   g_vi = NULL;
   if (!ok) return 0;
   /* begin/end outside the render pass, like the spec requires */
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   CmdBindTransformFeedbackBuffersEXT(cb, 0, 2, (VkBuffer[2]){xb[0].b, xb[1].b}, xoff, xsz);
   CmdBeginTransformFeedbackEXT(cb, 0, 0, NULL, NULL);
   begin_pass(cb, &t, pipe);
   VkDeviceSize voff = 0;
   CmdBindVertexBuffers(cb, 0, 1, &vb.b, &voff);
   CmdDraw(cb, NV, 1, 0, 0);
   CmdEndRenderPass(cb);
   CmdEndTransformFeedbackEXT(cb, 0, 0, NULL, NULL);
   if (!cb_submit(&t, cb)) return 0;
   /* the buffers are GPU-written, so invalidate the host mapping before reading it */
   VkMappedMemoryRange rg = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .offset = 0, .size = VK_WHOLE_SIZE};
   for (int i = 0; i < 2; i++) {
      rg.memory = xb[i].m;
      CHECK(InvalidateMappedMemoryRanges(dev, 1, &rg) == VK_SUCCESS, "invalidate xfb %d", i);
   }
   if (!g_fake) {
      const float *a = (const float *)xb[0].p;
      const uint32_t *w1 = (const uint32_t *)xb[1].p;
      for (int i = 0; i < NV; i++) {
         for (int c = 0; c < 4; c++)
            CHECK(a[i * 4 + c] == pos[i][c], "xfb buffer 0 vertex %d comp %d: got %g want %g", i, c, a[i * 4 + c], pos[i][c]);
         /* buffer 1: bind offset 16 + xfb_offset 4, stride 8 -> the float sits at word 5 + 2i */
         CHECK(((const float *)w1)[5 + 2 * i] == pos[i][0] * 2.0f, "xfb buffer 1 vertex %d: got %g want %g", i,
               ((const float *)w1)[5 + 2 * i], pos[i][0] * 2.0f);
         CHECK(w1[4 + 2 * i] == 0xccccccccu, "xfb buffer 1 stride: word before vertex %d must be untouched", i);
      }
      for (unsigned w = 0; w < 4; w++)
         CHECK(w1[w] == 0xccccccccu, "xb buffer 1: the 16 bytes before the bind offset must be untouched (word %u)", w);
      printf("{\"test\":\"tf_write\",\"buffers\":2,\"offsets\":[0,16],\"vertices\":%d,\"ok\":true}\n", NV);
   } else {
      printf("{\"test\":\"tf_write\",\"skipped\":\"buffer contents: fake backend\",\"ok\":true}\n");
   }
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   tgt_destroy(&t);
   hbuf_destroy(&vb);
   for (int i = 0; i < 2; i++) hbuf_destroy(&xb[i]);
   return 1;
}

/* VK_EXT_transform_feedback is on by default and MALI_EXPERIMENTAL_TF=0 is the opt-out (mali_tf.h). This case pins the
 * advertised surface of both states: the extension, the feature bit and the properties must follow one switch (an
 * extension advertised without its feature, or the reverse, is what makes zink think it can capture).
 * Before the flip the default run had no extension at all, so the first CHECK failed. */
static int
test_tf_default(void)
{
   const char *g = getenv("MALI_EXPERIMENTAL_TF");
   const int want = !(g && g[0] == '0');
   uint32_t en = 0;
   int have = 0;
   CHECK(g_edep && g_edep(g_pd, NULL, &en, NULL) == VK_SUCCESS, "count device extensions");
   VkExtensionProperties *e = calloc(en ? en : 1, sizeof *e);
   if (e && g_edep(g_pd, NULL, &en, e) == VK_SUCCESS)
      for (uint32_t i = 0; i < en; i++)
         if (!strcmp(e[i].extensionName, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME)) have = 1;
   free(e);
   CHECK(have == want, "VK_EXT_transform_feedback %s by default (MALI_EXPERIMENTAL_TF=0 opts out), gate %d",
         have ? "advertised" : "absent", want);
   printf("{\"test\":\"tf_default\",\"extension\":\"%s\",\"default\":%s,\"ok\":true}\n", have ? "advertised" : "absent",
          want ? "on" : "off");
   return 1;
}

/* The driver names the vertex program it selected for a draw (MALI_TF_DBG): capture fd 2 around one case, so "the
 * inactive draw ran the non-XFB program" is an assertion here and not a line someone has to read. */
static int g_dbgsave = -1;
static char g_dbgfile[128];

static void
tf_dbg_begin(void)
{
   snprintf(g_dbgfile, sizeof g_dbgfile, "/tmp/mali-tf-dbg.%ld.log", (long)getpid());
   setenv("MALI_TF_DBG", "1", 1);
   fflush(stderr);
   g_dbgsave = dup(2);
   if (!freopen(g_dbgfile, "w", stderr))   /* glibc marks freopen warn_unused_result; CI builds with -Werror */
      fprintf(stderr, "tf debug: cannot open %s\n", g_dbgfile);
}

static int
tf_dbg_count(const char *needle, uint64_t *va)
{
   fflush(stderr);
   FILE *f = fopen(g_dbgfile, "r");
   if (!f) return 0;
   char line[256];
   int n = 0;
   while (fgets(line, sizeof line, f)) {
      if (!strstr(line, needle)) continue;
      const char *at = strstr(line, " at 0x");
      if (va && at) sscanf(at + 4, "%" SCNx64, va);
      n++;
   }
   fclose(f);
   return n;
}

static void
tf_dbg_end(void)
{
   fflush(stderr);
   if (g_dbgsave >= 0) {
      dup2(g_dbgsave, 2);
      close(g_dbgsave);
      g_dbgsave = -1;
   }
   unlink(g_dbgfile);
   unsetenv("MALI_TF_DBG");
}

/* The mandatory guard of the default-on flip: an XFB program drawn with NO BeginTransformFeedback and NOTHING bound
 * must not store anywhere. The lowering used to run unconditionally while the buffer bases were patched only while TF
 * was active, so such a draw stored to GPU VA 0. The driver now compiles a second, non-XFB vertex program and runs it
 * for every draw outside Begin/End; this case checks that choice (both directions, through MALI_TF_DBG), that the draw
 * still renders, and (on hardware) that the bound buffers stay untouched. */
static int
test_tf_inactive(void)
{
   if (!g_tf) {
      printf("{\"test\":\"tf_inactive\",\"skipped\":\"VK_EXT_transform_feedback not advertised\",\"ok\":true}\n");
      return 1;
   }
   enum { NV = 3 };
   static const float pos[NV][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}};
   struct hbuf vb, xb;
   const VkDeviceSize xsz = NV * 16;
   if (!hbuf_create(&vb, sizeof pos, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return 0;
   memcpy(vb.p, pos, sizeof pos);
   if (!hbuf_create(&xb, xsz, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return 0;
   memset(xb.p, 0xcc, xsz);
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   VkVertexInputBindingDescription vbd = {0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
   VkVertexInputAttributeDescription vad = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
   VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                              .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vbd,
                                              .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &vad};
   VkPipelineLayout lay = VK_NULL_HANDLE;
   VkPipeline pipe = VK_NULL_HANDLE;
   g_vi = &vi;
   int ok = make_pipe(&t, "xfb.vert.spv", "solid.frag.spv", VK_NULL_HANDLE, 0, &lay, &pipe);
   g_vi = NULL;
   if (!ok) return 0;
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   CmdBindVertexBuffers(cb, 0, 1, &vb.b, (VkDeviceSize [1]){0});
   /* draw 1: no bind, no Begin -> the non-XFB program, nothing may be stored */
   tf_dbg_begin();
   begin_pass(cb, &t, pipe);
   CmdDraw(cb, NV, 1, 0, 0);
   CmdEndRenderPass(cb);
   /* draw 2: inside Begin/End with the buffer bound -> the XFB program (the flip must not disable capture) */
   CmdBindTransformFeedbackBuffersEXT(cb, 0, 1, &xb.b, (VkDeviceSize [1]){0}, (VkDeviceSize [1]){xsz});
   CmdBeginTransformFeedbackEXT(cb, 0, 0, NULL, NULL);
   begin_pass(cb, &t, pipe);
   CmdDraw(cb, NV, 1, 0, 0);
   CmdEndRenderPass(cb);
   CmdEndTransformFeedbackEXT(cb, 0, 0, NULL, NULL);
   if (!cb_submit(&t, cb)) { tf_dbg_end(); return 0; }
   uint64_t plva = 0, xfva = 0;
   int plain = tf_dbg_count("tf: draw uses the plain vertex program at 0x", &plva);
   int xf = tf_dbg_count("tf: draw uses the XFB vertex program at 0x", &xfva);
   tf_dbg_end();
   CHECK(plain == 1, "the draw outside Begin/End must run the non-XFB vertex program (%d of the driver's lines)", plain);
   CHECK(xf == 1, "the draw inside Begin/End must run the XFB vertex program (%d)", xf);
   CHECK(plva && xfva && plva != xfva, "the two vertex programs must be different binaries (0x%" PRIx64 " vs 0x%" PRIx64 ")",
         plva, xfva);
   if (!g_fake) {
      VkMappedMemoryRange rg = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .offset = 0, .size = VK_WHOLE_SIZE, .memory = xb.m};
      CHECK(InvalidateMappedMemoryRanges(dev, 1, &rg) == VK_SUCCESS, "invalidate xfb");
      /* draw 2 captured the three vertices: slot i holds pos[i] */
      for (int i = 0; i < NV; i++)
         for (int c = 0; c < 4; c++)
            CHECK(((const float *)xb.p)[i * 4 + c] == pos[i][c], "xfb buffer vertex %d comp %d: got %g want %g", i, c,
                  ((const float *)xb.p)[i * 4 + c], pos[i][c]);
      const uint8_t *p = pix(&t, W / 2, H / 2);   /* the last pass painted the red triangle: the inactive draw rendered too */
      CHECK(p[0] == 255 && p[1] == 0 && p[2] == 0, "centre %u,%u,%u want 255,0,0 (the XFB draw still renders)", p[0], p[1], p[2]);
      printf("{\"test\":\"tf_inactive\",\"plain\":1,\"xfb\":1,\"ok\":true}\n");
   } else {
      printf("{\"test\":\"tf_inactive\",\"skipped\":\"buffer and pixel contents: fake backend\",\"ok\":true}\n");
   }
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   tgt_destroy(&t);
   hbuf_destroy(&vb);
   hbuf_destroy(&xb);
   return 1;
}

/* vkResetCommandBuffer must drop the recorded TF state (mali_cmd.c reset_state): the buffers are destroyed right after
 * the bind, so a draw that still believed the range was open would dereference a freed handle. Before the fix the stale
 * active flag survived the reset. */
static int
test_tf_reset(void)
{
   if (!g_tf) {
      printf("{\"test\":\"tf_reset\",\"skipped\":\"VK_EXT_transform_feedback not advertised\",\"ok\":true}\n");
      return 1;
   }
   struct hbuf vb;
   if (!hbuf_create(&vb, 3 * 16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return 0;
   for (int i = 0; i < 12; i++) ((float *)vb.p)[i] = (float)(i + 1);
   struct hbuf xb;
   if (!hbuf_create(&xb, 3 * 16, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return 0;
   memset(xb.p, 0xcc, 3 * 16);
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   VkVertexInputBindingDescription vbd = {0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
   VkVertexInputAttributeDescription vad = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
   VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                              .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vbd,
                                              .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &vad};
   VkPipelineLayout lay = VK_NULL_HANDLE;
   VkPipeline pipe = VK_NULL_HANDLE;
   g_vi = &vi;
   int ok = make_pipe(&t, "xfb.vert.spv", "solid.frag.spv", VK_NULL_HANDLE, 0, &lay, &pipe);
   g_vi = NULL;
   if (!ok) return 0;
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   CmdBindTransformFeedbackBuffersEXT(cb, 0, 1, &xb.b, (VkDeviceSize [1]){0}, (VkDeviceSize [1]){48});
   CmdBeginTransformFeedbackEXT(cb, 0, 0, NULL, NULL);   /* range left open: the reset has to forget it */
   CHECK(g_rcb && g_rcb(cb, 0) == VK_SUCCESS, "reset");
   CHECK(BeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}) == VK_SUCCESS, "re-begin");
   hbuf_destroy(&xb);   /* the handle a stale tf record would still point at */
   CmdBindVertexBuffers(cb, 0, 1, &vb.b, (VkDeviceSize [1]){0});
   tf_dbg_begin();
   begin_pass(cb, &t, pipe);
   CmdDraw(cb, 3, 1, 0, 0);
   CmdEndRenderPass(cb);
   if (!cb_submit(&t, cb)) { tf_dbg_end(); return 0; }
   int plain = tf_dbg_count("tf: draw uses the plain vertex program at 0x", NULL);
   tf_dbg_end();
   CHECK(plain == 1, "the draw after the reset must not see the old range (%d plain lines)", plain);
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   tgt_destroy(&t);
   hbuf_destroy(&vb);
   return 1;
}

/* Two captures in one Begin/End. KNOWN-LIMIT: the counter buffers are not implemented (mali_cmd.c only marks them
 * written), so the second draw restarts at the bound offset instead of appending: the last draw wins and the slots
 * after it are left alone. The case asserts that behaviour so a silent change cannot pass unnoticed. */
static int
test_tf_multi(void)
{
   if (!g_tf) {
      printf("{\"test\":\"tf_multi\",\"skipped\":\"VK_EXT_transform_feedback not advertised\",\"ok\":true}\n");
      return 1;
   }
   enum { NV = 3 };
   static const float a[NV][4] = {{1, 0, 0, 0}, {2, 0, 0, 0}, {3, 0, 0, 0}};
   static const float b[NV][4] = {{7, 0, 0, 0}, {8, 0, 0, 0}, {9, 0, 0, 0}};
   struct hbuf vb;
   if (!hbuf_create(&vb, sizeof a + sizeof b, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return 0;
   memcpy(vb.p, a, sizeof a);
   memcpy(vb.p + sizeof a, b, sizeof b);
   struct hbuf xb;
   const VkDeviceSize xsz = 2 * NV * 16;   /* room for the append that is not implemented, plus a guard word */
   if (!hbuf_create(&xb, xsz, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return 0;
   memset(xb.p, 0xcc, xsz);
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   VkVertexInputBindingDescription vbd = {0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
   VkVertexInputAttributeDescription vad = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
   VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                              .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vbd,
                                              .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &vad};
   VkPipelineLayout lay = VK_NULL_HANDLE;
   VkPipeline pipe = VK_NULL_HANDLE;
   g_vi = &vi;
   int ok = make_pipe(&t, "xfb.vert.spv", "solid.frag.spv", VK_NULL_HANDLE, 0, &lay, &pipe);
   g_vi = NULL;
   if (!ok) return 0;
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   CmdBindVertexBuffers(cb, 0, 1, &vb.b, (VkDeviceSize [1]){0});
   CmdBindTransformFeedbackBuffersEXT(cb, 0, 1, &xb.b, (VkDeviceSize [1]){0}, (VkDeviceSize [1]){xsz});
   CmdBeginTransformFeedbackEXT(cb, 0, 0, NULL, NULL);
   begin_pass(cb, &t, pipe);
   CmdDraw(cb, NV, 1, 0, 0);
   CmdBindVertexBuffers(cb, 0, 1, &vb.b, (VkDeviceSize [1]){sizeof a});
   CmdDraw(cb, NV, 1, 0, 0);
   CmdEndRenderPass(cb);
   CmdEndTransformFeedbackEXT(cb, 0, 0, NULL, NULL);
   if (!cb_submit(&t, cb)) return 0;
   if (!g_fake) {
      VkMappedMemoryRange rg = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .offset = 0, .size = VK_WHOLE_SIZE, .memory = xb.m};
      CHECK(InvalidateMappedMemoryRanges(dev, 1, &rg) == VK_SUCCESS, "invalidate xfb");
      int first_is_b = 1, rest_untouched = 1;
      for (int i = 0; i < NV; i++)
         first_is_b &= ((const float *)xb.p)[i * 4] == b[i][0];
      for (int i = NV; i < 2 * NV; i++)
         rest_untouched &= ((const uint32_t *)xb.p)[i * 4] == 0xccccccccu;
      CHECK(first_is_b, "KNOWN-LIMIT: without counter buffers the second draw restarts at the bind offset (it must overwrite)");
      CHECK(rest_untouched, "KNOWN-LIMIT: nothing is appended past the bind offset");
      printf("{\"test\":\"tf_multi\",\"append\":\"not implemented (counters ignored)\",\"known_limit\":true,\"ok\":true}\n");
   } else {
      printf("{\"test\":\"tf_multi\",\"skipped\":\"buffer contents: fake backend\",\"ok\":true}\n");
   }
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   tgt_destroy(&t);
   hbuf_destroy(&vb);
   hbuf_destroy(&xb);
   return 1;
}

/* Output larger than the bound range. KNOWN-LIMIT: the per-buffer sizes are recorded and never read (mali_cmd.c), so
 * the stores run past the declared size instead of stopping at it. The case asserts that documented gap: the buffer is
 * big enough that the overrun stays inside the allocation, so it is a correctness bug here, not a fault. */
static int
test_tf_overflow(void)
{
   if (!g_tf) {
      printf("{\"test\":\"tf_overflow\",\"skipped\":\"VK_EXT_transform_feedback not advertised\",\"ok\":true}\n");
      return 1;
   }
   enum { NV = 6, NIN = 12 };   /* 6 vertices' worth of room, only 16 bytes declared */
   struct hbuf vb;
   if (!hbuf_create(&vb, NV * 16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return 0;
   for (int i = 0; i < NV * 4; i++) ((float *)vb.p)[i] = (float)(i + 1);
   struct hbuf xb;
   const VkDeviceSize xsz = NV * 16;
   if (!hbuf_create(&xb, xsz, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return 0;
   memset(xb.p, 0xcc, xsz);
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   VkVertexInputBindingDescription vbd = {0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
   VkVertexInputAttributeDescription vad = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
   VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                              .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vbd,
                                              .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &vad};
   VkPipelineLayout lay = VK_NULL_HANDLE;
   VkPipeline pipe = VK_NULL_HANDLE;
   g_vi = &vi;
   int ok = make_pipe(&t, "xfb.vert.spv", "solid.frag.spv", VK_NULL_HANDLE, 0, &lay, &pipe);
   g_vi = NULL;
   if (!ok) return 0;
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   CmdBindVertexBuffers(cb, 0, 1, &vb.b, (VkDeviceSize [1]){0});
   /* the declared size is one vertex; the draw has NV */
   CmdBindTransformFeedbackBuffersEXT(cb, 0, 1, &xb.b, (VkDeviceSize [1]){0}, (VkDeviceSize [1]){16});
   CmdBeginTransformFeedbackEXT(cb, 0, 0, NULL, NULL);
   begin_pass(cb, &t, pipe);
   CmdDraw(cb, NV, 1, 0, 0);
   CmdEndRenderPass(cb);
   CmdEndTransformFeedbackEXT(cb, 0, 0, NULL, NULL);
   if (!cb_submit(&t, cb)) return 0;
   if (!g_fake) {
      CHECK(coverage(&t) > 0, "the overflowing draw must still complete and render");
      VkMappedMemoryRange rg = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .offset = 0, .size = VK_WHOLE_SIZE, .memory = xb.m};
      CHECK(InvalidateMappedMemoryRanges(dev, 1, &rg) == VK_SUCCESS, "invalidate xfb");
      int past = 0;
      for (int i = NIN; i < NV * 4; i++)
         past += ((const uint32_t *)xb.p)[i] != 0xccccccccu;
      CHECK(past > 0, "KNOWN-LIMIT: writes are not clamped to the bound size (no word past 16 B written)");
      printf("{\"test\":\"tf_overflow\",\"declared\":16,\"written_past\":%d,\"ok\":true}\n", past);
   } else {
      printf("{\"test\":\"tf_overflow\",\"skipped\":\"buffer contents: fake backend\",\"ok\":true}\n");
   }
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   tgt_destroy(&t);
   hbuf_destroy(&vb);
   hbuf_destroy(&xb);
   return 1;
}

static int
test_wide(void)
{
   if (!g_feat.wideLines) { printf("{\"test\":\"wide\",\"skipped\":\"wideLines not advertised\",\"ok\":true}\n"); return 1; }
   int r1 = line_rows(1.0f), r5 = line_rows(5.0f);
   if (r1 < 0 || r5 < 0) return 0;
   if (!g_fake) {
      CHECK(r1 >= 1 && r1 <= 2, "width 1 covers %d rows", r1);
      CHECK(r5 >= 4 && r5 <= 6, "width 5 covers %d rows", r5);
      printf("{\"test\":\"wide\",\"rows1\":%d,\"rows5\":%d,\"ok\":true}\n", r1, r5);
   } else {
      printf("{\"test\":\"wide\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   }
   return 1;
}

/* shaderStorageImageExtendedFormats: write a known texel through imageStore into every spec-listed extended format, copy out, compare bytes. */
struct sfmt { const char *q; VkFormat f; int n, bits; char kind; };  /* kind: U unorm, S snorm, u uint, i sint, F float; special: n=0 packed */
static const struct sfmt sfmts[] = {
   {"r8", 9, 1, 8, 'U'}, {"r8_snorm", 10, 1, 8, 'S'}, {"r8ui", 13, 1, 8, 'u'}, {"r8i", 14, 1, 8, 'i'},
   {"rg8", 16, 2, 8, 'U'}, {"rg8_snorm", 17, 2, 8, 'S'}, {"rg8ui", 20, 2, 8, 'u'}, {"rg8i", 21, 2, 8, 'i'},
   {"rgb10_a2", 64, 0, 0, 'U'}, {"rgb10_a2ui", 68, 0, 0, 'u'},
   {"r16", 70, 1, 16, 'U'}, {"r16_snorm", 71, 1, 16, 'S'}, {"r16ui", 74, 1, 16, 'u'}, {"r16i", 75, 1, 16, 'i'}, {"r16f", 76, 1, 16, 'F'},
   {"rg16", 77, 2, 16, 'U'}, {"rg16_snorm", 78, 2, 16, 'S'}, {"rg16ui", 81, 2, 16, 'u'}, {"rg16i", 82, 2, 16, 'i'}, {"rg16f", 83, 2, 16, 'F'},
   {"rgba16", 91, 4, 16, 'U'}, {"rgba16_snorm", 92, 4, 16, 'S'}, {"rgba16ui", 95, 4, 16, 'u'}, {"rgba16i", 96, 4, 16, 'i'}, {"rgba16f", 97, 4, 16, 'F'},
   {"rg32ui", 101, 2, 32, 'u'}, {"rg32i", 102, 2, 32, 'i'}, {"rg32f", 103, 2, 32, 'F'},
   {"r11f_g11f_b10f", 122, 0, 0, 'F'},
};

/* expected little-endian texel into out[], returns size */
static int
sfmt_expect(const struct sfmt *f, uint8_t *out)
{
   if (!f->n) {
      uint32_t v = !strcmp(f->q, "rgb10_a2") ? 0xFFF003FFu : !strcmp(f->q, "rgb10_a2ui") ? (1u | 2u << 10 | 3u << 20 | 1u << 30) : 0x780003C0u;
      memcpy(out, &v, 4);
      return 4;
   }
   int bytes = f->bits / 8;
   for (int c = 0; c < f->n; c++) {
      uint64_t v;
      switch (f->kind) {
      case 'U': v = (c == 1) ? 0 : (f->bits == 32 ? 0xffffffffu : (1ull << f->bits) - 1); break;
      case 'S': v = (c == 1) ? 0 : (1ull << (f->bits - 1)) - 1; break;
      case 'u': v = (uint64_t[]){1, 2, 3, 1}[c]; break;
      case 'i': v = ((uint64_t[]){(uint64_t)-1, 2, (uint64_t)-3, 1}[c]) & (f->bits == 32 ? 0xffffffffu : (1ull << f->bits) - 1); break;
      default: v = (c == 1) ? 0 : (f->bits == 16 ? 0x3C00 : 0x3F800000u); break;
      }
      memcpy(out + c * bytes, &v, bytes);   /* little endian host */
   }
   return f->n * bytes;
}

static int
storfmt_one(struct tgt *t, const struct sfmt *f, int nf)
{
   VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f->f, .extent = {4, 4, 1},
                           .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                           .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   VkImage img;
   VkDeviceMemory mem;
   VkMemoryRequirements mr;
   if (CreateImage(dev, &ii, NULL, &img) != VK_SUCCESS) { printf("{\"storfmt\":\"%s\",\"fail\":\"CreateImage\"}\n", f->q); return 0; }
   GetImageMemoryRequirements(dev, img, &mr);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = memtype(mr.memoryTypeBits, 0)};
   CHECK(AllocateMemory(dev, &ai, NULL, &mem) == VK_SUCCESS, "mem");
   CHECK(BindImageMemory(dev, img, mem, 0) == VK_SUCCESS, "bind");
   VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f->f,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   VkImageView view;
   CHECK(CreateImageView(dev, &vi, NULL, &view) == VK_SUCCESS, "view");
   VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
   VkDescriptorSetLayout dsl;
   CHECK(CreateDescriptorSetLayout(dev, &dli, NULL, &dsl) == VK_SUCCESS, "dsl");
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
   VkDescriptorPool dp;
   CHECK(CreateDescriptorPool(dev, &dpi, NULL, &dp) == VK_SUCCESS, "dp");
   VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl};
   VkDescriptorSet ds;
   CHECK(AllocateDescriptorSets(dev, &dai, &ds) == VK_SUCCESS, "ds");
   VkDescriptorImageInfo dii = {.imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
   VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .descriptorCount = 1,
                             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &dii};
   UpdateDescriptorSets(dev, 1, &w, 0, NULL);
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl};
   VkPipelineLayout lay;
   CHECK(CreatePipelineLayout(dev, &pli, NULL, &lay) == VK_SUCCESS, "layout");
   char name[64];
   if (nf)   /* no format qualifier in the shader: one shader per component type */
      snprintf(name, sizeof name, "stornf_%c.comp.spv", f->kind == 'u' ? 'u' : f->kind == 'i' ? 'i' : 'f');
   else
      snprintf(name, sizeof name, "stor_%s.comp.spv", f->q);
   VkShaderModule m = load_spv(name);
   CHECK(m, "shader %s", name);
   VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rss = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = 16};
   VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = lay,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = g_sgctl ? &rss : NULL,
                .flags = g_sgctl ? VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT : 0, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}};
   VkPipeline pipe;
   VkResult r = CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe);
   DestroyShaderModule(dev, m, NULL);
   if (r != VK_SUCCESS) { printf("{\"storfmt\":\"%s\",\"fail\":\"pipeline %d\"}\n", f->q, r); g_fail++; return 0; }
   struct hbuf ob;
   if (!hbuf_create(&ob, 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return 0;
   memset(ob.p, 0xA5, 4096);
   VkCommandBuffer cb = cb_begin(t);
   CHECK(cb, "cmdbuf");
   VkImageMemoryBarrier ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, lay, 0, 1, &ds, 0, NULL);
   CmdDispatch(cb, 1, 1, 1);
   ib.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
   ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   ib.oldLayout = ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
   VkBufferImageCopy reg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {4, 4, 1}};
   CmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, ob.b, 1, &reg);
   if (!cb_submit(t, cb)) return 0;
   int ok = 1;
   if (!g_fake) {
      uint8_t exp[16];
      int tb = sfmt_expect(f, exp);
      for (int i = 0; i < 16 && ok; i++)
         if (memcmp(ob.p + i * tb, exp, tb)) {
            ok = 0;
            printf("{\"storfmt\":\"%s\",\"fail\":\"texel %d\",\"got\":\"", f->q, i);
            for (int k = 0; k < tb; k++) printf("%02x", ob.p[i * tb + k]);
            printf("\",\"want\":\"");
            for (int k = 0; k < tb; k++) printf("%02x", exp[k]);
            printf("\"}\n");
         }
   }
   if (!ok) g_fail++;
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   DestroyDescriptorPool(dev, dp, NULL);
   DestroyDescriptorSetLayout(dev, dsl, NULL);
   DestroyImageView(dev, view, NULL);
   DestroyImage(dev, img, NULL);
   FreeMemory(dev, mem, NULL);
   hbuf_destroy(&ob);
   return ok;
}

static int
test_storfmt(void)
{
   if (!g_feat.shaderStorageImageExtendedFormats) { printf("{\"test\":\"storfmt\",\"skipped\":\"shaderStorageImageExtendedFormats not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   int pass = 0, n = (int)(sizeof sfmts / sizeof *sfmts);
   for (int i = 0; i < n; i++) pass += storfmt_one(&t, &sfmts[i], 0);
   printf("{\"test\":\"storfmt\",\"formats\":%d,\"passed\":%d,\"ok\":%s}\n", n, g_fake ? n : pass, pass == n ? "true" : "false");
   tgt_destroy(&t);
   return pass == n;
}

/* shaderStorageImageWriteWithoutFormat (gamescope's composite shaders): the same texel through an imageStore with no format qualifier
 * (SPIR-V Unknown + StorageImageWriteWithoutFormat), into the extended formats above and the core storage formats. The texel format
 * can only come from the image view descriptor. */
static const struct sfmt sfmts_core[] = {
   {"rgba8", 37, 4, 8, 'U'}, {"rgba8_snorm", 38, 4, 8, 'S'}, {"rgba8ui", 41, 4, 8, 'u'}, {"rgba8i", 42, 4, 8, 'i'},
   {"r32ui", 98, 1, 32, 'u'}, {"r32i", 99, 1, 32, 'i'}, {"r32f", 100, 1, 32, 'F'},
   {"rgba32ui", 107, 4, 32, 'u'}, {"rgba32i", 108, 4, 32, 'i'}, {"rgba32f", 109, 4, 32, 'F'},
};

static int
test_storfmt_nf(void)
{
   if (!g_feat.shaderStorageImageWriteWithoutFormat) { printf("{\"test\":\"storfmt_nf\",\"skipped\":\"shaderStorageImageWriteWithoutFormat not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   int pass = 0, n = (int)(sizeof sfmts / sizeof *sfmts + sizeof sfmts_core / sizeof *sfmts_core);
   for (unsigned i = 0; i < sizeof sfmts / sizeof *sfmts; i++) pass += storfmt_one(&t, &sfmts[i], 1);
   for (unsigned i = 0; i < sizeof sfmts_core / sizeof *sfmts_core; i++) pass += storfmt_one(&t, &sfmts_core[i], 1);
   printf("{\"test\":\"storfmt_nf\",\"formats\":%d,\"passed\":%d,\"ok\":%s}\n", n, pass, pass == n ? "true" : "false");
   tgt_destroy(&t);
   return pass == n;
}

/* samplerYcbcrConversion: NV12 (G8_B8R8_2PLANE_420) sampled through an immutable ycbcr sampler; the driver samples luma (R8) and chroma (R8G8)
 * as two textures and converts in the shader. Chroma is constant so the (linear, siting-shifted) chroma reconstruction does not change the result;
 * luma is a gradient. Three conversions: 709 narrow, 601 full, 709 full with cosited-even siting. Pixels are checked on hardware only. */
static PFN_vkGetPhysicalDeviceImageFormatProperties2 g_ifp2;
static PFN_vkGetPhysicalDeviceFormatProperties2 g_fp2;

static void
ycc_ref(int model709, int full, int y, int cb, int cr, int out[3])
{
   double Y, Cb, Cr;
   if (full) { Y = y / 255.0; Cb = (cb - 128) / 255.0; Cr = (cr - 128) / 255.0; }
   else { Y = (y - 16) / 219.0; Cb = (cb - 128) / 224.0; Cr = (cr - 128) / 224.0; }
   double r, g, b;
   if (model709) { r = Y + 1.5748 * Cr; g = Y - 0.1873 * Cb - 0.4681 * Cr; b = Y + 1.8556 * Cb; }
   else { r = Y + 1.402 * Cr; g = Y - 0.344136 * Cb - 0.714136 * Cr; b = Y + 1.772 * Cb; }
   double v[3] = {r, g, b};
   for (int i = 0; i < 3; i++) out[i] = (int)((v[i] < 0 ? 0 : v[i] > 1 ? 1 : v[i]) * 255.0 + 0.5);
}

static int
test_ycbcr(void)
{
   if (!g_ycbcr) { printf("{\"test\":\"ycbcr\",\"skipped\":\"samplerYcbcrConversion not advertised\",\"ok\":true}\n"); return 1; }
   const VkFormat nv12 = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
   /* capabilities gamescope reads */
   VkFormatProperties2 fp = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
   g_fp2(g_pd, nv12, &fp);
   const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT |
                                     VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT;
   CHECK((fp.formatProperties.optimalTilingFeatures & need) == need, "NV12 optimal features %x", fp.formatProperties.optimalTilingFeatures);
   VkSamplerYcbcrConversionImageFormatProperties yp = {.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES};
   VkPhysicalDeviceImageFormatInfo2 pi = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .format = nv12, .type = VK_IMAGE_TYPE_2D,
                                          .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
   VkImageFormatProperties2 ip = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &yp};
   CHECK(g_ifp2(g_pd, &pi, &ip) == VK_SUCCESS, "NV12 sampled image properties");
   const uint32_t ndesc = yp.combinedImageSamplerDescriptorCount;
   CHECK(ndesc >= 2 && ndesc <= 3, "combinedImageSamplerDescriptorCount %u", ndesc);
   pi.format = VK_FORMAT_R8G8B8A8_UNORM;
   yp.combinedImageSamplerDescriptorCount = 0;
   CHECK(g_ifp2(g_pd, &pi, &ip) == VK_SUCCESS && yp.combinedImageSamplerDescriptorCount == 1, "plain format count %u", yp.combinedImageSamplerDescriptorCount);
   pi.format = nv12;
   pi.usage = VK_IMAGE_USAGE_STORAGE_BIT;
   CHECK(g_ifp2(g_pd, &pi, &ip) != VK_SUCCESS, "NV12 storage must be refused (not advertised)");

   struct tgt t;
   if (!tgt_create(&t)) return 0;
   enum { NW = 16, NH = 16 };
   uint8_t *data = malloc(NW * NH + NW * NH / 2);
   const int cb = 90, cr = 170;
   for (int y = 0; y < NH; y++)
      for (int x = 0; x < NW; x++) data[y * NW + x] = (uint8_t)(24 + 10 * x + 4 * y);
   for (int i = 0; i < NW * NH / 4; i++) { data[NW * NH + 2 * i] = cb; data[NW * NH + 2 * i + 1] = cr; }
   static const struct { int m709, full; VkChromaLocation loc; } cfg[3] = {
      {1, 0, VK_CHROMA_LOCATION_MIDPOINT}, {0, 1, VK_CHROMA_LOCATION_MIDPOINT}, {1, 1, VK_CHROMA_LOCATION_COSITED_EVEN}};
   int ok = 1;
   for (int c = 0; c < 3 && ok; c++) {
      VkSamplerYcbcrConversionCreateInfo yci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO, .format = nv12,
         .ycbcrModel = cfg[c].m709 ? VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709 : VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601,
         .ycbcrRange = cfg[c].full ? VK_SAMPLER_YCBCR_RANGE_ITU_FULL : VK_SAMPLER_YCBCR_RANGE_ITU_NARROW,
         .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
         .xChromaOffset = cfg[c].loc, .yChromaOffset = cfg[c].loc, .chromaFilter = VK_FILTER_LINEAR, .forceExplicitReconstruction = VK_FALSE};
      VkSamplerYcbcrConversion conv;
      CHECK(CreateSamplerYcbcrConversion(dev, &yci, NULL, &conv) == VK_SUCCESS, "CreateSamplerYcbcrConversion");
      VkSamplerYcbcrConversionInfo sci_y = {.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO, .conversion = conv};
      VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .pNext = &sci_y, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
                                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
      VkSampler smp;
      CHECK(CreateSampler(dev, &si, NULL, &smp) == VK_SUCCESS, "sampler with conversion");
      VkImageCreateInfo ici = img2d(nv12, NW, NH, 1, 1, 0);
      VkImage img;
      CHECK(CreateImage(dev, &ici, NULL, &img) == VK_SUCCESS, "NV12 image");
      VkMemoryRequirements mr;
      GetImageMemoryRequirements(dev, img, &mr);
      VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = memtype(mr.memoryTypeBits, 0)};
      VkDeviceMemory mem;
      CHECK(AllocateMemory(dev, &ai, NULL, &mem) == VK_SUCCESS, "NV12 memory");
      CHECK(BindImageMemory(dev, img, mem, 0) == VK_SUCCESS, "bind NV12");
      struct hbuf st;
      if (!hbuf_create(&st, NW * NH + NW * NH / 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return 0;
      memcpy(st.p, data, NW * NH + NW * NH / 2);
      VkCommandBuffer cb1 = cb_begin(&t);
      CHECK(cb1, "cmdbuf");
      VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 1, 0, 1};
      VkImageMemoryBarrier b1 = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                 .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = rng};
      CmdPipelineBarrier(cb1, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
      VkBufferImageCopy reg[2] = {
         {.bufferOffset = 0, .imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1}, .imageExtent = {NW, NH, 1}},
         {.bufferOffset = NW * NH, .imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1}, .imageExtent = {NW / 2, NH / 2, 1}}};
      CmdCopyBufferToImage(cb1, st.b, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, reg);
      VkImageMemoryBarrier b2 = b1;
      b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      CmdPipelineBarrier(cb1, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
      if (!cb_submit(&t, cb1)) return 0;
      hbuf_destroy(&st);
      VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &sci_y, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                  .format = nv12, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      VkImageView view;
      CHECK(CreateImageView(dev, &vi, NULL, &view) == VK_SUCCESS, "NV12 view");
      VkDescriptorSetLayoutBinding lb = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
                                         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .pImmutableSamplers = &smp};
      VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &lb};
      struct tex x = {0};
      CHECK(CreateDescriptorSetLayout(dev, &dli, NULL, &x.dsl) == VK_SUCCESS, "dsl with immutable ycbcr sampler");
      VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, ndesc};
      VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
      CHECK(CreateDescriptorPool(dev, &dpi, NULL, &x.pool) == VK_SUCCESS, "dpool");
      VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = x.pool, .descriptorSetCount = 1, .pSetLayouts = &x.dsl};
      CHECK(AllocateDescriptorSets(dev, &dai, &x.set) == VK_SUCCESS, "alloc set");
      VkDescriptorImageInfo ii = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = x.set, .dstBinding = 0, .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
      UpdateDescriptorSets(dev, 1, &w, 0, NULL);
      const float s[4] = {1.0f, 1.0f, 0, 0};
      ok = quad_draw(&t, &x, "ycbcr.frag.spv", s);
      int bad = 0;
      if (ok && !g_fake) {
         for (int ty = 1; ty < NH; ty += 5)
            for (int tx = 1; tx < NW; tx += 5) {
               int e[3];
               ycc_ref(cfg[c].m709, cfg[c].full, data[ty * NW + tx], cb, cr, e);
               const uint8_t *p = pix(&t, tx * 16 + 8, ty * 16 + 8);
               if (!near(p, e[0], e[1], e[2], 255, 6)) {
                  printf("{\"ycbcr_mismatch\":{\"cfg\":%d,\"texel\":[%d,%d],\"got\":[%u,%u,%u],\"want\":[%d,%d,%d]}}\n", c, tx, ty, p[0], p[1], p[2], e[0], e[1], e[2]);
                  bad++;
               }
            }
      }
      tex_destroy(&x);
      DestroyImageView(dev, view, NULL);
      DestroySampler(dev, smp, NULL);
      DestroySamplerYcbcrConversion(dev, conv, NULL);
      DestroyImage(dev, img, NULL);
      FreeMemory(dev, mem, NULL);
      if (bad) ok = 0;
   }
   free(data);
   if (ok) printf(g_fake ? "{\"test\":\"ycbcr\",\"skipped\":\"pixels: fake backend (calls and compile ran)\",\"ok\":true}\n" : "{\"test\":\"ycbcr\",\"configs\":3,\"ok\":true}\n");
   else printf("{\"test\":\"ycbcr\",\"ok\":false}\n");
   tgt_destroy(&t);
   return ok;
}

/* Specialization constants: a compute shader (uint, float, bool, local_size_x_id) and a fragment shader (float, uint, bool; ids 0/3/5)
 * built with the SPIR-V defaults and then with VkSpecializationInfo overrides (data order differs from the id order; one constant
 * is left at its default). */
static int
test_spec(void)
{
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   int ok = 1;
   /* ---- fragment ---- */
   struct { uint32_t mode; float r; uint32_t blue; } fd = {2, 0.75f, 1};
   VkSpecializationMapEntry fe[3] = {{0, offsetof(__typeof__(fd), r), 4}, {3, offsetof(__typeof__(fd), mode), 4}, {5, offsetof(__typeof__(fd), blue), 4}};
   VkSpecializationInfo fsi = {3, fe, sizeof fd, &fd};
   for (int pass = 0; pass < 2; pass++) {
      g_spec_fs = pass ? &fsi : NULL;
      VkPipelineLayout lay;
      VkPipeline pipe;
      if (!make_pipe(&t, "quad.vert.spv", "spec.frag.spv", VK_NULL_HANDLE, 16, &lay, &pipe)) { g_spec_fs = NULL; return 0; }
      g_spec_fs = NULL;
      VkCommandBuffer cb = cb_begin(&t);
      CHECK(cb, "cmdbuf");
      begin_pass(cb, &t, pipe);
      const float s[4] = {1.0f, 1.0f, 0, 0};
      CmdPushConstants(cb, lay, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, s);
      CmdDraw(cb, 6, 1, 0, 0);
      CmdEndRenderPass(cb);
      if (!cb_submit(&t, cb)) return 0;
      if (!g_fake) {
         const uint8_t *p = pix(&t, W / 2, H / 2);
         int good = pass ? near(p, 191, 255, 255, 255, 3) : near(p, 64, 0, 0, 255, 3);
         if (!good) { printf("{\"spec_frag_mismatch\":{\"overridden\":%d,\"got\":[%u,%u,%u,%u]}}\n", pass, p[0], p[1], p[2], p[3]); ok = 0; }
      }
      DestroyPipeline(dev, pipe, NULL);
      DestroyPipelineLayout(dev, lay, NULL);
   }
   /* ---- compute ---- */
   VkDescriptorSetLayoutBinding lb = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &lb};
   VkDescriptorSetLayout dsl;
   CHECK(CreateDescriptorSetLayout(dev, &dli, NULL, &dsl) == VK_SUCCESS, "dsl");
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
   VkDescriptorPool pool;
   CHECK(CreateDescriptorPool(dev, &dpi, NULL, &pool) == VK_SUCCESS, "pool");
   VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &dsl};
   VkDescriptorSet set;
   CHECK(AllocateDescriptorSets(dev, &dai, &set) == VK_SUCCESS, "set");
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl};
   VkPipelineLayout clay;
   CHECK(CreatePipelineLayout(dev, &pli, NULL, &clay) == VK_SUCCESS, "layout");
   struct { float f; uint32_t wg; uint32_t b; uint32_t a; } cd = {3.5f, 4, 1, 9};
   VkSpecializationMapEntry ce[4] = {{1, offsetof(__typeof__(cd), a), 4}, {2, offsetof(__typeof__(cd), f), 4}, {4, offsetof(__typeof__(cd), wg), 4}, {7, offsetof(__typeof__(cd), b), 4}};
   VkSpecializationInfo csi = {4, ce, sizeof cd, &cd};
   for (int pass = 0; pass < 2; pass++) {
      struct hbuf ob;
      if (!hbuf_create(&ob, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return 0;
      memset(ob.p, 0xEE, 256);
      VkDescriptorBufferInfo bi = {ob.b, 0, 256};
      VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi};
      UpdateDescriptorSets(dev, 1, &w, 0, NULL);
      VkShaderModule m = load_spv("spec.comp.spv");
      CHECK(m, "shader");
      VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = clay,
         .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main",
                   .pSpecializationInfo = pass ? &csi : NULL}};
      VkPipeline pipe;
      VkResult r = CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe);
      DestroyShaderModule(dev, m, NULL);
      CHECK(r == VK_SUCCESS, "compute pipeline = %d", r);
      VkCommandBuffer cb = cb_begin(&t);
      CHECK(cb, "cmdbuf");
      CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
      CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, clay, 0, 1, &set, 0, NULL);
      CmdDispatch(cb, 1, 1, 1);
      if (!cb_submit(&t, cb)) return 0;
      if (!g_fake) {
         const uint32_t *o = (const uint32_t *)ob.p;
         uint32_t wa = pass ? 9 : 7, wb = pass ? 1 : 0, wwg = pass ? 4 : 1;
         float fw = pass ? 3.5f : 2.5f;
         uint32_t fwb;
         memcpy(&fwb, &fw, 4);
         int good = o[0] == wa && o[1] == fwb && o[2] == wb && o[3] == 0xEEEEEEEEu && o[4 + wwg - 1] == 100 + wwg && o[4] == 100 + wwg &&
                    o[4 + wwg] == 0xEEEEEEEEu;
         if (!good) { printf("{\"spec_comp_mismatch\":{\"overridden\":%d,\"o\":[%u,0x%x,%u,0x%x,%u,%u,%u,%u,0x%x]}}\n", pass, o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7], o[8]); ok = 0; }
      }
      DestroyPipeline(dev, pipe, NULL);
      hbuf_destroy(&ob);
   }
   DestroyPipelineLayout(dev, clay, NULL);
   DestroyDescriptorPool(dev, pool, NULL);
   DestroyDescriptorSetLayout(dev, dsl, NULL);
   printf("{\"test\":\"spec\",\"ok\":%s%s}\n", ok ? "true" : "false", g_fake ? ",\"skipped\":\"pixels: fake backend (calls and compile ran)\"" : "");
   tgt_destroy(&t);
   return ok;
}

/* VK_KHR_maintenance5: the feature bit, vkCmdBindIndexBuffer2KHR (indexed draws, test idx32) and module-less stages (graphics + compute through
 * the run_comp tests). Same shaders and the same expected pixels as the plain-module runs, so any difference is the new path. */
static int
test_maint5(void)
{
   CHECK(g_m5, "maintenance5 feature bit not advertised");
   CHECK(g_cbib2, "vkCmdBindIndexBuffer2KHR missing");
   g_modless = 1; g_ib2 = 1;
   int ok = test_idx32();       /* graphics: module-less VS+FS, BindIndexBuffer2 */
   g_ib2 = 0;
   ok &= test_zinit();          /* compute: module-less CS */
   g_modless = 0;
   printf("{\"test\":\"maint5\",\"ok\":%s%s}\n", ok ? "true" : "false", g_fake ? ",\"skipped\":\"pixels: fake backend (calls and compile ran)\"" : "");
   return ok;
}

/* bufferDeviceAddress: push a 64-bit pointer (buffer address + 256) to a compute shader that writes through it. */
static int
test_bda(void)
{
   if (!g_bda) { printf("{\"test\":\"bda\",\"skipped\":\"bufferDeviceAddress not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096,
                            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
   VkBuffer buf;
   CHECK(CreateBuffer(dev, &bi, NULL, &buf) == VK_SUCCESS, "buffer");
   VkMemoryRequirements mr;
   GetBufferMemoryRequirements(dev, buf, &mr);
   VkMemoryAllocateFlagsInfo fi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &fi, .allocationSize = mr.size,
                              .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
   VkDeviceMemory mem;
   CHECK(AllocateMemory(dev, &ai, NULL, &mem) == VK_SUCCESS, "alloc");
   CHECK(BindBufferMemory(dev, buf, mem, 0) == VK_SUCCESS, "bind");
   uint32_t *p;
   CHECK(MapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&p) == VK_SUCCESS, "map");
   memset(p, 0xEE, 4096);
   VkBufferDeviceAddressInfo da = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buf};
   uint64_t addr = GetBufferDeviceAddress(dev, &da);
   CHECK(addr && !(addr & 3), "address 0x%llx", (unsigned long long)addr);
   CHECK(GetBufferDeviceAddress(dev, &da) == addr, "address stable");

   struct { uint64_t addr; uint32_t base; uint32_t pad; } pc = {addr + 256, 1000, 0};
   VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
   VkPipelineLayout lay;
   CHECK(CreatePipelineLayout(dev, &pli, NULL, &lay) == VK_SUCCESS, "layout");
   VkShaderModule m = load_spv("bda.comp.spv");
   CHECK(m, "shader");
   VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rss = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = 16};
   VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = lay,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = g_sgctl ? &rss : NULL,
                .flags = g_sgctl ? VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT : 0, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}};
   VkPipeline pipe;
   VkResult r = CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe);
   DestroyShaderModule(dev, m, NULL);
   CHECK(r == VK_SUCCESS, "compute pipeline = %d", r);
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   CmdPushConstants(cb, lay, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, &pc);
   CmdDispatch(cb, 2, 1, 1);
   if (!cb_submit(&t, cb)) return 0;
   if (!g_fake) {
      for (int i = 0; i < 128; i++)
         CHECK(p[64 + i] == 1000u + 3u * i, "elem %d = %u expected %u", i, p[64 + i], 1000u + 3u * i);
      CHECK(p[63] == 0xEEEEEEEEu && p[192] == 0xEEEEEEEEu, "stray write: p[63]=%x p[192]=%x", p[63], p[192]);
      printf("{\"test\":\"bda\",\"addr\":\"0x%llx\",\"ok\":true}\n", (unsigned long long)addr);
   } else {
      printf("{\"test\":\"bda\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   }
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   DestroyBuffer(dev, buf, NULL);
   FreeMemory(dev, mem, NULL);
   tgt_destroy(&t);
   return 1;
}

/* descriptor indexing (sampled images): a PARTIALLY_BOUND | UPDATE_AFTER_BIND | UPDATE_UNUSED_WHILE_PENDING array of 8 sampled
 * images with only 0..3 (and 5) bound, indexed non-uniformly (per pixel of a 2x2 quad) through a runtime array in the shader.
 * Slot 2 is rewritten AFTER the descriptor set was bound in the command buffer and before submit. */
static int
test_didx(void)
{
   if (!g_didx) { printf("{\"test\":\"didx\",\"skipped\":\"descriptor indexing not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   static const uint8_t col[6][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}, {0, 255, 255, 255}, {255, 0, 255, 255}};
   struct tex x[6];
   VkImageCreateInfo ici = img2d(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, 0);
   VkBufferImageCopy reg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {1, 1, 1}};
   VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                             .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                             .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
   for (int i = 0; i < 6; i++)
      if (!tex_create(&t, &x[i], &ici, VK_IMAGE_VIEW_TYPE_2D, 1, col[i], 4, &reg, 1, &si)) return 0;

   VkDescriptorBindingFlags bf[2] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                        VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
                                     VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT};
   VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                                                      .bindingCount = 2, .pBindingFlags = bf};
   VkDescriptorSetLayoutBinding lb[2] = {{.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 8, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
                                         {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT}};
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = &bfi,
                                          .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, .bindingCount = 2, .pBindings = lb};
   VkDescriptorSetLayout dsl;
   CHECK(CreateDescriptorSetLayout(dev, &dli, NULL, &dsl) == VK_SUCCESS, "dsl");
   VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
                                     .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = ps};
   VkDescriptorPool dp;
   CHECK(CreateDescriptorPool(dev, &dpi, NULL, &dp) == VK_SUCCESS, "dpool");
   VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl};
   VkDescriptorSet set;
   CHECK(AllocateDescriptorSets(dev, &dai, &set) == VK_SUCCESS, "alloc set");
   VkDescriptorImageInfo ii[5];
   for (int i = 0; i < 4; i++) ii[i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, x[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
   ii[4] = (VkDescriptorImageInfo){VK_NULL_HANDLE, x[5].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};   /* slot 5 */
   VkDescriptorImageInfo smpi = {x[0].smp, VK_NULL_HANDLE, 0};
   VkWriteDescriptorSet w[3] = {
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .dstArrayElement = 0, .descriptorCount = 4,
       .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .dstArrayElement = 5, .descriptorCount = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii[4]},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &smpi}};
   UpdateDescriptorSets(dev, 3, w, 0, NULL);

   VkPipelineLayout lay;
   VkPipeline pipe;
   if (!make_pipe(&t, "quad.vert.spv", "didx.frag.spv", dsl, 16, &lay, &pipe)) return 0;
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   begin_pass(cb, &t, pipe);
   CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, lay, 0, 1, &set, 0, NULL);
   const float sc[4] = {1, 1, 0, 0};
   CmdPushConstants(cb, lay, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, sc);
   CmdDraw(cb, 6, 1, 0, 0);
   CmdEndRenderPass(cb);
   /* update after bind: slot 2 (blue) -> image 4 (cyan), while the set is bound in a recorded, not yet submitted command buffer */
   VkDescriptorImageInfo upd = {VK_NULL_HANDLE, x[4].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
   VkWriteDescriptorSet wu = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .dstArrayElement = 2, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &upd};
   UpdateDescriptorSets(dev, 1, &wu, 0, NULL);
   if (!cb_submit(&t, cb)) return 0;
   if (!g_fake) {
      static const int expect[4] = {0, 1, 4, 3};   /* index = (x&1) + 2*(y&1) */
      int bad = 0;
      for (int y = 8; y < 24; y++)
         for (int xx = 8; xx < 24; xx++) {
            int e = expect[(xx & 1) + 2 * (y & 1)];
            if (!near(pix(&t, xx, y), col[e][0], col[e][1], col[e][2], 255, 2)) bad++;
         }
      if (bad) for (int k = 0; k < 4; k++) printf("{\"quad\":%d,\"rgba\":[%u,%u,%u,%u]}\n", k, pix(&t, 8 + (k & 1), 8 + (k >> 1))[0], pix(&t, 8 + (k & 1), 8 + (k >> 1))[1], pix(&t, 8 + (k & 1), 8 + (k >> 1))[2], pix(&t, 8 + (k & 1), 8 + (k >> 1))[3]);
      CHECK(bad == 0, "%d of 256 probed pixels have the wrong colour (first: %u,%u,%u,%u)", bad, pix(&t, 8, 8)[0], pix(&t, 8, 8)[1], pix(&t, 8, 8)[2], pix(&t, 8, 8)[3]);
      const uint8_t *q = pix(&t, 200, 201);
      CHECK(near(q, col[expect[(200 & 1) + 2 * (201 & 1)]][0], col[expect[(200 & 1) + 2 * (201 & 1)]][1], col[expect[(200 & 1) + 2 * (201 & 1)]][2], 255, 2), "far pixel wrong");
      printf("{\"test\":\"didx\",\"bad\":%d,\"ok\":true}\n", bad);
   } else {
      printf("{\"test\":\"didx\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   }
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   DestroyDescriptorPool(dev, dp, NULL);
   DestroyDescriptorSetLayout(dev, dsl, NULL);
   for (int i = 0; i < 6; i++) tex_destroy(&x[i]);
   tgt_destroy(&t);
   return 1;
}


/* robustness2 nullDescriptor: a VK_NULL_HANDLE descriptor must read as zero and swallow writes (no fault, no stray write). */
static VkBool32 g_null;
struct nd_case { const char *name; VkDescriptorType type; };
static int
nulld_one(struct tgt *t, const struct nd_case *c, VkSampler smp)
{
   VkDescriptorSetLayoutBinding b[2] = {
      {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
      {.binding = 1, .descriptorType = c->type, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b};
   VkDescriptorSetLayout dsl;
   CHECK(CreateDescriptorSetLayout(dev, &dli, NULL, &dsl) == VK_SUCCESS, "dsl");
   int same = c->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
   VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, same ? 2 : 1}, {c->type, 1}};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                     .poolSizeCount = same ? 1 : 2, .pPoolSizes = ps};
   VkDescriptorPool dp;
   CHECK(CreateDescriptorPool(dev, &dpi, NULL, &dp) == VK_SUCCESS, "dp");
   VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl};
   VkDescriptorSet ds;
   CHECK(AllocateDescriptorSets(dev, &dai, &ds) == VK_SUCCESS, "ds");
   struct hbuf ob;
   if (!hbuf_create(&ob, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return 0;
   memset(ob.p, 0xEE, 4096);
   VkDescriptorBufferInfo obi = {ob.b, 0, 4096}, nbi = {VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};
   VkDescriptorImageInfo nii = {smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
   VkBufferView nbv = VK_NULL_HANDLE;
   VkWriteDescriptorSet w[2] = {
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &obi},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1, .descriptorType = c->type,
       .pBufferInfo = &nbi, .pImageInfo = &nii, .pTexelBufferView = &nbv}};
   UpdateDescriptorSets(dev, 2, w, 0, NULL);
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl};
   VkPipelineLayout lay;
   CHECK(CreatePipelineLayout(dev, &pli, NULL, &lay) == VK_SUCCESS, "layout");
   char name[64];
   snprintf(name, sizeof name, "nulld_%s.comp.spv", c->name);
   VkShaderModule m = load_spv(name);
   CHECK(m, "shader %s", name);
   VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rss = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = 16};
   VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = lay,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = g_sgctl ? &rss : NULL,
                .flags = g_sgctl ? VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT : 0, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}};
   VkPipeline pipe;
   VkResult r = CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe);
   DestroyShaderModule(dev, m, NULL);
   if (r != VK_SUCCESS) { printf("{\"nulld\":\"%s\",\"fail\":\"pipeline %d\"}\n", c->name, r); g_fail++; return 0; }
   VkCommandBuffer cb = cb_begin(t);
   CHECK(cb, "cmdbuf");
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, lay, 0, 1, &ds, 0, NULL);
   CmdDispatch(cb, 1, 1, 1);
   if (!cb_submit(t, cb)) return 0;
   int ok = 1;
   if (!g_fake) {
      uint32_t got = *(uint32_t *)ob.p;
      ok = got == 5 && *(uint32_t *)(ob.p + 4) == 0xEEEEEEEEu;
      printf("{\"nulld\":\"%s\",\"got\":%u,\"want\":5,\"ok\":%s}\n", c->name, got, ok ? "true" : "false");
   }
   if (!ok) g_fail++;
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   DestroyDescriptorPool(dev, dp, NULL);
   DestroyDescriptorSetLayout(dev, dsl, NULL);
   hbuf_destroy(&ob);
   return ok;
}

static int
test_nulld(void)
{
   if (!g_null) { printf("{\"test\":\"nulld\",\"skipped\":\"nullDescriptor not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST};
   VkSampler smp;
   CHECK(CreateSampler(dev, &si, NULL, &smp) == VK_SUCCESS, "sampler");
   static const struct nd_case cases[] = {
      {"ssbow", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, {"ssbo", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, {"ubo", VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
      {"tex", VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER}, {"img", VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
      {"tbuf", VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER}, {"stbuf", VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER}};
   int pass = 0, n = (int)(sizeof cases / sizeof *cases);
   const char *only = getenv("NULLD_CASE");
   for (int i = 0; i < n; i++) {
      if (only && strcmp(only, cases[i].name)) { pass++; continue; }
      pass += nulld_one(&t, &cases[i], smp);
   }
   printf("{\"test\":\"nulld\",\"cases\":%d,\"passed\":%d,\"ok\":%s}\n", n, g_fake ? n : pass, pass == n ? "true" : "false");
   DestroySampler(dev, smp, NULL);   /* was leaked: the 152-byte LeakSanitizer finding */
   tgt_destroy(&t);
   return pass == n;
}


/* R4-B2 cheap shader-capability features: scalarBlockLayout, uniformBufferStandardLayout, shaderZeroInitializeWorkgroupMemory.
 * run_comp: one compute dispatch over a set whose bindings 0..n-1 are the given buffers/types. */
static VkBool32 g_inlineub, g_scalar, g_ubostd, g_zinit, g_demote, g_sgctl_feat, g_fullsg_feat;
static int
run_comp(struct tgt *t, const char *spv, int n, const VkDescriptorType *types, struct hbuf *bufs, uint32_t gx)
{
   VkDescriptorSetLayoutBinding b[4];
   VkDescriptorPoolSize ps[4];
   VkDescriptorBufferInfo bi[4];
   VkWriteDescriptorSet w[4];
   VkDescriptorSetLayout dsl;
   VkDescriptorPool dp;
   VkDescriptorSet ds;
   for (int i = 0; i < n; i++) {
      b[i] = (VkDescriptorSetLayoutBinding){.binding = i, .descriptorType = types[i], .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
      ps[i] = (VkDescriptorPoolSize){types[i], 1};
      bi[i] = (VkDescriptorBufferInfo){bufs[i].b, 0, VK_WHOLE_SIZE};
   }
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = n, .pBindings = b};
   CHECK(CreateDescriptorSetLayout(dev, &dli, NULL, &dsl) == VK_SUCCESS, "dsl");
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = n, .pPoolSizes = ps};
   CHECK(CreateDescriptorPool(dev, &dpi, NULL, &dp) == VK_SUCCESS, "dp");
   VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl};
   CHECK(AllocateDescriptorSets(dev, &dai, &ds) == VK_SUCCESS, "ds");
   for (int i = 0; i < n; i++)
      w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = i, .descriptorCount = 1,
                                    .descriptorType = types[i], .pBufferInfo = &bi[i]};
   UpdateDescriptorSets(dev, n, w, 0, NULL);
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl};
   VkPipelineLayout lay;
   CHECK(CreatePipelineLayout(dev, &pli, NULL, &lay) == VK_SUCCESS, "layout");
   VkShaderModule m = load_spv(spv);
   CHECK(m, "shader %s", spv);
   VkShaderModuleCreateInfo cmci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = g_ml_lastsz, .pCode = g_ml_last};
   VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rss = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = 16};
   VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = lay,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = g_modless ? (const void *)&cmci : g_sgctl ? (const void *)&rss : NULL,
                .flags = g_sgctl ? VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT : 0, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = g_modless ? VK_NULL_HANDLE : m, .pName = "main"}};
   VkPipeline pipe;
   VkResult r = CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe);
   if (g_modless) free(g_ml_last); else DestroyShaderModule(dev, m, NULL);
   CHECK(r == VK_SUCCESS, "pipeline %s = %d", spv, r);
   VkCommandBuffer cb = cb_begin(t);
   CHECK(cb, "cmdbuf");
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, lay, 0, 1, &ds, 0, NULL);
   CmdDispatch(cb, gx, 1, 1);
   if (!cb_submit(t, cb)) return 0;
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   DestroyDescriptorPool(dev, dp, NULL);
   DestroyDescriptorSetLayout(dev, dsl, NULL);
   return 1;
}

/* didxbuf: hardware probe for dynamic + non-uniform indexing of arrays of UBOs, SSBOs and storage images (didx_buf.comp).
 * Runs only when the device advertises (and we enable) shader{Uniform,Storage}BufferArray{Dynamic,NonUniform}Indexing and
 * StorageImageArray{Dynamic,NonUniform}Indexing, or when MALI_TEST_FORCE_DIDX_BUF=1 runs it anyway WITHOUT enabling the bits (the driver
 * rejects unadvertised features at vkCreateDevice), so the lowering can be verified on hardware before the bits are advertised. */
static int g_dbuf, g_dbuf_force;
static int
test_didxbuf(void)
{
   if (!g_dbuf) { printf("{\"test\":\"didxbuf\",\"skipped\":\"buffer/storage-image array indexing not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   enum { NT = 64, IW = 8, IH = 16, IMGB = IW * IH * 4, SW = 192 };
   struct hbuf ub[4], sb[4], ob, stg, rb;
   for (int i = 0; i < 4; i++) {
      if (!hbuf_create(&ub[i], 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || !hbuf_create(&sb[i], SW * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return 0;
      uint32_t *u = (uint32_t *)(void *)ub[i].p, *s = (uint32_t *)(void *)sb[i].p;
      for (int j = 0; j < 4; j++) u[j] = 0xA000u + i * 16 + j;
      for (int j = 0; j < SW; j++) s[j] = j < NT ? 0xB000u + i * 256 + j : 0xEEEEEEEEu;
   }
   const size_t osz = 16 + NT * 6 * 4;
   if (!hbuf_create(&ob, osz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !hbuf_create(&stg, 4 * IMGB, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
       !hbuf_create(&rb, 4 * IMGB, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return 0;
   memset(ob.p, 0xEE, osz);
   ((uint32_t *)(void *)ob.p)[0] = 2;   /* dyn index */
   uint32_t *st = (uint32_t *)(void *)stg.p;
   for (int i = 0; i < 4; i++)
      for (int k = 0; k < IW * IH; k++) st[i * IW * IH + k] = (k < IW * 8) ? 0xC000u + i * 0x100u + k : 0xEEEEEEEEu;
   memset(rb.p, 0, 4 * IMGB);

   VkImage img[4]; VkDeviceMemory imem[4]; VkImageView iv[4];
   VkImageCreateInfo ici = img2d(VK_FORMAT_R32_UINT, IW, IH, 1, 1, 0);
   ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   VkDescriptorImageInfo ii[4];
   for (int i = 0; i < 4; i++) {
      CHECK(CreateImage(dev, &ici, NULL, &img[i]) == VK_SUCCESS, "image");
      VkMemoryRequirements mr;
      GetImageMemoryRequirements(dev, img[i], &mr);
      VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = memtype(mr.memoryTypeBits, 0)};
      CHECK(AllocateMemory(dev, &ai, NULL, &imem[i]) == VK_SUCCESS, "image memory");
      CHECK(BindImageMemory(dev, img[i], imem[i], 0) == VK_SUCCESS, "bind image");
      VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                  .format = VK_FORMAT_R32_UINT, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      CHECK(CreateImageView(dev, &vi, NULL, &iv[i]) == VK_SUCCESS, "view");
      ii[i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, iv[i], VK_IMAGE_LAYOUT_GENERAL};
   }

   VkDescriptorSetLayoutBinding lb[4] = {
      {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
      {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = lb};
   VkDescriptorSetLayout dsl;
   CHECK(CreateDescriptorSetLayout(dev, &dli, NULL, &dsl) == VK_SUCCESS, "dsl");
   VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4}};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = ps};
   VkDescriptorPool dp;
   CHECK(CreateDescriptorPool(dev, &dpi, NULL, &dp) == VK_SUCCESS, "dp");
   VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl};
   VkDescriptorSet ds;
   CHECK(AllocateDescriptorSets(dev, &dai, &ds) == VK_SUCCESS, "ds");
   VkDescriptorBufferInfo ubi[4], sbi[4], obi = {ob.b, 0, VK_WHOLE_SIZE};
   for (int i = 0; i < 4; i++) { ubi[i] = (VkDescriptorBufferInfo){ub[i].b, 0, 16}; sbi[i] = (VkDescriptorBufferInfo){sb[i].b, 0, SW * 4}; }
   VkWriteDescriptorSet w[4] = {
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 4, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = ubi},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 4, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = sbi},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 2, .descriptorCount = 4, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = ii},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 3, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &obi}};
   UpdateDescriptorSets(dev, 4, w, 0, NULL);
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl};
   VkPipelineLayout lay;
   CHECK(CreatePipelineLayout(dev, &pli, NULL, &lay) == VK_SUCCESS, "layout");
   VkShaderModule m = load_spv("didx_buf.comp.spv");
   CHECK(m, "shader didx_buf.comp.spv");
   VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = lay,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}};
   VkPipeline pipe;
   VkResult r = CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe);
   DestroyShaderModule(dev, m, NULL);
   CHECK(r == VK_SUCCESS, "pipeline didx_buf = %d", r);

   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   VkImageMemoryBarrier ib[4];
   for (int i = 0; i < 4; i++)
      ib[i] = (VkImageMemoryBarrier){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                     .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .image = img[i], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 4, ib);
   for (int i = 0; i < 4; i++) {
      VkBufferImageCopy reg = {.bufferOffset = (VkDeviceSize)i * IMGB, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {IW, IH, 1}};
      CmdCopyBufferToImage(cb, stg.b, img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
   }
   for (int i = 0; i < 4; i++) {
      ib[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      ib[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      ib[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      ib[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
   }
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 4, ib);
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, lay, 0, 1, &ds, 0, NULL);
   CmdDispatch(cb, 1, 1, 1);
   for (int i = 0; i < 4; i++) {
      ib[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      ib[i].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      ib[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
   }
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 4, ib);
   for (int i = 0; i < 4; i++) {
      VkBufferImageCopy reg = {.bufferOffset = (VkDeviceSize)i * IMGB, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {IW, IH, 1}};
      CmdCopyImageToBuffer(cb, img[i], VK_IMAGE_LAYOUT_GENERAL, rb.b, 1, &reg);
   }
   if (!cb_submit(&t, cb)) return 0;

   if (!g_fake) {
      const uint32_t *o = (const uint32_t *)(const void *)ob.p + 4;
      const uint32_t *ri = (const uint32_t *)(const void *)rb.p;
      int bad[8] = {0}, first[8] = {0};
#define BAD(c, got) do { if (!(bad[c]++)) first[c] = (got); } while (0)
      for (int tt = 0; tt < NT; tt++) {
         uint32_t nu = tt & 3, di = 2, x = tt & 7, y = tt >> 3;
         uint32_t want[6] = {0xA000u + di * 16, 0xA000u + nu * 16 + 1, 0xB000u + di * 256 + tt, 0xB000u + nu * 256 + tt,
                             0xC000u + di * 0x100u + y * IW + x, 0xC000u + nu * 0x100u + y * IW + x};
         for (int k = 0; k < 6; k++) if (o[tt * 6 + k] != want[k]) BAD(k, o[tt * 6 + k]);
         /* every SSBO, both write regions: the selected one holds the written value, all others stay 0xEEEEEEEE */
         for (uint32_t i = 0; i < 4; i++) {
            const uint32_t *s = (const uint32_t *)(const void *)sb[i].p;
            uint32_t w6 = i == nu ? 0x5000u + tt * 8u + nu : 0xEEEEEEEEu; /* non-uniform SSBO write */
            uint32_t w7 = i == di ? 0x6000u + tt : 0xEEEEEEEEu;           /* dynamic SSBO write */
            if (s[64 + tt] != w6) BAD(6, s[64 + tt]);
            if (s[128 + tt] != w7) BAD(7, s[128 + tt]);
         }
      }
      int ibad = 0;
      for (int i = 0; i < 4; i++)
         for (int tt = 0; tt < NT; tt++) {
            uint32_t got = ri[i * IW * IH + (8 + (tt >> 3)) * IW + (tt & 7)], want = (uint32_t)(tt & 3) == (uint32_t)i ? 0xD000u + tt : 0xEEEEEEEEu;
            if (got != want) ibad++;
         }
      static const char *nm[8] = {"ubo_dyn", "ubo_nonuniform", "ssbo_dyn_read", "ssbo_nonuniform_read", "img_dyn_load", "img_nonuniform_load", "ssbo_nonuniform_write", "ssbo_dyn_write"};
      int tot = ibad;
      for (int k = 0; k < 8; k++) {
         tot += bad[k];
         printf("{\"didxbuf\":\"%s\",\"bad\":%d,\"first\":\"0x%x\"}\n", nm[k], bad[k], first[k]);
      }
      printf("{\"didxbuf\":\"img_nonuniform_store\",\"bad\":%d}\n", ibad);
      CHECK(tot == 0, "%d wrong results (forced=%d)", tot, g_dbuf_force);
      printf("{\"test\":\"didxbuf\",\"forced\":%s,\"ok\":true}\n", g_dbuf_force ? "true" : "false");
   } else printf("{\"test\":\"didxbuf\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   DestroyDescriptorPool(dev, dp, NULL);
   DestroyDescriptorSetLayout(dev, dsl, NULL);
   for (int i = 0; i < 4; i++) {
      DestroyImageView(dev, iv[i], NULL); DestroyImage(dev, img[i], NULL); FreeMemory(dev, imem[i], NULL);
      hbuf_destroy(&ub[i]); hbuf_destroy(&sb[i]);
   }
   hbuf_destroy(&ob); hbuf_destroy(&stg); hbuf_destroy(&rb);
   tgt_destroy(&t);
   return 1;
}

static int
test_scalar(void)
{
   if (!g_scalar) { printf("{\"test\":\"scalar\",\"skipped\":\"scalarBlockLayout not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   struct hbuf bf[3];
   if (!hbuf_create(&bf[0], 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !hbuf_create(&bf[1], 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
       !hbuf_create(&bf[2], 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return 0;
   memset(bf[0].p, 0xEE, 4096);
   float *u = (float *)bf[1].p, *s = (float *)bf[2].p, *o = (float *)bf[0].p;
   uint32_t *ui = (uint32_t *)bf[1].p;
   u[0] = 1; u[1] = 2; u[2] = 3; u[3] = 4; u[4] = 5; ui[5] = 6; ui[6] = 7; ui[7] = 8;   /* scalar: a@0 b@4..12 c@16 d@20 */
   s[0] = 20; s[1] = 21; s[2] = 22; s[3] = 23; s[4] = 24;
   const VkDescriptorType ty[3] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
   if (!run_comp(&t, "layout_scalar.comp.spv", 3, ty, bf, 1)) return 0;
   if (!g_fake) {
      static const float want[13] = {1, 2, 3, 4, 5, 6, 7, 8, 20, 21, 22, 23, 24};
      for (int i = 0; i < 13; i++) CHECK(o[i] == want[i], "o[%d]=%g want %g", i, o[i], want[i]);
      CHECK(s[4] == 99.0f && s[3] == 23.0f, "scalar SSBO write: c=%g b.z=%g", s[4], s[3]);
      CHECK(*(uint32_t *)&o[13] == 0xEEEEEEEEu, "stray write o[13]");
      printf("{\"test\":\"scalar\",\"ok\":true}\n");
   } else printf("{\"test\":\"scalar\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   for (int i = 0; i < 3; i++) hbuf_destroy(&bf[i]);
   tgt_destroy(&t);
   return 1;
}

/* inlineUniformBlock: 80-byte block A written by two partial vkUpdateDescriptorSets, 16-byte block B filled by a descriptor copy
 * from A (bytes 24..40), and an ordinary UBO C next to them (must stay intact). */
static int
test_inlineub(void)
{
   if (!g_inlineub) { printf("{\"test\":\"inlineub\",\"skipped\":\"inlineUniformBlock not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   struct hbuf bf[2];
   if (!hbuf_create(&bf[0], 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !hbuf_create(&bf[1], 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return 0;
   memset(bf[0].p, 0xEE, 4096);
   uint32_t *c = (uint32_t *)bf[1].p;
   for (int i = 0; i < 4; i++) c[i] = 0xC0DE0000u + i;
   uint32_t a[20];
   for (int i = 0; i < 20; i++) a[i] = 0xA0000000u + i * 0x111u;
   VkDescriptorSetLayoutBinding b[4] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
      {1, VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, 80, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
      {2, VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, 16, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
      {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
   VkDescriptorSetLayout dsl;
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = b};
   CHECK(CreateDescriptorSetLayout(dev, &dli, NULL, &dsl) == VK_SUCCESS, "dsl");
   VkDescriptorPoolInlineUniformBlockCreateInfo pib = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO,
                                                       .maxInlineUniformBlockBindings = 2};
   VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, 96}};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .pNext = &pib, .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = ps};
   VkDescriptorPool dp;
   CHECK(CreateDescriptorPool(dev, &dpi, NULL, &dp) == VK_SUCCESS, "dp");
   VkDescriptorSet ds;
   VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl};
   CHECK(AllocateDescriptorSets(dev, &dai, &ds) == VK_SUCCESS, "ds");
   VkDescriptorBufferInfo bi0 = {bf[0].b, 0, VK_WHOLE_SIZE}, bi3 = {bf[1].b, 0, 16};
   VkWriteDescriptorSetInlineUniformBlock ib1 = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK, .dataSize = 32, .pData = a};
   VkWriteDescriptorSetInlineUniformBlock ib2 = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK, .dataSize = 48, .pData = a + 8};
   VkWriteDescriptorSet w[4] = {
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi0},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = &ib1, .dstSet = ds, .dstBinding = 1, .dstArrayElement = 0, .descriptorCount = 32, .descriptorType = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = &ib2, .dstSet = ds, .dstBinding = 1, .dstArrayElement = 32, .descriptorCount = 48, .descriptorType = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK},
      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 3, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi3}};
   UpdateDescriptorSets(dev, 4, w, 0, NULL);
   VkCopyDescriptorSet cp = {.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, .srcSet = ds, .srcBinding = 1, .srcArrayElement = 24, .dstSet = ds,
                             .dstBinding = 2, .dstArrayElement = 0, .descriptorCount = 16};
   UpdateDescriptorSets(dev, 0, NULL, 1, &cp);
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl};
   VkPipelineLayout lay;
   CHECK(CreatePipelineLayout(dev, &pli, NULL, &lay) == VK_SUCCESS, "layout");
   VkShaderModule m = load_spv("inlineub.comp.spv");
   CHECK(m, "shader");
   VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = lay,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}};
   VkPipeline pipe;
   VkResult r = CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe);
   DestroyShaderModule(dev, m, NULL);
   CHECK(r == VK_SUCCESS, "pipeline = %d", r);
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, lay, 0, 1, &ds, 0, NULL);
   CmdDispatch(cb, 1, 1, 1);
   if (!cb_submit(&t, cb)) return 0;
   if (!g_fake) {
      const uint32_t *o = (const uint32_t *)bf[0].p;
      for (int i = 0; i < 20; i++) CHECK(o[i] == a[i], "A[%d]=%08x want %08x", i, o[i], a[i]);
      for (int i = 0; i < 4; i++) CHECK(o[20 + i] == a[6 + i], "B[%d]=%08x want %08x (copy)", i, o[20 + i], a[6 + i]);
      for (int i = 0; i < 4; i++) CHECK(o[24 + i] == c[i], "C[%d]=%08x want %08x", i, o[24 + i], c[i]);
      CHECK(o[28] == 0xEEEEEEEEu, "stray write");
      printf("{\"test\":\"inlineub\",\"ok\":true}\n");
   } else printf("{\"test\":\"inlineub\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   DestroyDescriptorPool(dev, dp, NULL);
   DestroyDescriptorSetLayout(dev, dsl, NULL);
   for (int i = 0; i < 2; i++) hbuf_destroy(&bf[i]);
   tgt_destroy(&t);
   return 1;
}

/* Push constants at the advertised limit: DXVK 3.1.x refuses the device below 256 B (dxvk_limits.h). A 256-byte block is pushed
 * whole; the VS and the FS read the last word (offset 252, the FS turns it into the colour) and the CS copies it to a storage
 * buffer. The shaders also carry FAU immediates, so a reservation that does not cover the push block corrupts one of the two
 * (t_compile checks the invariant directly; here the API path and the reported limit are checked against what works). */
static int
test_push256(void)
{
   uint32_t lim = g_props.limits.maxPushConstantsSize;
   CHECK(lim >= 256, "maxPushConstantsSize %u < 256 (DXVK 3.1.x needs 256)", lim);
   uint8_t pc[256];
   for (unsigned i = 0; i < sizeof pc; i += 4) {
      float f = i == 252 ? 1.0f : 0.0f;   /* offset 252: the value the shaders read */
      memcpy(pc + i, &f, 4);
   }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   struct hbuf ob;
   if (!hbuf_create(&ob, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return 0;
   memset(ob.p, 0xEE, 64);

   /* graphics: one layout, both stages, the full advertised block */
   VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = lim};
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
   VkPipelineLayout lay;
   CHECK(CreatePipelineLayout(dev, &pli, NULL, &lay) == VK_SUCCESS, "pipeline layout %u B", lim);
   /* the limit is the real one: a pipeline over it must be refused (layout creation itself is not validated: the flatten that
    * bounds it runs when a pipeline is created) */
   pcr.size = lim + 4;
   VkPipelineLayout over = VK_NULL_HANDLE;
   CHECK(CreatePipelineLayout(dev, &pli, NULL, &over) == VK_SUCCESS, "layout of %u B", lim + 4);
   pcr.size = lim;

   VkShaderModule vm = load_spv("push252.vert.spv"), fm = load_spv("push252.frag.spv");
   CHECK(vm && fm, "shaders push252.vert/frag");
   VkPipelineShaderStageCreateInfo st[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main"}};
   VkPipelineVertexInputStateCreateInfo vis = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo ias = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
   VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
   VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
   VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba};
   VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
   VkPipelineDynamicStateCreateInfo dys = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
   VkGraphicsPipelineCreateInfo gp = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
                                      .pVertexInputState = &vis, .pInputAssemblyState = &ias, .pViewportState = &vps,
                                      .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cbs,
                                      .pDynamicState = &dys, .layout = lay, .renderPass = t.rp};
   VkPipeline pipe;
   gp.layout = over;
   VkResult r = CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe);
   CHECK(r != VK_SUCCESS, "graphics pipeline with %u B push constants (over the advertised %u) accepted", lim + 4, lim);
   DestroyPipelineLayout(dev, over, NULL);
   gp.layout = lay;
   r = CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe);
   CHECK(r == VK_SUCCESS, "graphics pipeline with %u B push constants = %d", lim, r);
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf graphics");
   begin_pass(cb, &t, pipe);
   CmdPushConstants(cb, lay, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, lim, pc);
   CmdDraw(cb, 3, 1, 0, 0);
   CmdEndRenderPass(cb);
   if (!cb_submit(&t, cb)) return 0;
   if (!g_fake) {
      const uint8_t *p = pix(&t, W / 2, H / 2);
      CHECK(p[0] == 255 && p[1] == 0 && p[2] == 0, "centre %u,%u,%u want 255,0,0 (push constant at offset 252)", p[0], p[1], p[2]);
   }

   /* compute: same block, the word at 252 lands in the storage buffer */
   VkDescriptorSetLayoutBinding lb = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &lb};
   VkDescriptorSetLayout dsl;
   CHECK(CreateDescriptorSetLayout(dev, &dli, NULL, &dsl) == VK_SUCCESS, "dsl");
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
   VkDescriptorPool dp;
   CHECK(CreateDescriptorPool(dev, &dpi, NULL, &dp) == VK_SUCCESS, "dp");
   VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl};
   VkDescriptorSet ds;
   CHECK(AllocateDescriptorSets(dev, &dai, &ds) == VK_SUCCESS, "ds");
   VkDescriptorBufferInfo bi = {ob.b, 0, VK_WHOLE_SIZE};
   VkWriteDescriptorSet wds = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi};
   UpdateDescriptorSets(dev, 1, &wds, 0, NULL);
   VkPushConstantRange cpcr = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = lim};
   VkPipelineLayoutCreateInfo cpli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl,
                                      .pushConstantRangeCount = 1, .pPushConstantRanges = &cpcr};
   VkPipelineLayout clay;
   CHECK(CreatePipelineLayout(dev, &cpli, NULL, &clay) == VK_SUCCESS, "compute layout %u B", lim);
   VkShaderModule cm = load_spv("push252.comp.spv");
   CHECK(cm, "shader push252.comp.spv");
   VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = clay,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cm, .pName = "main"}};
   VkPipeline cpipe;
   r = CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &cpipe);
   CHECK(r == VK_SUCCESS, "compute pipeline with %u B push constants = %d", lim, r);
   VkCommandBuffer ccb = cb_begin(&t);   /* no render pass: a bare dispatch */
   CHECK(ccb, "cmdbuf compute");
   CmdBindPipeline(ccb, VK_PIPELINE_BIND_POINT_COMPUTE, cpipe);
   CmdBindDescriptorSets(ccb, VK_PIPELINE_BIND_POINT_COMPUTE, clay, 0, 1, &ds, 0, NULL);
   CmdPushConstants(ccb, clay, VK_SHADER_STAGE_COMPUTE_BIT, 0, lim, pc);
   CmdDispatch(ccb, 1, 1, 1);
   if (!cb_submit(&t, ccb)) return 0;
   if (!g_fake) {
      const uint32_t *o = (const uint32_t *)ob.p;
      CHECK(o[0] == 0x3f800000u, "CS read the word at offset 252: %08x want 3f800000", o[0]);
      printf("{\"test\":\"push256\",\"ok\":true}\n");
   } else printf("{\"test\":\"push256\",\"skipped\":\"pixels: fake backend (limit %u B, layouts, pipelines, draws and dispatch ran)\",\"ok\":true}\n", lim);
   DestroyPipeline(dev, cpipe, NULL);
   DestroyPipelineLayout(dev, clay, NULL);
   DestroyDescriptorPool(dev, dp, NULL);
   DestroyDescriptorSetLayout(dev, dsl, NULL);
   DestroyShaderModule(dev, cm, NULL);
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   DestroyShaderModule(dev, vm, NULL);
   DestroyShaderModule(dev, fm, NULL);
   hbuf_destroy(&ob);
   tgt_destroy(&t);
   return 1;
}

static int
test_ubostd(void)
{
   if (!g_ubostd) { printf("{\"test\":\"ubostd\",\"skipped\":\"uniformBufferStandardLayout not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   struct hbuf bf[2];
   if (!hbuf_create(&bf[0], 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !hbuf_create(&bf[1], 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return 0;
   memset(bf[0].p, 0xEE, 4096);
   float *u = (float *)bf[1].p, *o = (float *)bf[0].p;
   for (int i = 0; i < 7; i++) u[i] = 10.0f + i;   /* std430 in a UBO: x[4] stride 4, y@16, z@24 */
   const VkDescriptorType ty[2] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
   if (!run_comp(&t, "layout_ubostd.comp.spv", 2, ty, bf, 1)) return 0;
   if (!g_fake) {
      for (int i = 0; i < 7; i++) CHECK(o[i] == 10.0f + i, "o[%d]=%g want %g", i, o[i], 10.0f + i);
      printf("{\"test\":\"ubostd\",\"ok\":true}\n");
   } else printf("{\"test\":\"ubostd\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   for (int i = 0; i < 2; i++) hbuf_destroy(&bf[i]);
   tgt_destroy(&t);
   return 1;
}

static int
test_zinit(void)
{
   if (!g_zinit) { printf("{\"test\":\"zinit\",\"skipped\":\"shaderZeroInitializeWorkgroupMemory not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   struct hbuf bf;
   enum { WG = 64 };
   enum { DIRTY_WG = 2048 };
   if (!hbuf_create(&bf, DIRTY_WG * 64 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return 0;
   memset(bf.p, 0xEE, DIRTY_WG * 64 * 4);
   const VkDescriptorType ty[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
   /* first dirty the shared memory of every core (separate dispatch, shader without zero-init) so a missing lowering is caught */
   if (!run_comp(&t, "zdirty.comp.spv", 1, ty, &bf, DIRTY_WG)) return 0;
   memset(bf.p, 0xEE, WG * 64 * 4);
   /* every workgroup writes 0xFFFFFFFF into its shared array at the end; later workgroups on the same core must still start from 0 */
   if (!run_comp(&t, "zeroinit.comp.spv", 1, ty, &bf, WG)) return 0;
   if (!g_fake) {
      int bad = 0;
      for (int i = 0; i < WG * 64; i++) bad += ((uint32_t *)bf.p)[i] != 0;
      CHECK(bad == 0, "%d of %d shared reads were not zero (first %08x)", bad, WG * 64, ((uint32_t *)bf.p)[0]);
      printf("{\"test\":\"zinit\",\"workgroups\":%d,\"bad\":%d,\"ok\":true}\n", WG, bad);
   } else printf("{\"test\":\"zinit\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   hbuf_destroy(&bf);
   tgt_destroy(&t);
   return 1;
}

/* shaderDemoteToHelperInvocation: pixels with even x demote (no write, alpha stays 0); the odd-x pixels computed dFdx after the demote
 * and must see the demoted neighbour's value: r = dFdx * 0.5 = 128. A killed (non-helper) neighbour gives a wrong/zero derivative. */
static int
test_demote(void)
{
   if (!g_demote) { printf("{\"test\":\"demote\",\"skipped\":\"shaderDemoteToHelperInvocation not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   VkPipelineLayout lay;
   VkPipeline pipe;
   if (!make_pipe(&t, "quad.vert.spv", "demote.frag.spv", VK_NULL_HANDLE, 16, &lay, &pipe)) return 0;
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   begin_pass(cb, &t, pipe);
   const float s[4] = {W, H, 0, 0};
   CmdPushConstants(cb, lay, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, s);
   CmdDraw(cb, 6, 1, 0, 0);
   CmdEndRenderPass(cb);
   if (!cb_submit(&t, cb)) return 0;
   if (!g_fake) {
      int bad = 0, n = 0;
      for (int y = 4; y < H - 4; y++)
         for (int x = 4; x < W - 4; x++) {
            if (abs((x ^ 1) - (W - 1 - y)) <= 3 || abs(x - (W - 1 - y)) <= 3) continue;   /* quad diagonal: two triangles meet, quads straddle them */
            const uint8_t *p = pix(&t, x, y);
            n++;
            if (x & 1) bad += !near(p, 128, 255, 0, 255, 6);
            else bad += p[3] != 0;
         }
      CHECK(bad == 0, "%d of %d pixels wrong (odd %u,%u,%u,%u even %u,%u,%u,%u)", bad, n, pix(&t, 5, 9)[0], pix(&t, 5, 9)[1], pix(&t, 5, 9)[2],
            pix(&t, 5, 9)[3], pix(&t, 6, 9)[0], pix(&t, 6, 9)[1], pix(&t, 6, 9)[2], pix(&t, 6, 9)[3]);
      printf("{\"test\":\"demote\",\"pixels\":%d,\"bad\":%d,\"ok\":true}\n", n, bad);
   } else printf("{\"test\":\"demote\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   tgt_destroy(&t);
   return 1;
}

/* sampleRateShading with 1-sample rasterisation (always supported): sample shading is a no-op, gl_SampleID is 0 and gl_SamplePosition the
 * pixel centre. Shader uses the `sample` qualifier, gl_SampleID and gl_SamplePosition (SPIR-V SampleRateShading). Red = ok. */
static int
test_samplerate(void)
{
   if (!g_feat.sampleRateShading) { printf("{\"test\":\"samplerate\",\"skipped\":\"sampleRateShading not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   VkPipelineLayout lay;
   VkPipeline pipe;
   g_sample_shading = VK_TRUE;
   int ok = make_pipe(&t, "sampid.vert.spv", "sampid.frag.spv", VK_NULL_HANDLE, 0, &lay, &pipe);
   g_sample_shading = VK_FALSE;
   if (!ok) return 0;
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   begin_pass(cb, &t, pipe);
   CmdDraw(cb, 3, 1, 0, 0);
   CmdEndRenderPass(cb);
   if (!cb_submit(&t, cb)) return 0;
   if (!g_fake) {
      const uint8_t *p = pix(&t, W / 2, H / 2);
      CHECK(near(p, 255, 0, 0, 255, 2), "sample shading at 1 sample: %u,%u,%u,%u (want 255,0,0,255)", p[0], p[1], p[2], p[3]);
      printf("{\"test\":\"samplerate\",\"ok\":true}\n");
   } else printf("{\"test\":\"samplerate\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   tgt_destroy(&t);
   return 1;
}

/* shaderClipDistance: clip[0]=ndc.x, clip[2]=ndc.y-0.5; only x>=128 and y>=192 (ndc y>=0.5) may be covered. */
/* VK_EXT_provoking_vertex on hardware: flat colour (v0 red, v1 green, v2 blue) over a covering triangle; FIRST must read red, LAST blue */
static VkBool32 g_pv_last;
static int
test_provoking(void)
{
   if (!g_pv_last) { printf("{\"test\":\"provoking\",\"skipped\":\"provokingVertexLast not advertised\",\"ok\":true}\n"); return 1; }
   static const struct { VkProvokingVertexModeEXT m; const char *n; uint8_t r, g, b; } modes[2] = {
      {VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT, "first", 255, 0, 0}, {VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT, "last", 0, 0, 255}};
   for (int i = 0; i < 2; i++) {
      struct tgt t;
      if (!tgt_create(&t)) return 0;
      VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pv = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
                                                                     .provokingVertexMode = modes[i].m};
      g_rs_next = &pv;
      VkPipelineLayout lay;
      VkPipeline pipe;
      int okp = make_pipe(&t, "provoking.vert.spv", "provoking.frag.spv", VK_NULL_HANDLE, 0, &lay, &pipe);
      g_rs_next = NULL;
      if (!okp) return 0;
      VkCommandBuffer cb = cb_begin(&t);
      CHECK(cb, "cmdbuf");
      begin_pass(cb, &t, pipe);
      CmdDraw(cb, 3, 1, 0, 0);
      CmdEndRenderPass(cb);
      if (!cb_submit(&t, cb)) return 0;
      if (!g_fake) {
         int bad = 0, n = 0;
         for (int y = 4; y < H - 4; y += 7)
            for (int x = 4; x < W - 4; x += 7) { const uint8_t *p = pix(&t, x, y); n++; bad += !near(p, modes[i].r, modes[i].g, modes[i].b, 255, 2); }
         const uint8_t *p = pix(&t, 100, 100);
         CHECK(bad == 0, "provoking %s: %d of %d pixels wrong (centre %u,%u,%u,%u, want %u,%u,%u,255)", modes[i].n, bad, n, p[0], p[1], p[2], p[3], modes[i].r, modes[i].g, modes[i].b);
         printf("{\"test\":\"provoking\",\"mode\":\"%s\",\"centre\":[%u,%u,%u,%u],\"want\":[%u,%u,%u,255],\"ok\":true}\n", modes[i].n, p[0], p[1], p[2], p[3], modes[i].r, modes[i].g, modes[i].b);
      } else printf("{\"test\":\"provoking\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
      DestroyPipeline(dev, pipe, NULL);
      DestroyPipelineLayout(dev, lay, NULL);
      tgt_destroy(&t);
   }
   return 1;
}

static int
test_clip(void)
{
   if (!g_feat.shaderClipDistance) { printf("{\"test\":\"clip\",\"skipped\":\"shaderClipDistance not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   VkPipelineLayout lay;
   VkPipeline pipe;
   if (!make_pipe(&t, "clip.vert.spv", "solid.frag.spv", VK_NULL_HANDLE, 0, &lay, &pipe)) return 0;
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   begin_pass(cb, &t, pipe);
   CmdDraw(cb, 6, 1, 0, 0);
   CmdEndRenderPass(cb);
   if (!cb_submit(&t, cb)) return 0;
   if (!g_fake) {
      int bad = 0, n = 0;
      for (int y = 2; y < H - 2; y++)
         for (int x = 2; x < W - 2; x++) {
            if (abs(x - 128) <= 2 || abs(y - 192) <= 2) continue;
            int in = x >= 128 && y >= 192;
            const uint8_t *p = pix(&t, x, y);
            n++;
            bad += in ? !near(p, 255, 0, 0, 255, 2) : p[3] != 0;
         }
      CHECK(bad == 0, "%d of %d pixels wrong (in %u,%u,%u,%u  x<128 %u,%u,%u,%u  y<192 %u,%u,%u,%u)", bad, n, pix(&t, 200, 220)[0], pix(&t, 200, 220)[1], pix(&t, 200, 220)[2], pix(&t, 200, 220)[3], pix(&t, 60, 220)[0], pix(&t, 60, 220)[1], pix(&t, 60, 220)[2], pix(&t, 60, 220)[3], pix(&t, 200, 100)[0], pix(&t, 200, 100)[1], pix(&t, 200, 100)[2], pix(&t, 200, 100)[3]);
      printf("{\"test\":\"clip\",\"pixels\":%d,\"bad\":%d,\"ok\":true}\n", n, bad);
   } else printf("{\"test\":\"clip\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   tgt_destroy(&t);
   return 1;
}

/* shaderClipDistance: 8 distances: clip[0]=ndc.x, clip[7]=ndc.y-0.5; limits must report 8 / cull 0 / combined 8; only x>=128 and y>=192 (ndc y>=0.5) may be covered. */
static int
test_clip8(void)
{
   if (!g_feat.shaderClipDistance) { printf("{\"test\":\"clip8\",\"skipped\":\"shaderClipDistance not advertised\",\"ok\":true}\n"); return 1; }
   const VkPhysicalDeviceProperties pr = g_props;
   CHECK(pr.limits.maxClipDistances == 8 && pr.limits.maxCullDistances == (g_feat.shaderCullDistance ? 8u : 0u) && pr.limits.maxCombinedClipAndCullDistances == 8, "clip/cull limits %u/%u/%u", pr.limits.maxClipDistances, pr.limits.maxCullDistances, pr.limits.maxCombinedClipAndCullDistances);
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   VkPipelineLayout lay;
   VkPipeline pipe;
   if (!make_pipe(&t, "clip8.vert.spv", "solid.frag.spv", VK_NULL_HANDLE, 0, &lay, &pipe)) return 0;
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   begin_pass(cb, &t, pipe);
   CmdDraw(cb, 6, 1, 0, 0);
   CmdEndRenderPass(cb);
   if (!cb_submit(&t, cb)) return 0;
   if (!g_fake) {
      int bad = 0, n = 0;
      for (int y = 2; y < H - 2; y++)
         for (int x = 2; x < W - 2; x++) {
            if (abs(x - 128) <= 2 || abs(y - 192) <= 2) continue;
            int in = x >= 128 && y >= 192;
            const uint8_t *p = pix(&t, x, y);
            n++;
            bad += in ? !near(p, 255, 0, 0, 255, 2) : p[3] != 0;
         }
      CHECK(bad == 0, "%d of %d pixels wrong (in %u,%u,%u,%u  x<128 %u,%u,%u,%u  y<192 %u,%u,%u,%u)", bad, n, pix(&t, 200, 220)[0], pix(&t, 200, 220)[1], pix(&t, 200, 220)[2], pix(&t, 200, 220)[3], pix(&t, 60, 220)[0], pix(&t, 60, 220)[1], pix(&t, 60, 220)[2], pix(&t, 60, 220)[3], pix(&t, 200, 100)[0], pix(&t, 200, 100)[1], pix(&t, 200, 100)[2], pix(&t, 200, 100)[3]);
      printf("{\"test\":\"clip8\",\"pixels\":%d,\"bad\":%d,\"ok\":true}\n", n, bad);
   } else printf("{\"test\":\"clip8\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   tgt_destroy(&t);
   return 1;
}

/* shaderDrawParameters: see dparams.vert. Triangles A0..A2 (columns 0..2, indirect multi-draw), B (column 3), C (column 0, row 1). */
static int g_dparams;
static int
test_dparams(void)
{
   if (!g_dparams) { printf("{\"test\":\"dparams\",\"skipped\":\"shaderDrawParameters not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   VkPipelineLayout lay;
   VkPipeline pipe;
   if (!make_pipe(&t, "dparams.vert.spv", "solid.frag.spv", VK_NULL_HANDLE, 0, &lay, &pipe)) return 0;
   struct hbuf ib;
   if (!hbuf_create(&ib, 256, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)) return 0;
   static const uint32_t idx[3] = {0, 1, 2};
   static const uint32_t di[12] = {3, 1, 4, 1, 3, 1, 5, 3, 3, 1, 6, 5};
   memcpy(ib.p, idx, sizeof idx);
   memcpy(ib.p + 16, di, sizeof di);
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   begin_pass(cb, &t, pipe);
   BIND_IB(cb, ib.b, 0, VK_INDEX_TYPE_UINT32);
   CmdDrawIndirect(cb, ib.b, 16, 3, 16);
   CmdDraw(cb, 3, 1, 9, 100);
   CmdDrawIndexed(cb, 3, 1, 0, 5, 200);
   CmdEndRenderPass(cb);
   if (!cb_submit(&t, cb)) return 0;
   if (!g_fake) {
      static const int cx[5] = {21, 85, 149, 213, 21}, cy[5] = {117, 117, 117, 117, 181};
      for (int i = 0; i < 5; i++)
         CHECK(red_at(&t, cx[i], cy[i]), "triangle %d missing at (%d,%d): %u,%u,%u,%u", i, cx[i], cy[i], pix(&t, cx[i], cy[i])[0],
               pix(&t, cx[i], cy[i])[1], pix(&t, cx[i], cy[i])[2], pix(&t, cx[i], cy[i])[3]);
      uint32_t cov = coverage(&t);
      CHECK(abs((int)cov - 5 * 2048) <= 5 * 2048 / 50, "coverage %u expected %u", cov, 5 * 2048);
      printf("{\"test\":\"dparams\",\"coverage\":%u,\"ok\":true}\n", cov);
   } else printf("{\"test\":\"dparams\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   hbuf_destroy(&ib);
   tgt_destroy(&t);
   return 1;
}

/* Texel gather offset limits: the advertised minTexelGatherOffset/maxTexelGatherOffset (-8/7: the hardware's i4 narrow immediate and i8 register offset both cover it)
 * must be honoured at the boundary, constant and run-time. 32x32 RG8 T(x,y) = (8x, 8y); gather at the corner (16,16) -> base (15,15), component w = (15,15)+offset. */
static int
test_gatherlim(void)
{
   if (!g_feat.shaderImageGatherExtended) { printf("{\"test\":\"gatherlim\",\"skipped\":\"shaderImageGatherExtended not advertised\",\"ok\":true}\n"); return 1; }
   CHECK(g_props.limits.minTexelGatherOffset == -8, "minTexelGatherOffset = %d expected -8", g_props.limits.minTexelGatherOffset);
   CHECK(g_props.limits.maxTexelGatherOffset == 7, "maxTexelGatherOffset = %u expected 7", g_props.limits.maxTexelGatherOffset);
   CHECK(g_props.limits.minTexelOffset == -8 && g_props.limits.maxTexelOffset == 7, "texel offset limits %d..%u", g_props.limits.minTexelOffset, g_props.limits.maxTexelOffset);
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   uint8_t texel[32 * 32 * 2];
   for (int y = 0; y < 32; y++)
      for (int x = 0; x < 32; x++) { texel[2 * (32 * y + x)] = (uint8_t)(8 * x); texel[2 * (32 * y + x) + 1] = (uint8_t)(8 * y); }
   VkBufferImageCopy reg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {32, 32, 1}};
   VkImageCreateInfo ici = img2d(VK_FORMAT_R8G8_UNORM, 32, 32, 1, 1, 0);
   VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                             .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                             .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
   struct tex x;
   if (!tex_create(&t, &x, &ici, VK_IMAGE_VIEW_TYPE_2D, 1, texel, sizeof texel, &reg, 1, &si)) return 0;
   const float s[4] = {1.0f, 1.0f, 0, 0};
   if (!quad_draw(&t, &x, "gather_lim.frag.spv", s)) return 0;
   if (!g_fake) {
      static const int probe[4][2] = {{64, 64}, {192, 64}, {64, 192}, {192, 192}};
      static const int ox[4] = {-8, 7, -8, 7}, oy[4] = {-8, 7, 7, -8};
      for (int q = 0; q < 4; q++) {
         const uint8_t *p = pix(&t, probe[q][0], probe[q][1]);
         CHECK(near(p, 8 * (15 + ox[q]), 8 * (15 + oy[q]), 0, 255, 3), "quadrant %d offset (%d,%d) got %u,%u,%u,%u expected %d,%d,0,255", q, ox[q], oy[q], p[0], p[1], p[2], p[3], 8 * (15 + ox[q]), 8 * (15 + oy[q]));
      }
      printf("{\"test\":\"gatherlim\",\"ok\":true}\n");
   } else printf("{\"test\":\"gatherlim\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   tex_destroy(&x);
   tgt_destroy(&t);
   return 1;
}

/* shaderImageGatherExtended: 4x4 R8 texture T(x,y) = 16*(4y+x); gather at the texel corner (2,2) -> footprint base (1,1), components
 * (1,2) (2,2) (2,1) (1,1) + offset. Quadrants 0..2: run-time offset (q&1, q>>1); quadrant 3: textureGatherOffsets {(0,0),(1,0),(0,1),(1,1)}: texel c = base texel (1,1) + offsets[c]. */
static int
test_gather(void)
{
   if (!g_feat.shaderImageGatherExtended) { printf("{\"test\":\"gather\",\"skipped\":\"shaderImageGatherExtended not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   uint8_t texel[16];
   for (int i = 0; i < 16; i++) texel[i] = (uint8_t)(16 * i);
   VkBufferImageCopy reg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {4, 4, 1}};
   VkImageCreateInfo ici = img2d(VK_FORMAT_R8_UNORM, 4, 4, 1, 1, 0);
   VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                             .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                             .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
   struct tex x;
   if (!tex_create(&t, &x, &ici, VK_IMAGE_VIEW_TYPE_2D, 1, texel, sizeof texel, &reg, 1, &si)) return 0;
   const float s[4] = {1.0f, 1.0f, 0, 0};
   if (!quad_draw(&t, &x, "gather.frag.spv", s)) return 0;
   if (!g_fake) {
      static const int probe[4][2] = {{64, 64}, {192, 64}, {64, 192}, {192, 192}};
      for (int q = 0; q < 4; q++) {
         int e[4];
         for (int c = 0; c < 4; c++) {
            static const int bx[4] = {1, 2, 2, 1}, by[4] = {2, 2, 1, 1};   /* footprint components */
            static const int ox[4] = {0, 1, 0, 1}, oy[4] = {0, 0, 1, 1};   /* textureGatherOffsets */
            int tx = q < 3 ? bx[c] + (q & 1) : 1 + ox[c], ty = q < 3 ? by[c] + (q >> 1) : 1 + oy[c];   /* gatherOffsets: texel c = base (1,1) + offsets[c] */
            e[c] = 16 * (4 * ty + tx);
         }
         const uint8_t *p = pix(&t, probe[q][0], probe[q][1]);
         CHECK(near(p, e[0], e[1], e[2], e[3], 3), "quadrant %d got %u,%u,%u,%u expected %d,%d,%d,%d", q, p[0], p[1], p[2], p[3], e[0], e[1], e[2], e[3]);
      }
      printf("{\"test\":\"gather\",\"ok\":true}\n");
   } else printf("{\"test\":\"gather\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   tex_destroy(&x);
   tgt_destroy(&t);
   return 1;
}

static PFN_vkCreatePrivateDataSlot g_pd_cr;
static PFN_vkSetPrivateData g_pd_set;
static PFN_vkGetPrivateData g_pd_get;
static PFN_vkDestroyPrivateDataSlot g_pd_de;

/* VK_EXT_private_data (dEQP api.object_management.private_data enables it by name): the device was created with it, a slot stores a value per object */
static int
test_privdata(void)
{
   PFN_vkCreatePrivateDataSlot cr = g_pd_cr;
   PFN_vkSetPrivateData set = g_pd_set;
   PFN_vkGetPrivateData get = g_pd_get;
   PFN_vkDestroyPrivateDataSlot de = g_pd_de;
   CHECK(cr && set && get && de, "privdata: EXT entry points missing");
   VkPrivateDataSlotCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PRIVATE_DATA_SLOT_CREATE_INFO};
   VkPrivateDataSlot slot;
   CHECK(cr(dev, &ci, NULL, &slot) == VK_SUCCESS, "privdata: create slot");
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence f;
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "privdata: fence");
   uint64_t v = 7;
   get(dev, VK_OBJECT_TYPE_FENCE, (uint64_t)(uintptr_t)f, slot, &v);
   CHECK(v == 0, "privdata: unset value %llu", (unsigned long long)v);
   CHECK(set(dev, VK_OBJECT_TYPE_FENCE, (uint64_t)(uintptr_t)f, slot, 0x1234abcdull) == VK_SUCCESS, "privdata: set");
   get(dev, VK_OBJECT_TYPE_FENCE, (uint64_t)(uintptr_t)f, slot, &v);
   CHECK(v == 0x1234abcdull, "privdata: got %llx", (unsigned long long)v);
   DestroyFence(dev, f, NULL);
   de(dev, slot, NULL);
   printf("{\"test\":\"privdata\",\"ok\":true}\n");
   return 1;
}

/* fragmentStoresAndAtomics: see fsstore.frag. Checks the SSBO (stores + atomicAdd/atomicMax), and the storage image (copied back). */
static int
test_fsstore(void)
{
   if (!g_feat.fragmentStoresAndAtomics) { printf("{\"test\":\"fsstore\",\"skipped\":\"fragmentStoresAndAtomics not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   enum { N = 256 * 256 };
   struct hbuf sb, ob;
   if (!hbuf_create(&sb, (N + 2) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return 0;
   if (!hbuf_create(&ob, N * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return 0;
   memset(sb.p, 0, (N + 2) * 4);
   memset(ob.p, 0, N * 4);
   VkImageCreateInfo ici = img2d(VK_FORMAT_R32_UINT, 256, 256, 1, 1, 0);
   ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   VkImage img;
   CHECK(CreateImage(dev, &ici, NULL, &img) == VK_SUCCESS, "image");
   VkMemoryRequirements mr;
   GetImageMemoryRequirements(dev, img, &mr);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = memtype(mr.memoryTypeBits, 0)};
   VkDeviceMemory imem;
   CHECK(AllocateMemory(dev, &ai, NULL, &imem) == VK_SUCCESS, "image memory");
   CHECK(BindImageMemory(dev, img, imem, 0) == VK_SUCCESS, "bind image");
   VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                               .format = VK_FORMAT_R32_UINT, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   VkImageView view;
   CHECK(CreateImageView(dev, &vi, NULL, &view) == VK_SUCCESS, "view");
   VkDescriptorSetLayoutBinding lb[2] = {{.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
                                         {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT}};
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = lb};
   VkDescriptorSetLayout dsl;
   CHECK(CreateDescriptorSetLayout(dev, &dli, NULL, &dsl) == VK_SUCCESS, "dsl");
   VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = ps};
   VkDescriptorPool pool;
   CHECK(CreateDescriptorPool(dev, &dpi, NULL, &pool) == VK_SUCCESS, "dpool");
   VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &dsl};
   VkDescriptorSet set;
   CHECK(AllocateDescriptorSets(dev, &dai, &set) == VK_SUCCESS, "alloc set");
   VkDescriptorBufferInfo bi = {sb.b, 0, VK_WHOLE_SIZE};
   VkDescriptorImageInfo ii = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
   VkWriteDescriptorSet w[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi},
                                {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ii}};
   UpdateDescriptorSets(dev, 2, w, 0, NULL);
   VkPipelineLayout lay;
   VkPipeline pipe;
   if (!make_pipe(&t, "quad.vert.spv", "fsstore.frag.spv", dsl, 16, &lay, &pipe)) return 0;
   VkCommandBuffer cb = cb_begin(&t);
   CHECK(cb, "cmdbuf");
   VkImageMemoryBarrier b1 = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
   begin_pass(cb, &t, pipe);
   const float s[4] = {1.0f, 1.0f, 0, 0};
   CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, lay, 0, 1, &set, 0, NULL);
   CmdPushConstants(cb, lay, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, s);
   CmdDraw(cb, 6, 1, 0, 0);
   CmdEndRenderPass(cb);
   VkImageMemoryBarrier b2 = b1;
   b2.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
   b2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   b2.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
   CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
   VkBufferImageCopy reg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {256, 256, 1}};
   CmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, ob.b, 1, &reg);
   if (!cb_submit(&t, cb)) return 0;
   if (!g_fake) {
      const uint32_t *d = (const uint32_t *)(const void *)sb.p, *im = (const uint32_t *)(const void *)ob.p;
      uint32_t cov = 0, badbuf = 0, badimg = 0, maxi = 0;
      for (uint32_t y = 0; y < 256; y++)
         for (uint32_t x = 0; x < 256; x++) {
            uint32_t i = y * 256 + x;
            int red = pix(&t, (int)x, (int)y)[0] == 255 && pix(&t, (int)x, (int)y)[3] == 255;
            cov += red;
            if (red) maxi = i;
            badbuf += d[2 + i] != (red ? i + 1 : 0);
            badimg += im[i] != (red ? i + 1 : 0);
         }
      CHECK(cov > 60000, "coverage %u", cov);
      CHECK(d[0] == cov, "atomicAdd counter %u expected %u", d[0], cov);
      CHECK(d[1] == maxi, "atomicMax %u expected %u", d[1], maxi);
      CHECK(badbuf == 0, "%u SSBO elements wrong", badbuf);
      CHECK(badimg == 0, "%u storage-image texels wrong", badimg);
      printf("{\"test\":\"fsstore\",\"covered\":%u,\"ok\":true}\n", cov);
   } else printf("{\"test\":\"fsstore\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   DestroyPipeline(dev, pipe, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   DestroyDescriptorPool(dev, pool, NULL);
   DestroyDescriptorSetLayout(dev, dsl, NULL);
   DestroyImageView(dev, view, NULL);
   DestroyImage(dev, img, NULL);
   FreeMemory(dev, imem, NULL);
   hbuf_destroy(&sb);
   hbuf_destroy(&ob);
   tgt_destroy(&t);
   return 1;
}

/* vulkanMemoryModel + vulkanMemoryModelDeviceScope: see memmodel.comp (4 workgroups of 64). */
static VkBool32 g_mm;
static int
test_memmodel(void)
{
   if (!g_mm) { printf("{\"test\":\"memmodel\",\"skipped\":\"vulkanMemoryModel not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   struct hbuf bf;
   if (!hbuf_create(&bf, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return 0;
   memset(bf.p, 0, 4096);
   const VkDescriptorType ty[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
   if (!run_comp(&t, "memmodel.comp.spv", 1, ty, &bf, 4)) return 0;
   if (!g_fake) {
      const uint32_t *a = (const uint32_t *)(const void *)bf.p;
      int bad = 0;
      for (int g = 0; g < 4; g++)
         for (int i = 0; i < 64; i++) bad += a[8 + g * 64 + i] != 1000u + g + i;
      CHECK(bad == 0, "%d wrong non-private results", bad);
      CHECK(a[0] == 256, "relaxed device-scope atomicAdd counter %u expected 256", a[0]);
      CHECK(a[1] == 4, "tickets %u expected 4", a[1]);
      CHECK(a[2] == 4u * 1063u + 6u, "last-arriving workgroup summed %u expected %u", a[2], 4u * 1063u + 6u);
      printf("{\"test\":\"memmodel\",\"total\":%u,\"ok\":true}\n", a[2]);
   } else printf("{\"test\":\"memmodel\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   hbuf_destroy(&bf);
   tgt_destroy(&t);
   return 1;
}

/* subgroup operations (basic/vote/ballot/arithmetic/shuffle/shuffle_relative/clustered/quad): one 64-invocation workgroup, 16 results each. */
static VkPhysicalDeviceVulkan11Properties g_p11;
static PFN_vkGetPhysicalDeviceProperties2 g_gp2;
static int
test_subgroup_one(void)
{
   const VkSubgroupFeatureFlags need = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
      VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT |
      VK_SUBGROUP_FEATURE_CLUSTERED_BIT | VK_SUBGROUP_FEATURE_QUAD_BIT;
   if ((g_p11.subgroupSupportedOperations & need) != need || !(g_p11.subgroupSupportedStages & VK_SHADER_STAGE_COMPUTE_BIT)) {
      printf("{\"test\":\"subgroup\",\"skipped\":\"subgroup operations not advertised\",\"ok\":true}\n");
      return 1;
   }
   CHECK(g_p11.subgroupSize == 16, "subgroupSize %u", g_p11.subgroupSize);
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   struct hbuf bf;
   if (!hbuf_create(&bf, 64 * 16 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return 0;
   memset(bf.p, 0xEE, 64 * 16 * 4);
   const VkDescriptorType ty[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
   if (!run_comp(&t, "subgroup.comp.spv", 1, ty, &bf, 1)) return 0;
   if (!g_fake) {
      static const char *nm[16] = {"size", "sid", "add", "ballot", "bcastfirst", "shuffle", "vote", "incl/excl", "max", "down/xor", "elect", "quad", "clustered", "divergent", "bcast5", "up2"};
      int bad[16] = {0}, tot = 0;
      for (uint32_t i = 0; i < 64; i++) {
         uint32_t sid = i & 15, base = i & ~15u;
         uint32_t e[16];
         e[0] = 16; e[1] = sid;
         e[2] = 16 * base + 120;
         e[3] = 0x1f;
         e[4] = base;
         e[5] = base + ((sid + 3) & 15);
         e[6] = 1 | 2 | 0;
         e[7] = (sid + 1) * 256 + sid;
         e[8] = base + 15;
         e[9] = (sid + 1 < 16 ? i + 1 : i) * 256 + (i ^ 1);   /* ShuffleDown out of range is undefined: only check the in-range lanes */
         e[10] = sid == 0;
         e[11] = i ^ 1;
         e[12] = 4 * (i & ~3u) + 6;
         e[13] = sid < 8 ? 8 : 0;
         e[14] = base + 5;
         e[15] = sid >= 2 ? i - 2 : 0;
         for (int k = 0; k < 16; k++) {
            uint32_t got = ((uint32_t *)bf.p)[i * 16 + k];
            if (k == 9 && sid == 15) got = (got & 0xff) | (e[9] & ~0xffu);
            if (k == 15 && sid < 2) continue;   /* ShuffleUp out of range: undefined */
            if (got != e[k]) { if (!bad[k]++) printf("{\"info\":\"%s first bad i=%u got %08x want %08x\"}\n", nm[k], i, got, e[k]); }
         }
         tot++;
      }
      int nb = 0;
      for (int k = 0; k < 16; k++) nb += bad[k];
      CHECK(nb == 0, "%d wrong subgroup results (of %d invocations)", nb, tot);
      printf("{\"test\":\"subgroup\",\"invocations\":%d,\"bad\":%d,\"ok\":true}\n", tot, nb);
   } else printf("{\"test\":\"subgroup\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   hbuf_destroy(&bf);
   tgt_destroy(&t);
   return 1;
}

static int
test_subgroup(void)
{
   return test_subgroup_one();
}

/* subgroupSizeControl + computeFullSubgroups: the same 64-invocation subgroup shader, compiled with a required size of 16 and
 * REQUIRE_FULL_SUBGROUPS; every result of the 4 full subgroups must still match. Also checks the advertised limits. */
static int
test_sgctl(void)
{
   if (!g_sgctl_feat || !g_fullsg_feat) { printf("{\"test\":\"sgctl\",\"skipped\":\"subgroupSizeControl/computeFullSubgroups not advertised\",\"ok\":true}\n"); return 1; }
   VkPhysicalDeviceVulkan13Properties p13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
   VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p13};
   g_gp2(g_pd, &p2);
   CHECK(p13.minSubgroupSize == 16 && p13.maxSubgroupSize == 16 && (p13.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT), "limits min %u max %u stages %x", p13.minSubgroupSize, p13.maxSubgroupSize, p13.requiredSubgroupSizeStages);
   g_sgctl = 1;
   int ok = test_subgroup_one();
   g_sgctl = 0;
   /* A size the hardware does not have must be refused at pipeline creation, not silently compiled at 16: that is the
    * corruption path (masks/shuffles lowered for 32 lanes but executed by 16). */
   VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
                                     .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
   VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
   VkDescriptorSetLayout dsl;
   CHECK(CreateDescriptorSetLayout(dev, &dli, NULL, &dsl) == VK_SUCCESS, "dsl");
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl};
   VkPipelineLayout lay;
   CHECK(CreatePipelineLayout(dev, &pli, NULL, &lay) == VK_SUCCESS, "layout");
   VkShaderModule m = load_spv("subgroup.comp.spv");
   CHECK(m, "shader subgroup.comp.spv");
   for (unsigned bad = 8; bad <= 32; bad <<= 1) {
      if (bad == p13.maxSubgroupSize) continue;
      VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rss = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO,
                                                                 .requiredSubgroupSize = bad};
      VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = lay,
         .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = &rss,
                   .flags = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                   .module = m, .pName = "main"}};
      VkPipeline pipe = VK_NULL_HANDLE;
      VkResult r = CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe);
      CHECK(r != VK_SUCCESS, "requiredSubgroupSize %u accepted (%d)", bad, r);
      if (r == VK_SUCCESS) DestroyPipeline(dev, pipe, NULL);
   }
   DestroyShaderModule(dev, m, NULL);
   DestroyPipelineLayout(dev, lay, NULL);
   DestroyDescriptorSetLayout(dev, dsl, NULL);
   if (ok) printf("{\"test\":\"sgctl\",\"ok\":true}\n");
   return ok;
}

/* shaderInt16 + storageBuffer16BitAccess / shaderInt8 + storageBuffer8BitAccess */
static VkBool32 g_i16, g_i8, g_intdot;
static int
test_int16(void)
{
   if (!g_i16) { printf("{\"test\":\"int16\",\"skipped\":\"shaderInt16/storageBuffer16BitAccess not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   struct hbuf bf[2];
   if (!hbuf_create(&bf[0], 512, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !hbuf_create(&bf[1], 64 * 6 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return 0;
   int16_t *a = (int16_t *)bf[0].p;
   memset(a, 0xAB, 512);
   for (int i = 0; i < 64; i++) { a[i] = (int16_t)(i * 1013 - 30000 + (i & 1) * 12345); a[64 + i] = (int16_t)(7000 - i * 331 + (i % 3) * 20000); }
   memset(bf[1].p, 0xEE, 64 * 6 * 4);
   const VkDescriptorType ty[2] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
   if (!run_comp(&t, "int16.comp.spv", 2, ty, bf, 1)) return 0;
   if (!g_fake) {
      int bad = 0;
      for (int i = 0; i < 64; i++) {
         int16_t x = a[i], y = a[64 + i];
         uint32_t e[6] = {(uint16_t)(int16_t)(x * y), (uint32_t)(int32_t)(x >> 3), (uint16_t)x >> 3, (uint32_t)(int32_t)(x / 7), (uint32_t)(int32_t)x,
                          (uint32_t)(int32_t)(x > y ? x : y)};
         for (int k = 0; k < 6; k++) {
            uint32_t g = ((uint32_t *)bf[1].p)[i * 6 + k];
            if (g != e[k] && !bad++) printf("{\"info\":\"int16 i=%d k=%d x=%d y=%d got %08x want %08x\"}\n", i, k, x, y, g, e[k]);
         }
         int16_t st = (int16_t)(x * 3 + y);
         if (a[128 + i] != st && !bad++) printf("{\"info\":\"int16 store i=%d got %d want %d\"}\n", i, a[128 + i], st);
         /* neighbours of the 16-bit store must be untouched (0xABAB) */
         if (a[129 + i] != (i == 63 ? (int16_t)0xABAB : a[129 + i]) && 0) bad++;
      }
      CHECK(bad == 0, "%d wrong int16 results", bad);
      printf("{\"test\":\"int16\",\"invocations\":64,\"bad\":%d,\"ok\":true}\n", bad);
   } else printf("{\"test\":\"int16\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   for (int i = 0; i < 2; i++) hbuf_destroy(&bf[i]);
   tgt_destroy(&t);
   return 1;
}

static int
test_int8(void)
{
   if (!g_i8) { printf("{\"test\":\"int8\",\"skipped\":\"shaderInt8/storageBuffer8BitAccess not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   struct hbuf bf[2];
   if (!hbuf_create(&bf[0], 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !hbuf_create(&bf[1], 64 * 5 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return 0;
   uint8_t *a = bf[0].p;
   memset(a, 0xAB, 256);
   for (int i = 0; i < 64; i++) { a[i] = (uint8_t)(i * 37 + 11); a[64 + i] = (uint8_t)(250 - i * 5 + (i & 1) * 90); }
   memset(bf[1].p, 0xEE, 64 * 5 * 4);
   const VkDescriptorType ty[2] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
   if (!run_comp(&t, "int8.comp.spv", 2, ty, bf, 1)) return 0;
   if (!g_fake) {
      int bad = 0;
      for (int i = 0; i < 64; i++) {
         uint8_t x = a[i], y = a[64 + i];
         uint32_t e[5] = {(uint8_t)(x * y), (uint32_t)(int32_t)((int8_t)x >> 1), (uint32_t)(int32_t)(int8_t)x, (uint8_t)(x + y), x / 7u};
         for (int k = 0; k < 5; k++) {
            uint32_t g = ((uint32_t *)bf[1].p)[i * 5 + k];
            if (g != e[k] && !bad++) printf("{\"info\":\"int8 i=%d k=%d x=%u y=%u got %08x want %08x\"}\n", i, k, x, y, g, e[k]);
         }
         uint8_t st = (uint8_t)(x * 3 + y);
         if (a[128 + i] != st && !bad++) printf("{\"info\":\"int8 store i=%d got %u want %u\"}\n", i, a[128 + i], st);
      }
      for (int i = 192; i < 256; i++) bad += a[i] != 0xAB;   /* nothing written past the 64 stored bytes */
      CHECK(bad == 0, "%d wrong int8 results", bad);
      printf("{\"test\":\"int8\",\"invocations\":64,\"bad\":%d,\"ok\":true}\n", bad);
   } else printf("{\"test\":\"int8\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   for (int i = 0; i < 2; i++) hbuf_destroy(&bf[i]);
   tgt_destroy(&t);
   return 1;
}

/* intdot_props: VkPhysicalDeviceShaderIntegerDotProductProperties must be fully written (0xCD garbage must be gone) and honest:
 * no dot instruction exists in the backend, so every *Accelerated hint is VK_FALSE. Also checked through the Vulkan 1.3 properties chain. */
static int
test_intdot_props(void)
{
   if (!g_intdot) return 1;
   VkPhysicalDeviceShaderIntegerDotProductProperties dp;
   memset(&dp, 0xCD, sizeof(dp));
   dp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_PROPERTIES; dp.pNext = NULL;
   VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &dp};
   g_gp2(g_pd, &p2);
   const VkBool32 *b = (const VkBool32 *)((const char *)&dp + offsetof(VkPhysicalDeviceShaderIntegerDotProductProperties, integerDotProduct8BitUnsignedAccelerated));
   int n = (int)((sizeof(dp) - offsetof(VkPhysicalDeviceShaderIntegerDotProductProperties, integerDotProduct8BitUnsignedAccelerated)) / sizeof(VkBool32));
   int bad = 0;
   for (int i = 0; i < n; i++) if (b[i] != VK_FALSE) bad++;
   VkPhysicalDeviceVulkan13Properties p13;
   memset(&p13, 0xCD, sizeof(p13));
   p13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES; p13.pNext = NULL;
   p2.pNext = &p13;
   g_gp2(g_pd, &p2);
   if (p13.integerDotProduct8BitUnsignedAccelerated != VK_FALSE || p13.integerDotProduct64BitMixedSignednessAccelerated != VK_FALSE ||
       p13.integerDotProductAccumulatingSaturating4x8BitPackedSignedAccelerated != VK_FALSE) bad++;
   printf("{\"test\":\"intdot_props\",\"fields\":%d,\"bad\":%d,\"ok\":%s}\n", n, bad, bad || n != 30 ? "false" : "true");
   return !bad && n == 30;
}

/* shaderIntegerDotProduct: packed 4x8 u/s/mixed, accumulating-saturating and 8-/16-bit vector dot products against a C reference
 * (operands include 0xFF/0x80 bytes so signedness mistakes and missing saturation change the result). */
static int
test_intdot(void)
{
   if (!g_intdot) { printf("{\"test\":\"intdot\",\"skipped\":\"shaderIntegerDotProduct not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   struct hbuf bf[2];
   if (!hbuf_create(&bf[0], 192 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !hbuf_create(&bf[1], 64 * 9 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return 0;
   uint32_t *a = (uint32_t *)bf[0].p;
   uint32_t seed = 0x1234567u;
   for (int i = 0; i < 64; i++) {
      seed = seed * 1664525u + 1013904223u;
      a[i] = i == 0 ? 0xFFFFFFFFu : i == 1 ? 0x80808080u : seed;
      seed = seed * 1664525u + 1013904223u;
      a[64 + i] = i == 0 ? 0xFFFFFFFFu : i == 1 ? 0x80808080u : seed;
      a[128 + i] = i < 8 ? 0x7FFFFFF0u : i < 16 ? 0x80000010u : i < 24 ? 0xFFFFFF00u : (seed ^ (seed >> 7));   /* near the saturation limits */
   }
   memset(bf[1].p, 0xEE, 64 * 9 * 4);
   const VkDescriptorType ty[2] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
   if (!run_comp(&t, "intdot.comp.spv", 2, ty, bf, 1)) return 0;
   if (!g_fake) {
      int bad = 0;
      for (int i = 0; i < 64; i++) {
         uint32_t x = a[i], y = a[64 + i], c = a[128 + i];
         int64_t u8 = 0, s8 = 0, m8 = 0, u16 = 0, s16 = 0;
         for (int k = 0; k < 4; k++) {
            uint32_t xb = (x >> (8 * k)) & 0xff, yb = (y >> (8 * k)) & 0xff;
            u8 += (int64_t)xb * yb;
            s8 += (int64_t)(int8_t)xb * (int8_t)yb;
            m8 += (int64_t)xb * (int8_t)yb;
         }
         for (int k = 0; k < 2; k++) {
            uint32_t xh = (x >> (16 * k)) & 0xffff, yh = (y >> (16 * k)) & 0xffff;
            u16 += (int64_t)xh * yh;
            s16 += (int64_t)(int16_t)xh * (int16_t)yh;
         }
         int64_t us = (int64_t)c + u8; if (us > 0xffffffffll) us = 0xffffffffll;
         int64_t ss = (int64_t)(int32_t)c + s8; if (ss > INT32_MAX) ss = INT32_MAX; if (ss < INT32_MIN) ss = INT32_MIN;
         const uint32_t e[9] = {(uint32_t)u8, (uint32_t)s8, (uint32_t)m8, (uint32_t)us, (uint32_t)ss, (uint32_t)u8, (uint32_t)s8, (uint32_t)u16, (uint32_t)s16};
         for (int k = 0; k < 9; k++) {
            uint32_t g = ((uint32_t *)bf[1].p)[i * 9 + k];
            if (g != e[k] && !bad++) printf("{\"info\":\"intdot i=%d k=%d x=%08x y=%08x c=%08x got %08x want %08x\"}\n", i, k, x, y, c, g, e[k]);
         }
      }
      CHECK(bad == 0, "%d wrong integer dot product results", bad);
      printf("{\"test\":\"intdot\",\"invocations\":64,\"bad\":%d,\"ok\":true}\n", bad);
   } else printf("{\"test\":\"intdot\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   for (int i = 0; i < 2; i++) hbuf_destroy(&bf[i]);
   tgt_destroy(&t);
   return 1;
}

static int64_t
smod64(int64_t n, int64_t d)
{
   int64_t r = n % d;
   return (r != 0 && ((r ^ d) < 0)) ? r + d : r;
}

static int
test_int64(void)
{
   if (!g_feat.shaderInt64) { printf("{\"test\":\"int64\",\"skipped\":\"shaderInt64 not advertised\",\"ok\":true}\n"); return 1; }
   struct tgt t;
   if (!tgt_create(&t)) return 0;
   struct hbuf bf[2];
   if (!hbuf_create(&bf[0], 128 * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !hbuf_create(&bf[1], 64 * 8 * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return 0;
   uint64_t *a = (uint64_t *)bf[0].p;
   uint64_t seed = 0x9E3779B97F4A7C15ull;
   for (int i = 0; i < 128; i++) {
      seed = seed * 6364136223846793005ull + 1442695040888963407ull;
      a[i] = seed;
      if (i % 5 == 0) a[i] >>= (i % 40);        /* some small values too */
      if (i % 7 == 0) a[i] = ~a[i];
   }
   memset(bf[1].p, 0xEE, 64 * 8 * 8);
   const VkDescriptorType ty[2] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
   if (!run_comp(&t, "int64.comp.spv", 2, ty, bf, 1)) return 0;
   if (!g_fake) {
      int bad = 0;
      for (uint32_t i = 0; i < 64; i++) {
         uint64_t x = a[i], y = a[64 + i];
         int64_t sx = (int64_t)x;
         uint64_t e[8] = {x + y, x * y, (x << (i & 63)) ^ (y >> ((i * 7) & 63)), (x ^ y) - 1, (uint64_t)(sx >> (i & 31)), x < y ? 1 : 2,
                          (uint64_t)(sx < (int64_t)y) + (x & y) + (x | 3), x * 3 + y * 5};
         for (int k = 0; k < 8; k++) {
            uint64_t g = ((uint64_t *)bf[1].p)[i * 8 + k];
            if (g != e[k] && !bad++) printf("{\"info\":\"int64 i=%u k=%d x=%llx y=%llx got %llx want %llx\"}\n", i, k, (unsigned long long)x, (unsigned long long)y, (unsigned long long)g, (unsigned long long)e[k]);
         }
      }
      int bad_k[4] = {0};
      if (!run_comp(&t, "int64div.comp.spv", 2, ty, bf, 1)) return 0;
      for (uint32_t i = 0; i < 64; i++) {
         uint64_t x = a[i], y = a[64 + i] | 1;
         int64_t sx = (int64_t)x, sy = (int64_t)y;
         uint64_t e[4] = {x / y, x % y, (uint64_t)(sy == -1 ? -sx : sx / sy), (uint64_t)(sy == -1 ? 0 : smod64(sx, sy))};   /* GLSL % on signed = OpSMod (sign of the divisor) */
         for (int k = 0; k < 4; k++) {
            uint64_t g = ((uint64_t *)bf[1].p)[i * 4 + k];
            if (g != e[k]) bad_k[k]++;
            if (g != e[k] && !bad++) printf("{\"info\":\"int64div i=%u k=%d x=%llx y=%llx got %llx want %llx\"}\n", i, k, (unsigned long long)x, (unsigned long long)y, (unsigned long long)g, (unsigned long long)e[k]);
         }
      }
      printf("{\"info\":\"int64div bad per op %d %d %d %d\"}\n", bad_k[0], bad_k[1], bad_k[2], bad_k[3]);
      CHECK(bad == 0, "%d wrong int64 results", bad);
      printf("{\"test\":\"int64\",\"invocations\":64,\"bad\":%d,\"ok\":true}\n", bad);
   } else printf("{\"test\":\"int64\",\"skipped\":\"pixels: fake backend\",\"ok\":true}\n");
   for (int i = 0; i < 2; i++) hbuf_destroy(&bf[i]);
   tgt_destroy(&t);
   return 1;
}

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--only") && i + 1 < argc) g_only = argv[++i];
      else if (!strcmp(argv[i], "--spv-dir") && i + 1 < argc) g_spv = argv[++i];
      else { fprintf(stderr, "usage: t_vk_feat --direct icd.so [--spv-dir dir] [--only case]\n"); return 2; }
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
   VkInstance inst;
   if (CreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("{\"fail\":\"CreateInstance\"}\n"); return 1; }
#define I(n) PFN_vk##n n = (PFN_vk##n)gipa(inst, "vk" #n)
   I(EnumeratePhysicalDevices); I(GetPhysicalDeviceMemoryProperties); I(GetPhysicalDeviceProperties);
   g_edep = (PFN_vkEnumerateDeviceExtensionProperties)gipa(inst, "vkEnumerateDeviceExtensionProperties");
   I(GetPhysicalDeviceFeatures2); I(CreateDevice); I(GetDeviceProcAddr); I(DestroyInstance);
   VkPhysicalDevice pd;
   uint32_t cnt = 1;
   VkResult r = EnumeratePhysicalDevices(inst, &cnt, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || cnt < 1) { printf("{\"fail\":\"no device\"}\n"); return 1; }
   GetPhysicalDeviceProperties(pd, &g_props);
   GetPhysicalDeviceMemoryProperties(pd, &memprops);
   printf("{\"device\":\"%s\"}\n", g_props.deviceName);

   VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
   VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f11};
   VkPhysicalDeviceRobustness2FeaturesEXT rb2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, .pNext = &f12};
   VkPhysicalDeviceMaintenance5FeaturesKHR fm5 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, .pNext = &f12};
   rb2.pNext = &fm5;
   VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT fdv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, .pNext = &rb2};
   VkPhysicalDeviceLineRasterizationFeaturesEXT flr = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT, .pNext = &fdv};
   VkPhysicalDeviceCustomBorderColorFeaturesEXT fcb = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .pNext = &flr};
   VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &fcb};
   GetPhysicalDeviceFeatures2(pd, &f2);
   g_cbc = fcb.customBorderColors; g_cbc_nofmt = fcb.customBorderColorWithoutFormat;
   g_lr_rect = flr.rectangularLines; g_lr_bres = flr.bresenhamLines; g_lr_smooth = flr.smoothLines;
   g_lr_stip_rect = flr.stippledRectangularLines; g_lr_stip_bres = flr.stippledBresenhamLines; g_lr_stip_smooth = flr.stippledSmoothLines;
   g_div = fdv.vertexAttributeInstanceRateDivisor; g_div0 = fdv.vertexAttributeInstanceRateZeroDivisor;
   if (g_div || g_div0) {
      VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT dvp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT};
      VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &dvp};
      ((PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2"))(pd, &p2);
      g_div_max = dvp.maxVertexAttribDivisor;
   }
   {
      VkPhysicalDeviceCustomBorderColorPropertiesEXT cbp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT};
      VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &cbp};
      ((PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2"))(pd, &p2);
      g_cbc_max = cbp.maxCustomBorderColorSamplers;
   }
   g_m5 = fm5.maintenance5;
   g_null = rb2.nullDescriptor;
   VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
   f11.pNext = &f13;
   GetPhysicalDeviceFeatures2(pd, &f2);
   {
      VkPhysicalDeviceVulkan11Properties p11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
      VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p11};
      ((PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2"))(pd, &p2);
      g_p11 = p11;
   }
   g_pd = pd; g_gp2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2");
   g_scalar = f12.scalarBlockLayout; g_ubostd = f12.uniformBufferStandardLayout; g_zinit = f13.shaderZeroInitializeWorkgroupMemory; g_demote = f13.shaderDemoteToHelperInvocation; g_intdot = f13.shaderIntegerDotProduct; g_inlineub = f13.inlineUniformBlock; g_sgctl_feat = f13.subgroupSizeControl; g_fullsg_feat = f13.computeFullSubgroups;
   g_feat = f2.features;
   g_mm = f12.vulkanMemoryModel && f12.vulkanMemoryModelDeviceScope;
   g_dparams = f11.shaderDrawParameters;
   g_ycbcr = f11.samplerYcbcrConversion;
   g_imageless = f12.imagelessFramebuffer;
   g_ifp2 = (PFN_vkGetPhysicalDeviceImageFormatProperties2)gipa(inst, "vkGetPhysicalDeviceImageFormatProperties2");
   g_fp2 = (PFN_vkGetPhysicalDeviceFormatProperties2)gipa(inst, "vkGetPhysicalDeviceFormatProperties2");
   g_i16 = f2.features.shaderInt16 && f11.storageBuffer16BitAccess;
   g_i8 = f12.shaderInt8 && f12.storageBuffer8BitAccess;
   g_mirror = f12.samplerMirrorClampToEdge;
   g_bda = f12.bufferDeviceAddress;
   g_didx = f12.runtimeDescriptorArray && f12.shaderSampledImageArrayNonUniformIndexing && f12.descriptorBindingPartiallyBound &&
            f12.descriptorBindingSampledImageUpdateAfterBind && f12.descriptorBindingUpdateUnusedWhilePending;
   {
      const char *fe = getenv("MALI_TEST_FORCE_DIDX_BUF");   /* test-only: probe the lowering before the bits are advertised */
      g_dbuf_force = fe && *fe == '1';
      g_dbuf = (g_feat.shaderUniformBufferArrayDynamicIndexing && g_feat.shaderStorageBufferArrayDynamicIndexing && g_feat.shaderStorageImageArrayDynamicIndexing &&
                f12.shaderUniformBufferArrayNonUniformIndexing && f12.shaderStorageBufferArrayNonUniformIndexing && f12.shaderStorageImageArrayNonUniformIndexing) || g_dbuf_force;
   }
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   /* enable exactly what is advertised (everything advertised is under test somewhere) */
   VkPhysicalDeviceVulkan12Features e12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .samplerMirrorClampToEdge = f12.samplerMirrorClampToEdge, .bufferDeviceAddress = f12.bufferDeviceAddress, .runtimeDescriptorArray = f12.runtimeDescriptorArray,
      .shaderSampledImageArrayNonUniformIndexing = f12.shaderSampledImageArrayNonUniformIndexing, .descriptorBindingPartiallyBound = f12.descriptorBindingPartiallyBound,
      .descriptorBindingSampledImageUpdateAfterBind = f12.descriptorBindingSampledImageUpdateAfterBind, .descriptorBindingUpdateUnusedWhilePending = f12.descriptorBindingUpdateUnusedWhilePending,
      .shaderUniformBufferArrayNonUniformIndexing = f12.shaderUniformBufferArrayNonUniformIndexing, .shaderStorageBufferArrayNonUniformIndexing = f12.shaderStorageBufferArrayNonUniformIndexing,
      .shaderStorageImageArrayNonUniformIndexing = f12.shaderStorageImageArrayNonUniformIndexing,
                                           .timelineSemaphore = f12.timelineSemaphore,
      .scalarBlockLayout = f12.scalarBlockLayout, .uniformBufferStandardLayout = f12.uniformBufferStandardLayout,
      .vulkanMemoryModel = f12.vulkanMemoryModel, .vulkanMemoryModelDeviceScope = f12.vulkanMemoryModelDeviceScope};
   VkPhysicalDeviceVulkan13Features e13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .shaderZeroInitializeWorkgroupMemory = f13.shaderZeroInitializeWorkgroupMemory, .shaderIntegerDotProduct = f13.shaderIntegerDotProduct, .shaderDemoteToHelperInvocation = f13.shaderDemoteToHelperInvocation, .subgroupSizeControl = f13.subgroupSizeControl, .computeFullSubgroups = f13.computeFullSubgroups, .inlineUniformBlock = f13.inlineUniformBlock};
   e12.pNext = &e13;
   VkPhysicalDeviceVulkan11Features e11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &e12, .storageBuffer16BitAccess = f11.storageBuffer16BitAccess, .shaderDrawParameters = f11.shaderDrawParameters, .samplerYcbcrConversion = f11.samplerYcbcrConversion};
   e12.imagelessFramebuffer = f12.imagelessFramebuffer;
   e12.shaderInt8 = f12.shaderInt8; e12.storageBuffer8BitAccess = f12.storageBuffer8BitAccess;
   VkPhysicalDeviceMaintenance5FeaturesKHR em5 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, .pNext = &e11, .maintenance5 = g_m5};
   VkPhysicalDeviceRobustness2FeaturesEXT er2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, .pNext = &em5, .nullDescriptor = rb2.nullDescriptor};
   VkPhysicalDeviceFeatures2 e2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &er2, .features = g_feat};
   VkPhysicalDeviceCustomBorderColorFeaturesEXT ecb = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .pNext = e2.pNext,
                                                       .customBorderColors = fcb.customBorderColors, .customBorderColorWithoutFormat = fcb.customBorderColorWithoutFormat};
   VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT edv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, .pNext = &ecb,
                                                            .vertexAttributeInstanceRateDivisor = fdv.vertexAttributeInstanceRateDivisor,
                                                            .vertexAttributeInstanceRateZeroDivisor = fdv.vertexAttributeInstanceRateZeroDivisor};
   VkPhysicalDeviceLineRasterizationFeaturesEXT elr = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT, .pNext = &edv, .rectangularLines = flr.rectangularLines};
   e2.pNext = &elr;
   VkPhysicalDeviceProvokingVertexFeaturesEXT epv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT, .pNext = e2.pNext, .provokingVertexLast = VK_TRUE};
   { VkPhysicalDeviceProvokingVertexFeaturesEXT fpv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
     VkPhysicalDeviceFeatures2 q2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &fpv};
     GetPhysicalDeviceFeatures2(pd, &q2); g_pv_last = fpv.provokingVertexLast; epv.provokingVertexLast = fpv.provokingVertexLast; }
   if (g_pv_last) e2.pNext = &epv;
   /* VK_EXT_transform_feedback: enabled only when the driver advertises it (default on since the flip; the loader
    * runs with MALI_EXPERIMENTAL_TF=0 only in the opt-out case), so --only tf_record can reach the three commands. */
   VkPhysicalDeviceTransformFeedbackFeaturesEXT ftf = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, .pNext = e2.pNext};
   {
      VkPhysicalDeviceTransformFeedbackFeaturesEXT q = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
      VkPhysicalDeviceFeatures2 q2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &q};
      GetPhysicalDeviceFeatures2(pd, &q2);
      g_tf = q.transformFeedback;
      ftf.transformFeedback = q.transformFeedback;
      ftf.geometryStreams = q.geometryStreams;
   }
   if (g_tf) { ftf.pNext = e2.pNext; e2.pNext = &ftf; }
   const char *dexts[8]; uint32_t ndx = 0;
   if (g_pv_last) dexts[ndx++] = "VK_EXT_provoking_vertex";
   if (g_div || g_div0) dexts[ndx++] = "VK_EXT_vertex_attribute_divisor";
   if (g_lr_rect) dexts[ndx++] = "VK_EXT_line_rasterization";
   if (g_cbc) dexts[ndx++] = "VK_EXT_custom_border_color";
   if (g_m5) dexts[ndx++] = "VK_KHR_maintenance5";
   if (rb2.nullDescriptor) dexts[ndx++] = "VK_EXT_robustness2";
   dexts[ndx++] = "VK_EXT_private_data";
   if (g_tf) dexts[ndx++] = "VK_EXT_transform_feedback";
   e13.privateData = f13.privateData;
   VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &e2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
                            .enabledExtensionCount = ndx, .ppEnabledExtensionNames = dexts};
   if (CreateDevice(pd, &di, NULL, &dev) != VK_SUCCESS) { printf("{\"fail\":\"CreateDevice\"}\n"); return 1; }
#define LOAD(n) n = (PFN_vk##n)GetDeviceProcAddr(dev, "vk" #n); if (!n) { printf("{\"fail\":\"missing vk" #n "\"}\n"); return 1; }
   DEV_FNS(LOAD)
   /* the three TF commands exist only when VK_EXT_transform_feedback is enabled on the device: optional loads */
   CmdBindTransformFeedbackBuffersEXT = (PFN_vkCmdBindTransformFeedbackBuffersEXT)GetDeviceProcAddr(dev, "vkCmdBindTransformFeedbackBuffersEXT");
   CmdBeginTransformFeedbackEXT = (PFN_vkCmdBeginTransformFeedbackEXT)GetDeviceProcAddr(dev, "vkCmdBeginTransformFeedbackEXT");
   CmdEndTransformFeedbackEXT = (PFN_vkCmdEndTransformFeedbackEXT)GetDeviceProcAddr(dev, "vkCmdEndTransformFeedbackEXT");
   /* When the device has the extension enabled, all three commands must be exposed: a missing one must not turn into
    * a "not advertised" skip (CodeRabbit on #313, t_vk_feat.c:3594). */
   if (g_tf && (!CmdBindTransformFeedbackBuffersEXT || !CmdBeginTransformFeedbackEXT || !CmdEndTransformFeedbackEXT)) {
      printf("{\"fail\":\"VK_EXT_transform_feedback advertised without all three commands\"}\n");
      return 1;
   }
   g_gsl = (PFN_vkGetImageSubresourceLayout)GetDeviceProcAddr(dev, "vkGetImageSubresourceLayout");
   g_rf = (PFN_vkResetFences)GetDeviceProcAddr(dev, "vkResetFences");
   g_rcb = (PFN_vkResetCommandBuffer)GetDeviceProcAddr(dev, "vkResetCommandBuffer");
   g_pd_cr = (PFN_vkCreatePrivateDataSlot)GetDeviceProcAddr(dev, "vkCreatePrivateDataSlotEXT");
   g_pd_set = (PFN_vkSetPrivateData)GetDeviceProcAddr(dev, "vkSetPrivateDataEXT");
   g_pd_get = (PFN_vkGetPrivateData)GetDeviceProcAddr(dev, "vkGetPrivateDataEXT");
   g_pd_de = (PFN_vkDestroyPrivateDataSlot)GetDeviceProcAddr(dev, "vkDestroyPrivateDataSlotEXT");
   g_cbib2 = (PFN_vkCmdBindIndexBuffer2KHR)GetDeviceProcAddr(dev, "vkCmdBindIndexBuffer2KHR");
   GetDeviceQueue(dev, 0, 0, &queue);

   int ok = 1;
   if (ok && CASE_ON("imageless")) ok &= test_imageless();
   if (ok && CASE_ON("firstinst")) ok &= test_firstinst();
   if (ok && CASE_ON("mdilimit")) ok &= test_mdilimit();
   if (ok && CASE_ON("idx32")) ok &= test_idx32();
   if (ok && CASE_ON("aniso")) ok &= test_aniso();
   if (ok && CASE_ON("mirror")) ok &= test_mirror();
   if (ok && CASE_ON("cbc")) ok &= test_cbc();
   if (ok && CASE_ON("divisor")) ok &= test_divisor();
   if (ok && g_only && CASE_ON("inst_divisor")) ok &= test_inst_divisor();
   if (ok && CASE_ON("cubearray")) ok &= test_cubearray();
   if (ok && CASE_ON("arrtex")) ok &= test_arrtex();
   if (ok && CASE_ON("wide")) ok &= test_wide();
   if (ok && CASE_ON("linerast")) ok &= test_linerast();
   if (ok && g_only && CASE_ON("tf_reject")) ok &= test_tf_reject();   /* explicit only: keeps the golden BO/address sequence */
   if (ok && g_only && CASE_ON("tf_record")) ok &= test_tf_record();
   if (ok && g_only && CASE_ON("tf_write")) ok &= test_tf_write();   /* stage 4: the data must land in the bound buffers */   /* stage 2: the commands record; the draw still refuses */
   /* VK_EXT_transform_feedback on by default (MALI_EXPERIMENTAL_TF=0 opts out): the guard that keeps a non-capturing draw
    * safe, the reset path, and the two documented limits. Explicit only, like the cases above. */
   if (ok && g_only && CASE_ON("tf_inactive")) ok &= test_tf_inactive();
   if (ok && g_only && CASE_ON("tf_reset")) ok &= test_tf_reset();
   if (ok && g_only && CASE_ON("tf_multi")) ok &= test_tf_multi();
   if (ok && g_only && CASE_ON("tf_overflow")) ok &= test_tf_overflow();
   if (ok && CASE_ON("tf_default")) ok &= test_tf_default();   /* default-on: the extension must be there with no env */
   if (ok && CASE_ON("storfmt")) ok &= test_storfmt();
   if (ok && CASE_ON("storfmt_nf")) ok &= test_storfmt_nf();
   if (ok && CASE_ON("ycbcr")) ok &= test_ycbcr();
   if (ok && CASE_ON("spec")) ok &= test_spec();
   if (ok && CASE_ON("maint5")) ok &= test_maint5();
   if (ok && CASE_ON("bda")) ok &= test_bda();
   if (ok && CASE_ON("didx")) ok &= test_didx();
   if (ok && CASE_ON("didxbuf")) ok &= test_didxbuf();
   if (ok && CASE_ON("nulld")) ok &= test_nulld();
   if (ok && CASE_ON("scalar")) ok &= test_scalar();
   if (ok && CASE_ON("inlineub")) ok &= test_inlineub();
   if (ok && CASE_ON("push256")) ok &= test_push256();
   if (ok && CASE_ON("ubostd")) ok &= test_ubostd();
   if (ok && CASE_ON("zinit")) ok &= test_zinit();
   if (ok && CASE_ON("int16")) ok &= test_int16();
   if (ok && CASE_ON("int64")) ok &= test_int64();
   if (ok && CASE_ON("int8")) ok &= test_int8();
   if (ok && CASE_ON("intdot")) ok &= test_intdot() && test_intdot_props();
   if (ok && CASE_ON("subgroup")) ok &= test_subgroup();
   if (ok && CASE_ON("sgctl")) ok &= test_sgctl();
   if (ok && CASE_ON("demote")) ok &= test_demote();
   if (ok && CASE_ON("clip")) ok &= test_clip();
   if (ok && CASE_ON("provoking")) ok &= test_provoking();
   if (ok && CASE_ON("samplerate")) ok &= test_samplerate();
   if (ok && CASE_ON("clip8")) ok &= test_clip8();
   if (ok && CASE_ON("dparams")) ok &= test_dparams();
   if (ok && CASE_ON("gather")) ok &= test_gather();
   if (ok && CASE_ON("gatherlim")) ok &= test_gatherlim();
   if (ok && CASE_ON("memmodel")) ok &= test_memmodel();
   if (ok && CASE_ON("fsstore")) ok &= test_fsstore();
   if (ok && CASE_ON("privdata")) ok &= test_privdata();

   DeviceWaitIdle(dev);
   DestroyDevice(dev, NULL);
   DestroyInstance(inst, NULL);
   dlclose(lib);
   printf("{\"pass\":%s}\n", (ok && !g_fail) ? "true" : "false");
   return (ok && !g_fail) ? 0 : 1;
}
