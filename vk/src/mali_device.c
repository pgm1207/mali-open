/* SPDX-License-Identifier: MIT */
#include "mali_private.h"
#include "mali_feature_names.h"   /* vkCreateDevice diagnostic: field names of the generated feature table */

#include <errno.h>

#include "mali_entrypoints.h"
#include "mali_cmd.h"
#include "mali_queue.h"
#include "vk_alloc.h"
#include "vk_cmd_enqueue_entrypoints.h"
#include "vk_common_entrypoints.h"

/* v10 (G610/G710/G510/G310), v11 (G615), v13 (G925 class): arch 12 shares the v13 table. */
static const struct vk_device_entrypoint_table *
mali_arch_entrypoints(const struct mali_physical_device *pdev)
{
   return pdev->gpu.arch_major >= 12 ? &mali_v13_device_entrypoints :
          pdev->gpu.arch_major == 10 ? &mali_v10_device_entrypoints : &mali_v11_device_entrypoints;
}

static VkResult
mali_get_timestamp(struct vk_device *vk, uint64_t *timestamp)
{
   struct mali_device *dev = container_of(vk, struct mali_device, vk);
   const struct mali_backend_ops *be = mali_device_pdev(dev)->backend;
   return be->gpu_timestamp && be->gpu_timestamp(dev->kdev, timestamp) == 0 ? VK_SUCCESS : VK_ERROR_FEATURE_NOT_PRESENT;
}

/* The runtime's own check (third_party/mesa/src/vulkan/util/vk_physical_device_features_gen.py:234) returns the
 * right VkResult but its message only reaches a VK_EXT_debug_utils messenger, which the apps that hit this (zink,
 * wined3d) do not install: the failure was a black box ("ZINK: vkCreateDevice failed
 * (VK_ERROR_FEATURE_NOT_PRESENT)"). Name the first unsupported field on stderr, one line, then let
 * vk_device_init return the error. */
static const char *
mali_first_unsupported_feature(const struct vk_features *req, const struct vk_features *supported,
                               const struct mali_feature_name *table, size_t count)
{
   for (size_t i = 0; i < count; i++)
      if (*(const bool *)((const char *)req + table[i].off) &&
          !*(const bool *)((const char *)supported + table[i].off))
         return table[i].name;
   return NULL;
}

static VkResult
mali_check_device_features(const struct mali_physical_device *pdev, const VkDeviceCreateInfo *pCreateInfo)
{
   const struct vk_features *supported = &pdev->vk.supported_features;
   struct vk_features req = {0};
   const char *missing;

   if (pCreateInfo->pEnabledFeatures) {
      vk_set_physical_device_features_1_0(&req, pCreateInfo->pEnabledFeatures);
      missing = mali_first_unsupported_feature(&req, supported, mali_1_0_feature_names,
                                               ARRAY_SIZE(mali_1_0_feature_names));
      if (missing) {
         fprintf(stderr, "mali_vk: vkCreateDevice: unsupported feature VkPhysicalDeviceFeatures::%s\n", missing);
         return VK_ERROR_FEATURE_NOT_PRESENT;
      }
   }

   /* Every VkPhysicalDevice*Features struct in the chain (Features2 itself included) flattened the way the
    * runtime does it, so the name we print is the field name the application actually set. */
   struct vk_features chain = {0};
   VkPhysicalDeviceFeatures2 features2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = (void *)pCreateInfo->pNext,
   };
   vk_set_physical_device_features(&chain, &features2);
   /* The chain carries core 1.1+ groups and extensions alike: check the extension table, then the core 1.0 name
    * table, so a group requested only through pNext is still refused by its field name. */
   missing = mali_first_unsupported_feature(&chain, supported, mali_ext_feature_names,
                                            ARRAY_SIZE(mali_ext_feature_names));
   if (!missing)
      missing = mali_first_unsupported_feature(&chain, supported, mali_1_0_feature_names,
                                               ARRAY_SIZE(mali_1_0_feature_names));
   if (missing) {
      fprintf(stderr, "mali_vk: vkCreateDevice: unsupported feature %s (requested through the "
              "VkPhysicalDeviceFeatures2 chain)\n", missing);
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }
   return VK_SUCCESS;
}

