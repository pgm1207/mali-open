/* SPDX-License-Identifier: MIT */
/* Own Mali Vulkan front end on Mesa's common Vulkan runtime (vk_*). Nothing here is derived from
 * src/panfrost/vulkan. */
#ifndef MALI_PRIVATE_H
#define MALI_PRIVATE_H

#include "vk_device.h"
#include "vk_instance.h"
#include "vk_log.h"
#include "vk_physical_device.h"
#include "vk_queue.h"
#include "vk_sync.h"
#include "vk_sync_timeline.h"
#include <pthread.h>

#include "mali_backend.h"
#include "wsi_common.h"

#define MALI_VENDOR_ID_ARM 0x13B5

struct mali_instance {
   struct vk_instance vk;
};

struct mali_physical_device {
   struct vk_physical_device vk;

   const struct mali_backend_ops *backend;
   struct mali_gpu_info gpu;
   VkPhysicalDeviceMemoryProperties memory;
   uint64_t mem_used;   /* bytes of live VkDeviceMemory over all devices (atomic; VK_EXT_memory_budget) */
   uint64_t mem_cap;    /* device-wide commit budget: min(heap_bytes/2, RAM/4), MALI_VRAM_CAP_MB overrides (mali_memory.c) */

   /* M4: binary fences/semaphores on kbase fences + the runtime's emulated timeline on top. */
   struct vk_sync_timeline_type timeline_type;
   const struct vk_sync_type *sync_types[3];

   /* M14 (mali_wsi.c): Mesa wsi_common, Wayland platform, linear dma-buf swapchain images. */
   struct wsi_device wsi;
   PFN_vkDestroyQueryPool wsi_real_destroy_query_pool;   /* what wsi.DestroyQueryPool pointed to before the NULL-tolerant guard */
};

struct mali_queue {
   struct vk_queue vk;
};

struct mali_device {
   struct vk_device vk;
   struct vk_device_dispatch_table cmd_dispatch;   /* primary command buffers: the driver's own Cmd* (secondaries record into the runtime cmd queue) */

   struct mali_kdev *kdev;
   struct mali_queue queue;
   bool has_queue;

   /* M4 (mali_sync.c, mali_memory.c, mali_queue.c) */
   pthread_mutex_t sync_lock; /* guards every mali_sync's state */
   pthread_cond_t sync_cv;    /* a sync got a fence or was signalled */
   int32_t mem_count;         /* live VkDeviceMemory, vs maxMemoryAllocationCount */
   struct mali_hwctx *hw;     /* M9: per-device frame resources (mali_queue.c) */
   int present_fence_fd;      /* dup of the last submission's sync_file: the CPU-side present paths wait it (-1 = none) */
};

VK_DEFINE_HANDLE_CASTS(mali_instance, vk.base, VkInstance, VK_OBJECT_TYPE_INSTANCE)
VK_DEFINE_HANDLE_CASTS(mali_physical_device, vk.base, VkPhysicalDevice,
                       VK_OBJECT_TYPE_PHYSICAL_DEVICE)
VK_DEFINE_HANDLE_CASTS(mali_device, vk.base, VkDevice, VK_OBJECT_TYPE_DEVICE)

/* Instance-level API version. The instance level is implemented by the common runtime, so it can
 * be ahead of the device apiVersion, which starts at 1.0 and is raised per milestone. */
#define MALI_INSTANCE_API_VERSION VK_MAKE_API_VERSION(0, 1, 3, VK_HEADER_VERSION)
#define MALI_DEVICE_API_VERSION   VK_MAKE_API_VERSION(0, 1, 3, VK_HEADER_VERSION)

VkResult mali_enumerate_physical_devices(struct vk_instance *vk_instance);
/* MALI_TRACE=<file> post-mortem trace line (no-op unless set); see mali_instance.c */
void mali_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* mali_memory.c: a backend pool really freed `size` bytes of BOs it had kept (see mali_backend_ops.bo_pool_watch). */
void mali_memory_pool_release(void *pdev, size_t size);
/* M5/M7 seams (mali_resources.c): installs the descriptor/image resolvers and the BO-backed GPU pools. Idempotent. */
void mali_resources_init(void);
/* M14 */
VkResult mali_wsi_init(struct mali_physical_device *pdev);
void mali_wsi_finish(struct mali_physical_device *pdev);
extern const uint64_t mali_wsi_modifier; /* DRM_FORMAT_MOD_LINEAR, the only one advertised */
/* mali_wsi.c: vkCreateSwapchainKHR with the X11 diagnostics and the no-DRI3 software fallback (mali_device.c). */
VkResult mali_wsi_create_swapchain(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
VkResult mali_wsi_acquire_next_image(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*);
VkResult mali_wsi_queue_present(VkQueue, const VkPresentInfoKHR*);
VkResult mali_wsi_get_present_modes(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkPresentModeKHR*);
void mali_wsi_destroy_swapchain(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
void mali_destroy_physical_device(struct vk_physical_device *vk_pdev);

#endif
