/* SPDX-License-Identifier: MIT */
/* Software-rendered frames on the desktop (MALI_SOFT=1): a real Vulkan client creates images, render passes, pipelines and
 * command buffers through our ICD; the frame the command buffer records is rendered by mali_soft.c on the CPU, and the
 * pixels are checked and printed as a per-tile colour map (the golden). White-box: linked statically with the driver
 * objects (build.sh) so it can register a CPU program per pipeline.
 *
 * What it catches without a phone: clear/render-area coverage (a store that drops a 64-row block second row), row stride,
 * attachment formats, depth test/write order, culling, blending, write masks, and LOAD semantics on a hand-built frame.
 * See docs/host-harness.md for what it does NOT model.
 *
 *   MALI_BACKEND=fake MALI_SOFT=1 t_soft_frame --spv-dir <vk/shader> [--case name]
 * Exit 0 = pass. Screens: the tile map on stdout, compared with tests/golden/soft-frame.txt by tests/soft-frame.sh.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mali_cmd.h"
#include "mali_queue.h"
#include "mali_soft.h"

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance, const char *);

static int g_fail;
#define CHECK(c, ...)                                                                                                     \
   do {                                                                                                                   \
      if (!(c)) {                                                                                                         \
         printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                      \
         printf(__VA_ARGS__);                                                                                             \
         printf("\n");                                                                                                    \
         g_fail = 1;                                                                                                      \
         return 0;                                                                                                        \
      }                                                                                                                   \
   } while (0)

static VkInstance inst;
static PFN_vkGetInstanceProcAddr gipa;
static VkDevice dev;
static PFN_vkVoidFunction (*gdpa)(VkDevice, const char *);
#define F(n) static PFN_vk##n p_##n
F(CreateInstance); F(EnumeratePhysicalDevices); F(CreateDevice); F(GetDeviceQueue); F(DeviceWaitIdle);
F(CreateImage); F(GetImageMemoryRequirements); F(GetImageSubresourceLayout); F(BindImageMemory); F(DestroyImage);
F(AllocateMemory); F(FreeMemory); F(MapMemory); F(CreateImageView); F(DestroyImageView);
F(CreateBuffer); F(GetBufferMemoryRequirements); F(BindBufferMemory); F(DestroyBuffer);
F(CreateRenderPass); F(DestroyRenderPass); F(CreateFramebuffer); F(DestroyFramebuffer);
F(CreateShaderModule); F(DestroyShaderModule); F(CreatePipelineLayout); F(DestroyPipelineLayout);
F(CreateGraphicsPipelines); F(DestroyPipeline); F(CreateCommandPool); F(DestroyCommandPool);
F(AllocateCommandBuffers); F(FreeCommandBuffers); F(BeginCommandBuffer); F(EndCommandBuffer);
F(CmdBeginRenderPass); F(CmdEndRenderPass); F(CmdBindPipeline); F(CmdBindVertexBuffers); F(CmdDraw);
F(CmdSetViewport); F(CmdSetScissor); F(QueueSubmit); F(CreateFence); F(WaitForFences); F(DestroyFence);
#undef F

static const char *g_spv = "shader";
static VkQueue q;

/* No vkGetPhysicalDeviceMemoryProperties in the static-link entrypoint set: try the types the allocation allows. */
static int
scene_alloc(uint32_t bits, VkDeviceSize size, VkDeviceMemory *out)
{
   for (uint32_t i = 0; i < 32; i++) {
      if (!(bits & (1u << i)))
         continue;
      VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = size,
                                 .memoryTypeIndex = i};
      if (p_AllocateMemory(dev, &ai, NULL, out) == VK_SUCCESS)
         return 1;
   }
   return 0;
}

/* ---- CPU programs (stand-ins for the pipeline's shaders: see docs/host-harness.md) ---- */
static void
vs_gradient(void *u, const float attr[MALI_MAX_ATTR][4], const uint8_t *push, unsigned vid, unsigned iid,
            float clip[4], float vary[MALI_SOFT_VARY][4])
{
   (void)u; (void)push; (void)vid; (void)iid;
   clip[0] = attr[0][0]; clip[1] = attr[0][1]; clip[2] = attr[0][2]; clip[3] = attr[0][3];
   memcpy(vary[0], attr[1], sizeof vary[0]);
   vary[1][0] = (attr[0][0] + 1.0f) * 0.5f;
   vary[1][1] = (attr[0][1] + 1.0f) * 0.5f;
   vary[1][2] = 0.0f;
   vary[1][3] = 1.0f;
}

static void
fs_colour(void *u, const float vary[MALI_SOFT_VARY][4], const uint8_t *push, unsigned x, unsigned y, float out[4])
{
   (void)u; (void)push; (void)x; (void)y;
   memcpy(out, vary[0], 4 * sizeof(float));
}

static void
fs_uv(void *u, const float vary[MALI_SOFT_VARY][4], const uint8_t *push, unsigned x, unsigned y, float out[4])
{
   (void)u; (void)push; (void)x; (void)y;
   out[0] = vary[1][0]; out[1] = vary[1][1]; out[2] = 0.5f; out[3] = 1.0f;
}

static const struct mali_soft_program PROG_GRAD = {.name = "gradient", .vs = vs_gradient, .fs = fs_uv};
static const struct mali_soft_program PROG_COL = {.name = "colour", .vs = vs_gradient, .fs = fs_colour};

/* ---- one scene: images, pass, pipeline, command buffer ---- */
#define MAXCASE 5
struct scene {
   unsigned w, h;
   VkFormat cfmt, dfmt;
   VkImage col, dep;
   VkDeviceMemory colm, depm;
   VkImageView colv, depv;
   VkRenderPass rp;
   VkFramebuffer fb;
   VkPipelineLayout lay;
   VkPipeline pipe;
   VkCommandPool pool;
   VkCommandBuffer cb;
   VkBuffer vb;
   VkDeviceMemory vbm;
   uint8_t *px;
   uint32_t pitch;
   VkClearColorValue clear;
   float clear_depth;
};



