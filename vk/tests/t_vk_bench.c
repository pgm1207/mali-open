/* SPDX-License-Identifier: MIT */
/* hwbench: is a frame limited by memory bandwidth, by the shader cores, or by the tiler?
 *
 * Three microbenchmarks, all recorded through our own Vulkan API (loader-free: dlopens the ICD like
 * t_vk_draw --direct), so the same binary runs on the fake backend (plumbing only) and on a phone:
 *
 *   bw    (a) full-screen fragment shader, one NEAREST fetch per pixel from a large texture, 4-byte write.
 *             Sweep the texture size at a fixed 512x512 target: while the footprint fits in the GPU's caches
 *             the time is flat, and it climbs once the reads have to reach DRAM.
 *             Then sweep the target size at a fixed 2K texture: the per-pixel cost tells us whether the
 *             fragment pipe is the limit at a given resolution.
 *   alu   (b) 64 dependent FMAs per pixel plus one fetch from a 4x4 texture (always in cache). Sweep the
 *             target size: ns/pixel stays flat when the shader cores are the limit.
 *   geom  (c) high vertex counts made of 2-pixel triangles with a constant-colour fragment shader, so no
 *             shading and almost no rasterisation happens. Sweep the vertex count: ns/vertex flat = vertex
 *             processing limits, ms/frame flat while vertices explode = a fixed tiler/primitives limit.
 *
 * Every row reports wall ms/frame, the CPU submit cost, and (when the driver can) GPU time from
 * VK_QUERY_TYPE_TIMESTAMP written by the GPU around the frame loop. Fixed per-submit cost (kick, fence,
 * timestamp pair) is removed by measuring the same workload at f and 2f frames and taking the slope.
 *
 *   t_vk_bench --direct <icd.so> [--spv-dir shader] [--mode all|bw|alu|geom] [--frames N] [--quick]
 *              [--json] [--json-only] [--csv]
 *
 * Fake backend (MALI_BACKEND=fake): the timings are plumbing, not hardware; rows say so in "note", and the
 * GPU time is n/a because no GPU executes the timestamp store.
 */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_ROWS 64

#define DEV_FNS(X)                                                                                 \
   X(DestroyDevice) X(GetDeviceQueue) X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory)  \
   X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(QueueSubmit) X(DeviceWaitIdle)  \
   X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory)                  \
   X(CreateImageView) X(DestroyImageView) X(GetImageSubresourceLayout) X(CreateBuffer)             \
   X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(CreateShaderModule)       \
   X(DestroyShaderModule) X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateRenderPass)      \
   X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) X(CreateGraphicsPipelines)        \
   X(DestroyPipeline) X(CreateCommandPool) X(DestroyCommandPool) X(ResetCommandPool)                 \
   X(AllocateCommandBuffers) X(FreeCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer)         \
   X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdBindPipeline) X(CmdSetViewport) X(CmdSetScissor)   \
   X(CmdBindVertexBuffers) X(CmdPushConstants) X(CmdDraw) X(CreateDescriptorSetLayout)               \
   X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool)                    \
   X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateSampler) X(DestroySampler)              \
   X(CmdBindDescriptorSets) X(CreateQueryPool) X(DestroyQueryPool) X(CmdResetQueryPool)              \
   X(GetQueryPoolResults)

#define DECL(n) static PFN_vk##n n;
DEV_FNS(DECL)
static PFN_vkCmdWriteTimestamp2 CmdWriteTimestamp2;

static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memprops;

static const char *g_spv = "shader";
static int g_fake, g_frames = 12, g_quick;
static int g_json, g_json_only, g_csv;
static double g_ts_period_ns = 1.0; /* ns per GPU timestamp tick */
static int g_gpu_ts = 1;            /* the device can report GPU time (until proven otherwise) */

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
   if (!buf || fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return VK_NULL_HANDLE; }
   fclose(f);
   VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = buf};
   VkShaderModule m = VK_NULL_HANDLE;
   if (CreateShaderModule(dev, &ci, NULL, &m) != VK_SUCCESS)
      m = VK_NULL_HANDLE;
   free(buf);
   return m;
}

