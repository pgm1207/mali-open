/* SPDX-License-Identifier: MIT */
#include "mali_private.h"
#include "mali_queue.h"
#include "mali_format.h"
#include "mali_msaa.h"
#include "mali_compile.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "mali_entrypoints.h"
#include "util/list.h"
#include "vk_alloc.h"
#include "vk_util.h"
#include "vk_common_entrypoints.h"
#include <time.h>

/* M1 policy: every limit is the Vulkan 1.0 required minimum (spec table "Required Limits").
 * The milestone that implements a feature raises its limits to what the hardware does. */
static void
mali_get_properties(const struct mali_gpu_info *gpu, struct vk_properties *p)
{
   /* 4x is advertised only with MALI_EXPERIMENTAL_MSAA=1: without it 4x pipelines are rejected (mali_pipeline.c), so advertising it would lie. */
   const VkSampleCountFlags samples_1_4 = VK_SAMPLE_COUNT_1_BIT | (mali_experimental_msaa() ? VK_SAMPLE_COUNT_4_BIT : 0);

   *p = (struct vk_properties){
      .apiVersion = MALI_DEVICE_API_VERSION,
      .driverVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
      .vendorID = MALI_VENDOR_ID_ARM,
      .deviceID = gpu->gpu_id >> 16,
      .deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
      /* pipelineCacheUUID: zero until the shader cache exists (M6, from the build-id). */

      .maxImageDimension1D = 4096,
      .maxImageDimension2D = 4096,
      .maxImageDimension3D = 256,
      .maxImageDimensionCube = 4096,
      .maxImageArrayLayers = 256,
      .maxTexelBufferElements = 65536,
      .maxUniformBufferRange = 16384,
      .maxStorageBufferRange = 1u << 27,
      .maxPushConstantsSize = 256,   /* 64 FAU words. Hardware: 64 FAU slots = 128 words (Mesa pan_compiler.h PAN_MAX_PUSH, all known
                                      * Bifrost/Valhall); PanVK advertises 256 on the same GPUs. VS/reserved words 0..9 leave 118
                                      * words, FS (v13 blend words 4,5) and CS start at word 6/0. DXVK 3.1.x requires >= 256. */
      .maxMemoryAllocationCount = 4096,
      .maxSamplerAllocationCount = 4000,
      .bufferImageGranularity = 1,
      .maxBoundDescriptorSets = 8,   /* SRT table slots: 0 = driver set, 1..8 = user sets, 9 = dynamic-buffer table (16 in hardware) */
      .maxPerStageDescriptorSamplers = 16,
      .maxPerStageDescriptorUniformBuffers = 12,
      .maxPerStageDescriptorStorageBuffers = 4,
      .maxPerStageDescriptorSampledImages = 16,
      .maxPerStageDescriptorStorageImages = 4,
      .maxPerStageDescriptorInputAttachments = 4,
      .maxPerStageResources = 128,
      .maxDescriptorSetSamplers = 96,
      .maxDescriptorSetUniformBuffers = 72,
      .maxDescriptorSetUniformBuffersDynamic = 8,
      .maxDescriptorSetStorageBuffers = 24,
      .maxDescriptorSetStorageBuffersDynamic = 4,
      .maxDescriptorSetSampledImages = 96,
      .maxDescriptorSetStorageImages = 24,
      .maxDescriptorSetInputAttachments = 4,
      .maxVertexInputAttributes = 16,
      .maxVertexInputBindings = 16,
      .maxVertexInputAttributeOffset = 2047,
      .maxVertexInputBindingStride = 2048,
      .maxVertexOutputComponents = 64,
      .maxFragmentInputComponents = 64,
      .maxFragmentOutputAttachments = 4,
      .maxFragmentDualSrcAttachments = 1,   /* dualSrcBlend: blend shader, attachment 0 */
      .maxFragmentCombinedOutputResources = 4,
      .maxComputeSharedMemorySize = 16384,
      .maxComputeWorkGroupCount = {65535, 65535, 65535},
      .maxComputeWorkGroupInvocations = 128,
      .maxComputeWorkGroupSize = {128, 128, 64},
      .subPixelPrecisionBits = 4,
      .subTexelPrecisionBits = 4,
      .mipmapPrecisionBits = 4,
      .maxDrawIndexedIndexValue = 0xffffffffu,
      .maxDrawIndirectCount = 0xffff,   /* multiDrawIndirect: record_indirect/expand_indirect expand any drawCount x stride (t_vk_feat mdilimit); spec minimum with the feature is 2^16-1 */
      .maxSamplerLodBias = 2.0f,
      .maxSamplerAnisotropy = 16.0f,
      .maxViewports = 16,   /* multiViewport: state only, see .multiViewport */
      .maxViewportDimensions = {4096, 4096},
      .viewportBoundsRange = {-8192.0f, 8191.0f},
      .minMemoryMapAlignment = 64,
      .minTexelBufferOffsetAlignment = 256,
      .minUniformBufferOffsetAlignment = 256,
      .minStorageBufferOffsetAlignment = 256,
      .minTexelOffset = -8,
      .maxTexelOffset = 7,
      .minTexelGatherOffset = -8,   /* shaderImageGatherExtended: i4 narrow / i8 register texel offset; t_vk_feat gatherlim */
      .maxTexelGatherOffset = 7,
      .maxFramebufferWidth = 4096,
      .maxFramebufferHeight = 4096,
      .maxFramebufferLayers = 256,
      .framebufferColorSampleCounts = samples_1_4,
      .framebufferDepthSampleCounts = samples_1_4,
      .framebufferStencilSampleCounts = samples_1_4,
      .framebufferNoAttachmentsSampleCounts = samples_1_4,
      .maxColorAttachments = 4,   /* the Vulkan minimum; KNOWN GAP: only ONE colour attachment renders (mali_cmd.c "need exactly one colour attachment"), see vk/README.md "Known gaps" */
      .sampledImageColorSampleCounts = samples_1_4,
      .sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT,
      .sampledImageDepthSampleCounts = samples_1_4,
      .sampledImageStencilSampleCounts = samples_1_4,
      .storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT,
      .maxSampleMaskWords = 1,
      .maxClipDistances = 8,                 /* shaderClipDistance: VARYING_SLOT_CLIP_DIST0/1 (t_vk_feat clip8) */
      .maxCullDistances = 0,                 /* shaderCullDistance stays off: no cull lowering */
      .maxCombinedClipAndCullDistances = 8,
      .timestampPeriod = gpu->ts_hz ? (float)(1e9 / (double)gpu->ts_hz) : 1.0f,
      .discreteQueuePriorities = 2,
      .pointSizeRange = {1.0f, 1.0f},
      .lineWidthRange = {1.0f, 8.0f},
      .lineWidthGranularity = 0.125f,
      .standardSampleLocations = true,
      .optimalBufferCopyOffsetAlignment = 1,
      .optimalBufferCopyRowPitchAlignment = 1,
      .nonCoherentAtomSize = 64,
      .maxTimelineSemaphoreValueDifference = UINT64_MAX,
      /* descriptor indexing limits. Driver reality: binding flags are not interpreted, descriptors live in pool memory the GPU reads at
       * submit, so an update after bind is a plain write. Only sampled images/samplers advertise the UAB features, but every UAB limit
       * is reported (>= its non-UAB twin, Vulkan 1.2 required-limits table). Samplers/sampled images: a set must fit the 1M-slot
       * layout cap (mali_get_descriptor_set_layout_support) and the 1 GiB pool cap: 500000 * 32 B = 16 MB. t_vk_info "uab limits" */
      .maxUpdateAfterBindDescriptorsInAllPools = 500000, .shaderSampledImageArrayNonUniformIndexingNative = false,
      .quadDivergentImplicitLod = false,
      .maxPerStageDescriptorUpdateAfterBindSamplers = 500000, .maxPerStageDescriptorUpdateAfterBindSampledImages = 500000,
      .maxPerStageDescriptorUpdateAfterBindUniformBuffers = 12, .maxPerStageDescriptorUpdateAfterBindStorageBuffers = 4,
      .maxPerStageDescriptorUpdateAfterBindStorageImages = 4, .maxPerStageDescriptorUpdateAfterBindInputAttachments = 4,
      .maxPerStageUpdateAfterBindResources = 500000,
      .maxDescriptorSetUpdateAfterBindSamplers = 500000, .maxDescriptorSetUpdateAfterBindSampledImages = 500000,
      .maxDescriptorSetUpdateAfterBindUniformBuffers = 72, .maxDescriptorSetUpdateAfterBindUniformBuffersDynamic = 8,
      .maxDescriptorSetUpdateAfterBindStorageBuffers = 24, .maxDescriptorSetUpdateAfterBindStorageBuffersDynamic = 4,
      .maxDescriptorSetUpdateAfterBindStorageImages = 24, .maxDescriptorSetUpdateAfterBindInputAttachments = 4,
   };

#ifndef MALI_GIT_SHA
#define MALI_GIT_SHA "unknown"
#endif
   snprintf(p->deviceName, sizeof(p->deviceName), "%s (mali-own%s)", gpu->name,
            gpu->fake ? ", fake backend" : "");

   /* ---- Vulkan 1.1 ---- R4-M1: only what is true. No subgroup operation or stage is advertised yet
    * (R4-M4); the warp is 16 lanes on Valhall/5th gen (PanVK oracle on the same GPU reports 16, and Mesa's
    * pan_subgroup_size() is 16 for every arch >= 9: shader_present 0xee0057 is non-contiguous, the warp is not). */
   memcpy(p->driverUUID, "mali-own-vk\0\0\0\0\0", VK_UUID_SIZE);
   memcpy(p->deviceUUID, &gpu->gpu_id, sizeof(gpu->gpu_id));
   const unsigned subgroup_size = mali_subgroup_size_for_arch(gpu->arch_major);
   p->subgroupSize = subgroup_size;
   /* subgroup ops: same set as PanVK on this compiler (bifrost_nir.c lowers the rest); not in vertex shaders (PanVK: no helper-lane guarantees) */
   p->subgroupSupportedStages = VK_SHADER_STAGE_COMPUTE_BIT;   /* fragment stage only after a pixel test */
   p->subgroupSupportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
                                    VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT |
                                    VK_SUBGROUP_FEATURE_CLUSTERED_BIT | VK_SUBGROUP_FEATURE_QUAD_BIT;   /* t_vk_feat subgroup */
   p->subgroupQuadOperationsInAllStages = false;
   p->pointClippingBehavior = VK_POINT_CLIPPING_BEHAVIOR_ALL_CLIP_PLANES;
   p->maxPerSetDescriptors = 1024;
   p->maxMemoryAllocationSize = 1u << 30;

   /* ---- Vulkan 1.2 ---- driverID has no Mali-open value; PanVK is the nearest sibling (same compiler). */
   /* Deliberate: zink keys real workarounds on this id (implicit_sync off, renderpass optimisation for tilers, invalid linear modifier,
    * no msrtss); an unregistered id would lose them. Revisit only if a PanVK quirk turns out wrong for us. */
   p->driverID = VK_DRIVER_ID_MESA_PANVK;
   snprintf(p->driverName, sizeof(p->driverName), "mali-own");
   snprintf(p->driverInfo, sizeof(p->driverInfo), "mali-open %s (mali-own), not conformant", MALI_GIT_SHA);
   /* conformanceVersion stays 0.0.0.0: we do not claim conformance. */
   p->denormBehaviorIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
   p->roundingModeIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
   p->framebufferIntegerColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;

   /* ---- Vulkan 1.3 ---- */
   p->minSubgroupSize = subgroup_size;
   p->maxSubgroupSize = subgroup_size;
   p->requiredSubgroupSizeStages = VK_SHADER_STAGE_COMPUTE_BIT;   /* min == max == 16: the only size; t_vk_feat sgctl */
   p->maxComputeWorkgroupSubgroups = p->maxComputeWorkGroupInvocations / subgroup_size;
   p->storageTexelBufferOffsetAlignmentBytes = p->minTexelBufferOffsetAlignment;
   p->uniformTexelBufferOffsetAlignmentBytes = p->minTexelBufferOffsetAlignment;
   p->maxBufferSize = 1u << 30;
   p->provokingVertexModePerPipeline = false;   /* the bit lives in the per-render-pass FBD and tiler context */
   p->transformFeedbackPreservesTriangleFanProvokingVertex = false;
   p->lineSubPixelPrecisionBits = 4;   /* == subPixelPrecisionBits; t_vk_feat linerast */
   p->maxCustomBorderColorSamplers = 4000;   /* == maxSamplerAllocationCount: a custom colour costs nothing beyond the descriptor */
   p->maxVertexAttribDivisor = 0xfffffff;   /* NPOT divisor encoding is exact to 2^28 (t_resources attr_instance) */
   p->supportsNonZeroFirstInstance = false;   /* KHR property: stays false until t_vk_feat divisor (firstInstance cases) passes on hardware */
   p->maxInlineUniformBlockSize = 256; p->maxPerStageDescriptorInlineUniformBlocks = 4; p->maxDescriptorSetInlineUniformBlocks = 4;
   p->maxCombinedImageSamplerDescriptorCount = 1; /* maintenance6; blockTexelViewCompatibleMultipleLayers stays false */
   p->maxInlineUniformTotalSize = 256;   /* t_vk_feat inlineub */
   /* ---- shaderIntegerDotProduct properties: honest. No Bifrost/Valhall dot instruction is used (vtn + bifrost_nir.c lower to mul+add),
    * so every *Accelerated hint is false. The runtime writes the whole struct from vk_properties (t_vk_feat intdot_props fills it with 0xCD first). */
   p->integerDotProduct16BitMixedSignednessAccelerated = false;
   p->integerDotProduct16BitSignedAccelerated = false;
   p->integerDotProduct16BitUnsignedAccelerated = false;
   p->integerDotProduct32BitMixedSignednessAccelerated = false;
   p->integerDotProduct32BitSignedAccelerated = false;
   p->integerDotProduct32BitUnsignedAccelerated = false;
   p->integerDotProduct4x8BitPackedMixedSignednessAccelerated = false;
   p->integerDotProduct4x8BitPackedSignedAccelerated = false;
   p->integerDotProduct4x8BitPackedUnsignedAccelerated = false;
   p->integerDotProduct64BitMixedSignednessAccelerated = false;
   p->integerDotProduct64BitSignedAccelerated = false;
   p->integerDotProduct64BitUnsignedAccelerated = false;
   p->integerDotProduct8BitMixedSignednessAccelerated = false;
   p->integerDotProduct8BitSignedAccelerated = false;
   p->integerDotProduct8BitUnsignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating16BitMixedSignednessAccelerated = false;
   p->integerDotProductAccumulatingSaturating16BitSignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating16BitUnsignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating32BitMixedSignednessAccelerated = false;
   p->integerDotProductAccumulatingSaturating32BitSignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating32BitUnsignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating4x8BitPackedMixedSignednessAccelerated = false;
   p->integerDotProductAccumulatingSaturating4x8BitPackedSignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating4x8BitPackedUnsignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating64BitMixedSignednessAccelerated = false;
   p->integerDotProductAccumulatingSaturating64BitSignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating64BitUnsignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating8BitMixedSignednessAccelerated = false;
   p->integerDotProductAccumulatingSaturating8BitSignedAccelerated = false;
   p->integerDotProductAccumulatingSaturating8BitUnsignedAccelerated = false;
}