static int
scene_init(struct scene *s, unsigned w, unsigned h, VkFormat cfmt, VkFormat dfmt,
           const float clear[4], float clear_depth)
{
   memset(s, 0, sizeof *s);
   s->w = w; s->h = h; s->cfmt = cfmt; s->dfmt = dfmt;
   for (unsigned i = 0; i < 4; i++) s->clear.float32[i] = clear[i];
   s->clear_depth = clear_depth;

   VkImageCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                           .format = cfmt, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                           .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                           .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   CHECK(p_CreateImage(dev, &ic, NULL, &s->col) == VK_SUCCESS, "create colour image");
   VkMemoryRequirements mr; p_GetImageMemoryRequirements(dev, s->col, &mr);
   CHECK(scene_alloc(mr.memoryTypeBits, mr.size, &s->colm), "alloc colour");
   CHECK(p_BindImageMemory(dev, s->col, s->colm, 0) == VK_SUCCESS, "bind colour");
   VkImageViewCreateInfo vc = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = s->col,
                               .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = cfmt,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   CHECK(p_CreateImageView(dev, &vc, NULL, &s->colv) == VK_SUCCESS, "colour view");
   CHECK(p_MapMemory(dev, s->colm, 0, VK_WHOLE_SIZE, 0, (void **)&s->px) == VK_SUCCESS, "map colour");
   VkImageSubresource sr = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
   VkSubresourceLayout sl; p_GetImageSubresourceLayout(dev, s->col, &sr, &sl);
   s->pitch = (uint32_t)sl.rowPitch;
   s->px += sl.offset;

   if (dfmt) {
      VkImageCreateInfo di = ic;
      di.format = dfmt; di.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
      CHECK(p_CreateImage(dev, &di, NULL, &s->dep) == VK_SUCCESS, "create depth image");
      p_GetImageMemoryRequirements(dev, s->dep, &mr);
      CHECK(scene_alloc(mr.memoryTypeBits, mr.size, &s->depm), "alloc depth");
      CHECK(p_BindImageMemory(dev, s->dep, s->depm, 0) == VK_SUCCESS, "bind depth");
      VkImageViewCreateInfo vd = vc;
      vd.image = s->dep; vd.format = dfmt;
      vd.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
      CHECK(p_CreateImageView(dev, &vd, NULL, &s->depv) == VK_SUCCESS, "depth view");
   }

   VkAttachmentDescription at[2] = {
      {.format = cfmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
       .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
       .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
       .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {.format = dfmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
       .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
       .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
       .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
   VkAttachmentReference cr = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkAttachmentReference dr = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                              .pColorAttachments = &cr, .pDepthStencilAttachment = dfmt ? &dr : NULL};
   VkRenderPassCreateInfo rc = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = dfmt ? 2u : 1u,
                                .pAttachments = at, .subpassCount = 1, .pSubpasses = &sp};
   CHECK(p_CreateRenderPass(dev, &rc, NULL, &s->rp) == VK_SUCCESS, "render pass");
   VkImageView fbv[2] = {s->colv, s->depv};
   VkFramebufferCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = s->rp,
                                 .attachmentCount = dfmt ? 2u : 1u, .pAttachments = fbv, .width = w, .height = h, .layers = 1};
   CHECK(p_CreateFramebuffer(dev, &fc, NULL, &s->fb) == VK_SUCCESS, "framebuffer");

   VkCommandPoolCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0};
   CHECK(p_CreateCommandPool(dev, &pc, NULL, &s->pool) == VK_SUCCESS, "command pool");
   VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s->pool,
                                     .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   CHECK(p_AllocateCommandBuffers(dev, &ca, &s->cb) == VK_SUCCESS, "command buffer");

   /* a quad (6 vertices) and a centre triangle (3), pos.xyzw + colour.xyzw */
   /* 21 vertices, pos.xyzw + colour.xyzw: quad 0..5, DEPTH_RED 6..8, DEPTH_GREEN 9..11, DEPTH_BLUE 12..14,
    * BLEND (white, alpha .5) 15..17, CULL (reversed winding) 18..20 */
   const float tri[3][4] = {{-0.9f, -0.9f, 0, 1}, {0.9f, -0.9f, 0, 1}, {0.0f, 0.9f, 0, 1}};
   float v[21][8] = {
      {-1, -1, 0.5f, 1, 1, 0, 0, 1},  {1, -1, 0.5f, 1, 0, 1, 0, 1},   {-1, 1, 0.5f, 1, 0, 0, 1, 1},
      {-1, 1, 0.5f, 1, 0, 0, 1, 1},   {1, -1, 0.5f, 1, 0, 1, 0, 1},   {1, 1, 0.5f, 1, 1, 1, 1, 1},
   };
   const float cols[4][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 1, 0.5f}};
   const float zs[4] = {0.9f, 0.1f, 0.9f, 0.25f};
   for (unsigned t = 0; t < 4; t++)
      for (unsigned k = 0; k < 3; k++) {
         v[6 + t * 3 + k][0] = tri[k][0]; v[6 + t * 3 + k][1] = tri[k][1]; v[6 + t * 3 + k][2] = zs[t]; v[6 + t * 3 + k][3] = 1;
         memcpy(&v[6 + t * 3 + k][4], cols[t], 4 * sizeof(float));
      }
   for (unsigned k = 0; k < 3; k++) {   /* CULL: the same triangle with vertices 1 and 2 swapped (clockwise) */
      unsigned src = k == 0 ? 0 : (k == 1 ? 2 : 1);
      v[18 + k][0] = tri[src][0]; v[18 + k][1] = tri[src][1]; v[18 + k][2] = 0.5f; v[18 + k][3] = 1;
      v[18 + k][4] = 1; v[18 + k][5] = 0; v[18 + k][6] = 1; v[18 + k][7] = 1;
   }
   VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sizeof v,
                            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
   CHECK(p_CreateBuffer(dev, &bc, NULL, &s->vb) == VK_SUCCESS, "vertex buffer");
   VkMemoryRequirements bmr; p_GetBufferMemoryRequirements(dev, s->vb, &bmr);
   CHECK(scene_alloc(bmr.memoryTypeBits, bmr.size, &s->vbm), "alloc vb");
   CHECK(p_BindBufferMemory(dev, s->vb, s->vbm, 0) == VK_SUCCESS, "bind vb");
   void *bp; CHECK(p_MapMemory(dev, s->vbm, 0, VK_WHOLE_SIZE, 0, &bp) == VK_SUCCESS, "map vb");
   memcpy(bp, v, sizeof v);

   VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
   VkPipelineLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &pcr};
   CHECK(p_CreatePipelineLayout(dev, &lci, NULL, &s->lay) == VK_SUCCESS, "pipeline layout");
   return 1;
}

static int
scene_pipe(struct scene *s, const struct mali_soft_program *prog, VkCullModeFlags cull, int blend,
           int depth_test, uint32_t wmask)
{
   char path[512];
   snprintf(path, sizeof path, "%s/rot.vert.spv", g_spv);
   FILE *f = fopen(path, "rb");
   CHECK(f, "open %s", path);
   static uint32_t vsbuf[16384], fsbuf[4096];
   size_t n = fread(vsbuf, 1, sizeof vsbuf, f); fclose(f);
   snprintf(path, sizeof path, "%s/var.frag.spv", g_spv);
   f = fopen(path, "rb"); CHECK(f, "open %s", path);
   size_t m = fread(fsbuf, 1, sizeof fsbuf, f); fclose(f);
   VkShaderModuleCreateInfo sm = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = vsbuf};
   VkShaderModule vsm, fsm;
   CHECK(p_CreateShaderModule(dev, &sm, NULL, &vsm) == VK_SUCCESS, "vs module");
   sm.codeSize = m; sm.pCode = fsbuf;
   CHECK(p_CreateShaderModule(dev, &sm, NULL, &fsm) == VK_SUCCESS, "fs module");

   VkPipelineShaderStageCreateInfo st[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vsm, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsm, .pName = "main"}};
   VkVertexInputBindingDescription bd = {0, 32, VK_VERTEX_INPUT_RATE_VERTEX};
   VkVertexInputAttributeDescription ad[2] = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
                                              {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16}};
   VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                              .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bd,
                                              .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = ad};
   VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkViewport vp = {0, 0, (float)s->w, (float)s->h, 0.0f, 1.0f};
   VkRect2D sc = {{0, 0}, {s->w, s->h}};
   VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc};
   VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = cull,
                                                .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
   VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                              .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                               .depthTestEnable = depth_test, .depthWriteEnable = depth_test,
                                               .depthCompareOp = VK_COMPARE_OP_LESS};
   VkPipelineColorBlendAttachmentState cb = {.blendEnable = blend,
      .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask = wmask};
   VkPipelineColorBlendStateCreateInfo bs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                             .attachmentCount = 1, .pAttachments = &cb};
   VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                      .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                      .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                      .pDepthStencilState = s->dfmt ? &ds : NULL, .pColorBlendState = &bs,
                                      .layout = s->lay, .renderPass = s->rp};
   VkResult r = p_CreateGraphicsPipelines(dev, NULL, 1, &pi, NULL, &s->pipe);
   p_DestroyShaderModule(dev, vsm, NULL);
   p_DestroyShaderModule(dev, fsm, NULL);
   CHECK(r == VK_SUCCESS, "create pipeline (%d)", r);
   const struct mali_compiled *kvs, *kfs;
   mali_soft_pipeline_keys(s->pipe, &kvs, &kfs);
   mali_soft_bind_program(kvs, kfs, prog);
   return 1;
}