/* image + memory + view; *view may be NULL for an attachment we never sample */
static int
make_image(uint32_t w, uint32_t h, VkImageTiling tiling, VkImageUsageFlags usage, int want_host,
           VkImage *img, VkDeviceMemory *mem, VkImageView *view)
{
   VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                           .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1,
                           .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = tiling, .usage = usage,
                           .initialLayout = want_host ? VK_IMAGE_LAYOUT_PREINITIALIZED : VK_IMAGE_LAYOUT_UNDEFINED};
   if (CreateImage(dev, &ii, NULL, img) != VK_SUCCESS)
      return -1;
   VkMemoryRequirements mr;
   GetImageMemoryRequirements(dev, *img, &mr);
   VkMemoryPropertyFlags wf = want_host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
   int mt = memtype(mr.memoryTypeBits, wf);
   if (mt < 0 && want_host)
      mt = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                              .memoryTypeIndex = (uint32_t)(mt < 0 ? 0 : mt)};
   if (AllocateMemory(dev, &ma, NULL, mem) != VK_SUCCESS)
      return -1;
   if (BindImageMemory(dev, *img, *mem, 0) != VK_SUCCESS)
      return -1;
   if (!view)
      return 0;
   VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img,
                               .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   return CreateImageView(dev, &vi, NULL, view) == VK_SUCCESS ? 0 : -1;
}

enum { M_BW, M_ALU, M_GEOM };
static const char *const mode_name[] = {"bw", "alu", "geom"};

struct bench {
   int mode;
   uint32_t fbw, fbh, texw, texh, nverts;
   VkShaderModule vs, fs;
   VkPipelineLayout playout;
   VkRenderPass rp;
   VkPipeline pipe;
   VkImage col, tex;
   VkDeviceMemory colmem, texmem, vbmem;
   VkImageView colv, texv;
   VkFramebuffer fb;
   VkSampler smp;
   VkDescriptorSetLayout dsl;
   VkDescriptorPool dpool;
   VkDescriptorSet dset;
   VkBuffer vb;
   VkCommandPool pool;
   VkCommandBuffer cb;
   VkFence fence;
   VkQueryPool qp;
   VkImageSubresourceRange sr;
};