/* VK_EXT_physical_device_drm (vk/STEAM-GAPS.md, zink choose_pdev). The GPU is reached through kbase
 * (/dev/mali0), which is not a DRM device. DroidDeck's guest publishes that same char device as
 * /dev/dri/renderD<N> (a node with the same st_rdev; its graphics report says "render node for the
 * guest: /dev/dri/renderD105 -> /dev/mali0 (10:105)"), and that alias is the node an EGL/GBM client
 * opens and stats, so its major/minor are an honest identity for this physical device.
 * A node is reported only when stat() proves it is the very device the backend opened; a /dev/dri
 * node of another device (the guest's /dev/dri/card0 is 226:0) is never claimed, and with no such
 * alias the extension stays unadvertised (zink 26.0.8 then prints "failed to choose pdev" instead
 * of being sent to a device it cannot present from).
 * BL_DRM_STANDIN is the app's declaration of the GPU node (the same variable PanVK's kbase build
 * reads); MALI_DRM_DIR overrides the directory searched (tests, containers). */
struct mali_drm_node {
   dev_t render;
   dev_t primary;
};

static dev_t
mali_gpu_node_rdev(const struct mali_gpu_info *gpu)
{
   if (gpu->node_rdev)
      return (dev_t)gpu->node_rdev;

   const char *standin = getenv("BL_DRM_STANDIN");
   struct stat st;
   if (standin && *standin && stat(standin, &st) == 0)
      return st.st_rdev;

   return 0;
}