/* One render pass with n draws: `firsts[i]`..`+counts[i]-1` of the vertex buffer. Draws after the first see what the
 * earlier ones stored (loadOp CLEAR only applies at the start of the pass), which is what the blend and mask cases need. */
static int
scene_draw_n(struct scene *s, const unsigned *firsts, const unsigned *counts, unsigned n)
{
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   CHECK(p_BeginCommandBuffer(s->cb, &bi) == VK_SUCCESS, "begin");
   VkClearValue cv[2] = {{.color = s->clear}, {.depthStencil = {s->clear_depth, 0}}};
   VkRenderPassBeginInfo rb = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = s->rp,
                               .framebuffer = s->fb, .renderArea = {{0, 0}, {s->w, s->h}},
                               .clearValueCount = s->dfmt ? 2u : 1u, .pClearValues = cv};
   p_CmdBeginRenderPass(s->cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
   p_CmdBindPipeline(s->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipe);
   VkDeviceSize off = 0;
   p_CmdBindVertexBuffers(s->cb, 0, 1, &s->vb, &off);
   for (unsigned i = 0; i < n; i++)
      p_CmdDraw(s->cb, counts[i], 1, firsts[i], 0);
   p_CmdEndRenderPass(s->cb);
   CHECK(p_EndCommandBuffer(s->cb) == VK_SUCCESS, "end");
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fe; CHECK(p_CreateFence(dev, &fi, NULL, &fe) == VK_SUCCESS, "fence");
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &s->cb};
   CHECK(p_QueueSubmit(q, 1, &si, fe) == VK_SUCCESS, "submit");
   CHECK(p_WaitForFences(dev, 1, &fe, VK_TRUE, 5000000000ull) == VK_SUCCESS, "wait");
   p_DestroyFence(dev, fe, NULL);
   return 1;
}

static int
scene_draw(struct scene *s, unsigned first, unsigned count)
{
   const unsigned f[1] = {first}, c[1] = {count};
   return scene_draw_n(s, f, c, 1);
}

static void
scene_free(struct scene *s)
{
   if (s->pipe) p_DestroyPipeline(dev, s->pipe, NULL);
   if (s->lay) p_DestroyPipelineLayout(dev, s->lay, NULL);
   if (s->pool) p_DestroyCommandPool(dev, s->pool, NULL);
   if (s->fb) p_DestroyFramebuffer(dev, s->fb, NULL);
   if (s->rp) p_DestroyRenderPass(dev, s->rp, NULL);
   if (s->colv) p_DestroyImageView(dev, s->colv, NULL);
   if (s->depv) p_DestroyImageView(dev, s->depv, NULL);
   if (s->vb) p_DestroyBuffer(dev, s->vb, NULL);
   if (s->vbm) p_FreeMemory(dev, s->vbm, NULL);
   if (s->col) p_DestroyImage(dev, s->col, NULL);
   if (s->dep) p_DestroyImage(dev, s->dep, NULL);
   if (s->colm) p_FreeMemory(dev, s->colm, NULL);
   if (s->depm) p_FreeMemory(dev, s->depm, NULL);
   memset(s, 0, sizeof *s);
}

static void
readpx(const struct scene *s, unsigned x, unsigned y, float out[4])
{
   const uint8_t *p = s->px + (size_t)y * s->pitch;
   const unsigned bpp = s->cfmt == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : s->cfmt == VK_FORMAT_R32G32B32A32_SFLOAT ? 16 : 4;
   p += (size_t)x * bpp;
   if (s->cfmt == VK_FORMAT_R32G32B32A32_SFLOAT) { memcpy(out, p, 16); return; }
   if (s->cfmt == VK_FORMAT_R16G16B16A16_SFLOAT) {
      float v[4];
      for (unsigned i = 0; i < 4; i++) {
         uint16_t h; memcpy(&h, p + 2 * i, 2);
         uint32_t sg = (h >> 15) & 1, ex = (h >> 10) & 0x1f, mn = h & 0x3ff;
         float f = ex == 0 ? ldexpf((float)mn, -24) : ldexpf((float)(mn | 0x400), (int)ex - 25);
         v[i] = sg ? -f : f;
      }
      memcpy(out, v, sizeof v);
   } else {
      for (unsigned i = 0; i < 4; i++) out[i] = p[i] / 255.0f;
      if (s->cfmt == VK_FORMAT_B8G8R8A8_UNORM || s->cfmt == VK_FORMAT_B8G8R8A8_SRGB) {   /* memory is B,G,R,A: return RGBA */
         const float t = out[0]; out[0] = out[2]; out[2] = t;
      }
   }
}

/* ---- the golden: mean colour of every 32x32 tile, as hex, one line per tile row (a dropped block row is a row of 0s) ---- */
#define TS 32
static void
tilemap(const char *name, const struct scene *s)
{
   printf("case %s %ux%u\n", name, s->w, s->h);
   const unsigned tx = (s->w + TS - 1) / TS, ty = (s->h + TS - 1) / TS;
   for (unsigned by = 0; by < ty; by++) {
      printf("  row%u:", by);
      for (unsigned bx = 0; bx < tx; bx++) {
         double acc[3] = {0, 0, 0};
         unsigned n = 0;
         for (unsigned y = by * TS; y < (by + 1) * TS && y < s->h; y++)
            for (unsigned x = bx * TS; x < (bx + 1) * TS && x < s->w; x++) {
               float c[4]; readpx(s, x, y, c);
               for (unsigned k = 0; k < 3; k++) acc[k] += c[k] < 0 ? 0 : c[k] > 1 ? 1 : c[k];
               n++;
            }
         unsigned r = (unsigned)lrint(255.0 * acc[0] / n), g = (unsigned)lrint(255.0 * acc[1] / n), b = (unsigned)lrint(255.0 * acc[2] / n);
         printf(" %02x%02x%02x", r, g, b);
      }
      printf("\n");
   }
   const struct mali_soft_stats *st = mali_soft_last();
   printf("  stats draws=%u tris=%u frags=%u unsupported=%u\n", st->draws, st->tris, st->frags, st->unsupported);
}