static int
bench_build(struct bench *b, int mode, uint32_t fbw, uint32_t fbh, uint32_t texw, uint32_t texh, uint32_t nverts)
{
   memset(b, 0, sizeof *b);
   b->mode = mode;
   b->fbw = fbw;
   b->fbh = fbh;
   b->texw = texw;
   b->texh = texh;
   b->nverts = nverts;
   b->sr = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

   if (make_image(fbw, fbh, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &b->col, &b->colmem, &b->colv))
      return -1;

   VkAttachmentDescription at = {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
                                 .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                 .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                 .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                 .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                              .pColorAttachments = &ar};
   VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
                                 .pAttachments = &at, .subpassCount = 1, .pSubpasses = &sp};
   if (CreateRenderPass(dev, &rpi, NULL, &b->rp) != VK_SUCCESS)
      return -1;
   VkImageView av[] = {b->colv};
   VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = b->rp,
                                  .attachmentCount = 1, .pAttachments = av, .width = fbw, .height = fbh, .layers = 1};
   if (CreateFramebuffer(dev, &fbi, NULL, &b->fb) != VK_SUCCESS)
      return -1;

   const char *vsn = mode == M_GEOM ? "geom.vert.spv" : "quad.vert.spv";
   const char *fsn = mode == M_GEOM ? "geom.frag.spv" : mode == M_BW ? "bw.frag.spv" : "alu.frag.spv";
   b->vs = load_spv(vsn);
   b->fs = load_spv(fsn);
   if (!b->vs || !b->fs)
      return -1;

   VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
   if (mode != M_GEOM) {
      VkDescriptorSetLayoutBinding db = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
      VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                             .bindingCount = 1, .pBindings = &db};
      if (CreateDescriptorSetLayout(dev, &dli, NULL, &b->dsl) != VK_SUCCESS)
         return -1;
      pli.setLayoutCount = 1;
      pli.pSetLayouts = &b->dsl;
   }
   if (CreatePipelineLayout(dev, &pli, NULL, &b->playout) != VK_SUCCESS)
      return -1;

   VkVertexInputBindingDescription vbind = {0, 8, VK_VERTEX_INPUT_RATE_VERTEX};
   VkVertexInputAttributeDescription vattr = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
   VkPipelineVertexInputStateCreateInfo vin = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   if (mode == M_GEOM) {
      vin.vertexBindingDescriptionCount = 1;
      vin.pVertexBindingDescriptions = &vbind;
      vin.vertexAttributeDescriptionCount = 1;
      vin.pVertexAttributeDescriptions = &vattr;
   }
   VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                           .viewportCount = 1, .scissorCount = 1};
   VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
   VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                              .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
   VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                             .attachmentCount = 1, .pAttachments = &cba};
   VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
   VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                          .dynamicStateCount = 2, .pDynamicStates = dyn};
   VkPipelineShaderStageCreateInfo st[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = b->vs, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = b->fs, .pName = "main"},
   };
   VkGraphicsPipelineCreateInfo gpi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                       .pStages = st, .pVertexInputState = &vin, .pInputAssemblyState = &ia,
                                       .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                       .pColorBlendState = &cb, .pDynamicState = &ds, .layout = b->playout,
                                       .renderPass = b->rp, .subpass = 0};
   if (CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &b->pipe) != VK_SUCCESS)
      return -1;

   if (mode != M_GEOM) {
      if (make_image(texw, texh, VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_SAMPLED_BIT, 1, &b->tex, &b->texmem, &b->texv))
         return -1;
      /* fill the texture through the linear layout: position-dependent bytes so it is not compressible */
      uint8_t *tp = NULL;
      if (MapMemory(dev, b->texmem, 0, VK_WHOLE_SIZE, 0, (void **)&tp) != VK_SUCCESS)
         return -1;
      VkImageSubresource sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
      VkSubresourceLayout tl;
      GetImageSubresourceLayout(dev, b->tex, &sr, &tl);
      for (uint32_t j = 0; j < texh; j++) {
         uint8_t *row = tp + tl.offset + j * tl.rowPitch;
         for (uint32_t i = 0; i < texw; i++) {
            uint32_t v = i * 2654435761u ^ j * 40503u;
            row[i * 4 + 0] = (uint8_t)v;
            row[i * 4 + 1] = (uint8_t)(v >> 8);
            row[i * 4 + 2] = (uint8_t)(v >> 16);
            row[i * 4 + 3] = 255;
         }
      }
      UnmapMemory(dev, b->texmem);
      VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                                .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
      if (CreateSampler(dev, &si, NULL, &b->smp) != VK_SUCCESS)
         return -1;
      VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
      VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                        .poolSizeCount = 1, .pPoolSizes = &ps};
      if (CreateDescriptorPool(dev, &dpi, NULL, &b->dpool) != VK_SUCCESS)
         return -1;
      VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                         .descriptorPool = b->dpool, .descriptorSetCount = 1, .pSetLayouts = &b->dsl};
      if (AllocateDescriptorSets(dev, &dai, &b->dset) != VK_SUCCESS)
         return -1;
      VkDescriptorImageInfo di = {b->smp, b->texv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = b->dset, .dstBinding = 0,
                                .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                .pImageInfo = &di};
      UpdateDescriptorSets(dev, 1, &w, 0, NULL);
   } else {
      /* tiny triangles: 2 px wide, spread over the target so the tiler has to bin them everywhere */
      size_t bytes = (size_t)nverts * 8;
      VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
                               .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
      if (CreateBuffer(dev, &bi, NULL, &b->vb) != VK_SUCCESS)
         return -1;
      VkMemoryRequirements mr;
      GetBufferMemoryRequirements(dev, b->vb, &mr);
      VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                 .memoryTypeIndex = (uint32_t)memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
      if (AllocateMemory(dev, &ma, NULL, &b->vbmem) != VK_SUCCESS || BindBufferMemory(dev, b->vb, b->vbmem, 0) != VK_SUCCESS)
         return -1;
      float *vv = NULL;
      if (MapMemory(dev, b->vbmem, 0, VK_WHOLE_SIZE, 0, (void **)&vv) != VK_SUCCESS)
         return -1;
      uint32_t s = 42;
      for (uint32_t t = 0; t < nverts / 3; t++) {
         s = s * 1664525u + 1013904223u;
         float cx = (float)((s >> 8) & 0xffff) / 65535.0f * 2.0f - 1.0f;
         s = s * 1664525u + 1013904223u;
         float cy = (float)((s >> 8) & 0xffff) / 65535.0f * 2.0f - 1.0f;
         float dx = 2.0f / (float)fbw, dy = 2.0f / (float)fbh;
         vv[t * 6 + 0] = cx; vv[t * 6 + 1] = cy;
         vv[t * 6 + 2] = cx + dx; vv[t * 6 + 3] = cy;
         vv[t * 6 + 4] = cx; vv[t * 6 + 5] = cy + dy;
      }
      UnmapMemory(dev, b->vbmem);
   }

   VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
   if (CreateCommandPool(dev, &cpi, NULL, &b->pool) != VK_SUCCESS)
      return -1;
   VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = b->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   if (AllocateCommandBuffers(dev, &cbai, &b->cb) != VK_SUCCESS)
      return -1;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   if (CreateFence(dev, &fi, NULL, &b->fence) != VK_SUCCESS)
      return -1;
   if (g_gpu_ts && CmdWriteTimestamp2) {
      VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                  .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2};
      if (CreateQueryPool(dev, &qi, NULL, &b->qp) != VK_SUCCESS) {
         b->qp = VK_NULL_HANDLE;
         g_gpu_ts = 0;
      }
   }
   return 0;
}