static VkResult
mali_CreateDevice_impl(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
   VK_FROM_HANDLE(mali_physical_device, pdev, physicalDevice);
   struct mali_device *dev;
   VkResult result;

   dev = vk_zalloc2(&pdev->vk.instance->alloc, pAllocator, sizeof(*dev), 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!dev)
      return vk_error(pdev, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* Per-arch entry points (files compiled once per PAN_ARCH, from M6 on) win over the
    * arch-independent ones; the runtime adds its vk_common_* defaults last. */
   struct vk_device_dispatch_table dispatch_table;
   /* Secondary command buffers: every Cmd* of the main table becomes vk_cmd_enqueue_unless_primary_Cmd* (records into the runtime queue for
    * secondaries, calls the primary dispatch table below otherwise); vkCmdExecuteCommands replays the queue into the primary. */
   vk_device_dispatch_table_from_entrypoints(&dispatch_table, &vk_cmd_enqueue_unless_primary_device_entrypoints, true);
   vk_device_dispatch_table_from_entrypoints(
      &dev->cmd_dispatch,
      mali_arch_entrypoints(pdev),
      true);
   vk_device_dispatch_table_from_entrypoints(&dev->cmd_dispatch, &mali_device_entrypoints, false);
   vk_device_dispatch_table_from_entrypoints(&dev->cmd_dispatch, &wsi_device_entrypoints, false);
   vk_device_dispatch_table_from_entrypoints(&dev->cmd_dispatch, &vk_common_device_entrypoints, false);
   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table,
      mali_arch_entrypoints(pdev),
      false);
   vk_device_dispatch_table_from_entrypoints(&dispatch_table, &mali_device_entrypoints, false);
   vk_device_dispatch_table_from_entrypoints(&dispatch_table, &wsi_device_entrypoints, false);
   dispatch_table.CreateSwapchainKHR = mali_wsi_create_swapchain;   /* mali_wsi.c: X11 diagnostics + no-DRI3 fallback */
   dispatch_table.AcquireNextImageKHR = mali_wsi_acquire_next_image;   /* mali_wsi.c: stage trace around the swapchain wait */
   dispatch_table.AcquireNextImage2KHR = mali_wsi_acquire_next_image2; /* same, on the chain's wsi_device (CR #323 T3) */
   dispatch_table.QueuePresentKHR = mali_wsi_queue_present;           /* mali_wsi.c: stage trace around present */
   dispatch_table.DestroySwapchainKHR = mali_wsi_destroy_swapchain;   /* mali_wsi.c: release the MIT-SHM segment */
   dispatch_table.WaitForPresentKHR = mali_wsi_wait_for_present;      /* mali_wsi.c: software X11 chains never complete an id */

   result = mali_check_device_features(pdev, pCreateInfo);
   if (result != VK_SUCCESS) {
      vk_free2(&pdev->vk.instance->alloc, pAllocator, dev);
      return vk_error(pdev, result);
   }
   result = vk_device_init(&dev->vk, &pdev->vk, &dispatch_table, pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free2(&pdev->vk.instance->alloc, pAllocator, dev);
      return result;
   }

   mali_resources_init();
   mali_queue_device_init(dev);
   result = pdev->backend->device_create(&pdev->gpu, &dev->kdev);
   if (result != 0) {
      result = vk_error(dev, result == -ENOMEM ? VK_ERROR_OUT_OF_HOST_MEMORY
                                                : VK_ERROR_INITIALIZATION_FAILED);
      mali_queue_device_finish(dev);
      vk_device_finish(&dev->vk);
      vk_free2(&pdev->vk.instance->alloc, pAllocator, dev);
      return result;
   }

   dev->vk.command_dispatch_table = &dev->cmd_dispatch;   /* after vk_device_init (it zeroes the struct) */
   /* Queue commands (vkQueuePresentKHR) are resolved from the command dispatch table, not the device one, so the
    * present hook has to be installed here as well; without this the hook never ran and its trace stayed empty. */
   dev->cmd_dispatch.QueuePresentKHR = mali_wsi_queue_present;
   dev->cmd_dispatch.AcquireNextImageKHR = mali_wsi_acquire_next_image;
   /* A backend pool keeps freed BOs committed: its releases go back to this physical device's VRAM budget. */
   if (pdev->backend->bo_pool_watch)
      pdev->backend->bo_pool_watch(dev->kdev, mali_memory_pool_release, pdev);
   dev->vk.command_buffer_ops = &mali_cmd_ops;
   dev->vk.get_timestamp = mali_get_timestamp;                       /* VK_KHR_calibrated_timestamps: runtime does the rest */
   dev->vk.calibrate_time_domain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_KHR;
   result = mali_hw_init(dev);
   if (result != VK_SUCCESS) {
      pdev->backend->device_destroy(dev->kdev);
      mali_queue_device_finish(dev);
      vk_device_finish(&dev->vk);
      vk_free2(&pdev->vk.instance->alloc, pAllocator, dev);
      return vk_error(pdev, result);
   }

   /* One queue family with one queue (see mali_GetPhysicalDeviceQueueFamilyProperties2). */
   for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
      const VkDeviceQueueCreateInfo *qinfo = &pCreateInfo->pQueueCreateInfos[i];

      if (qinfo->queueFamilyIndex != 0 || qinfo->queueCount != 1 || dev->has_queue) {
         result = vk_error(dev, VK_ERROR_INITIALIZATION_FAILED);
         goto fail;
      }

      result = vk_queue_init(&dev->queue.vk, &dev->vk, qinfo, 0);
      if (result != VK_SUCCESS)
         goto fail;
      dev->has_queue = true;
      mali_queue_setup(dev);
   }

   *pDevice = mali_device_to_handle(dev);
   return VK_SUCCESS;

