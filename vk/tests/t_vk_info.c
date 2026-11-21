/* SPDX-License-Identifier: MIT */
/* R4-M1 test: Vulkan 1.3 device report, honest. Loader-free (dlopen the ICD).
 *   t_vk_info --direct <icd.so> [--dump]
 * Checks: apiVersion 1.3; vulkaninfo-style dump (--dump); Vulkan11/12/13 feature+property structs agree
 * with the individual pNext structs; every advertised feature is probed (privateData, maintenance4,
 * hostQueryReset, synchronization2, dynamicRendering proc, timelineSemaphore, BC); every feature NOT in
 * the allow-list is false; limits >= spec minimum and <= PanVK-on-same-GPU ceiling; queue/format
 * consistency; vkGetDeviceProcAddr non-NULL for all core 1.0-1.3 device entrypoints.
 * Exit 0 = pass. */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail, g_dump;
#define CHECK(c, ...)                                                                              \
   do {                                                                                            \
      if (!(c)) {                                                                                  \
         printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c);                                       \
         printf(__VA_ARGS__);                                                                      \
         printf("\n");                                                                             \
         g_fail = 1;                                                                               \
      }                                                                                            \
   } while (0)

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;

/* All core 1.0..1.3 device-level entrypoints that a loader/DXVK may fetch. */
static const char *const core_dev[] = {
   "vkGetDeviceQueue2", "vkBindBufferMemory2", "vkBindImageMemory2", "vkGetBufferMemoryRequirements2",
   "vkGetImageMemoryRequirements2", "vkGetImageSparseMemoryRequirements2", "vkTrimCommandPool",
   "vkCreateDescriptorUpdateTemplate", "vkDestroyDescriptorUpdateTemplate",
   "vkUpdateDescriptorSetWithTemplate", "vkGetDescriptorSetLayoutSupport", "vkCreateSamplerYcbcrConversion",
   "vkDestroySamplerYcbcrConversion", "vkGetDeviceGroupPeerMemoryFeatures", "vkCmdSetDeviceMask",
   "vkCmdDispatchBase", "vkCreateRenderPass2", "vkCmdBeginRenderPass2", "vkCmdNextSubpass2",
   "vkCmdEndRenderPass2", "vkCmdDrawIndirectCount", "vkCmdDrawIndexedIndirectCount",
   "vkResetQueryPool", "vkGetSemaphoreCounterValue", "vkWaitSemaphores", "vkSignalSemaphore",
   "vkGetBufferDeviceAddress", "vkGetBufferOpaqueCaptureAddress", "vkGetDeviceMemoryOpaqueCaptureAddress",
   "vkCreatePrivateDataSlot", "vkDestroyPrivateDataSlot", "vkSetPrivateData", "vkGetPrivateData",
   "vkCmdSetEvent2", "vkCmdResetEvent2", "vkCmdWaitEvents2", "vkCmdPipelineBarrier2", "vkCmdWriteTimestamp2",
   "vkQueueSubmit2", "vkCmdCopyBuffer2", "vkCmdCopyImage2", "vkCmdCopyBufferToImage2",
   "vkCmdCopyImageToBuffer2", "vkCmdBlitImage2", "vkCmdResolveImage2", "vkCmdBeginRendering",
   "vkCmdEndRendering", "vkCmdSetCullMode", "vkCmdSetFrontFace", "vkCmdSetPrimitiveTopology",
   "vkCmdSetViewportWithCount", "vkCmdSetScissorWithCount", "vkCmdBindVertexBuffers2",
   "vkCmdSetDepthTestEnable", "vkCmdSetDepthWriteEnable", "vkCmdSetDepthCompareOp",
   "vkCmdSetDepthBoundsTestEnable", "vkCmdSetStencilTestEnable", "vkCmdSetStencilOp",
   "vkCmdSetRasterizerDiscardEnable", "vkCmdSetDepthBiasEnable", "vkCmdSetPrimitiveRestartEnable",
   "vkGetDeviceBufferMemoryRequirements", "vkGetDeviceImageMemoryRequirements",
   "vkGetDeviceImageSparseMemoryRequirements",
   /* 1.0 */
   "vkCreateGraphicsPipelines", "vkCreateComputePipelines", "vkCmdDrawIndirect", "vkCmdDrawIndexedIndirect",
   "vkCmdDispatchIndirect", "vkQueueBindSparse", "vkCmdBeginQuery", "vkCmdEndQuery",
};

static const char *g_icd;
static VkInstance inst;

static void
eq(const char *name, uint64_t a, uint64_t b)
{
   CHECK(a == b, "%s: aggregate %llu != individual %llu", name, (unsigned long long)a, (unsigned long long)b);
}

int
main(int argc, char **argv)
{
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) g_icd = argv[++i];
      else if (!strcmp(argv[i], "--dump")) g_dump = 1;
   }
   if (!g_icd) { printf("usage: t_vk_info --direct <icd.so> [--dump]\n"); return 2; }
   void *lib = dlopen(g_icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
   gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   CHECK(gipa, "no vk_icdGetInstanceProcAddr");
   if (!gipa) return 1;

#define I(n) PFN_vk##n n = (PFN_vk##n)gipa(inst, "vk" #n)
   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   PFN_vkEnumerateInstanceVersion EIV = (PFN_vkEnumerateInstanceVersion)gipa(NULL, "vkEnumerateInstanceVersion");
   uint32_t iv = 0;
   CHECK(EIV && EIV(&iv) == VK_SUCCESS && iv >= VK_API_VERSION_1_3, "instance version %x", iv);

   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   CHECK(CreateInstance(&ici, NULL, &inst) == VK_SUCCESS, "CreateInstance 1.3");
   if (!inst) return 1;
   I(EnumeratePhysicalDevices); I(GetPhysicalDeviceProperties2); I(GetPhysicalDeviceFeatures2);
   I(GetPhysicalDeviceQueueFamilyProperties2); I(GetPhysicalDeviceFormatProperties2); I(GetPhysicalDeviceImageFormatProperties);
   I(EnumerateDeviceExtensionProperties);
   I(CreateDevice); I(DestroyInstance);

   uint32_t n = 1;
   VkPhysicalDevice pd;
   CHECK(EnumeratePhysicalDevices(inst, &n, &pd) == VK_SUCCESS && n == 1, "enumerate n=%u", n);

   /* ---- properties ---- */
   VkPhysicalDeviceVulkan11Properties p11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
   VkPhysicalDeviceVulkan12Properties p12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES, .pNext = &p11};
   VkPhysicalDeviceVulkan13Properties p13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES, .pNext = &p12};
   VkPhysicalDeviceSubgroupProperties sg = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, .pNext = &p13};
   VkPhysicalDeviceMaintenance3Properties m3 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES, .pNext = &sg};
   VkPhysicalDeviceMaintenance4Properties m4 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES, .pNext = &m3};
   VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, .pNext = &m4};
   VkPhysicalDeviceIDProperties idp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, .pNext = &drv};
   VkPhysicalDeviceTimelineSemaphoreProperties tsp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES, .pNext = &idp};
   VkPhysicalDeviceProperties2 pr = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &tsp};
   GetPhysicalDeviceProperties2(pd, &pr);
   const VkPhysicalDeviceLimits *L = &pr.properties.limits;

   CHECK(VK_API_VERSION_MAJOR(pr.properties.apiVersion) == 1 && VK_API_VERSION_MINOR(pr.properties.apiVersion) == 3,
         "apiVersion %x", pr.properties.apiVersion);
   CHECK(pr.properties.vendorID == 0x13b5, "vendor");
   eq("driverID", p12.driverID, drv.driverID);
   CHECK(drv.driverID != 0 && drv.driverName[0], "driver id/name");
   CHECK(!strcmp(drv.driverName, "mali-own"), "driverName '%s'", drv.driverName);