/* ---- cases ---- */
static const unsigned RED = 6, GREEN = 9, BLUE = 12, BLEND = 15, CULL = 18;

/* 256x256: the row-band bug ("rows 64-127 read zero"), depth order, blend, cull, write mask. */
static int
case_rgba8(void)
{
   const float clear[4] = {0.25f, 0.5f, 0.75f, 1.0f};
   struct scene s;
   CHECK(scene_init(&s, 256, 256, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT, clear, 1.0f), "init");
   CHECK(scene_pipe(&s, &PROG_GRAD, 0, 0, 1, 0xf), "pipe gradient");
   CHECK(scene_draw(&s, 0, 6), "draw quad");
   float c[4];
   /* every 32-row band must carry the gradient: a store that drops a block row shows up here */
   for (unsigned band = 0; band < 8; band++) {
      readpx(&s, 10, band * TS + TS / 2, c);
      CHECK(c[1] > 0.05f, "band %u (row %u) is black: %f,%f,%f", band, band * TS, c[0], c[1], c[2]);
   }
   readpx(&s, 250, 8, c);
   CHECK(c[0] > 0.9f && c[1] < 0.1f, "top-right is not the red gradient corner: %f,%f,%f", c[0], c[1], c[2]);
   readpx(&s, 8, 250, c);
   CHECK(c[1] > 0.9f && c[0] < 0.1f, "bottom-left is not the green gradient corner: %f,%f,%f", c[0], c[1], c[2]);
   tilemap("rgba8-gradient", &s);
   p_DestroyPipeline(dev, s.pipe, NULL); s.pipe = 0;
   /* depth, one pass: far red (z .9), near green (z .1), far blue (z .9) again -> the centre stays green */
   CHECK(scene_pipe(&s, &PROG_COL, 0, 0, 1, 0xf), "pipe colour");
   const unsigned df[3] = {RED, GREEN, BLUE}, dc[3] = {3, 3, 3};
   CHECK(scene_draw_n(&s, df, dc, 3), "depth draws");
   readpx(&s, 128, 128, c);
   CHECK(c[1] > 0.5f && c[0] < 0.5f && c[2] < 0.5f, "depth order: centre is %f,%f,%f, expected the near (green) triangle", c[0], c[1], c[2]);
   tilemap("rgba8-depth", &s);
   p_DestroyPipeline(dev, s.pipe, NULL); s.pipe = 0;
   /* cull BACK: the reversed-winding triangle draws nothing, so the centre is the clear colour */
   CHECK(scene_pipe(&s, &PROG_COL, VK_CULL_MODE_BACK_BIT, 0, 1, 0xf), "pipe cull");
   CHECK(scene_draw(&s, CULL, 3), "draw culled");
   readpx(&s, 128, 128, c);
   CHECK(fabsf(c[0] - 0.25f) < 0.02f && fabsf(c[1] - 0.5f) < 0.02f && fabsf(c[2] - 0.75f) < 0.02f,
         "cull BACK: the back-facing triangle was drawn (%f,%f,%f, expected the clear colour)", c[0], c[1], c[2]);
   tilemap("rgba8-cull", &s);
   p_DestroyPipeline(dev, s.pipe, NULL); s.pipe = 0;
   /* blend, one pass: red then white at alpha .5 -> (1, .5, .5) */
   CHECK(scene_pipe(&s, &PROG_COL, 0, 1, 1, 0xf), "pipe blend");
   const unsigned bf[2] = {RED, BLEND}, bcn[2] = {3, 3};
   CHECK(scene_draw_n(&s, bf, bcn, 2), "blend draws");
   readpx(&s, 128, 128, c);
   CHECK(c[0] > 0.9f && fabsf(c[1] - 0.5f) < 0.06f && fabsf(c[2] - 0.5f) < 0.06f,
         "blend SRC_ALPHA/1-SRC_ALPHA over red: %f,%f,%f, expected ~1,.5,.5", c[0], c[1], c[2]);
   tilemap("rgba8-blend", &s);
   p_DestroyPipeline(dev, s.pipe, NULL); s.pipe = 0;
   /* write mask G|B: the draw writes G and B only, so R keeps whatever the pass cleared to (0.25) */
   CHECK(scene_pipe(&s, &PROG_COL, 0, 0, 0, 0x6), "pipe mask");
   CHECK(scene_draw(&s, GREEN, 3), "mask draw");
   readpx(&s, 128, 128, c);
   CHECK(fabsf(c[0] - 0.25f) < 0.02f && c[1] > 0.9f && c[2] < 0.1f,
         "write mask G|B: expected (0.25,1,0) (R untouched by the draw), got %f,%f,%f", c[0], c[1], c[2]);
   tilemap("rgba8-mask", &s);
   scene_free(&s);
   return 1;
}

/* 64x64 RGBA16F and a D24X8 depth attachment: format and depth-format coverage. */
static int
case_formats(void)
{
   const float clear[4] = {0.5f, 0.25f, 0.125f, 1.0f};
   struct scene s;
   CHECK(scene_init(&s, 64, 64, VK_FORMAT_R16G16B16A16_SFLOAT, 0, clear, 1.0f), "init f16");
   CHECK(scene_pipe(&s, &PROG_COL, 0, 0, 0, 0xf), "pipe f16");
   CHECK(scene_draw(&s, RED, 3), "draw red triangle");
   float c[4];
   readpx(&s, 32, 32, c);
   CHECK(c[0] > 0.9f && c[1] < 0.1f, "RGBA16F readback: %f,%f,%f", c[0], c[1], c[2]);
   readpx(&s, 1, 1, c);
   CHECK(fabsf(c[0] - 0.5f) < 0.01f && fabsf(c[1] - 0.25f) < 0.01f, "RGBA16F clear: %f,%f", c[0], c[1]);
   tilemap("formats-rgba16f", &s);
   scene_free(&s);
   CHECK(scene_init(&s, 64, 64, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D16_UNORM, clear, 1.0f), "init d16");
   CHECK(scene_pipe(&s, &PROG_COL, 0, 0, 1, 0xf), "pipe d16");
   const unsigned df[3] = {RED, GREEN, BLUE}, dc[3] = {3, 3, 3};
   CHECK(scene_draw_n(&s, df, dc, 3), "d24 draws");
   readpx(&s, 32, 32, c);
   CHECK(c[1] > 0.9f && c[0] < 0.1f, "D16 depth order: %f,%f,%f, expected green", c[0], c[1], c[2]);
   tilemap("formats-d16", &s);
   scene_free(&s);
   return 1;
}

/* A hand-built frame (no Vulkan objects): CLEAR restricted to a render area. This is the second-block-row case the
 * front end cannot express today (colour loadOp LOAD is rejected by mali_cmd.c). */