static bool
mali_drm_node_at(const char *dir, const char *name, dev_t gpu_rdev, dev_t *out)
{
   char path[PATH_MAX];
   struct stat st;

   if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
      return false;
   if (stat(path, &st) != 0 || !S_ISCHR(st.st_mode) || st.st_rdev != gpu_rdev)
      return false;

   *out = st.st_rdev;
   return true;
}

static bool
mali_drm_find_node(const struct mali_gpu_info *gpu, struct mali_drm_node *node)
{
   dev_t gpu_rdev = mali_gpu_node_rdev(gpu);
   *node = (struct mali_drm_node){0};
   if (!gpu_rdev)
      return false;

   const char *dir = getenv("MALI_DRM_DIR");
   if (!dir || !*dir)
      dir = "/dev/dri";

   DIR *d = opendir(dir);
   if (!d)
      return false;

   struct dirent *e;
   while ((e = readdir(d))) {
      if (!strncmp(e->d_name, "renderD", 7)) {
         if (!node->render)
            mali_drm_node_at(dir, e->d_name, gpu_rdev, &node->render);
      } else if (!strncmp(e->d_name, "card", 4)) {
         if (!node->primary)
            mali_drm_node_at(dir, e->d_name, gpu_rdev, &node->primary);
      }
   }
   closedir(d);

   return node->render || node->primary;
}