static void
bench_destroy(struct bench *b)
{
   if (b->fence) DestroyFence(dev, b->fence, NULL);
   if (b->cb) FreeCommandBuffers(dev, b->pool, 1, &b->cb);
   if (b->pool) DestroyCommandPool(dev, b->pool, NULL);
   if (b->qp) DestroyQueryPool(dev, b->qp, NULL);
   if (b->dpool) DestroyDescriptorPool(dev, b->dpool, NULL);
   if (b->dsl) DestroyDescriptorSetLayout(dev, b->dsl, NULL);
   if (b->smp) DestroySampler(dev, b->smp, NULL);
   if (b->vb) DestroyBuffer(dev, b->vb, NULL);
   if (b->vbmem) FreeMemory(dev, b->vbmem, NULL);
   if (b->texv) DestroyImageView(dev, b->texv, NULL);
   if (b->tex) DestroyImage(dev, b->tex, NULL);
   if (b->texmem) FreeMemory(dev, b->texmem, NULL);
   if (b->pipe) DestroyPipeline(dev, b->pipe, NULL);
   if (b->playout) DestroyPipelineLayout(dev, b->playout, NULL);
   if (b->vs) DestroyShaderModule(dev, b->vs, NULL);
   if (b->fs) DestroyShaderModule(dev, b->fs, NULL);
   if (b->fb) DestroyFramebuffer(dev, b->fb, NULL);
   if (b->rp) DestroyRenderPass(dev, b->rp, NULL);
   if (b->colv) DestroyImageView(dev, b->colv, NULL);
   if (b->col) DestroyImage(dev, b->col, NULL);
   if (b->colmem) FreeMemory(dev, b->colmem, NULL);
}

struct run {
   double wall_ms, cpu_ms, gpu_ms; /* gpu_ms < 0: no GPU time available */
};