static int
case_render_area(void)
{
   struct scene s;
   const float clear[4] = {1.0f, 0.0f, 0.0f, 1.0f};
   CHECK(scene_init(&s, 256, 256, VK_FORMAT_R8G8B8A8_UNORM, 0, clear, 1.0f), "init");
   struct mali_cmd_target t;
   CHECK(mali_cmd_res.image_view(s.colv, &t) == 0, "image view target");
   CHECK(mali_soft_device_hw(dev), "device hw context");
   struct mali_frame f;
   memset(&f, 0, sizeof f);
   f.width = s.w; f.height = s.h; f.samples = 1; f.has_colour = true;
   f.colour = (struct mali_att){.va = t.va, .row_stride = t.row_stride, .rt = t.rt, .clear = true,
                                .clear_color = {1.0f, 0.0f, 0.0f, 1.0f}};
   f.area[2] = (int32_t)s.w; f.area[3] = (int32_t)s.h;
   CHECK(mali_soft_frame(mali_soft_device_hw(dev), &f), "soft frame full clear: %s", mali_soft_last()->why);
   f.area[1] = 64; f.area[3] = 64;                      /* only rows 64..127 */
   f.colour.clear_color[0] = 0.0f; f.colour.clear_color[1] = 1.0f;
   CHECK(mali_soft_frame(mali_soft_device_hw(dev), &f), "soft frame area clear: %s", mali_soft_last()->why);
   float c[4];
   readpx(&s, 10, 10, c);
   CHECK(c[0] > 0.9f && c[1] < 0.1f, "row 10 was touched by a clear limited to rows 64..127: %f,%f,%f", c[0], c[1], c[2]);
   readpx(&s, 10, 100, c);
   CHECK(c[1] > 0.9f && c[0] < 0.1f, "row 100 missed the clear limited to rows 64..127: %f,%f,%f", c[0], c[1], c[2]);
   readpx(&s, 10, 200, c);
   CHECK(c[0] > 0.9f && c[1] < 0.1f, "row 200 was touched by a clear limited to rows 64..127: %f,%f,%f", c[0], c[1], c[2]);
   tilemap("render-area", &s);
   scene_free(&s);
   return 1;
}


/* ---- hand-built frames: MRT, LOAD, and the two-vertical-halves (AFBC rows 64..127) cases ----
 * No Vulkan pipeline: the draw carries dummy shader keys that the CPU programs below are registered under, and its vertices
 * live in hw->alloc memory (the rasteriser reaches both through hw->va_cpu like any attachment). Each render target is a
 * `scene` image so the test owns its row stride and format and can read the pixels back. */
static void
fs_mrt4(void *u, const float vary[MALI_SOFT_VARY][4], const uint8_t *push, unsigned x, unsigned y, float out[MALI_SOFT_MRT][4])
{
   (void)u; (void)push; (void)x; (void)y;
   /* RT0 = the vertex colour, RT1 = its inverse, RT2 = the colour with R and B swapped, RT3 = a constant grey */
   for (unsigned k = 0; k < 3; k++) { out[0][k] = vary[0][k]; out[1][k] = 1.0f - vary[0][k]; }
   out[0][3] = out[1][3] = out[2][3] = out[3][3] = 1.0f;
   out[2][0] = vary[0][2]; out[2][1] = vary[0][1]; out[2][2] = vary[0][0];
   out[3][0] = out[3][1] = out[3][2] = 0.5f;
}
static const char key_mrt_vs, key_mrt_fs, key_grad_vs, key_grad_fs;
static const struct mali_soft_program PROG_MRT = {.name = "mrt", .vs = vs_gradient, .fs = fs_colour, .fs_mrt = fs_mrt4};

/* independentBlend: every attachment has a different colour and its own blend state. The FS reads its push block (the
 * first float is added to RT2's blue), so a pass that loses the FS push constants also fails this case. */
static void
fs_blend4(void *u, const float vary[MALI_SOFT_VARY][4], const uint8_t *push, unsigned x, unsigned y, float out[MALI_SOFT_MRT][4])
{
   (void)u; (void)vary; (void)x; (void)y;
   float add;
   memcpy(&add, push, sizeof add);
   out[0][0] = 0.8f; out[0][1] = 0.1f; out[0][2] = 0.4f; out[0][3] = 1.0f;
   out[1][0] = 0.2f; out[1][1] = 0.6f; out[1][2] = 0.9f; out[1][3] = 0.5f;
   out[2][0] = 0.7f; out[2][1] = 0.7f; out[2][2] = 0.2f + add; out[2][3] = 1.0f;
   out[3][0] = 0.0f; out[3][1] = 0.9f; out[3][2] = 0.3f; out[3][3] = 1.0f;
}
static const char key_blend_vs, key_blend_fs;
static const struct mali_soft_program PROG_BLEND = {.name = "mrt-blend", .vs = vs_gradient, .fs = fs_colour, .fs_mrt = fs_blend4};

/* A rectangle in NDC [x0,x1]x[y0,y1] (y = -1 is row 0), two triangles, vertex colour c (or the gradient's own colours when
 * c == NULL: the corner colours differ so every 32x32 tile of a covered area has its own value). */
static int
hand_quad(struct mali_frame *f, const struct mali_hw *hw, const void *vskey, const void *fskey, float x0, float y0,
          float x1, float y1, const float *c, const struct mali_fixed *fx, float fspush)
{
   uint64_t va;
   float (*v)[8] = hw->alloc(hw->user, 6 * sizeof *v, 64, &va);
   CHECK(v, "alloc vertex buffer");
   const float px[6][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x0, y1}, {x1, y0}, {x1, y1}};
   const float cc[4][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 1, 1}};
   const unsigned ci[6] = {0, 1, 2, 2, 1, 3};
   for (unsigned i = 0; i < 6; i++) {
      v[i][0] = px[i][0]; v[i][1] = px[i][1]; v[i][2] = 0.5f; v[i][3] = 1;
      memcpy(&v[i][4], c ? c : cc[ci[i]], 4 * sizeof(float));
      if (c) v[i][7] = 1;
   }
   struct mali_draw d;
   memset(&d, 0, sizeof d);
   d.gfx.vs = vskey; d.gfx.fs = fskey; d.gfx.prim = 8; d.gfx.nattr = 2;
   d.gfx.attr[0] = (struct mali_attr){.binding = 0, .offset = 0, .stride = 32, .vk_format = VK_FORMAT_R32G32B32A32_SFLOAT};
   d.gfx.attr[1] = (struct mali_attr){.binding = 0, .offset = 16, .stride = 32, .vk_format = VK_FORMAT_R32G32B32A32_SFLOAT};
   d.vp[2] = (float)f->width; d.vp[3] = (float)f->height; d.vp[5] = 1.0f;
   d.sc[2] = (int32_t)f->width; d.sc[3] = (int32_t)f->height;
   d.vb_va[0] = va; d.vb_size[0] = 6 * sizeof *v;
   d.count = 6; d.instance_count = 1;
   if (fx) d.fx = *fx;   /* fixed state for the pass (independentBlend case) */
   memcpy(d.push[1], &fspush, sizeof fspush);   /* the FS push block this case's fragment program reads */
   CHECK(mali_frame_add_draw(f, &d) == 0, "add draw");
   return 1;
}

static int
hand_target(const struct scene *s, struct mali_att *a)
{
   struct mali_cmd_target t;
   CHECK(mali_cmd_res.image_view(s->colv, &t) == 0, "image view target");
   a->va = t.va; a->row_stride = t.row_stride; a->rt = t.rt;
   return 1;
}