static VkResult
mali_physical_device_init(struct mali_physical_device *pdev, struct mali_instance *instance,
                          const struct mali_backend_ops *backend, const struct mali_gpu_info *gpu)
{
   struct vk_physical_device_dispatch_table dispatch_table;
   vk_physical_device_dispatch_table_from_entrypoints(&dispatch_table,
                                                      &mali_physical_device_entrypoints, true);
   vk_physical_device_dispatch_table_from_entrypoints(&dispatch_table, &wsi_physical_device_entrypoints, false);
   dispatch_table.GetPhysicalDeviceSurfacePresentModesKHR = mali_wsi_get_present_modes;   /* + IMMEDIATE on the software path */

   struct mali_drm_node drm;
   bool has_drm = mali_drm_find_node(gpu, &drm);

   /* No optional features yet. */
   const struct vk_device_extension_table extensions = {
      .KHR_timeline_semaphore = true,
      .KHR_sampler_ycbcr_conversion = true,   /* t_vk_feat ycbcr: G8_B8R8_2PLANE_420 */
      .KHR_descriptor_update_template = true, /* M7: templates (core only from 1.1) */
      .KHR_external_memory = true,
      .KHR_external_memory_fd = true,
      .EXT_external_memory_dma_buf = true,
      .KHR_external_semaphore = true,
      .KHR_external_semaphore_fd = true,
      .KHR_external_fence = true,
      .KHR_external_fence_fd = true,
      .KHR_swapchain = true,                 /* M14 */
      .KHR_swapchain_mutable_format = true,
      .EXT_image_drm_format_modifier = true, /* LINEAR only */
      .EXT_depth_clip_enable = true,         /* t_dxvk_caps; pixels: t_vk_draw fx_dclip_* */
      .EXT_provoking_vertex = true,          /* FBD word 11 bit 14 + tiler ctx word 2 bit 18 (First provoking vertex); t_cmd provoking checks. NOT per pipeline: one mode per render pass */
      .EXT_extended_dynamic_state3 = true,   /* polygonMode only (mali_cmd.c snapshot); every other extendedDynamicState3* bit stays false. t_vk_draw poly_dyn_* */
      .KHR_shader_expect_assume = true,      /* t_dxvk_caps: spirv_to_nir default caps include ExpectAssumeKHR (OpExpectKHR passthrough, OpAssumeTrueKHR dropped) */
      .KHR_load_store_op_none = true,        /* t_cmd loadstore: LOAD_OP_NONE/DONT_CARE do not preload, LOAD does; STORE_OP_NONE */
      .EXT_line_rasterization = true, .KHR_line_rasterization = true,   /* t_vk_feat linerast: rectangularLines only (hardware default wide line) */
      .KHR_maintenance1 = true,              /* promoted 1.1 name (zink checks the extension, not the version): t_steam_caps TrimCommandPoolKHR */
      .EXT_custom_border_color = true,       /* t_vk_feat cbc: runtime border_color_value -> sampler descriptor words 4..7 (same path as the fixed colours) */
      .KHR_maintenance5 = true,              /* t_vk_feat maint5: BindIndexBuffer2, module-less stages (zink requires it, STEAM-GAPS.md) */
      .EXT_vertex_attribute_divisor = true, .KHR_vertex_attribute_divisor = true,   /* t_vk_feat divisor */
      .KHR_shader_integer_dot_product = true, /* t_vk_feat intdot */
      .KHR_maintenance6 = true,              /* t_cmd: BindDescriptorSets2/PushConstants2 == 1.0 forms */
      .EXT_extended_dynamic_state = true,   /* core 1.3 setters (runtime-tracked, read in mali_cmd.c snapshot); t_vk_draw dyn cull/depth/stencil/topology, t_angle_seq procaddr */
      .EXT_memory_budget = true,             /* t_dxvk_caps: budget/usage track vkAllocateMemory */
      .KHR_calibrated_timestamps = true,     /* t_dxvk_caps: device/monotonic/raw domains */
      .EXT_image_view_min_lod = true,        /* t_image view_min_lod; t_dxvk_caps name == feature */
      .KHR_unified_image_layouts = true,     /* t_dxvk_caps: image layouts are no-ops on Mali (barriers never transition), so every layout is usable for every access */
      .EXT_robustness2 = true,               /* nullDescriptor only (t_vk_feat nulld) */
      .EXT_extended_dynamic_state2 = true,   /* rasterizerDiscard/depthBias/primitiveRestart enables are read in mali_cmd.c; dynamic LogicOp/patchControlPoints stay off (static logicOp runs through the blend shader) */
      /* Steam session (vk/STEAM-GAPS.md; t_steam_caps). All of it is Mesa wsi_common / runtime behaviour: the driver adds no code. */
      .EXT_queue_family_foreign = true,      /* queue-family ownership indices are ignored by the barrier code (coherent dma-buf memory): t_steam_caps barriers */
      /* Promoted-to-core names whose feature bit is already true and probed (t_vk_draw/t_vk_meta/t_vk_feat); ANGLE and DXVK look at the extension name.
         The runtime aliases the KHR/EXT entry points onto the core ones. t_steam_caps checks name == feature bit. */
      .KHR_dynamic_rendering = true, .KHR_synchronization2 = true, .KHR_maintenance4 = true, .EXT_private_data = true,
      .KHR_buffer_device_address = true, .EXT_scalar_block_layout = true, .EXT_host_query_reset = true, .KHR_uniform_buffer_standard_layout = true,
      /* zink 26.0.8 (the guest's Mesa) refuses a device without these two NAMES (zink_device_info.py: required=True, silent in release builds).
         Both are runtime behaviour: vk_common_CreateRenderPass2/CmdBeginRenderPass2 build on dynamic rendering, the imageless framebuffer's views come
         from VkRenderPassAttachmentBeginInfo. t_vk_feat imageless (pixels on hardware), t_steam_caps by-name list. */
      .KHR_create_renderpass2 = true, .KHR_imageless_framebuffer = true,
      .KHR_present_id = true, .KHR_present_wait = true,                    /* gamescope chains both feature structs unconditionally */
      .EXT_swapchain_maintenance1 = true, .KHR_swapchain_maintenance1 = true, /* gamescope's WSI layer forces it onto every client device */
      .EXT_hdr_metadata = true,              /* wsi_SetHdrMetadataEXT (wsi_common); stored by the Wayland chain, no-op elsewhere */
      .KHR_incremental_present = true,       /* wsi_common parses VkPresentRegionsKHR; regions are only a damage hint and may be ignored (t_vk_present, t_steam_caps) */
   };
   /* VK_EXT_physical_device_drm only together with node identity: a DRM alias of our own device (t_vk_drm). */
   struct vk_device_extension_table enabled = extensions;
   /* Also advertise when the app hands us a stand-in: SessionService.kt:1044-1053 exports
    * DD_OPEN_DRIVER=1 + DD_DRM_STANDIN=/dev/mali0 in open-driver mode, and /dev/mali0 is a misc char device that
    * mali_drm_find_node() cannot match at init (no /dev/dri alias yet). Without the extension gamescope stops at
    * "physical device has no primary node". The node is resolved per query in mali_drm_refresh(). */
   if (has_drm || getenv("DD_DRM_STANDIN") || getenv("DD_OPEN_DRIVER"))
      enabled.EXT_physical_device_drm = true;
   /* The ONE feature table (R4-PLAN rule). A bit is true only if tests/t_vk_info.c probes it and passes;
    * dynamicRendering is probed by t_vk_draw/t_vk_meta (BeginRendering draws). Everything else stays false. */
   const struct vk_features features = {
      /* Vulkan 1.0 makes robustBufferAccess mandatory and Mesa's vk_physical_device_check_device_features
       * (vk_device.c:211) rejects a device without it: zink could not create a device
       * ("ZINK: vkCreateDevice failed (VK_ERROR_FEATURE_NOT_PRESENT)"). What it guarantees here: with the feature
       * on, mali_pipeline.c sets the compile layout's robust_access flag, so nir_lower_robust_access clamps
       * buffer/image indices and offsets before the backend sees them (the same lowering robustness2 would use;
       * get_ssbo_size/get_ubo_size stays refused, which is why robustBufferAccess2 stays unadvertised). */
      .robustBufferAccess = true,
      .timelineSemaphore = true,
      .textureCompressionBC = true,
      .hostQueryReset = true,
      .synchronization2 = true,
      .dynamicRendering = true,
      .imagelessFramebuffer = true,   /* t_vk_feat imageless */
      .maintenance4 = true,
      .privateData = true,
      .bufferDeviceAddress = true, /* t_vk_feat bda */
      .shaderSampledImageArrayDynamicIndexing = true, .runtimeDescriptorArray = true,    /* t_vk_feat didx */
      .shaderSampledImageArrayNonUniformIndexing = true, .descriptorBindingPartiallyBound = true,
      .descriptorBindingSampledImageUpdateAfterBind = true, .descriptorBindingUpdateUnusedWhilePending = true,
      .multiDrawIndirect = true,
      .sampleRateShading = true,   /* 1-sample: sample shading is a no-op (t_vk_feat samplerate); 4-sample per-sample shading needs MALI_EXPERIMENTAL_MSAA=1 (t_vk_draw msaa_sample_shading) */
      .depthClamp = true,          /* t_vk_draw fx_depth_clamp */
      .drawIndirectFirstInstance = true, .fullDrawIndexUint32 = true, .samplerAnisotropy = true, .wideLines = true, .shaderStorageImageExtendedFormats = true, .shaderStorageImageWriteWithoutFormat = true /* t_vk_feat storfmt_nf (hardware pixels; smoke) */, .samplerMirrorClampToEdge = true,
      .imageCubeArray = true,      /* t_vk_feat (all five) */
      .scalarBlockLayout = true, .uniformBufferStandardLayout = true,   /* t_vk_feat scalar / ubostd */
      .shaderZeroInitializeWorkgroupMemory = true,                      /* t_vk_feat zinit */
      .shaderDrawParameters = true,                                     /* t_vk_feat dparams */
      .samplerYcbcrConversion = true,                                   /* t_vk_feat ycbcr (NV12 only; pixels on hardware, smoke) */
      .shaderClipDistance = true,                                       /* t_vk_feat clip */
      .shaderImageGatherExtended = true,                                /* t_vk_feat gather */
      .fragmentStoresAndAtomics = true,                                 /* t_vk_feat fsstore */
      .vulkanMemoryModel = true, .vulkanMemoryModelDeviceScope = true,  /* t_vk_feat memmodel */
      .shaderInt64 = true,                                              /* t_vk_feat int64 */
      .shaderInt16 = true, .storageBuffer16BitAccess = true,            /* t_vk_feat int16 */
      .shaderInt8 = true, .storageBuffer8BitAccess = true,              /* t_vk_feat int8 */
      .subgroupSizeControl = true, .computeFullSubgroups = true,        /* t_vk_feat sgctl */
      .inlineUniformBlock = true,  /* t_vk_feat inlineub */
      .shaderDemoteToHelperInvocation = true,                           /* t_vk_feat demote */
      .extendedDynamicState3PolygonMode = true,   /* mali_cmd.c: dyn->rs.polygon_mode -> gfx.poly_mode */
      .shaderIntegerDotProduct = true,                                  /* t_vk_feat intdot (vtn lowers OpSDot/UDot/SUDot; bifrost_nir.c lowers the unaccelerated forms) */
      .provokingVertexLast = true, /* mali_cmd.c snapshot_draw -> frame.provoking_last -> FBD/tiler ctx bit cleared */
      .depthClipEnable = true,     /* mali_cmd.c: vk_rasterization_state_depth_clip_enable -> ZSD depth_clip */
      .extendedDynamicState = true,
      .maintenance5 = true,        /* t_vk_feat maint5 */
      .shaderExpectAssume = true,  /* t_dxvk_caps */
      .maintenance6 = true,
      .minLod = true,              /* t_image view_min_lod */
      .unifiedImageLayouts = true, /* t_dxvk_caps unified */
      .nullDescriptor = true,      /* t_vk_feat nulld */
      .extendedDynamicState2 = true,   /* t_dxvk_caps; dynamic LogicOp stays false (static logicOp runs through the blend shader) and PatchControlPoints stays false (no tessellation) */
      .rectangularLines = true,    /* t_vk_feat linerast; bresenham/smooth/stippled stay false: nothing implements them */
      .customBorderColors = true, .customBorderColorWithoutFormat = true,   /* t_vk_feat cbc */
      .vertexAttributeInstanceRateDivisor = false, .vertexAttributeInstanceRateZeroDivisor = true,   /* divisor > 1 is NOT advertised: the hardware divides (firstInstance + i), Vulkan wants firstInstance + i / divisor, and dEQP draw.*.instanced.*attrib_divisor_{2,20} (firstInstance 0,1,3,4,20) Fail on v13 (job 20261118-142435-48673771); needs a per-run descriptor rebase. Implementation stays in mali_cs_emit.c. t_vk_feat divisor (zero: divisor 1 with stride 0, as panvk) */
      /* multiViewport: no geometry shader and no shaderOutputViewportIndex, so every primitive uses viewport/scissor 0; 1..15 are accepted state
         and ignored (mali_cmd.c). t_vk_draw multivp */
      .multiViewport = true,
      .presentId = true, .presentWait = true, .swapchainMaintenance1 = true,   /* t_steam_caps: advertised together with the extensions */
      .depthBiasClamp = true,      /* t_vk_draw fx_bias_*_clamp */
      .dualSrcBlend = true,        /* t_vk_draw fx_dual_* (blend shader) */
      .fillModeNonSolid = true,    /* t_vk_draw --only poly (line: CPU-built edge index list; point: point list) */
      .occlusionQueryPrecise = true, /* t_vk_draw occlusion: exact covered-pixel counts with the PRECISE bit */
      .logicOp = true,             /* t_vk_draw fx_logic_* (static logicOpEnable through the blend shader) */
      .independentBlend = true,    /* t_vk_draw fx_blend_* (colour attachment 0; the driver renders one colour attachment) */
   };
   struct vk_properties properties;
   mali_get_properties(gpu, &properties);
   /* VK_EXT_physical_device_drm: the numbers are stat() of the node that backs /dev/mali0 (the bridge's
    * renderD<N> alias in the guest), so a stat-based consumer matches. A drmGetDevice2-based consumer
    * (zink's zink_render_rdev) still fails: the node is a misc char device, not a DRM device. */
   if (has_drm) {
      properties.drmHasRender = drm.render != 0;
      properties.drmRenderMajor = major(drm.render);
      properties.drmRenderMinor = minor(drm.render);
      properties.drmHasPrimary = drm.primary != 0;
      properties.drmPrimaryMajor = major(drm.primary);
      properties.drmPrimaryMinor = minor(drm.primary);
   }

   VkResult result = vk_physical_device_init(&pdev->vk, &instance->vk, &enabled, &features,
                                             &properties, &dispatch_table);
   if (result != VK_SUCCESS)
      return result;

   pdev->backend = backend;
   pdev->gpu = *gpu;
   mali_sync_init_physical(pdev);

   /* Device-wide commit budget. kbase commits and pins every byte of a native BO at allocation time
    * (libkbase/src/bo.c:45, commit_pages = va_pages), so it is not reclaimable under pressure: without a
    * total, a session can pin half of RAM (the heap we advertise) and take the phone into an OOM panic.
    * Default: half the advertised heap, clamped to a quarter of physical RAM. MALI_VRAM_CAP_MB overrides. */
   {
      long pages = sysconf(_SC_PHYS_PAGES), psz = sysconf(_SC_PAGE_SIZE);
      uint64_t ram = pages > 0 && psz > 0 ? (uint64_t)pages * (uint64_t)psz : 0;
      uint64_t cap = gpu->heap_bytes / 2;
      if (ram && ram / 4 < cap)
         cap = ram / 4;
      /* The override is a positive whole number of MiB. Anything else ("auto", "12abc", "", "0", "-1") is a typo:
       * strtoull would turn it into 0 (a zero cap refuses every allocation) or wrap, so it is ignored instead and
       * the default above stays. */
      const char *env = getenv("MALI_VRAM_CAP_MB");
      bool overridden = false;
      if (env && env[0]) {
         char *end = NULL;
         errno = 0;
         const char *p = env;
         while (*p == ' ' || *p == '\t')
            p++;
         unsigned long long mb = strtoull(env, &end, 10);
         if (*p != '-' && end != env && *end == '\0' && errno == 0 && mb > 0 && mb <= (UINT64_MAX >> 20)) {
            cap = mb << 20;
            overridden = true;
         } else if (getenv("MALI_WSI_DEBUG")) {
            fprintf(stderr, "mali_vk: VRAM budget: ignoring MALI_VRAM_CAP_MB=%s (not a positive whole number of "
                    "MiB), keeping %llu MiB\n", env, (unsigned long long)(cap >> 20));
         }
      }
      pdev->mem_cap = cap;
      if (getenv("MALI_WSI_DEBUG"))
         fprintf(stderr, "mali_vk: VRAM budget: cap %llu MiB (heap %llu MiB%s, RAM %llu MiB), %s\n",
                 (unsigned long long)(cap >> 20), (unsigned long long)(gpu->heap_bytes >> 20),
                 overridden ? " overridden by MALI_VRAM_CAP_MB" : "", (unsigned long long)(ram >> 20),
                 pdev->gpu.name);
   }

   /* One heap. Type 0 is the mandatory HOST_VISIBLE|HOST_COHERENT type; type 1 is CPU-cached for
    * readback. Which kbase allocation flags back them is decided in M3 on hardware. */
   pdev->memory = (VkPhysicalDeviceMemoryProperties){
      .memoryTypeCount = 2,
      .memoryTypes = {
         {.propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
          .heapIndex = 0},
         {.propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
          .heapIndex = 0},
      },
      .memoryHeapCount = 1,
      .memoryHeaps = {{.size = gpu->heap_bytes, .flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT}},
   };

   result = mali_wsi_init(pdev);
   if (result != VK_SUCCESS) {
      vk_physical_device_finish(&pdev->vk);
      return result;
   }

   return VK_SUCCESS;
}