fail:
   if (dev->has_queue)
      vk_queue_finish(&dev->queue.vk);
   mali_hw_finish(dev);
   pdev->backend->device_destroy(dev->kdev);
   mali_queue_device_finish(dev);
   vk_device_finish(&dev->vk);
   vk_free2(&pdev->vk.instance->alloc, pAllocator, dev);
   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
   VkResult r = mali_CreateDevice_impl(physicalDevice, pCreateInfo, pAllocator, pDevice);

   mali_trace("vkCreateDevice exts=%u queues=%u -> %d", pCreateInfo->enabledExtensionCount, pCreateInfo->queueCreateInfoCount, (int)r);
   if (r != VK_SUCCESS)
      for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++)
         mali_trace("  requested ext %s", pCreateInfo->ppEnabledExtensionNames[i]);
   return r;
}

VKAPI_ATTR void VKAPI_CALL
mali_DestroyDevice(VkDevice _device, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(mali_device, dev, _device);

   if (!dev)
      return;

   /* vk_device_init stored the effective allocator (pAllocator or the instance's). */
   const VkAllocationCallbacks alloc = dev->vk.alloc;

   if (dev->has_queue)
      vk_queue_finish(&dev->queue.vk);
   struct mali_physical_device *pdev = container_of(dev->vk.physical, struct mali_physical_device, vk);
   mali_hw_finish(dev);
   pdev->backend->device_destroy(dev->kdev);
   mali_queue_device_finish(dev);
   vk_device_finish(&dev->vk);
   vk_free(&alloc, dev);
}

/* R4-M1: core 1.2/1.3 entrypoints the runtime does not provide. The features behind them are not
 * advertised (bufferDeviceAddressCaptureReplay, sparse), so they report "nothing". */
VKAPI_ATTR uint64_t VKAPI_CALL
mali_GetBufferOpaqueCaptureAddress(VkDevice device, const VkBufferDeviceAddressInfo *pInfo)
{
   return 0;
}

VKAPI_ATTR uint64_t VKAPI_CALL
mali_GetDeviceMemoryOpaqueCaptureAddress(VkDevice device, const VkDeviceMemoryOpaqueCaptureAddressInfo *pInfo)
{
   return 0;
}

VKAPI_ATTR void VKAPI_CALL
mali_GetDeviceImageSparseMemoryRequirements(VkDevice device, const VkDeviceImageMemoryRequirements *pInfo,
                                            uint32_t *pSparseMemoryRequirementCount,
                                            VkSparseImageMemoryRequirements2 *pSparseMemoryRequirements)
{
   *pSparseMemoryRequirementCount = 0;
}