static int
hand_frame(const struct scene *const *rt, unsigned n, const struct mali_att *attrs, int32_t ax, int32_t ay,
           int32_t aw, int32_t ah, const struct mali_soft_program *prog, const void *vk, const void *fk,
           float qx0, float qy0, float qx1, float qy1, const float *col, const struct mali_fixed *fx, float fspush)
{
   const struct mali_hw *hw = mali_soft_device_hw(dev);
   CHECK(hw, "device hw context");
   struct mali_frame f;
   memset(&f, 0, sizeof f);
   f.width = rt[0]->w; f.height = rt[0]->h; f.samples = 1; f.has_colour = true; f.ncolour = n;
   for (unsigned i = 0; i < n; i++) {
      struct mali_att *a = i ? &f.colour_x[i - 1] : &f.colour;
      *a = attrs[i];
      if (!hand_target(rt[i], a)) return 0;
   }
   f.area[0] = ax; f.area[1] = ay; f.area[2] = aw; f.area[3] = ah;
   mali_soft_bind_program(vk, fk, prog);
   if (!hand_quad(&f, hw, vk, fk, qx0, qy0, qx1, qy1, col, fx, fspush)) { mali_frame_finish(&f); return 0; }
   const bool ok = mali_soft_frame(hw, &f);
   mali_frame_finish(&f);
   CHECK(ok, "soft frame: %s", mali_soft_last()->why);
   return 1;
}

static int
near4(const float c[4], float r, float g, float b)
{
   return fabsf(c[0] - r) < 0.02f && fabsf(c[1] - g) < 0.02f && fabsf(c[2] - b) < 0.02f;
}

/* Two-pass MRT. Pass 1 draws the left half into RT0 (RGBA8) and RT1 (RGBA16F), both CLEARed and stored. Pass 2 draws the
 * top half: RT1 LOADs (and keeps pass 1 outside the quad), RT0 is loadOp DONT_CARE + storeOp DONT_CARE, so pass 2 must
 * leave RT0's memory exactly as pass 1 stored it. Each attachment has its own format, stride and flags. */
static int
case_mrt(void)
{
   struct scene a, b;
   const float z[4] = {0, 0, 0, 1};
   CHECK(scene_init(&a, 128, 128, VK_FORMAT_R8G8B8A8_UNORM, 0, z, 1.0f), "init rt0");
   CHECK(scene_init(&b, 128, 128, VK_FORMAT_R16G16B16A16_SFLOAT, 0, z, 1.0f), "init rt1");
   CHECK(a.pitch != b.pitch, "RT0/RT1 strides should differ (%u vs %u)", a.pitch, b.pitch);
   const struct scene *rt[2] = {&a, &b};
   const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1};
   struct mali_att p1[2] = {{.clear = true, .clear_color = {0.2f, 0.2f, 0.2f, 1}}, {.clear = true, .clear_color = {0, 0, 0.5f, 1}}};
   CHECK(hand_frame(rt, 2, p1, 0, 0, 128, 128, &PROG_MRT, &key_mrt_vs, &key_mrt_fs, -1, -1, 0, 1, red, NULL, 0.0f), "pass 1");
   struct mali_att p2[2] = {{.dont_care = true, .no_store = true}, {0}};   /* RT1: LOAD */
   CHECK(hand_frame(rt, 2, p2, 0, 0, 128, 128, &PROG_MRT, &key_mrt_vs, &key_mrt_fs, -1, -1, 1, 0, green, NULL, 0.0f), "pass 2");
   float c[4];
   /* RT0 after pass 2 = after pass 1: red left half, grey right half, pass 2's green nowhere */
   readpx(&a, 20, 100, c); CHECK(near4(c, 1, 0, 0), "RT0 bottom-left %f,%f,%f, expected red (pass 1)", c[0], c[1], c[2]);
   readpx(&a, 100, 20, c); CHECK(near4(c, 0.2f, 0.2f, 0.2f), "RT0 top-right %f,%f,%f: a storeOp DONT_CARE pass wrote memory", c[0], c[1], c[2]);
   readpx(&a, 100, 100, c); CHECK(near4(c, 0.2f, 0.2f, 0.2f), "RT0 bottom-right %f,%f,%f, expected the pass 1 clear", c[0], c[1], c[2]);
   /* RT1: the inverse of the vertex colour where a quad covered it, the LOADed pass 1 contents elsewhere */
   readpx(&b, 20, 100, c); CHECK(near4(c, 0, 1, 1), "RT1 bottom-left %f,%f,%f, expected cyan (pass 1 kept by LOAD)", c[0], c[1], c[2]);
   readpx(&b, 20, 20, c); CHECK(near4(c, 1, 0, 1), "RT1 top-left %f,%f,%f, expected magenta (pass 2 over pass 1)", c[0], c[1], c[2]);
   readpx(&b, 100, 20, c); CHECK(near4(c, 1, 0, 1), "RT1 top-right %f,%f,%f, expected magenta (pass 2)", c[0], c[1], c[2]);
   readpx(&b, 100, 100, c); CHECK(near4(c, 0, 0, 0.5f), "RT1 bottom-right %f,%f,%f, expected the pass 1 clear (LOAD)", c[0], c[1], c[2]);
   tilemap("mrt-rt0", &a);
   tilemap("mrt-rt1", &b);
   scene_free(&a); scene_free(&b);
   return 1;
}

/* Four attachments in one pass, with formats RGBA8, BGRA8, RGBA16F and RGBA32F and their own strides; RT3 is a LOAD of a
 * pre-filled image and the quad covers a quarter of it. */
static int
case_mrt4(void)
{
   struct scene s[4];
   const float z[4] = {0, 0, 0, 1};
   const VkFormat fm[4] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
                           VK_FORMAT_R32G32B32A32_SFLOAT};
   for (unsigned i = 0; i < 4; i++)
      CHECK(scene_init(&s[i], 64, 64, fm[i], 0, z, 1.0f), "init rt%u", i);
   /* pre-fill RT3 with a CLEAR pass of its own, then use it as LOAD */
   const struct scene *one[1] = {&s[3]};
   struct mali_att pre = {.clear = true, .clear_color = {0.25f, 0.5f, 0.75f, 1}};
   CHECK(hand_frame(one, 1, &pre, 0, 0, 64, 64, &PROG_MRT, &key_mrt_vs, &key_mrt_fs, 0, 0, 0, 0, z, NULL, 0.0f), "prefill");
   const struct scene *rt[4] = {&s[0], &s[1], &s[2], &s[3]};
   struct mali_att at[4] = {{.clear = true, .clear_color = {0, 0, 0, 1}}, {.clear = true, .clear_color = {0, 0, 0, 1}},
                            {.clear = true, .clear_color = {0, 0, 0, 1}}, {0}};
   const float col[4] = {1, 0.25f, 0.5f, 1};
   CHECK(hand_frame(rt, 4, at, 0, 0, 64, 64, &PROG_MRT, &key_mrt_vs, &key_mrt_fs, 0, 0, 1, 1, col, NULL, 0.0f), "4-RT pass");
   float c[4];
   readpx(&s[0], 48, 48, c); CHECK(near4(c, 1, 0.25f, 0.5f), "RT0 %f,%f,%f", c[0], c[1], c[2]);
   readpx(&s[1], 48, 48, c); CHECK(near4(c, 0, 0.75f, 0.5f), "RT1 (BGRA8, inverse) %f,%f,%f", c[0], c[1], c[2]);
   readpx(&s[2], 48, 48, c); CHECK(near4(c, 0.5f, 0.25f, 1), "RT2 (RGBA16F, R/B swapped) %f,%f,%f", c[0], c[1], c[2]);
   readpx(&s[3], 48, 48, c); CHECK(near4(c, 0.5f, 0.5f, 0.5f), "RT3 inside the quad %f,%f,%f", c[0], c[1], c[2]);
   readpx(&s[3], 8, 8, c); CHECK(near4(c, 0.25f, 0.5f, 0.75f), "RT3 outside the quad %f,%f,%f, expected the LOADed pre-fill", c[0], c[1], c[2]);
   readpx(&s[0], 8, 8, c); CHECK(near4(c, 0, 0, 0), "RT0 outside the quad %f,%f,%f", c[0], c[1], c[2]);
   tilemap("mrt4-rt1-bgra8", &s[1]);
   tilemap("mrt4-rt2-rgba16f", &s[2]);
   tilemap("mrt4-rt3-load-rgba32f", &s[3]);
   for (unsigned i = 0; i < 4; i++) scene_free(&s[i]);
   return 1;
}