VkResult
mali_enumerate_physical_devices(struct vk_instance *vk_instance)
{
   struct mali_instance *instance = container_of(vk_instance, struct mali_instance, vk);
   const struct mali_backend_ops *backend = mali_backend_select();
   struct mali_gpu_info gpu;

   /* "No compatible device" is VK_SUCCESS with an empty list: the loader fails the whole
    * enumeration if any ICD returns an error. */
   if (backend == NULL || backend->probe(&gpu) != 0)
      return VK_SUCCESS;

   if (gpu.arch_major != 10 && gpu.arch_major != 11 && gpu.arch_major != 13) {
      vk_logw(VK_LOG_OBJS(vk_instance), "mali: unsupported GPU arch %u.%u (id 0x%08x)",
              gpu.arch_major, gpu.arch_minor, gpu.gpu_id);
      return VK_SUCCESS;
   }

   struct mali_physical_device *pdev =
      vk_zalloc(&instance->vk.alloc, sizeof(*pdev), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!pdev)
      return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result = mali_physical_device_init(pdev, instance, backend, &gpu);
   if (result != VK_SUCCESS) {
      vk_free(&instance->vk.alloc, pdev);
      return result;
   }

   list_addtail(&pdev->vk.link, &instance->vk.physical_devices.list);
   return VK_SUCCESS;
}