/* Submit `frames` copies of the workload in one command buffer; fixed submit cost is the same at f and 2f. */
static int
bench_run(struct bench *b, uint32_t frames, struct run *out)
{
   ResetCommandPool(dev, b->pool, 0);
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   if (BeginCommandBuffer(b->cb, &bi) != VK_SUCCESS)
      return -1;
   if (b->qp) CmdResetQueryPool(b->cb, b->qp, 0, 2);
   if (b->qp) CmdWriteTimestamp2(b->cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, b->qp, 0);
   VkClearValue clear = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}};
   VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = b->rp,
                                .framebuffer = b->fb,
                                .renderArea = {{0, 0}, {b->fbw, b->fbh}}, .clearValueCount = 1, .pClearValues = &clear};
   VkViewport vport = {0, 0, (float)b->fbw, (float)b->fbh, 0.0f, 1.0f};
   VkRect2D scissor = {{0, 0}, {b->fbw, b->fbh}};
   float pc[4] = {1.0f, 1.0f, 0.0f, 0.0f};
   for (uint32_t i = 0; i < frames; i++) {
      CmdBeginRenderPass(b->cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
      CmdBindPipeline(b->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, b->pipe);
      CmdSetViewport(b->cb, 0, 1, &vport);
      CmdSetScissor(b->cb, 0, 1, &scissor);
      CmdPushConstants(b->cb, b->playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
      if (b->mode == M_GEOM) {
         VkDeviceSize off = 0;
         CmdBindVertexBuffers(b->cb, 0, 1, &b->vb, &off);
         CmdDraw(b->cb, b->nverts, 1, 0, 0);
      } else {
         CmdBindDescriptorSets(b->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, b->playout, 0, 1, &b->dset, 0, NULL);
         CmdDraw(b->cb, 6, 1, 0, 0);
      }
      CmdEndRenderPass(b->cb);
   }
   if (b->qp) CmdWriteTimestamp2(b->cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, b->qp, 1);
   if (EndCommandBuffer(b->cb) != VK_SUCCESS)
      return -1;

   ResetFences(dev, 1, &b->fence);
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &b->cb};
   double t0 = now_us();
   if (QueueSubmit(queue, 1, &si, b->fence) != VK_SUCCESS)
      return -1;
   double t1 = now_us();
   WaitForFences(dev, 1, &b->fence, VK_TRUE, UINT64_MAX);
   double t2 = now_us();

   out->wall_ms = (t2 - t0) / 1000.0;
   out->cpu_ms = (t1 - t0) / 1000.0;
   out->gpu_ms = -1.0;
   if (b->qp) {
      uint64_t ts[2] = {0, 0};
      VkResult r = GetQueryPoolResults(dev, b->qp, 0, 2, sizeof ts, ts, sizeof(uint64_t),
                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
      if (r == VK_SUCCESS && ts[1] > ts[0])
         out->gpu_ms = (double)(ts[1] - ts[0]) * g_ts_period_ns / 1e6;
      else
         g_gpu_ts = 0; /* fake backend: no GPU executes the timestamp store */
   }
   return 0;
}

struct row {
   const char *mode, *case_name;
   uint32_t fbw, fbh, texw, texh, nverts, frames;
   double wall_ms, cpu_ms, gpu_ms, ns_unit;
   double mb_footprint, req_gbps;
   char note[96];
};

static struct row g_rows[MAX_ROWS];
static int g_nrows;

static void
measure(int mode, const char *case_name, uint32_t fbw, uint32_t fbh, uint32_t texw, uint32_t texh, uint32_t nverts)
{
   struct bench b;
   if (bench_build(&b, mode, fbw, fbh, texw, texh, nverts)) {
      printf("{\"fail\":\"build %s %ux%u tex %ux%u verts %u\"}\n", mode_name[mode], fbw, fbh, texw, texh, nverts);
      return;
   }
   struct run a = {0}, c = {0};
   uint32_t f = (uint32_t)g_frames;
   if (bench_run(&b, f, &a) || bench_run(&b, 2 * f, &c)) {
      printf("{\"fail\":\"run %s\"}\n", mode_name[mode]);
      bench_destroy(&b);
      return;
   }
   /* slope: the second submission has 2f frames, so subtract the identical fixed cost */
   struct row *r = &g_rows[g_nrows < MAX_ROWS ? g_nrows++ : MAX_ROWS - 1];
   memset(r, 0, sizeof *r);
   r->mode = mode_name[mode];
   r->case_name = case_name;
   r->fbw = fbw;
   r->fbh = fbh;
   r->texw = texw;
   r->texh = texh;
   r->nverts = nverts;
   r->frames = 2 * f;
   r->wall_ms = (c.wall_ms - a.wall_ms) / f;
   r->cpu_ms = (c.cpu_ms - a.cpu_ms) / f;
   r->gpu_ms = (c.gpu_ms >= 0 && a.gpu_ms >= 0) ? (c.gpu_ms - a.gpu_ms) / f : -1.0;
   if (r->wall_ms < 0) r->wall_ms = c.wall_ms / (2 * f);
   if (r->cpu_ms < 0) r->cpu_ms = c.cpu_ms / (2 * f);
   double units = mode == M_GEOM ? (double)nverts : (double)fbw * fbh;
   r->ns_unit = units > 0 ? r->wall_ms * 1e6 / units : 0;
   if (mode != M_GEOM) {
      r->mb_footprint = (double)texw * texh * 4.0 / (1024.0 * 1024.0);
      r->req_gbps = r->wall_ms > 0 ? (double)fbw * fbh * 4.0 / (r->wall_ms * 1e6) * 1000.0 : 0;
   }
   if (g_fake)
      snprintf(r->note, sizeof r->note, "fake backend: plumbing only");
   else if (mode == M_BW && r->mb_footprint < 2.0)
      snprintf(r->note, sizeof r->note, "fits L2");
   bench_destroy(&b);
}

static void
print_table(void)
{
   printf("# hwbench: %d rows, %s\n", g_nrows, g_fake ? "fake backend (timings are plumbing, not hardware)" : "wall time incl. submit");
   printf("%-4s %-6s %-10s %-10s %-9s %7s %9s %8s %8s %8s %7s\n",
          "mode", "case", "target", "texture", "vertices", "frames", "wall ms/f", "cpu ms", "gpu ms/f", "ns/unit", "texture MB");
   for (int i = 0; i < g_nrows; i++) {
      const struct row *r = &g_rows[i];
      char fb[24], tx[24], vt[24];
      snprintf(fb, sizeof fb, "%ux%u", r->fbw, r->fbh);
      if (r->mode[0] == 'g') snprintf(tx, sizeof tx, "-"); else snprintf(tx, sizeof tx, "%ux%u", r->texw, r->texh);
      if (r->nverts) snprintf(vt, sizeof vt, "%u", r->nverts); else snprintf(vt, sizeof vt, "-");
      char gpu[16];
      if (r->gpu_ms < 0) snprintf(gpu, sizeof gpu, "n/a"); else snprintf(gpu, sizeof gpu, "%.3f", r->gpu_ms);
      printf("%-4s %-6s %-10s %-10s %-9s %7u %9.3f %8.3f %8s %8.2f %7.2f%s\n", r->mode, r->case_name, fb, tx, vt,
             r->frames, r->wall_ms, r->cpu_ms, gpu, r->ns_unit, r->mb_footprint,
             r->note[0] ? "  " : "");
      if (r->note[0])
         printf("     note: %s\n", r->note);
   }
   printf("# ns/unit: bw/alu = ns per pixel, geom = ns per vertex. gpu ms/f comes from VK_QUERY_TYPE_TIMESTAMP\n"
          "# written by the GPU around the frame loop; n/a = the backend has no timestamp query (fake).\n");
}

static void
print_json(void)
{
   for (int i = 0; i < g_nrows; i++) {
      const struct row *r = &g_rows[i];
      char gpu[16];
      if (r->gpu_ms < 0) snprintf(gpu, sizeof gpu, "null"); else snprintf(gpu, sizeof gpu, "%.4f", r->gpu_ms);
      printf("{\"bench\":\"%s\",\"case\":\"%s\",\"fb\":\"%ux%u\",\"tex\":\"%ux%u\",\"verts\":%u,\"frames\":%u,"
             "\"wall_ms\":%.4f,\"cpu_ms\":%.4f,\"gpu_ms\":%s,\"unit\":\"%s\",\"n_unit\":%u,\"ns_unit\":%.3f,"
             "\"tex_mb\":%.2f,\"read_gbps\":%.2f,\"fake\":%s}\n",
             r->mode, r->case_name, r->fbw, r->fbh, r->texw, r->texh, r->nverts, r->frames, r->wall_ms, r->cpu_ms,
             gpu, r->mode[0] == 'g' ? "vertex" : "pixel",
             r->mode[0] == 'g' ? r->nverts : r->fbw * r->fbh, r->ns_unit, r->mb_footprint, r->req_gbps,
             g_fake ? "true" : "false");
   }
}

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--spv-dir") && i + 1 < argc) g_spv = argv[++i];
      else if (!strcmp(argv[i], "--frames") && i + 1 < argc) g_frames = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--quick")) g_quick = 1;
      else if (!strcmp(argv[i], "--json")) g_json = 1;
      else if (!strcmp(argv[i], "--json-only")) { g_json = 1; g_json_only = 1; }
      else if (!strcmp(argv[i], "--csv")) g_csv = 1;
      else { printf("usage: t_vk_bench --direct icd.so [--spv-dir dir] [--frames N] [--quick] [--json|--json-only]\n"); return 2; }
   }
   if (!icd) { printf("usage: t_vk_bench --direct icd.so ...\n"); return 2; }
   const char *bk = getenv("MALI_BACKEND");
   g_fake = bk && !strcmp(bk, "fake");
   if (g_frames < 1) g_frames = 12;
   if (g_quick && g_frames > 4) g_frames = 4;

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
   PFN_vkEnumeratePhysicalDevices EPD = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceMemoryProperties GMP = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
   PFN_vkGetPhysicalDeviceProperties GPP = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
   PFN_vkCreateDevice CreateDevice = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
   PFN_vkGetDeviceProcAddr GDPA = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
   PFN_vkDestroyInstance DestroyInstance = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
   uint32_t cnt = 1;
   VkResult r = EPD(inst, &cnt, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || cnt < 1) { printf("{\"fail\":\"no device\"}\n"); return 1; }
   VkPhysicalDeviceProperties pp;
   GPP(pd, &pp);
   GMP(pd, &memprops);
   g_ts_period_ns = pp.limits.timestampPeriod > 0 ? pp.limits.timestampPeriod : 1.0;
   printf("# device: %s\n", pp.deviceName);

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
   if (CreateDevice(pd, &di, NULL, &dev) != VK_SUCCESS) { printf("{\"fail\":\"CreateDevice\"}\n"); return 1; }