/* independentBlend (advertised in mali_physical_device.c) with four attachments. Each attachment is CLEARed and drawn to a
 * different colour; RT1..RT3 each blend with their own state: RT1 ADD ONE/ONE on RGB only, RT2 CONSTANT_COLOR /
 * CONSTANT_ALPHA with constants RT0 does not have, RT3 REVERSE_SUBTRACT with the write mask R|B (G keeps its clear). RT1
 * is RGBA16F, so its bytes per pixel differ from RT0's: an address or a row stride taken from RT0 cannot pass. Applying
 * attachment 0's state to all of them (the behaviour before this case) gives RT1 0.2,0.6,0.9; RT2 0.7,0.7,0.25 and RT3
 * 0,0.9,0.3 instead of the values below. */
static int
case_blend(void)
{
   struct scene s[4];
   const float z[4] = {0, 0, 0, 1};
   const VkFormat fm[4] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
                           VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
   for (unsigned i = 0; i < 4; i++)
      CHECK(scene_init(&s[i], 64, 64, fm[i], 0, z, 1.0f), "init rt%u", i);
   CHECK(s[1].pitch > s[0].pitch, "RT1 must be larger per row than RT0 (8 vs 4 bytes/px): %u vs %u", s[1].pitch, s[0].pitch);
   const struct scene *rt[4] = {&s[0], &s[1], &s[2], &s[3]};
   struct mali_att at[4] = {
      {.clear = true, .clear_color = {0.10f, 0.20f, 0.30f, 1}},
      {.clear = true, .clear_color = {0.50f, 0.25f, 0.125f, 1}},
      {.clear = true, .clear_color = {0.05f, 0.15f, 0.25f, 1}},
      {.clear = true, .clear_color = {0.90f, 0.90f, 0.90f, 1}},
   };
   struct mali_fixed fx;
   memset(&fx, 0, sizeof fx);
   fx.set = true;
   fx.wmask = 0xf;                        /* RT0: no blend, all channels */
   fx.xatt[0] = (struct mali_fx_att){.en = true, .wmask = 0x7,
                                     .src_c = VK_BLEND_FACTOR_ONE, .dst_c = VK_BLEND_FACTOR_ONE,
                                     .src_a = VK_BLEND_FACTOR_ONE, .dst_a = VK_BLEND_FACTOR_ONE,
                                     .op_c = VK_BLEND_OP_ADD, .op_a = VK_BLEND_OP_ADD};
   fx.xatt[1] = (struct mali_fx_att){.en = true, .wmask = 0xf,
                                     .src_c = VK_BLEND_FACTOR_CONSTANT_COLOR, .dst_c = VK_BLEND_FACTOR_ZERO,
                                     .src_a = VK_BLEND_FACTOR_CONSTANT_ALPHA, .dst_a = VK_BLEND_FACTOR_ZERO,
                                     .op_c = VK_BLEND_OP_ADD, .op_a = VK_BLEND_OP_ADD,
                                     .bconst = {0.5f, 0.25f, 0.125f, 0.5f}};
   fx.xatt[2] = (struct mali_fx_att){.en = true, .wmask = 0x5,
                                     .src_c = VK_BLEND_FACTOR_ONE, .dst_c = VK_BLEND_FACTOR_ONE,
                                     .src_a = VK_BLEND_FACTOR_ONE, .dst_a = VK_BLEND_FACTOR_ONE,
                                     .op_c = VK_BLEND_OP_REVERSE_SUBTRACT, .op_a = VK_BLEND_OP_ADD};
   const float vc[4] = {0, 0, 0, 0};      /* unused by fs_blend4 */
   CHECK(hand_frame(rt, 4, at, 0, 0, 64, 64, &PROG_BLEND, &key_blend_vs, &key_blend_fs, 0, 0, 1, 1, vc, &fx, 0.05f),
         "4-RT independentBlend pass");
   float c[4];
   readpx(&s[0], 32, 32, c); CHECK(near4(c, 0.8f, 0.1f, 0.4f), "RT0 (no blend) %f,%f,%f", c[0], c[1], c[2]);
   readpx(&s[1], 32, 32, c);
   CHECK(near4(c, 0.7f, 0.85f, 1.025f), "RT1 (ADD ONE/ONE) %f,%f,%f, expected dst+src = 0.7,0.85,1.025", c[0], c[1], c[2]);
   readpx(&s[2], 32, 32, c);
   CHECK(near4(c, 0.35f, 0.175f, 0.03125f), "RT2 (CONSTANT_COLOR) %f,%f,%f, expected 0.35,0.175,0.03125", c[0], c[1], c[2]);
   CHECK(fabsf(c[3] - 0.5f) < 0.02f, "RT2 alpha %f, expected CONSTANT_ALPHA 0.5", c[3]);
   readpx(&s[3], 32, 32, c);
   CHECK(near4(c, 0.9f, 0.9f, 0.6f), "RT3 (REVERSE_SUBTRACT, mask R|B) %f,%f,%f, expected 0.9,0.9,0.6", c[0], c[1], c[2]);
   tilemap("mrt-blend-rt1-rgba16f", &s[1]);
   tilemap("mrt-blend-rt2-constant", &s[2]);
   tilemap("mrt-blend-rt3-subtract", &s[3]);
   for (unsigned i = 0; i < 4; i++) scene_free(&s[i]);
   return 1;
}

/* colour loadOp LOAD of a partially covered pass: the quad covers the bottom-right quarter only, everything else must
 * keep the previous pass's contents (blue, with a green stripe from an earlier LOAD pass). */