#ifdef MALI_EXPECT_SHA
   { /* full identity: "mali-open <sha|unknown>[-dirty] (mali-own), not conformant"; build.sh passes the sha it compiled in */
      char want[VK_MAX_DRIVER_INFO_SIZE];
      snprintf(want, sizeof want, "mali-open %s (mali-own), not conformant", MALI_EXPECT_SHA);
      CHECK(!strcmp(drv.driverInfo, want), "driverInfo '%s' != '%s'", drv.driverInfo, want);
   }
#else
   { /* no expected sha known (prebuilt test): still require the exact shape with a non-empty id */
      const char *id = drv.driverInfo + 10, *sp = strchr(id, ' ');
      CHECK(!strncmp(drv.driverInfo, "mali-open ", 10) && sp && sp > id &&
            !strcmp(sp, " (mali-own), not conformant"), "driverInfo '%s'", drv.driverInfo);
   }
#endif
   CHECK(strstr(pr.properties.deviceName, "mali-own"), "deviceName '%s'", pr.properties.deviceName);
   CHECK(drv.conformanceVersion.major == 0 && drv.conformanceVersion.minor == 0 && drv.conformanceVersion.subminor == 0 &&
         drv.conformanceVersion.patch == 0, "conformanceVersion must stay 0.0.0.0");
   CHECK(!memcmp(p11.driverUUID, idp.driverUUID, VK_UUID_SIZE) && !memcmp(p11.deviceUUID, idp.deviceUUID, VK_UUID_SIZE), "UUID mismatch 1.1 vs ID struct");
   eq("subgroupSize", p11.subgroupSize, sg.subgroupSize);
   CHECK(p11.subgroupSize == 16 && p13.minSubgroupSize == 16 && p13.maxSubgroupSize == 16, "subgroup 16");
   CHECK(p13.maxComputeWorkgroupSubgroups * p13.maxSubgroupSize <= L->maxComputeWorkGroupInvocations, "wg subgroups");
   /* subgroup ops: compute only, backed by t_vk_feat subgroup; a stage or op may be added only with its own test */
   CHECK(p11.subgroupSupportedStages == VK_SHADER_STAGE_COMPUTE_BIT && !(p11.subgroupSupportedOperations & ~0xffu & ~0u) && !(p11.subgroupSupportedOperations & ~(0xffu)), "subgroup ops == tested set");
   eq("maxPerSetDescriptors", p11.maxPerSetDescriptors, m3.maxPerSetDescriptors);
   eq("maxMemoryAllocationSize", p11.maxMemoryAllocationSize, m3.maxMemoryAllocationSize);
   CHECK(m3.maxPerSetDescriptors >= 1024 && m3.maxMemoryAllocationSize >= (1u << 30), "maintenance3 minimums");
   eq("maxBufferSize", p13.maxBufferSize, m4.maxBufferSize);
   CHECK(m4.maxBufferSize >= (1u << 30), "maxBufferSize");
   CHECK(tsp.maxTimelineSemaphoreValueDifference == p12.maxTimelineSemaphoreValueDifference &&
         tsp.maxTimelineSemaphoreValueDifference >= (1ull << 31) - 1, "timeline diff");
   /* spec minimums (1.0 required-limits table) and PanVK-on-Poco ceilings (R4-ORACLE) */
   CHECK(L->maxImageDimension2D >= 4096 && L->maxFramebufferWidth >= 4096 && L->maxColorAttachments >= 4, "image minimums");
   CHECK(L->minTexelGatherOffset == -8 && L->maxTexelGatherOffset == 7 && L->minTexelOffset == -8 && L->maxTexelOffset == 7, "texel (gather) offset limits (t_vk_feat gatherlim)");
   CHECK(L->maxPushConstantsSize >= 128 && L->maxPushConstantsSize <= 256, "push constants");
   CHECK(L->maxUniformBufferRange >= 16384 && L->maxUniformBufferRange <= 1048576, "ubo range");
   CHECK(L->maxBoundDescriptorSets == 8, "bound sets %u (8 SRT tables: t_vk_draw sets8 binds all)", L->maxBoundDescriptorSets);
   CHECK(L->maxComputeWorkGroupInvocations >= 128 && L->maxComputeWorkGroupInvocations <= 1024, "wg invocations");
   CHECK(L->maxComputeSharedMemorySize >= 16384 && L->maxComputeSharedMemorySize <= 32768, "shared mem");
   CHECK(L->maxSamplerAnisotropy <= 16.0f && L->maxViewports >= 1 && L->maxViewports <= 16, "aniso/viewports");
   CHECK(L->maxFragmentOutputAttachments <= 8 && L->maxVertexInputAttributes >= 16 && L->maxVertexInputBindings >= 16, "vertex/frag");
   CHECK(L->maxPerStageDescriptorSamplers >= 16 && L->maxPerStageDescriptorSampledImages >= 16, "per-stage minimums");
   CHECK(L->timestampPeriod > 0.0f, "timestampPeriod");
   CHECK(L->maxClipDistances >= 8 && L->maxCombinedClipAndCullDistances >= L->maxClipDistances, "clip distance limits (shaderClipDistance on: min 8)");
   /* UBO counts follow the transform-feedback gate (mali_physical_device.c header comment): 128 with TF (the
    * default), otherwise the Vulkan 1.0 minimums (12 per stage / 72 per set). gate off keeps st
    * from advertising ARB_uniform_buffer_object on a GL 2.1 / GLSL 1.20 context, where its shaders cannot compile. */
   {
      /* Transform feedback is ON by default since #313; MALI_EXPERIMENTAL_TF=0 opts out. */
      const char *g = getenv("MALI_EXPERIMENTAL_TF");
      const int tf = !(g && g[0] == '0');
      const uint32_t want_stage = tf ? 128 : 12, want_set = tf ? 128 : 72;
      CHECK(L->maxPerStageDescriptorUniformBuffers == want_stage, "per-stage UBOs %u == %u (TF gate %d)", L->maxPerStageDescriptorUniformBuffers, want_stage, tf);
      CHECK(L->maxDescriptorSetUniformBuffers == want_set, "per-set UBOs %u == %u (TF gate %d)", L->maxDescriptorSetUniformBuffers, want_set, tf);
      CHECK(L->maxPerStageDescriptorUniformBuffers >= 12 && L->maxDescriptorSetUniformBuffers >= 72, "UBO minimums (1.0 table)");
   }
   CHECK(L->minUniformBufferOffsetAlignment && !(L->minUniformBufferOffsetAlignment & (L->minUniformBufferOffsetAlignment - 1)), "ubo align pow2");
   CHECK(L->nonCoherentAtomSize && !(L->nonCoherentAtomSize & (L->nonCoherentAtomSize - 1)), "atom pow2");
   CHECK(L->framebufferColorSampleCounts & VK_SAMPLE_COUNT_1_BIT, "fb samples");
   {  /* 4x is advertised iff pipeline creation accepts it: opt-in via MALI_EXPERIMENTAL_MSAA=1 (LOAD/resolve incomplete) */
      const char *g = getenv("MALI_EXPERIMENTAL_MSAA");
      VkSampleCountFlags want = (g && g[0] == '0') ? 0 : VK_SAMPLE_COUNT_4_BIT;
      VkImageFormatProperties ifp;
      VkResult r = GetPhysicalDeviceImageFormatProperties(pd, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &ifp);
      CHECK((L->framebufferColorSampleCounts & VK_SAMPLE_COUNT_4_BIT) == want, "4x fb colour samples follow the MSAA gate");
      CHECK((L->framebufferDepthSampleCounts & VK_SAMPLE_COUNT_4_BIT) == want, "4x fb depth samples follow the MSAA gate");
      CHECK(r == VK_SUCCESS && (ifp.sampleCounts & VK_SAMPLE_COUNT_4_BIT) == want, "4x image format samples follow the MSAA gate");
      /* the default (env unset) must really be 4x for the depth format too, or the flip silently regresses */
      VkImageFormatProperties dip;
      VkResult dr = GetPhysicalDeviceImageFormatProperties(pd, VK_FORMAT_D32_SFLOAT, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, &dip);
      CHECK(dr == VK_SUCCESS && (dip.sampleCounts & VK_SAMPLE_COUNT_4_BIT) == want, "4x depth image format samples follow the MSAA gate");
   }
   /* VK_EXT_transform_feedback is ON by default since 29a67a9d (a zink client needs it for GL 3.0):
    * MALI_EXPERIMENTAL_TF=0 is the legacy opt-out. The extension, the feature bit and the properties follow the same
    * gate (mali_tf.h `tf_enabled()`: true unless the env starts with '0'), like the UBO counts above (line 172). */
   {
      const char *g = getenv("MALI_EXPERIMENTAL_TF");
      const int want = !(g && g[0] == '0');
      uint32_t en = 0;
      int have = 0;
      EnumerateDeviceExtensionProperties(pd, NULL, &en, NULL);
      VkExtensionProperties *e = calloc(en ? en : 1, sizeof *e);
      if (e && EnumerateDeviceExtensionProperties(pd, NULL, &en, e) == VK_SUCCESS)
         for (uint32_t i = 0; i < en; i++)
            if (!strcmp(e[i].extensionName, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME)) have = 1;
      free(e);
      CHECK(have == want, "VK_EXT_transform_feedback %s, gate %d", have ? "advertised" : "absent", want);

      VkPhysicalDeviceTransformFeedbackFeaturesEXT tff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
      VkPhysicalDeviceFeatures2 q2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &tff};
      GetPhysicalDeviceFeatures2(pd, &q2);
      CHECK(tff.transformFeedback == (VkBool32)want, "transformFeedback feature follows the gate");
      CHECK(tff.geometryStreams == VK_FALSE, "geometryStreams stays false (no geometry stage)");

      VkPhysicalDeviceTransformFeedbackPropertiesEXT tfp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT};
      VkPhysicalDeviceProperties2 q3 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &tfp};
      GetPhysicalDeviceProperties2(pd, &q3);
      if (want) {
         CHECK(tfp.maxTransformFeedbackStreams >= 1 && tfp.maxTransformFeedbackBuffers >= 1 &&
               tfp.maxTransformFeedbackBufferSize > 0 && tfp.maxTransformFeedbackBufferDataSize > 0 &&
               tfp.maxTransformFeedbackBufferDataStride > 0, "transform feedback properties");
         CHECK(!tfp.transformFeedbackQueries && !tfp.transformFeedbackDraw &&
               !tfp.transformFeedbackRasterizationStreamSelect && !tfp.transformFeedbackStreamsLinesTriangles,
               "only implemented transform-feedback properties may be set");
      }
   }
   CHECK(p13.maxInlineUniformBlockSize == 256 && p13.maxInlineUniformTotalSize == 256, "inline UBO limits (t_vk_feat inlineub)");

   /* ---- features ---- */
   VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
   VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f11};
   VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &f12};
   VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13};
   GetPhysicalDeviceFeatures2(pd, &f2);
   VkPhysicalDeviceTimelineSemaphoreFeatures ftl = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
   VkPhysicalDeviceHostQueryResetFeatures fhq = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, .pNext = &ftl};
   VkPhysicalDeviceSynchronization2Features fs2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, .pNext = &fhq};
   VkPhysicalDeviceDynamicRenderingFeatures fdr = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, .pNext = &fs2};
   VkPhysicalDeviceMaintenance4Features fm4 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, .pNext = &fdr};
   VkPhysicalDevicePrivateDataFeatures fpd = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES, .pNext = &fm4};
   VkPhysicalDeviceFeatures2 g2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &fpd};
   GetPhysicalDeviceFeatures2(pd, &g2);
   {  /* VK_EXT_image_view_min_lod: the advertised minLod feature must be reported true (t_image view_min_lod covers the descriptor field) */
      VkPhysicalDeviceImageViewMinLodFeaturesEXT ml = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT};
      VkPhysicalDeviceFeatures2 m2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &ml};
      GetPhysicalDeviceFeatures2(pd, &m2);
      CHECK(ml.minLod == VK_TRUE, "minLod feature advertised true (got %u)", ml.minLod);
   }
   /* uab limits: each UAB limit >= its non-UAB twin; the types with UAB features (samplers, sampled images) reach the 500000 minimum */
   CHECK(p12.maxPerStageDescriptorUpdateAfterBindSamplers >= L->maxPerStageDescriptorSamplers &&
         p12.maxPerStageDescriptorUpdateAfterBindUniformBuffers >= L->maxPerStageDescriptorUniformBuffers &&
         p12.maxPerStageDescriptorUpdateAfterBindStorageBuffers >= L->maxPerStageDescriptorStorageBuffers &&
         p12.maxPerStageDescriptorUpdateAfterBindSampledImages >= L->maxPerStageDescriptorSampledImages &&
         p12.maxPerStageDescriptorUpdateAfterBindStorageImages >= L->maxPerStageDescriptorStorageImages &&
         p12.maxPerStageDescriptorUpdateAfterBindInputAttachments >= L->maxPerStageDescriptorInputAttachments &&
         p12.maxPerStageUpdateAfterBindResources >= L->maxPerStageResources, "uab per-stage >= non-UAB");
   CHECK(p12.maxDescriptorSetUpdateAfterBindSamplers >= L->maxDescriptorSetSamplers &&
         p12.maxDescriptorSetUpdateAfterBindUniformBuffers >= L->maxDescriptorSetUniformBuffers &&
         p12.maxDescriptorSetUpdateAfterBindUniformBuffersDynamic >= L->maxDescriptorSetUniformBuffersDynamic &&
         p12.maxDescriptorSetUpdateAfterBindStorageBuffers >= L->maxDescriptorSetStorageBuffers &&
         p12.maxDescriptorSetUpdateAfterBindStorageBuffersDynamic >= L->maxDescriptorSetStorageBuffersDynamic &&
         p12.maxDescriptorSetUpdateAfterBindSampledImages >= L->maxDescriptorSetSampledImages &&
         p12.maxDescriptorSetUpdateAfterBindStorageImages >= L->maxDescriptorSetStorageImages &&
         p12.maxDescriptorSetUpdateAfterBindInputAttachments >= L->maxDescriptorSetInputAttachments, "uab per-set >= non-UAB");
   if (f12.descriptorBindingSampledImageUpdateAfterBind) {
      CHECK(p12.maxUpdateAfterBindDescriptorsInAllPools >= 500000 && p12.maxPerStageDescriptorUpdateAfterBindSampledImages >= 500000 &&
            p12.maxDescriptorSetUpdateAfterBindSampledImages >= 500000 && p12.maxPerStageUpdateAfterBindResources >= 500000,
            "uab sampled image limits >= 500000");
      CHECK(p12.maxPerStageDescriptorUpdateAfterBindSamplers >= 500000 && p12.maxDescriptorSetUpdateAfterBindSamplers >= 500000, "uab sampler limits >= 500000");
   }
   eq("timelineSemaphore", f12.timelineSemaphore, ftl.timelineSemaphore);
   eq("hostQueryReset", f12.hostQueryReset, fhq.hostQueryReset);
   eq("synchronization2", f13.synchronization2, fs2.synchronization2);
   eq("dynamicRendering", f13.dynamicRendering, fdr.dynamicRendering);
   eq("maintenance4", f13.maintenance4, fm4.maintenance4);
   eq("privateData", f13.privateData, fpd.privateData);
   CHECK(f2.features.textureCompressionBC == g2.features.textureCompressionBC && f2.features.textureCompressionBC, "BC");
   /* Mandatory in Vulkan 1.0 and required by Mesa's vk_physical_device_check_device_features (vk_device.c:211):
    * without it zink cannot create a device at all ("ZINK: vkCreateDevice failed (VK_ERROR_FEATURE_NOT_PRESENT)"). */
   CHECK(f2.features.robustBufferAccess, "robustBufferAccess (Vulkan 1.0 mandatory)");

   /* Allow-list: any VkBool32 in the core structs that is true and not listed fails. */
   {
      const VkBool32 *c = (const VkBool32 *)&f2.features;
      const unsigned nc = sizeof(f2.features) / 4;
      unsigned bc_idx = (unsigned)((const VkBool32 *)&f2.features.textureCompressionBC - c);
      /* multiDrawIndirect: backed by t_vk_draw --only mdi (drawCount 2, pixel-checked on the Poco) */
      unsigned mdi_idx = (unsigned)((const VkBool32 *)&f2.features.multiDrawIndirect - c);
      CHECK(f2.features.multiDrawIndirect, "multiDrawIndirect advertised");
      CHECK(L->maxDrawIndirectCount >= 0xffff, "maxDrawIndirectCount %u: multiDrawIndirect requires >= 2^16-1", L->maxDrawIndirectCount);
      /* Core features with a backing test (feature -> test): add a line here ONLY together with its passing test. */
      static const size_t tested[] = {
         offsetof(VkPhysicalDeviceFeatures, robustBufferAccess),       /* Vulkan 1.0 mandatory (zink: vk_device.c:211); the device below is created with it enabled */
         offsetof(VkPhysicalDeviceFeatures, shaderClipDistance),       /* t_vk_feat clip */
         offsetof(VkPhysicalDeviceFeatures, shaderImageGatherExtended), /* t_vk_feat gather */
         offsetof(VkPhysicalDeviceFeatures, fragmentStoresAndAtomics), /* t_vk_feat fsstore */
         offsetof(VkPhysicalDeviceFeatures, shaderInt64),              /* t_vk_feat int64 */
         offsetof(VkPhysicalDeviceFeatures, shaderInt16),              /* t_vk_feat int16 */
         offsetof(VkPhysicalDeviceFeatures, depthBiasClamp),   /* t_vk_draw fx_bias_*_clamp */
         offsetof(VkPhysicalDeviceFeatures, occlusionQueryPrecise),    /* t_vk_draw occlusion */
         offsetof(VkPhysicalDeviceFeatures, drawIndirectFirstInstance), /* t_vk_feat firstinst */
         offsetof(VkPhysicalDeviceFeatures, fullDrawIndexUint32),       /* t_vk_feat idx32 */
         offsetof(VkPhysicalDeviceFeatures, samplerAnisotropy),         /* t_vk_feat aniso */
         offsetof(VkPhysicalDeviceFeatures, imageCubeArray),
         offsetof(VkPhysicalDeviceFeatures, shaderSampledImageArrayDynamicIndexing), /* t_vk_feat didx */
         offsetof(VkPhysicalDeviceFeatures, fillModeNonSolid),         /* t_vk_draw --only poly */
         offsetof(VkPhysicalDeviceFeatures, multiViewport),            /* t_vk_draw --only multivp */
         offsetof(VkPhysicalDeviceFeatures, wideLines),                 /* t_vk_feat wide */            /* t_vk_feat cubearray */
         offsetof(VkPhysicalDeviceFeatures, shaderStorageImageExtendedFormats), /* t_vk_feat storfmt */
         offsetof(VkPhysicalDeviceFeatures, shaderStorageImageWriteWithoutFormat), /* t_vk_feat storfmt_nf */
         offsetof(VkPhysicalDeviceFeatures, logicOp),                  /* t_vk_draw --only fx_logic_* */
         offsetof(VkPhysicalDeviceFeatures, dualSrcBlend),             /* t_vk_draw --only fx_dual_* (passed on the Poco X8, v13) */
         offsetof(VkPhysicalDeviceFeatures, sampleRateShading),        /* t_vk_feat samplerate (1 sample); t_vk_draw msaa_sample_shading (4 samples, MALI_EXPERIMENTAL_MSAA=1) */
      };
      /* depthClamp / independentBlend: backed by t_vk_draw --only fx (fx_depth_clamp, fx_blend_*; pixel-checked on the Poco) */
      unsigned dc_idx = (unsigned)((const VkBool32 *)&f2.features.depthClamp - c), ib_idx = (unsigned)((const VkBool32 *)&f2.features.independentBlend - c);
      CHECK(f2.features.depthClamp && f2.features.independentBlend, "depthClamp / independentBlend advertised");
      for (unsigned i = 0; i < nc; i++)
      {
         int listed = i == bc_idx || i == mdi_idx || i == dc_idx || i == ib_idx;
         for (unsigned k = 0; k < sizeof tested / sizeof *tested; k++) listed |= tested[k] == i * 4u;
         CHECK(!c[i] || listed, "core feature #%u advertised without a test", i);
      }
      const VkBool32 *a = (const VkBool32 *)((const char *)&f11 + 16);
      const unsigned s16 = (unsigned)((const VkBool32 *)&f11.storageBuffer16BitAccess - a);   /* t_vk_feat int16 */
      const unsigned sdp = (unsigned)((const VkBool32 *)&f11.shaderDrawParameters - a);   /* t_vk_feat dparams */
      const unsigned ycc = (unsigned)((const VkBool32 *)&f11.samplerYcbcrConversion - a);   /* t_vk_feat ycbcr */
      for (unsigned i = 0; i < (sizeof(f11) - 16) / 4; i++)
         CHECK(!a[i] || i == s16 || i == sdp || i == ycc, "vk11 feature #%u advertised without a test", i);
      const VkBool32 *b = (const VkBool32 *)((const char *)&f12 + 16);
      const unsigned imgl = (unsigned)((const VkBool32 *)&f12.imagelessFramebuffer - b),   /* t_vk_feat imageless */
                     tl = (unsigned)((const VkBool32 *)&f12.timelineSemaphore - b), hq = (unsigned)((const VkBool32 *)&f12.hostQueryReset - b),
                     mc = (unsigned)((const VkBool32 *)&f12.samplerMirrorClampToEdge - b),
                     bda = (unsigned)((const VkBool32 *)&f12.bufferDeviceAddress - b), vmm = (unsigned)((const VkBool32 *)&f12.vulkanMemoryModel - b), vmd = (unsigned)((const VkBool32 *)&f12.vulkanMemoryModelDeviceScope - b),
                     di0 = (unsigned)((const VkBool32 *)&f12.runtimeDescriptorArray - b), di1 = (unsigned)((const VkBool32 *)&f12.shaderSampledImageArrayNonUniformIndexing - b),
                     di2 = (unsigned)((const VkBool32 *)&f12.descriptorBindingPartiallyBound - b), di3 = (unsigned)((const VkBool32 *)&f12.descriptorBindingSampledImageUpdateAfterBind - b),
                     di4 = (unsigned)((const VkBool32 *)&f12.descriptorBindingUpdateUnusedWhilePending - b),
                     s8 = (unsigned)((const VkBool32 *)&f12.storageBuffer8BitAccess - b), i8 = (unsigned)((const VkBool32 *)&f12.shaderInt8 - b),   /* t_vk_feat int8 */
                     sbl = (unsigned)((const VkBool32 *)&f12.scalarBlockLayout - b), ubs = (unsigned)((const VkBool32 *)&f12.uniformBufferStandardLayout - b); /* t_vk_feat scalar/ubostd */ /* t_vk_feat didx */ /* t_vk_feat bda */ /* t_vk_feat mirror */
      for (unsigned i = 0; i < (sizeof(f12) - 16) / 4; i++)
         CHECK(!b[i] || i == imgl || i == tl || i == hq || i == mc || i == bda || i == vmm || i == vmd || i == di0 || i == di1 || i == di2 || i == di3 || i == di4 || i == sbl || i == ubs || i == s8 || i == i8, "vk12 feature #%u advertised without a test", i);
      const VkBool32 *d = (const VkBool32 *)((const char *)&f13 + 16);
      const unsigned s2 = (unsigned)((const VkBool32 *)&f13.synchronization2 - d), dr = (unsigned)((const VkBool32 *)&f13.dynamicRendering - d),
                     m4i = (unsigned)((const VkBool32 *)&f13.maintenance4 - d), pdi = (unsigned)((const VkBool32 *)&f13.privateData - d),
                     zi = (unsigned)((const VkBool32 *)&f13.shaderZeroInitializeWorkgroupMemory - d),   /* t_vk_feat zinit */
                     dm = (unsigned)((const VkBool32 *)&f13.shaderDemoteToHelperInvocation - d), idp = (unsigned)((const VkBool32 *)&f13.shaderIntegerDotProduct - d),   /* t_vk_feat intdot */
                     sgc = (unsigned)((const VkBool32 *)&f13.subgroupSizeControl - d), cfs = (unsigned)((const VkBool32 *)&f13.computeFullSubgroups - d), iub = (unsigned)((const VkBool32 *)&f13.inlineUniformBlock - d);   /* t_vk_feat demote */
      for (unsigned i = 0; i < (sizeof(f13) - 16) / 4; i++)
         CHECK(!d[i] || i == s2 || i == dr || i == m4i || i == pdi || i == zi || i == dm || i == idp || i == sgc || i == cfs || i == iub, "vk13 feature #%u advertised without a test", i);
   }

   /* ---- queue + format consistency ---- */
   uint32_t qn = 0;
   GetPhysicalDeviceQueueFamilyProperties2(pd, &qn, NULL);
   CHECK(qn >= 1, "queue families");
   VkQueueFamilyProperties2 qp[4] = {0};
   for (int i = 0; i < 4; i++) qp[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
   if (qn > 4) qn = 4;
   GetPhysicalDeviceQueueFamilyProperties2(pd, &qn, qp);
   CHECK(qp[0].queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT, "gfx queue");
   CHECK(qp[0].queueFamilyProperties.timestampValidBits == 64, "timestampValidBits");
   {
      VkFormatProperties2 fp = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
      GetPhysicalDeviceFormatProperties2(pd, VK_FORMAT_R8G8B8A8_UNORM, &fp);
      CHECK(fp.formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, "RGBA8 sampled");
      CHECK(fp.formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, "RGBA8 color");
      /* HL2's swapchain/zink render targets: the packed A8B8G8R8 aliases must be renderable and sampled like RGBA8. */
      static const VkFormat argb8[] = {VK_FORMAT_A8B8G8R8_UNORM_PACK32, VK_FORMAT_A8B8G8R8_SRGB_PACK32};
      for (unsigned i = 0; i < 2; i++) {
         VkFormatProperties2 afp = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
         GetPhysicalDeviceFormatProperties2(pd, argb8[i], &afp);
         CHECK(afp.formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,
               "A8B8G8R8 variant %d: colour attachment", (int)argb8[i]);
         CHECK(afp.formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
               "A8B8G8R8 variant %d: sampled", (int)argb8[i]);
         CHECK(afp.formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT,
               "A8B8G8R8 variant %d: transfer src", (int)argb8[i]);
      }
      GetPhysicalDeviceFormatProperties2(pd, VK_FORMAT_BC7_UNORM_BLOCK, &fp);
      CHECK(fp.formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, "BC7 sampled (textureCompressionBC)");
      CHECK(!(fp.formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT), "BC7 not renderable");
   }
   /* FormatProperties3 without-format / depth-comparison bits must agree with the advertised features and the storage/sampled bits of the same tiling */
   {
      const uint64_t RD = 0x80000000ull, WR = 0x100000000ull, DC = 0x200000000ull;
      VkBool32 feat_rd = VK_FALSE, feat_wr = VK_FALSE;
      feat_rd = f2.features.shaderStorageImageReadWithoutFormat; feat_wr = f2.features.shaderStorageImageWriteWithoutFormat;
      unsigned nwr = 0, ndc = 0;
      for (unsigned fmt = 1; fmt <= 184; fmt++) {
         VkFormatProperties3 f3 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
         VkFormatProperties2 f2p = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &f3};
         GetPhysicalDeviceFormatProperties2(pd, (VkFormat)fmt, &f2p);
         const uint64_t t[3] = {f3.linearTilingFeatures, f3.optimalTilingFeatures, f3.bufferFeatures};
         const uint64_t st[3] = {VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT, VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT, VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_BIT};
         for (int k = 0; k < 3; k++) {
            CHECK(!(t[k] & RD) || feat_rd, "format %u set %d: READ_WITHOUT_FORMAT but shaderStorageImageReadWithoutFormat is false", fmt, k);
            CHECK(!(t[k] & WR) || (feat_wr && (t[k] & st[k])), "format %u set %d: WRITE_WITHOUT_FORMAT without feature or without the storage bit", fmt, k);
            CHECK(!(t[k] & RD) || (t[k] & st[k]), "format %u set %d: READ_WITHOUT_FORMAT without the storage bit", fmt, k);
            if ((t[k] & st[k]) && feat_wr) { CHECK(t[k] & WR, "format %u set %d: storage bit without WRITE_WITHOUT_FORMAT", fmt, k); nwr++; }
         }
         CHECK(!(f3.optimalTilingFeatures & DC) || (f3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT), "format %u: DEPTH_COMPARISON without SAMPLED_IMAGE", fmt);
         CHECK(!((f3.linearTilingFeatures | f3.bufferFeatures) & DC), "format %u: DEPTH_COMPARISON on linear/buffer", fmt);
         ndc += (f3.optimalTilingFeatures & DC) != 0;
      }
      CHECK(nwr > 0 && ndc > 0, "format sweep saw no storage-capable (%u) or depth-comparison (%u) format", nwr, ndc);
      CHECK(f2.features.shaderStorageImageWriteWithoutFormat, "writeWithoutFormat advertised");
   }

   /* ---- device + probes ---- */
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
                                  .pQueuePriorities = (float[]){1.0f}};
   VkPhysicalDeviceVulkan13Features en13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .synchronization2 = VK_TRUE, .dynamicRendering = VK_TRUE, .maintenance4 = VK_TRUE, .privateData = VK_TRUE,
      .inlineUniformBlock = VK_TRUE};   /* the layout-support probe uses an inline uniform block binding */
   VkPhysicalDeviceVulkan12Features en12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &en13,
      .timelineSemaphore = VK_TRUE, .hostQueryReset = VK_TRUE};
   const char *ext[] = {"VK_KHR_swapchain"};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &(VkPhysicalDeviceFeatures2){
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &en12,
      .features.textureCompressionBC = VK_TRUE},
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = ext};
   VkDevice dev = NULL;
   VkResult r = CreateDevice(pd, &dci, NULL, &dev);
   CHECK(r == VK_SUCCESS, "CreateDevice with advertised features: %d", r);
   if (!dev) return 1;
   gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
   for (unsigned i = 0; i < sizeof(core_dev) / sizeof(core_dev[0]); i++)
      CHECK(gdpa(dev, core_dev[i]) != NULL, "vkGetDeviceProcAddr(%s) is NULL", core_dev[i]);
   CHECK(gipa(inst, "vkGetPhysicalDeviceFeatures2") && gipa(inst, "vkGetPhysicalDeviceToolProperties"), "instance procs");

   /* An unadvertised feature must be rejected at device create (spec: VK_ERROR_FEATURE_NOT_PRESENT). */
   {
      VkPhysicalDeviceFeatures bad = {.geometryShader = VK_TRUE};
      VkDeviceCreateInfo d2 = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .pEnabledFeatures = &bad};
      VkDevice d = NULL;
      /* The driver names the field on stderr (the runtime's own message only reaches a debug messenger, which the
       * apps that hit this - zink, wined3d - do not install): capture it around the call. */
      char msg[512] = "";
      fflush(stderr);
      int saved = dup(2);
      FILE *cap = tmpfile();
      CHECK(saved >= 0 && cap, "stderr capture: dup/tmpfile failed");   /* no dup2 before the file exists */
      CHECK(dup2(fileno(cap), 2) >= 0, "stderr capture: dup2 failed");
      VkResult rr = CreateDevice(pd, &d2, NULL, &d);
      fflush(stderr);
      dup2(saved, 2);
      close(saved);
      rewind(cap);
      if (cap) {
         size_t n = fread(msg, 1, sizeof(msg) - 1, cap);
         msg[n] = 0;
         fclose(cap);
      }
      CHECK(rr == VK_ERROR_FEATURE_NOT_PRESENT, "geometryShader must be rejected (%d)", rr);
      CHECK(strstr(msg, "unsupported feature VkPhysicalDeviceFeatures::geometryShader") != NULL,
            "message names the feature: %s", msg);
   }
   /* Same for a feature struct in the pNext chain (extension features): the flattened field name is printed. */
   {
      VkPhysicalDeviceRobustness2FeaturesEXT r2 = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, .robustBufferAccess2 = VK_TRUE};
      VkDeviceCreateInfo d3 = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qci, .pNext = &r2};
      VkDevice d = NULL;
      char msg[512] = "";
      fflush(stderr);
      int saved = dup(2);
      FILE *cap = tmpfile();
      CHECK(saved >= 0 && cap && dup2(fileno(cap), 2) >= 0, "stderr capture (chain)");
      VkResult rr = CreateDevice(pd, &d3, NULL, &d);
      fflush(stderr);
      dup2(saved, 2);
      close(saved);
      rewind(cap);
      if (cap) {
         size_t n = fread(msg, 1, sizeof(msg) - 1, cap);
         msg[n] = 0;
         fclose(cap);
      }
      CHECK(rr == VK_ERROR_FEATURE_NOT_PRESENT, "robustBufferAccess2 must be rejected (%d)", rr);
      CHECK(strstr(msg, "unsupported feature robustBufferAccess2") != NULL, "message names the chain feature: %s", msg);
   }