void
mali_destroy_physical_device(struct vk_physical_device *vk_pdev)
{
   struct mali_physical_device *pdev = container_of(vk_pdev, struct mali_physical_device, vk);
   struct vk_instance *instance = vk_pdev->instance;

   mali_wsi_finish(pdev);
   /* Every VkDeviceMemory of this physical device is gone with its devices; a non-zero counter here would mean
    * one was leaked, so it must not be inherited by anything that reuses the slot. */
   pdev->mem_used = 0;
   vk_physical_device_finish(&pdev->vk);
   vk_free(&instance->alloc, pdev);
}

VKAPI_ATTR void VKAPI_CALL
mali_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice,
                                             uint32_t *pQueueFamilyPropertyCount,
                                             VkQueueFamilyProperties2 *pQueueFamilyProperties)
{
   VK_OUTARRAY_MAKE_TYPED(VkQueueFamilyProperties2, out, pQueueFamilyProperties,
                          pQueueFamilyPropertyCount);

   vk_outarray_append_typed(VkQueueFamilyProperties2, &out, p)
   {
      p->queueFamilyProperties = (VkQueueFamilyProperties){
         .queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
         .queueCount = 1,
         .timestampValidBits = 64,
         .minImageTransferGranularity = {1, 1, 1},
      };
   }
}