static int
case_load(void)
{
   struct scene s;
   const float z[4] = {0, 0, 0, 1};
   CHECK(scene_init(&s, 128, 128, VK_FORMAT_R8G8B8A8_UNORM, 0, z, 1.0f), "init");
   const struct scene *rt[1] = {&s};
   const float green[4] = {0, 1, 0, 1}, red[4] = {1, 0, 0, 1};
   struct mali_att clr = {.clear = true, .clear_color = {0, 0, 1, 1}}, load = {0};
   CHECK(hand_frame(rt, 1, &clr, 0, 0, 128, 128, &PROG_MRT, &key_mrt_vs, &key_mrt_fs, 0, 0, 0, 0, green, NULL, 0.0f), "clear pass");
   CHECK(hand_frame(rt, 1, &load, 0, 0, 128, 128, &PROG_MRT, &key_mrt_vs, &key_mrt_fs, -0.25f, -1, 0.25f, 1, green, NULL, 0.0f), "stripe pass");
   CHECK(hand_frame(rt, 1, &load, 0, 0, 128, 128, &PROG_MRT, &key_mrt_vs, &key_mrt_fs, 0, 0, 1, 1, red, NULL, 0.0f), "partial pass");
   float c[4];
   readpx(&s, 10, 10, c); CHECK(near4(c, 0, 0, 1), "outside both quads %f,%f,%f, expected the cleared blue (LOAD lost it)", c[0], c[1], c[2]);
   readpx(&s, 64, 20, c); CHECK(near4(c, 0, 1, 0), "stripe %f,%f,%f, expected green from the second pass", c[0], c[1], c[2]);
   readpx(&s, 100, 100, c); CHECK(near4(c, 1, 0, 0), "partial quad %f,%f,%f, expected red", c[0], c[1], c[2]);
   tilemap("load-partial", &s);
   scene_free(&s);
   return 1;
}

/* The AFBC-class failure: the image is rendered in two vertical halves, rows 0..63 then rows 64..127 (render areas, second
 * pass LOADs). Memory starts zeroed, so a second half that is skipped reads back black; the checks (and the golden) fail. */
static int
case_halves(void)
{
   struct scene s;
   const float z[4] = {0, 0, 0, 1};
   CHECK(scene_init(&s, 128, 128, VK_FORMAT_R8G8B8A8_UNORM, 0, z, 1.0f), "init");
   const struct scene *rt[1] = {&s};
   struct mali_att first = {.clear = true, .clear_color = {0.1f, 0.1f, 0.1f, 1}}, second = {0};
   CHECK(hand_frame(rt, 1, &first, 0, 0, 128, 64, &PROG_GRAD, &key_grad_vs, &key_grad_fs, -1, -1, 1, 1, NULL, NULL, 0.0f), "rows 0..63");
   CHECK(hand_frame(rt, 1, &second, 0, 64, 128, 64, &PROG_GRAD, &key_grad_vs, &key_grad_fs, -1, -1, 1, 1, NULL, NULL, 0.0f), "rows 64..127");
   float c[4];
   for (unsigned y = 4; y < 128; y += 20) {
      readpx(&s, 100, y, c);
      CHECK(c[0] + c[1] + c[2] > 0.3f, "row %u is black: the %s half was never written", y, y < 64 ? "first" : "second (rows 64..127)");
   }
   float lo[4], hi[4];
   readpx(&s, 100, 63, lo); readpx(&s, 100, 64, hi);
   CHECK(fabsf(lo[1] - hi[1]) < 0.05f, "rows 63/64 of one gradient differ by %f in G: a seam at the half boundary", fabsf(lo[1] - hi[1]));
   tilemap("halves", &s);
   scene_free(&s);
   return 1;
}

int
main(int argc, char **argv)
{
   const char *only = NULL;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--spv-dir") && i + 1 < argc) g_spv = argv[++i];
      else if (!strcmp(argv[i], "--case") && i + 1 < argc) only = argv[++i];
      else { printf("usage: t_soft_frame [--spv-dir dir] [--case name]\n"); return 2; }
   }
   gipa = (PFN_vkGetInstanceProcAddr)vk_icdGetInstanceProcAddr(NULL, "vkGetInstanceProcAddr");
   PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkInstanceCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
   if (ci(&ic, NULL, &inst) != VK_SUCCESS) { printf("FAIL: vkCreateInstance\n"); return 1; }
   PFN_vkEnumeratePhysicalDevices en = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
   uint32_t n = 0; en(inst, &n, NULL);
   VkPhysicalDevice pds[4];
   if (n > 4) n = 4;
   en(inst, &n, pds);
   if (!n) { printf("FAIL: no physical device (MALI_BACKEND=fake?)\n"); return 1; }
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qc};
   PFN_vkCreateDevice cd = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
   if (cd(pds[0], &dc, NULL, &dev) != VK_SUCCESS) { printf("FAIL: vkCreateDevice\n"); return 1; }
   gdpa = (PFN_vkVoidFunction(*)(VkDevice, const char *))gipa(inst, "vkGetDeviceProcAddr");
#define F(n) p_##n = (PFN_vk##n)gdpa(dev, "vk" #n); if (!p_##n) { printf("FAIL: no vk" #n "\n"); return 1; }
   F(GetDeviceQueue); F(DeviceWaitIdle) F(CreateImage); F(GetImageMemoryRequirements); F(GetImageSubresourceLayout)
   F(BindImageMemory); F(DestroyImage); F(AllocateMemory); F(FreeMemory); F(MapMemory); F(CreateImageView)
   F(DestroyImageView); F(CreateBuffer); F(GetBufferMemoryRequirements); F(BindBufferMemory); F(DestroyBuffer)
   F(CreateRenderPass); F(DestroyRenderPass); F(CreateFramebuffer); F(DestroyFramebuffer); F(CreateShaderModule)
   F(DestroyShaderModule); F(CreatePipelineLayout); F(DestroyPipelineLayout); F(CreateGraphicsPipelines)
   F(DestroyPipeline); F(CreateCommandPool); F(DestroyCommandPool); F(AllocateCommandBuffers)
   F(FreeCommandBuffers); F(BeginCommandBuffer); F(EndCommandBuffer); F(CmdBeginRenderPass); F(CmdEndRenderPass)
   F(CmdBindPipeline); F(CmdBindVertexBuffers); F(CmdDraw); F(QueueSubmit); F(CreateFence); F(WaitForFences)
   F(DestroyFence); F(CmdSetViewport); F(CmdSetScissor)
#undef F
   p_GetDeviceQueue(dev, 0, 0, &q);

   int ok = 1;
   if (!only || !strcmp(only, "rgba8")) ok &= case_rgba8();
   if (!only || !strcmp(only, "formats")) ok &= case_formats();
   if (!only || !strcmp(only, "render-area")) ok &= case_render_area();
   if (!only || !strcmp(only, "mrt")) ok &= case_mrt();
   if (!only || !strcmp(only, "mrt4")) ok &= case_mrt4();
   if (!only || !strcmp(only, "mrt-blend")) ok &= case_blend();
   if (!only || !strcmp(only, "load")) ok &= case_load();
   if (!only || !strcmp(only, "halves")) ok &= case_halves();
   p_DeviceWaitIdle(dev);
   printf("%s\n", ok && !g_fail ? "PASS" : "FAILED");
   return ok && !g_fail ? 0 : 1;
}