#define D(n) PFN_vk##n n = (PFN_vk##n)gdpa(dev, "vk" #n)
   D(GetDeviceQueue2); D(CreatePrivateDataSlot); D(DestroyPrivateDataSlot); D(SetPrivateData); D(GetPrivateData);
   D(GetDeviceBufferMemoryRequirements); D(CreateQueryPool); D(DestroyQueryPool); D(ResetQueryPool);
   D(GetQueryPoolResults); D(CreateFence); D(DestroyFence); D(WaitForFences); D(QueueSubmit2);
   D(CreateDescriptorSetLayout); D(DestroyDescriptorSetLayout); D(CreateDescriptorPool); D(DestroyDescriptorPool);
   D(AllocateDescriptorSets); D(FreeDescriptorSets);
   D(CreateCommandPool); D(DestroyCommandPool); D(AllocateCommandBuffers); D(BeginCommandBuffer);
   D(EndCommandBuffer); D(CmdPipelineBarrier2); D(DestroyDevice); D(CreateSemaphore); D(DestroySemaphore);
   D(GetSemaphoreCounterValue); D(WaitSemaphores);

   VkQueue q = NULL;
   GetDeviceQueue2(dev, &(VkDeviceQueueInfo2){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2, .queueFamilyIndex = 0, .queueIndex = 0}, &q);
   CHECK(q != NULL, "GetDeviceQueue2");

   /* privateData */
   {
      VkPrivateDataSlot slot;
      CHECK(CreatePrivateDataSlot(dev, &(VkPrivateDataSlotCreateInfo){.sType = VK_STRUCTURE_TYPE_PRIVATE_DATA_SLOT_CREATE_INFO}, NULL, &slot) == VK_SUCCESS, "CreatePrivateDataSlot");
      VkFence f;
      CreateFence(dev, &(VkFenceCreateInfo){.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}, NULL, &f);
      uint64_t v = 1;
      CHECK(SetPrivateData(dev, VK_OBJECT_TYPE_FENCE, (uint64_t)f, slot, 0xabcdef12345ull) == VK_SUCCESS, "SetPrivateData");
      GetPrivateData(dev, VK_OBJECT_TYPE_FENCE, (uint64_t)f, slot, &v);
      CHECK(v == 0xabcdef12345ull, "GetPrivateData roundtrip %llx", (unsigned long long)v);
      DestroyFence(dev, f, NULL);
      DestroyPrivateDataSlot(dev, slot, NULL);
   }
   /* maintenance4: requirements without creating the buffer */
   {
      VkDeviceBufferMemoryRequirements req = {.sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,
         .pCreateInfo = &(VkBufferCreateInfo){.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT}};
      VkMemoryRequirements2 mr = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
      GetDeviceBufferMemoryRequirements(dev, &req, &mr);
      CHECK(mr.memoryRequirements.size >= 4096 && mr.memoryRequirements.memoryTypeBits, "maintenance4 buffer reqs size=%llu bits=%x",
            (unsigned long long)mr.memoryRequirements.size, mr.memoryRequirements.memoryTypeBits);
   }
   /* bind_memory2: a swapchain-aliased bind (memory == NULL + BindImageMemorySwapchainInfo) is not implemented and must not report success */
   {
      PFN_vkCreateImage CreateImage = (PFN_vkCreateImage)gdpa(dev, "vkCreateImage");
      PFN_vkDestroyImage DestroyImage = (PFN_vkDestroyImage)gdpa(dev, "vkDestroyImage");
      PFN_vkBindImageMemory2 BindImageMemory2 = (PFN_vkBindImageMemory2)gdpa(dev, "vkBindImageMemory2");
      VkImage im = VK_NULL_HANDLE;
      VkImageCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
         .extent = {16, 16, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_SAMPLED_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
      CHECK(CreateImage(dev, &ic, NULL, &im) == VK_SUCCESS, "CreateImage for bind test");
      VkBindImageMemorySwapchainInfoKHR sw = {.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_SWAPCHAIN_INFO_KHR, .swapchain = (VkSwapchainKHR)(uintptr_t)0x1234, .imageIndex = 0};
      VkBindImageMemoryInfo bi = {.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO, .pNext = &sw, .image = im, .memory = VK_NULL_HANDLE};
      VkResult bst = VK_SUCCESS;
      VkBindMemoryStatus bs = {.sType = VK_STRUCTURE_TYPE_BIND_MEMORY_STATUS, .pResult = &bst};
      sw.pNext = &bs;
      CHECK(BindImageMemory2(dev, 1, &bi) == VK_ERROR_UNKNOWN, "BindImageMemory2 with an unimplemented swapchain bind must return VK_ERROR_UNKNOWN");
      CHECK(bst == VK_ERROR_UNKNOWN, "the failed bind reports VK_ERROR_UNKNOWN through VkBindMemoryStatus (got %d)", (int)bst);
      DestroyImage(dev, im, NULL);
   }
   /* hostQueryReset */
   {
      VkQueryPool qpool;
      CHECK(CreateQueryPool(dev, &(VkQueryPoolCreateInfo){.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 4}, NULL, &qpool) == VK_SUCCESS, "CreateQueryPool");
      ResetQueryPool(dev, qpool, 0, 4);
      uint64_t res[4 * 2];
      VkResult rr = GetQueryPoolResults(dev, qpool, 0, 4, sizeof(res), res, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
      CHECK(rr == VK_NOT_READY || rr == VK_SUCCESS, "results after host reset: %d", rr);
      for (int i = 0; i < 4; i++) CHECK(res[i * 2 + 1] == 0, "query %d available after reset", i);
      DestroyQueryPool(dev, qpool, NULL);
   }
   /* EARLY_RETURN_ON_FAILURE, ci.flags form and maintenance5 flags2 form: [0] has no shader (FEATURE_NOT_PRESENT), [1] is garbage SPIR-V
      (INVALID_SHADER_NV). Early return reports the first failure, a plain batch the last one. */
   {
      PFN_vkCreateComputePipelines CCP = (PFN_vkCreateComputePipelines)gdpa(dev, "vkCreateComputePipelines");
      uint32_t junk[5] = {0xdeadbeef, 0, 0, 0, 0};
      VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof junk, .pCode = junk};
      VkPipelineCreateFlags2CreateInfoKHR f2 = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR, .flags = VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT_KHR};
      for (int v = 0; v < 3; v++) {
         VkComputePipelineCreateInfo pi[2] = {
            {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .pName = "main"}},
            {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = &smci, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .pName = "main"}},
         };
         if (v == 1) pi[0].flags = VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT;
         if (v == 2) pi[0].pNext = &f2;
         VkPipeline pp[2] = {(VkPipeline)1, (VkPipeline)1};
         VkResult rr = CCP(dev, VK_NULL_HANDLE, 2, pi, NULL, pp);
         CHECK(rr == (v ? VK_ERROR_FEATURE_NOT_PRESENT : VK_ERROR_INVALID_SHADER_NV), "early-return variant %d: result %d", v, rr);
         CHECK(pp[0] == VK_NULL_HANDLE && pp[1] == VK_NULL_HANDLE, "early-return variant %d: handles not nulled", v);
      }
   }
   /* synchronization2: submit2 + barrier2 + fence, and timeline signal via submit2 */
   {
      VkCommandPool cp;
      VkCommandBuffer cb;
      CHECK(CreateCommandPool(dev, &(VkCommandPoolCreateInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0}, NULL, &cp) == VK_SUCCESS, "pool");
      AllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1}, &cb);
      BeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO});
      CmdPipelineBarrier2(cb, &(VkDependencyInfo){.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
         .pMemoryBarriers = &(VkMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT}});
      CHECK(EndCommandBuffer(cb) == VK_SUCCESS, "End");
      VkSemaphoreTypeCreateInfo tci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
      VkSemaphore tl;
      CHECK(CreateSemaphore(dev, &(VkSemaphoreCreateInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &tci}, NULL, &tl) == VK_SUCCESS, "timeline");
      VkFence f;
      CreateFence(dev, &(VkFenceCreateInfo){.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}, NULL, &f);
      VkCommandBufferSubmitInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
      VkSemaphoreSubmitInfo sig = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = tl, .value = 5, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
      VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbi,
                          .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &sig};
      CHECK(QueueSubmit2(q, 1, &si, f) == VK_SUCCESS, "QueueSubmit2");
      CHECK(WaitForFences(dev, 1, &f, VK_TRUE, 10ull * 1000000000ull) == VK_SUCCESS, "fence wait");
      uint64_t val = 0, want = 5;
      CHECK(WaitSemaphores(dev, &(VkSemaphoreWaitInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &tl, .pValues = &want}, 10ull * 1000000000ull) == VK_SUCCESS, "timeline wait 5");
      GetSemaphoreCounterValue(dev, tl, &val);
      CHECK(val >= 5, "timeline value %llu", (unsigned long long)val);
      DestroyFence(dev, f, NULL);
      DestroySemaphore(dev, tl, NULL);
      DestroyCommandPool(dev, cp, NULL);
   }
   /* vkGetDescriptorSetLayoutSupport (mali_get_descriptor_set_layout_support) and vkFreeDescriptorSets (mali_free_descriptor_sets) */
   {
      PFN_vkGetDescriptorSetLayoutSupport gdsls = (PFN_vkGetDescriptorSetLayoutSupport)gdpa(dev, "vkGetDescriptorSetLayoutSupport");
      CHECK(gdsls, "no vkGetDescriptorSetLayoutSupport");
      if (gdsls) {
         VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 4,
                                           .stageFlags = VK_SHADER_STAGE_ALL};
         VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
         VkDescriptorSetLayoutSupport sup = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT};
         gdsls(dev, &lci, &sup);
         CHECK(sup.supported == VK_TRUE, "plain UBO layout reported unsupported");
         /* inline uniform block: descriptorCount is a byte size, multiple of 4 and at most 256 */
         b.descriptorType = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK;
         b.descriptorCount = 64;
         sup.supported = VK_FALSE;
         gdsls(dev, &lci, &sup);
         CHECK(sup.supported == VK_TRUE, "inline uniform block 64 B reported unsupported");
         b.descriptorCount = 62;   /* not a multiple of 4 */
         sup.supported = VK_TRUE;
         gdsls(dev, &lci, &sup);
         CHECK(sup.supported == VK_FALSE, "inline uniform block 62 B reported supported");
         b.descriptorCount = 1024; /* larger than the 256 B limit */
         sup.supported = VK_TRUE;
         gdsls(dev, &lci, &sup);
         CHECK(sup.supported == VK_FALSE, "inline uniform block 1024 B reported supported");
         /* a type the driver does not implement (acceleration structure) */
         b.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
         b.descriptorCount = 1;
         sup.supported = VK_TRUE;
         gdsls(dev, &lci, &sup);
         CHECK(sup.supported == VK_FALSE, "acceleration structure binding reported supported");
         /* more descriptor slots than the driver can address (the support query must say no, not crash) */
         b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
         b.descriptorCount = 0x7fffffffu;
         sup.supported = VK_TRUE;
         gdsls(dev, &lci, &sup);
         CHECK(sup.supported == VK_FALSE, "2^31 storage buffers reported supported");
      }

      VkDescriptorSetLayoutBinding sb = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
                                         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
      VkDescriptorSetLayout dsl;
      CHECK(CreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1, .pBindings = &sb}, NULL, &dsl) == VK_SUCCESS, "CreateDescriptorSetLayout");
      /* descriptor capacity for 4 sets, maxSets is 2: the third allocation must fail on maxSets, not on descriptor storage */
      VkDescriptorPoolSize ps = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 4};
      VkDescriptorPool pool;
      CHECK(CreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &ps}, NULL, &pool) == VK_SUCCESS,
            "CreateDescriptorPool");
      VkDescriptorSetLayout lays[3] = {dsl, dsl, dsl};
      VkDescriptorSet sets[3] = {0}, one = VK_NULL_HANDLE;
      VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool,
                                         .descriptorSetCount = 2, .pSetLayouts = lays};
      CHECK(AllocateDescriptorSets(dev, &dai, sets) == VK_SUCCESS && sets[0] && sets[1] && sets[0] != sets[1], "allocate 2 sets (maxSets 2)");
      dai.descriptorSetCount = 1;
      VkResult over = AllocateDescriptorSets(dev, &dai, &one);
      CHECK(over == VK_ERROR_OUT_OF_POOL_MEMORY && one == VK_NULL_HANDLE, "third set from a maxSets=2 pool: %d", over);
      /* free one set (and a VK_NULL_HANDLE entry, which must be ignored): its slot is available again */
      VkDescriptorSet fr[2] = {sets[0], VK_NULL_HANDLE};
      CHECK(FreeDescriptorSets(dev, pool, 2, fr) == VK_SUCCESS, "FreeDescriptorSets");
      CHECK(AllocateDescriptorSets(dev, &dai, &one) == VK_SUCCESS && one, "allocate after free");
      /* the pool is full again; the surviving set is still valid, so freeing it and the new one empties the pool */
      CHECK(AllocateDescriptorSets(dev, &dai, &(VkDescriptorSet){0}) == VK_ERROR_OUT_OF_POOL_MEMORY, "pool full again");
      VkDescriptorSet rest[2] = {sets[1], one};
      CHECK(FreeDescriptorSets(dev, pool, 2, rest) == VK_SUCCESS, "free remaining");
      dai.descriptorSetCount = 2;
      CHECK(AllocateDescriptorSets(dev, &dai, sets) == VK_SUCCESS && sets[0] && sets[1], "allocate 2 after freeing all");
      DestroyDescriptorPool(dev, pool, NULL);   /* frees the sets still in the pool */
      DestroyDescriptorSetLayout(dev, dsl, NULL);
   }
   DestroyDevice(dev, NULL);

   if (g_dump) {
      printf("apiVersion=%u.%u.%u device=%s driver=%s (%s) subgroupSize=%u maxImageDimension2D=%u maxColorAttachments=%u\n",
             VK_API_VERSION_MAJOR(pr.properties.apiVersion), VK_API_VERSION_MINOR(pr.properties.apiVersion),
             VK_API_VERSION_PATCH(pr.properties.apiVersion), pr.properties.deviceName, drv.driverName, drv.driverInfo,
             p11.subgroupSize, L->maxImageDimension2D, L->maxColorAttachments);
   }
   PFN_vkDestroyInstance di = DestroyInstance;
   di(inst, NULL);
   printf("{\"test\":\"t_vk_info\",\"pass\":%s}\n", g_fail ? "false" : "true");
   return g_fail;
}