VKAPI_ATTR void VKAPI_CALL
mali_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice,
                                        VkPhysicalDeviceMemoryProperties2 *pMemoryProperties)
{
   VK_FROM_HANDLE(mali_physical_device, pdev, physicalDevice);

   pMemoryProperties->memoryProperties = pdev->memory;
   /* VK_EXT_memory_budget: one heap; the budget is the heap, usage what this process still holds allocated */
   vk_foreach_struct(stype, ext, pMemoryProperties->pNext) {
      if (stype != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT)
         continue;
      VkPhysicalDeviceMemoryBudgetPropertiesEXT *b = ext;
      memset(b->heapBudget, 0, sizeof b->heapBudget);
      memset(b->heapUsage, 0, sizeof b->heapUsage);
      uint64_t used = p_atomic_read(&pdev->mem_used);
      b->heapUsage[0] = used;
      b->heapBudget[0] = pdev->memory.memoryHeaps[0].size > used ? pdev->memory.memoryHeaps[0].size : used;
   }
}

/* Format, image-format and sparse-image-format properties: mali_format.c (M5). */

VKAPI_ATTR void VKAPI_CALL
mali_GetPhysicalDeviceExternalBufferProperties(
   VkPhysicalDevice physicalDevice, const VkPhysicalDeviceExternalBufferInfo *pExternalBufferInfo,
   VkExternalBufferProperties *pExternalBufferProperties)
{
   /* dma-buf only (M4). */
   VkExternalMemoryProperties p = {0};
   if (pExternalBufferInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
      p = (VkExternalMemoryProperties){
         .externalMemoryFeatures = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
                                   VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
         .exportFromImportedHandleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
         .compatibleHandleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      };
   pExternalBufferProperties->externalMemoryProperties = p;
}

/* VK_EXT_physical_device_drm is evaluated when a client asks, not once when the physical device is created: the
 * guest binds /dev/dri nodes (or the app's stand-in) after the first vkEnumeratePhysicalDevices, and gamescope
 * enumerates early, so a cached answer says "no primary node" for a node that exists a moment later. Re-probe with
 * stat() (cheap) behind a one second cache, and report each half independently. An explicit node from the app wins:
 * DD_DRM_STANDIN / VKBRIDGE_DRM_RENDER / BL_DRM_STANDIN is reported as the render node, and as primary when no card
 * matched (gamescope refuses a device without a primary when it drives DRM, and a misc alias is all the guest has). */
/* True when /dev/dri has a node with this rdev: the identity we report must name a node a client can open. */
static bool
drm_rdev_has_alias(dev_t rdev)
{
   /* The same directory policy as the rest of the node lookup: MALI_DRM_DIR overrides /dev/dri (tests point it at a
    * temp dir of real chardev nodes), and by-path is only an alias table of that same directory. */
   const char *base = getenv("MALI_DRM_DIR");
   if (!base || !*base)
      base = "/dev/dri";
   char bypath[PATH_MAX];
   snprintf(bypath, sizeof bypath, "%s/by-path", base);
   const char *dirs[] = { base, bypath };
   for (unsigned d = 0; d < 2; d++) {
      DIR *dp = opendir(dirs[d]);
      if (!dp)
         continue;
      struct dirent *de;
      while ((de = readdir(dp))) {
         if (de->d_name[0] == '.')
            continue;
         char path[PATH_MAX];
         struct stat st;
         if (snprintf(path, sizeof path, "%s/%s", dirs[d], de->d_name) >= (int)sizeof path)
            continue;
         if (stat(path, &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == rdev) {
            closedir(dp);
            return true;
         }
      }
      closedir(dp);
   }
   return false;
}

static void
mali_drm_refresh(struct mali_physical_device *pdev)
{
   static struct timespec last;
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   if (last.tv_sec && now.tv_sec - last.tv_sec < 1 && now.tv_nsec >= last.tv_nsec)
      return;
   last = now;

   struct vk_properties *p = &pdev->vk.properties;
   const bool was_primary = p->drmHasPrimary, was_render = p->drmHasRender;
   struct mali_drm_node node = {0};
   bool any = mali_drm_find_node(&pdev->gpu, &node);

   /* Only a stand-in the application actually created may be reported as a node. BL_DRM_STANDIN names the GPU
    * character device itself (a hint for identifying aliases, not a node), and claiming it as render is exactly
    * the "DRM identity without a matching node" t_vk_drm rejects. */
   /* No primary and no explicit stand-in: a /dev/dri/renderD* whose rdev is the very node we opened (/dev/mali0
    * bound into the guest as a DRM alias, major 10) is the app's stand-in in all but name, so report it as render
    * and as primary. The stock app does not export DD_DRM_STANDIN, and gamescope refuses a device without a primary.
    * MALI_DRM_AUTO_STANDIN=0 opts out. The rdev equality is the honesty check: any other node is not ours. */
   if (!node.primary && (!getenv("DD_DRM_STANDIN") || getenv("DD_OPEN_DRIVER"))) {
      const char *off = getenv("MALI_DRM_AUTO_STANDIN");
      const dev_t gpu_rdev = mali_gpu_node_rdev(&pdev->gpu);
      if (!(off && !strcmp(off, "0")) && gpu_rdev) {
         const char *dir = getenv("MALI_DRM_DIR");
         if (!dir || !*dir)
            dir = "/dev/dri";
         DIR *d = opendir(dir);
         if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
               if (strncmp(e->d_name, "renderD", 7))
                  continue;
               char path[PATH_MAX];
               snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
               struct stat st;
               if (stat(path, &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == gpu_rdev) {
                  node.render = st.st_rdev;
                  node.primary = st.st_rdev;
                  any = true;
                  static bool auto_logged;
                  if (!auto_logged) {
                     auto_logged = true;
                     fprintf(stderr, "mali_vk: wsi drm: %s is our own node (10/226 alias of the GPU char device); "
                             "reporting it as primary and render (MALI_DRM_AUTO_STANDIN=0 to disable)\n", path);
                  }
                  break;
               }
            }
            closedir(d);
         }
      }
   }
   static const char *const envs[] = { "DD_DRM_STANDIN", NULL };
   for (unsigned i = 0; envs[i]; i++) {
      const char *e = getenv(envs[i]);
      struct stat st;
      dev_t standin_rdev = 0;
      if (e && *e && stat(e, &st) == 0 && S_ISCHR(st.st_mode))
         standin_rdev = st.st_rdev;
      else if (e && *e) {
         /* The app names /dev/mali0, which is not a DRM node name: take the node we actually opened instead and
          * find the /dev/dri alias with that rdev (the guest binds it into /dev/dri as renderD<minor>). */
         standin_rdev = (dev_t)pdev->gpu.node_rdev;
      }
      /* The stand-in must be a node the app can actually see as /dev/dri: take our own rdev only when it has an
       * alias there, otherwise the identity would name a node no client can open. */
      if (standin_rdev && !drm_rdev_has_alias(standin_rdev))
         standin_rdev = 0;
      if (standin_rdev && (!node.render || !node.primary)) {
         /* Resolve through /dev/dri by rdev if the path itself is not a /dev/dri node. */
         if (!node.render)
            node.render = standin_rdev;
         /* Only the app's own stand-in (DD_DRM_STANDIN) may stand in for the primary as well: the bridge's
          * BL_DRM_STANDIN/VKBRIDGE_DRM_RENDER alias is a render node, and claiming it as a card would be a lie
          * (t_vk_drm: "hasPrimary without a card node"). */
         if (!node.primary)
            node.primary = standin_rdev;
         any = true;
         break;
      }
   }

   p->drmHasPrimary = any && node.primary != 0;
   p->drmPrimaryMajor = p->drmHasPrimary ? (int64_t)major(node.primary) : 0;
   p->drmPrimaryMinor = p->drmHasPrimary ? (int64_t)minor(node.primary) : 0;
   p->drmHasRender = any && node.render != 0;
   p->drmRenderMajor = p->drmHasRender ? (int64_t)major(node.render) : 0;
   p->drmRenderMinor = p->drmHasRender ? (int64_t)minor(node.render) : 0;

   if (p->drmHasPrimary != was_primary || p->drmHasRender != was_render)
      fprintf(stderr, "mali_vk: wsi drm: primary=%s(%lld:%lld) render=%s(%lld:%lld) (re-probed)\n",
              p->drmHasPrimary ? "node" : "none", (long long)p->drmPrimaryMajor, (long long)p->drmPrimaryMinor,
              p->drmHasRender ? "node" : "none", (long long)p->drmRenderMajor, (long long)p->drmRenderMinor);
}

VKAPI_ATTR void VKAPI_CALL
mali_GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2 *pProperties)
{
   VK_FROM_HANDLE(mali_physical_device, pdev, physicalDevice);
   vk_common_GetPhysicalDeviceProperties2(physicalDevice, pProperties);
   mali_drm_refresh(pdev);
   for (VkBaseOutStructure *ext = pProperties->pNext; ext; ext = ext->pNext) {
      if (ext->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT) {
         VkPhysicalDeviceDrmPropertiesEXT *drm = (VkPhysicalDeviceDrmPropertiesEXT *)ext;
         drm->hasPrimary = pdev->vk.properties.drmHasPrimary;
         drm->hasRender = pdev->vk.properties.drmHasRender;
         drm->primaryMajor = pdev->vk.properties.drmPrimaryMajor;
         drm->primaryMinor = pdev->vk.properties.drmPrimaryMinor;
         drm->renderMajor = pdev->vk.properties.drmRenderMajor;
         drm->renderMinor = pdev->vk.properties.drmRenderMinor;
      }
   }
}