#define LOAD(n) n = (PFN_vk##n)GDPA(dev, "vk" #n); if (!n) { printf("{\"fail\":\"missing vk" #n "\"}\n"); return 1; }
   DEV_FNS(LOAD)
   CmdWriteTimestamp2 = (PFN_vkCmdWriteTimestamp2)GDPA(dev, "vkCmdWriteTimestamp2");
   if (!CmdWriteTimestamp2)
      g_gpu_ts = 0;
   GetDeviceQueue(dev, 0, 0, &queue);

   static const int bw_tex[] = {128, 256, 512, 1024, 2048, 4096};
   static const int bw_fb[][2] = {{256, 256}, {512, 512}, {1024, 1024}, {1280, 720}, {1920, 1080}};
   static const int alu_fb[][2] = {{256, 256}, {512, 512}, {1024, 1024}, {1280, 720}, {1920, 1080}};
   static const uint32_t geom_v[] = {3u * 341, 4096, 16384, 65536, 262144, 1048576};

   if (g_quick) {
      /* one row per benchmark: enough to prove the plumbing on the fake backend, ~0.2 s on a phone */
      measure(M_BW, "tex", 512, 512, 128, 128, 0);
      measure(M_BW, "tex", 512, 512, 4096, 4096, 0);
      measure(M_ALU, "pixels", 256, 256, 4, 4, 0);
      measure(M_GEOM, "verts", 512, 512, 0, 0, 4096);
   } else {
      for (unsigned i = 0; i < sizeof bw_tex / sizeof bw_tex[0]; i++)
         measure(M_BW, "tex", 512, 512, (uint32_t)bw_tex[i], (uint32_t)bw_tex[i], 0);
      for (unsigned i = 0; i < sizeof bw_fb / sizeof bw_fb[0]; i++)
         measure(M_BW, "target", (uint32_t)bw_fb[i][0], (uint32_t)bw_fb[i][1], 2048, 2048, 0);
      for (unsigned i = 0; i < sizeof alu_fb / sizeof alu_fb[0]; i++)
         measure(M_ALU, "pixels", (uint32_t)alu_fb[i][0], (uint32_t)alu_fb[i][1], 4, 4, 0);
      for (unsigned i = 0; i < sizeof geom_v / sizeof geom_v[0]; i++)
         measure(M_GEOM, "verts", 512, 512, 0, 0, geom_v[i]);
   }

   if (!g_json_only) print_table();
   if (g_json) print_json();
   (void)g_csv;

   DeviceWaitIdle(dev);
   DestroyDevice(dev, NULL);
   DestroyInstance(inst, NULL);
   dlclose(lib);
   if (!g_nrows) { printf("{\"fail\":\"no rows measured\"}\n"); return 1; }
   return 0;
}
