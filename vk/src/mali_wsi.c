/* SPDX-License-Identifier: MIT */
/* M14: window system integration through Mesa's wsi_common (Wayland platform). Swapchain images are
 * ordinary driver images made linear (scanout flag / DRM modifier LINEAR) on exportable dma-buf BOs
 * (MALI_BO_EXPORT via VkExportMemoryAllocateInfo); presentation sync is implicit: wsi_common exports a
 * sync_file from a semaphore the present submission signals (mali_sync export_sync_file) and imports it
 * into the image's dma-buf (DMA_BUF_IOCTL_IMPORT_SYNC_FILE). No explicit-sync protocol (gamescope has none).
 * Everything else lives in the runtime, wsi_common and the entrypoints in mali_image.c / mali_memory.c. */
#include "mali_private.h"
#include "mali_image.h"
#include "mali_queue.h"

#include "wsi_common_private.h"   /* struct wsi_interface (pdev->wsi.wsi[]) */
#include "vk_fence.h"             /* VkSwapchainPresentFenceInfoEXT in the shm shortcut below (#325) */

#include <pthread.h>   /* the per-call wsi mode and the X11 chain table are process-wide (CR #323 T4/T5) */

/* wsi_common.c's surface entrypoints. Mesa declares them in no header (only wsi_physical_device_entrypoints
 * references them), so the wrappers below carry their own prototypes. */
VKAPI_ATTR VkResult VKAPI_CALL wsi_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32 *);
VKAPI_ATTR VkResult VKAPI_CALL wsi_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR *);
VKAPI_ATTR VkResult VKAPI_CALL wsi_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkSurfaceFormatKHR *);
VKAPI_ATTR VkResult VKAPI_CALL wsi_GetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR*, VkSurfaceCapabilities2KHR *);
VKAPI_ATTR VkResult VKAPI_CALL wsi_GetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR*, uint32_t *, VkSurfaceFormat2KHR *);

/* wsi_common.c: what the device dispatch would have called; not in wsi_common.h in this tree. */
VKAPI_ATTR VkResult VKAPI_CALL wsi_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
                                                     const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain);
VKAPI_ATTR VkResult VKAPI_CALL wsi_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                                       VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex);
VKAPI_ATTR VkResult VKAPI_CALL wsi_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo);
VKAPI_ATTR void VKAPI_CALL wsi_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                  const VkAllocationCallbacks *pAllocator);
#if defined(VK_USE_PLATFORM_XCB_KHR)
VKAPI_ATTR VkBool32 VKAPI_CALL wsi_GetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice, uint32_t,
                                                                             xcb_connection_t *, xcb_visualid_t);
#endif
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
VKAPI_ATTR VkBool32 VKAPI_CALL wsi_GetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice, uint32_t,
                                                                                   struct wl_display *);
#endif
#if defined(VK_USE_PLATFORM_XLIB_KHR)
VKAPI_ATTR VkBool32 VKAPI_CALL wsi_GetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice, uint32_t,
                                                                              Display *, VisualID);
#endif
VKAPI_ATTR VkResult VKAPI_CALL wsi_WaitForPresentKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t presentId,
                                                    uint64_t timeout);

#include "vk_alloc.h"
#include "vk_queue.h"      /* VK_FROM_HANDLE(vk_queue) */
#include "vk_semaphore.h"  /* struct vk_semaphore -> &sem->sync */
#include "vk_fence.h"      /* VkSwapchainPresentFenceInfoEXT in the shm shortcut (#325) */
#include "vk_log.h"
#include "vk_util.h"

#include <pthread.h>
#include <stdlib.h>
#include <errno.h>
#include <poll.h>
#include <string.h>

#if defined(VK_USE_PLATFORM_XCB_KHR)
#include <xcb/xcb.h>
#include <xcb/xcbext.h>   /* xcb_poll_for_reply: the non-blocking gate drain */
#include <xcb/shm.h>
#endif
#if defined(__linux__)
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#endif

struct mali_x11_shm;
static void mali_x11_shm_destroy(VkSwapchainKHR sc);
static struct mali_x11_shm *mali_x11_shm_find(VkSwapchainKHR sc);
static bool mali_x11_shm_present(struct mali_device *dev, struct wsi_swapchain *chain, uint32_t image_index, bool *gated);
static void mali_x11_shm_drain(struct mali_x11_shm *st, bool force);

#define MALI_DRM_FORMAT_MOD_LINEAR 0ull
const uint64_t mali_wsi_modifier = MALI_DRM_FORMAT_MOD_LINEAR;

static VKAPI_PTR PFN_vkVoidFunction
mali_wsi_proc_addr(VkPhysicalDevice h, const char *name)
{
   VK_FROM_HANDLE(mali_physical_device, pdev, h);
   return vk_instance_get_proc_addr_unchecked(pdev->vk.instance, name);
}

/* wsi_common calls DestroyQueryPool(VK_NULL_HANDLE) unconditionally per image; the driver may not implement
 * queries yet (entrypoint NULL), so route through a guard that tolerates both. */
static VKAPI_ATTR void VKAPI_CALL
mali_wsi_destroy_query_pool(VkDevice _dev, VkQueryPool pool, const VkAllocationCallbacks *a)
{
   VK_FROM_HANDLE(mali_device, dev, _dev);
   PFN_vkDestroyQueryPool real = mali_device_pdev(dev)->wsi_real_destroy_query_pool;   /* per physical device, no process-global */
   if (pool != VK_NULL_HANDLE && real)
      real(_dev, pool, a);
}

/* X11 WSI (wsi_common_x11, see STEAM-GAPS.md "X11 WSI"): two present paths, chosen once per device.
 *   dma-buf (default on hardware): images are the linear exportable BOs above; the X server imports them through DRI3
 *     (Present). Needs an X server that can import our dma-bufs: Xwayland with glamor on a render node. DroidDeck's
 *     Xwayland on Mali has no DRM node, hence no DRI3 (docs/development/mali-venus.md).
 *   software (fake backend, or MALI_WSI=sw): wsi_common copies each frame into a CPU-visible linear buffer and the X server
 *     gets it by MIT-SHM / PutImage (what lavapipe does, and what the vendor bridge does for X11). Needs no dma-buf at all,
 *     so it also works on the fake backend (whose BO fds are not dma-bufs). The device-wide switch also moves the Wayland
 *     platform to wl_shm: do not use it in a process whose Wayland swapchains must stay zero-copy (the gamescope layer's).
 * MALI_WSI=dmabuf forces the first path on the fake backend (it then fails at swapchain creation; for tests). */
/* Nothing here touches the X server until an X11 surface exists: mali_wsi_init() runs inside
 * vkEnumeratePhysicalDevices for every physical device, whatever the platform, so a blocking xcb_connect() there
 * hung Wayland clients (vkcube said nothing at all) and X11 clients before their first log line. Extension
 * presence is read from the client's own setup reply (no round trip, no connect). */
#if defined(VK_USE_PLATFORM_XCB_KHR)
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/shm.h>
#endif

static bool
mali_wsi_force_no_dri3(void)
{
   const char *force = getenv("MALI_WSI_FORCE_NO_DRI3");   /* test hook: behave like a server without DRI3 */
   return force && *force && strcmp(force, "0");
}

static bool
mali_wsi_use_sw(const struct mali_physical_device *pdev)
{
   const char *e = getenv("MALI_WSI");
   if (e && !strcmp(e, "sw"))
      return true;
   if (e && (!strcmp(e, "dmabuf") || !strcmp(e, "dmabuf-strict")))
      return false;
   /* MALI_WSI=hw: pretend to be hardware on the fake backend - start on the dma-buf path and let the per-surface
    * decision (mali_wsi_x11_maybe_sw) fall back where the server cannot serve it. Host test hook for
    * tests/xwsi-nodri3.sh; the BO fds of the fake backend are not dma-bufs, so a server that does have DRI3 then
    * fails at import exactly like MALI_WSI=dmabuf. */
   if (e && !strcmp(e, "hw"))
      return false;
   /* No X probe here (see above): the software path is chosen per surface, on the first surface query. */
   return pdev->gpu.fake;
}

/* X11 swapchain diagnostics and the no-DRI3 fallback (STEAM-GAPS.md "X11 WSI").
 *
 * wsi_common_x11's dma-buf path imports every swapchain image into an X pixmap with DRI3
 * (wsi_common_x11.c x11_image_init -> xcb_dri3_pixmap_from_buffer / _from_buffers). An X server without DRI3
 * answers that request with an error, x11_image_init returns VK_ERROR_INITIALIZATION_FAILED and the whole
 * vkCreateSwapchainKHR fails: zink reports it as "could not create swapchain" (zink_resource.c:1974) and the GL
 * client then dies on its GLX drawable. Xwayland on a GPU with no DRM node is exactly such a server
 * (docs/development/mali-venus.md), and the software path (shm/PutImage) is the only one that works there.
 *
 * MALI_WSI_DEBUG=1 prints one line per X11 swapchain creation with the request, what the surface offers, what the
 * connection can do and the VkResult; a failed creation always prints a line. */
/* A software swapchain image is a CPU buffer with no dma-buf fd, so the dma-buf implicit-sync path cannot work on
 * it: wsi_common creates a dma-buf semaphore when wsi_drm_check_dma_buf_sync_file_import_export()
 * (wsi_common_drm.c:97) answers "works", and then every present calls
 * wsi_dma_buf_import_sync_file(image->dma_buf_fd == -1, ...), which fails with EBADF and is mapped to
 * VK_ERROR_FEATURE_NOT_PRESENT (wsi_common_drm.c:214) before the frame is put. Declare the capability we have
 * (none) in wsi's cache, which stores result - 1. This has to be applied when the *swapchain* is created, not only
 * when the device was built in software mode: on hardware the software path is chosen per surface. */
static void
mali_wsi_disable_dmabuf_sync(struct wsi_device *wsi)
{
   wsi->cached_sync_file_import_export_result = (uint32_t)(VK_ERROR_FEATURE_NOT_PRESENT - 1);
}

static bool
mali_wsi_debug(void)
{
   static int on = -1;
   if (on < 0) {
      const char *e = getenv("MALI_WSI_DEBUG");
      on = e && *e && strcmp(e, "0") ? 1 : 0;
   }
   return on;
}

static const char *
mali_wsi_platform_name(int platform)
{
   switch (platform) {
   case VK_ICD_WSI_PLATFORM_XCB:     return "xcb";
   case VK_ICD_WSI_PLATFORM_XLIB:    return "xlib";
   case VK_ICD_WSI_PLATFORM_WAYLAND: return "wayland";
   default:                          return "other";
   }
}

struct mali_wsi_x11_probe {
   bool have_conn;
   bool dri3, present, mit_shm;
};

/* Reads the client's own X connection (the one the surface was made on). Extension presence comes from the
 * connection's prefetched setup reply (xcb_get_extension_data never sends a request), so this cannot block on an
 * unresponsive server. XLIB surfaces are not probed (that would need libX11 in the ICD) and take software. */
#if defined(VK_USE_PLATFORM_XCB_KHR)
static void
mali_wsi_x11_probe_conn(xcb_connection_t *conn, struct mali_wsi_x11_probe *p)
{
   memset(p, 0, sizeof(*p));
   if (!conn)
      return;
   const struct xcb_query_extension_reply_t *dri3 = xcb_get_extension_data(conn, &xcb_dri3_id);
   const struct xcb_query_extension_reply_t *present = xcb_get_extension_data(conn, &xcb_present_id);
   const struct xcb_query_extension_reply_t *shm = xcb_get_extension_data(conn, &xcb_shm_id);
   p->have_conn = true;
   p->dri3 = dri3 && dri3->present && !mali_wsi_force_no_dri3();
   p->present = present && present->present;
   p->mit_shm = shm && shm->present;
}
#endif

static void
mali_wsi_x11_probe(VkIcdSurfaceBase *surface, struct mali_wsi_x11_probe *p)
{
   memset(p, 0, sizeof(*p));
#if defined(VK_USE_PLATFORM_XCB_KHR)
   if (surface->platform != VK_ICD_WSI_PLATFORM_XCB)
      return;
   mali_wsi_x11_probe_conn(((VkIcdSurfaceXcb *)surface)->connection, p);
#else
   (void)surface;
#endif
}

static bool
mali_wsi_x11_forced_shm(void)
{
   const char *e = getenv("MALI_WSI");
   return e && (!strcmp(e, "shm") || !strcmp(e, "xput") || !strcmp(e, "sw"));
}

static bool
mali_wsi_x11_forced_dmabuf(void)
{
   const char *e = getenv("MALI_WSI");
   return e && (!strcmp(e, "dmabuf") || !strcmp(e, "dmabuf-strict"));
}

/* The X11 present path is a device-wide decision (wsi_device->sw), and it has to be taken at the *first* surface
 * query, not in vkCreateSwapchainKHR. zink (kopper) asks vkGetPhysicalDeviceSurfaceSupportKHR before it builds
 * anything and gives up when the answer is FALSE (zink_kopper.c:130-132); wsi_common_x11 answers FALSE on a server
 * without DRI3 while wsi_device->sw is false (wsi_common_x11.c:713-719, after "No DRI3 support detected" at
 * :411-420), so zink prints "could not create swapchain" (zink_resource.c:1974) and the GL client never gets a
 * window - it never reaches vkCreateSwapchainKHR, where this switch used to be made. Every query we override
 * (support, caps, formats, present modes, Xcb/Xlib presentation support) and the swapchain now take the same
 * decision, so they all describe the path the swapchain will really use.
 * Untouched when the server has DRI3 *and* Present (the dma-buf path, which the CMF and X8 use), and
 * MALI_WSI=dmabuf still forces dma-buf everywhere. */
static bool mali_wsi_x11_sw_decided(const struct mali_physical_device *pdev, uintptr_t conn, int platform);
static void mali_wsi_info_add(VkSwapchainKHR sc, VkIcdSurfaceBase *surface, const VkSwapchainCreateInfoKHR *ci,
                              struct wsi_swapchain *chain, bool sw);
static void mali_wsi_info_remove(VkSwapchainKHR sc);   /* #329: the MALI_WSI_DUMP / MALI_WSI_DEBUG chain table */
static void mali_wsi_present_trace(struct mali_device *dev, VkSwapchainKHR handle, struct wsi_swapchain *chain,
                                   uint32_t image_index);
static bool mali_wsi_x11_sw_record(struct mali_physical_device *pdev, uintptr_t conn, int platform);
static uintptr_t mali_wsi_x11_conn(VkIcdSurfaceBase *surface);

/* Returns this call's decision: true when the surface runs on the software (PutImage) instance. The caller uses the
 * return value, not a fresh table lookup, so a full table cannot lose the decision of the probe that just made it
 * (CR #323 T2). */
static bool
mali_wsi_x11_maybe_sw_conn(struct mali_physical_device *pdev, int platform, uintptr_t conn,
                           const struct mali_wsi_x11_probe *probe)
{
   /* Not wsi_dev->sw: that is the *current call's* mode, so another thread's XLIB software call would make this
    * XCB query skip its own decision (CR #323 N1). wsi_sw_base is the device's own floor. */
   if (pdev->wsi_sw_base)
      return true;
   if (mali_wsi_x11_sw_decided(pdev, conn, platform))
      return true;
   if (platform != VK_ICD_WSI_PLATFORM_XCB && platform != VK_ICD_WSI_PLATFORM_XLIB)
      return false;
   if (mali_wsi_x11_forced_dmabuf())
      return false;
   const bool forced_shm = mali_wsi_x11_forced_shm();
   if (!forced_shm && probe->dri3 && probe->present)
      return false;
   /* The decision stays in the pdev: wsi.sw is device-wide, and Mesa's Wayland driver turns every Wayland swapchain
    * into a wl_shm/CPU one while it is set (wsi_common_wayland.c:3940/:1506). A Steam client asks about its X11
    * window first (zink_kopper.c:130) and would lose the dma-buf path for the gamescope WSI layer's Wayland
    * swapchain, which is how #322 broke X8. It is applied by mali_wsi_sw_enter() for X11 calls only. */
   /* Per protocol, not per device: an XLIB query must not change what an XCB swapchain on the same device does
    * (CR #323 T0). The device-wide fields (wsi.sw, supports_modifiers, the dma-buf sync cache) are set for the
    * duration of one call by mali_wsi_sw_enter() and restored by mali_wsi_sw_leave() (CR #323 T1). */
   mali_wsi_x11_sw_record(pdev, conn, platform);   /* per connection, not per device: CR #323 N2 */
   fprintf(stderr, "mali_vk: wsi: %s surface: software (PutImage) present path (%s) (MALI_WSI=dmabuf for dma-buf)\n",
           mali_wsi_platform_name(platform),
           forced_shm ? "forced" : !probe->present ? "server has no Present" : "server has no DRI3");
   return true;
}

static bool
mali_wsi_x11_maybe_sw(struct mali_physical_device *pdev, VkIcdSurfaceBase *surface,
                      const struct mali_wsi_x11_probe *probe)
{
   return mali_wsi_x11_maybe_sw_conn(pdev, surface->platform, mali_wsi_x11_conn(surface), probe);
}

static bool
mali_wsi_x11_maybe_sw_surface(struct mali_physical_device *pdev, VkSurfaceKHR surface)
{
   VkIcdSurfaceBase *base = (VkIcdSurfaceBase *)(uintptr_t)surface;
   struct mali_wsi_x11_probe probe;
   if (!base)
      return false;
   mali_wsi_x11_probe(base, &probe);
   return mali_wsi_x11_maybe_sw(pdev, base, &probe);
}

/* What the surface offers, as wsi_common_x11 answers it, so a failed creation can be read without a second
 * process: the requested format/usage/mode/extent/count next to the accepted ones. */
static void
mali_wsi_x11_log_surface(struct mali_physical_device *pdev, VkIcdSurfaceBase *surface)
{
   struct wsi_interface *iface = pdev->wsi.wsi[surface->platform];
   if (!iface)
      return;
   VkSurfaceCapabilities2KHR caps = { .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR };
   if (iface->get_capabilities2 &&
       iface->get_capabilities2(surface, pdev->vk.wsi_device, NULL, &caps) == VK_SUCCESS) {
      const VkSurfaceCapabilitiesKHR *c = &caps.surfaceCapabilities;
      fprintf(stderr, "mali_vk: wsi surface caps: currentExtent=%ux%u minImageCount=%u maxImageCount=%u "
              "supportedUsage=0x%x compositeAlpha=0x%x transform=0x%x\n",
              c->currentExtent.width, c->currentExtent.height, c->minImageCount, c->maxImageCount,
              c->supportedUsageFlags, c->supportedCompositeAlpha, c->currentTransform);
   }
   VkSurfaceFormatKHR fmt[16];
   uint32_t n = 16;
   if (iface->get_formats && iface->get_formats(surface, pdev->vk.wsi_device, &n, fmt) == VK_SUCCESS) {
      fprintf(stderr, "mali_vk: wsi surface formats:");
      for (uint32_t i = 0; i < n && i < 16; i++)
         fprintf(stderr, " %u/%u", fmt[i].format, fmt[i].colorSpace);
      fprintf(stderr, "%s\n", n > 16 ? " ..." : "");
   }
   VkPresentModeKHR mode[8];
   n = 8;
   if (iface->get_present_modes && mali_wsi_get_present_modes(mali_physical_device_to_handle(pdev),
                                                             (VkSurfaceKHR)(uintptr_t)surface, &n, mode) == VK_SUCCESS) {
      fprintf(stderr, "mali_vk: wsi present modes:");
      for (uint32_t i = 0; i < n && i < 8; i++)
         fprintf(stderr, " %u", mode[i]);
      fprintf(stderr, "%s\n", n > 8 ? " ..." : "");
   }
}

/* One wsi call's present path, as the wsi_device it runs on. Two immutable instances live in the pdev (the dma-buf
 * one wsi_device_init built, and the software copy mali_wsi_init makes), so a call only has to point the physical
 * device's wsi_device at the right one - nothing is mutated, and a swapchain keeps the instance it was created with
 * for its whole life (create, async present, destroy). */
struct mali_wsi_mode {
   struct wsi_device *saved;
};
static struct mali_wsi_mode mali_wsi_sw_enter(struct mali_physical_device *pdev, VkIcdSurfaceBase *surface,
                                               bool decided_sw);
static struct mali_wsi_mode mali_wsi_sw_enter_value(struct mali_physical_device *pdev, bool sw);
static void mali_wsi_sw_leave(struct mali_physical_device *pdev, struct mali_wsi_mode saved);
static void mali_wsi_x11_chain_remove(VkSwapchainKHR sc);
static bool mali_wsi_chain_is_x11(VkSwapchainKHR sc);
static bool mali_wsi_x11_chain_mode(VkSwapchainKHR sc, bool *sw);

/* wsi_common_x11 advertises all four present modes, but a software (PutImage) swapchain is only useful to a
 * measurement when IMMEDIATE is really offered: with FIFO/MAILBOX the producer rate equals the displayed rate by
 * construction, which is what hid the GPU headroom (1600x720 and 2400x1080 both 118 fps). IMMEDIATE is additive,
 * FIFO/MAILBOX stay first and unchanged for real apps. */
static void mali_wsi_present_id_set(VkSwapchainKHR sc, uint64_t id);
static uint64_t mali_wsi_present_id_take(VkSwapchainKHR sc);

/* An X11 software (MIT-SHM or PutImage) chain cannot ever complete a present id in wsi_common_x11: the software
 * present sends xcb_put_image/xcb_shm_put_image and no Present request, so there is no PresentCompleteNotify event to
 * run x11_present_complete() (wsi_common_x11.c:1702), the only place chain->present_id advances
 * (wsi_common_x11.c:1444), and x11_wait_for_present() loops on present_progress_cond until its timeout
 * (wsi_common_x11.c:3075). An app that enables VK_KHR_present_wait - DXVK's frame limiter, gamescope - would pay its
 * whole wait timeout on every frame presented on this path and never see a completed present. The frame is already on
 * the server when our present returns (copy + xcb_flush), so report it as presented: a false VK_TIMEOUT blocks the
 * client on every frame. Every other chain keeps Mesa's own wait. */
VKAPI_ATTR VkResult VKAPI_CALL
mali_wsi_wait_for_present(VkDevice device, VkSwapchainKHR swapchain, uint64_t present_id, uint64_t timeout)
{
   /* #323 records the instance each X11 chain was created on (mali_wsi_sw_enter_value), so this is the chain's own
    * decision, not the device-wide one: a dma-buf chain on the same connection keeps Mesa's real present wait. */
   bool sw = false;
   if (mali_wsi_x11_chain_mode(swapchain, &sw) && sw)
      return VK_SUCCESS;
   return wsi_WaitForPresentKHR(device, swapchain, present_id, timeout);
}

VKAPI_ATTR void VKAPI_CALL
mali_wsi_destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *alloc)
{
   /* x11_image_finish() reads chain->base.wsi->sw (wsi_common_x11.c:2807) and takes the DRI3 branch unless it is set,
    * so a chain created on the software path must be destroyed on it: a no-MIT-SHM server otherwise gets a
    * xcb_sync_destroy_fence on a fence x11_image_init never created. Recorded per chain at creation (CR #323 T2). */
   bool chain_sw = false;
   const bool is_x11 = mali_wsi_x11_chain_mode(swapchain, &chain_sw);
   struct mali_wsi_mode saved = {0};
   if (is_x11)
      saved = mali_wsi_sw_enter_value(mali_device_pdev(mali_device_from_handle(device)), chain_sw);
   /* CR #288: the one place the presentation feedback of the last frame must be pumped. wsi_common_wayland asserts
    * that no present id is still queued while it frees the chain (wsi_common_wayland.c:3782, :2779), and an app that
    * presents its last frame never pumps it again. Bounded (20 ms ns cap), once per chain and outside the frame
    * loop - instead of the 20 ms wait per present this used to be (see struct mali_wsi_present_id). Runs before
    * wsi_DestroySwapchainKHR because the chain, its queue and wait_for_present2 are Mesa's and die with it. */
   VK_FROM_HANDLE(wsi_swapchain, chain, swapchain);
   const uint64_t last_id = mali_wsi_present_id_take(swapchain);
   if (chain && last_id && chain->present_wait_enabled && chain->wait_for_present2)
      chain->wait_for_present2(chain, last_id, 20ull * 1000 * 1000);   /* ns, bounded */
   /* Drop the record first: wsi_DestroySwapchainKHR runs Mesa's cleanup, which frees the chain before it returns, and
    * another thread creating a chain could be handed that address and register it - a later remove would then clear
    * the new chain's entry (CR #323 N3). */
   mali_wsi_info_remove(swapchain);   /* #329's dump table: drop it before Mesa frees the chain */
   mali_wsi_x11_chain_remove(swapchain);
   mali_x11_shm_destroy(swapchain);
   wsi_DestroySwapchainKHR(device, swapchain, alloc);
   if (is_x11)
      mali_wsi_sw_leave(mali_device_pdev(mali_device_from_handle(device)), saved);
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_wsi_get_present_modes(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
                           uint32_t *count, VkPresentModeKHR *modes)
{
   VK_FROM_HANDLE(vk_physical_device, vk_pdev, physicalDevice);
   struct mali_physical_device *pdev = container_of(vk_pdev, struct mali_physical_device, vk);
   struct wsi_interface *iface = pdev->wsi.wsi[((VkIcdSurfaceBase *)(uintptr_t)surface)->platform];

   if (!iface)
      return VK_ERROR_SURFACE_LOST_KHR;
   /* before the list is built: IMMEDIATE goes with the software path */
   const bool sw_now = mali_wsi_x11_maybe_sw_surface(pdev, surface);
   const struct mali_wsi_mode saved = mali_wsi_sw_enter(pdev, (VkIcdSurfaceBase *)(uintptr_t)surface, sw_now);
   if (!modes) {
      VkResult r = iface->get_present_modes((VkIcdSurfaceBase *)(uintptr_t)surface, pdev->vk.wsi_device, count, NULL);
      mali_wsi_sw_leave(pdev, saved);
      if (r == VK_SUCCESS && pdev->wsi.sw && *count < UINT32_MAX)
         (*count)++;   /* upper bound: the enumeration below decides whether IMMEDIATE is really missing */
      return r;
   }
   /* Enumerate, then return the list unchanged except for IMMEDIATE appended on the software path. */
   VkPresentModeKHR all[8];
   uint32_t n = 8;
   VkResult r = iface->get_present_modes((VkIcdSurfaceBase *)(uintptr_t)surface, pdev->vk.wsi_device, &n, all);
   mali_wsi_sw_leave(pdev, saved);
   if (r != VK_SUCCESS && r != VK_INCOMPLETE)
      return r;
   const bool x11 = ((VkIcdSurfaceBase *)(uintptr_t)surface)->platform == VK_ICD_WSI_PLATFORM_XCB ||
                    ((VkIcdSurfaceBase *)(uintptr_t)surface)->platform == VK_ICD_WSI_PLATFORM_XLIB;
   if (pdev->wsi.sw && x11) {   /* X11 only: Wayland's driver (wsi_common_wayland) has its own list */
      bool has = false;
      for (uint32_t i = 0; i < n; i++)
         has |= all[i] == VK_PRESENT_MODE_IMMEDIATE_KHR;
      if (!has && n < 8)
         all[n++] = VK_PRESENT_MODE_IMMEDIATE_KHR;
   }
   const uint32_t out = MIN2(*count, n);
   memcpy(modes, all, out * sizeof(*modes));
   *count = out;
   return out < n ? VK_INCOMPLETE : VK_SUCCESS;
}

/* Mesa's wsi_device->sw is device-wide, but the no-DRI3 fallback is an X11 decision (see the long note in
 * mali_wsi_create_swapchain): wsi_common_wayland binds wl_shm instead of zwp_linux_dmabuf_v1 and builds every
 * swapchain as a CPU/shm one while it is set (wsi_common_wayland.c:3940, :1506). Run one wsi call for a surface of
 * `platform` on the path that platform uses: X11 keeps the device-wide decision (mali_wsi_x11_maybe_sw), everything
 * else gets the path the device was built with (mali_wsi_use_sw: MALI_WSI, the fake backend). */
/* Is an X11 chain? X11 acquire/present read chain->base.wsi->sw too (wsi_common_x11.c:2101/2654/2807), so those
 * need the same switch, and destroy needs the mode the chain was *created* with (x11_image_finish branches on
 * chain->base.wsi->sw, wsi_common_x11.c:2807 - CR #323 T2). The table is process-wide (Mesa's wsi_device is shared
 * by every chain of a physical device), so every access takes the lock below, and it records the mode. A chain that
 * does not fit is refused at creation instead of being silently untracked (CR #323 T3). */
struct mali_wsi_x11_chain {
   VkSwapchainKHR sc;
   bool sw;
};
/* 16, not 8: a swapchain replacement (oldSwapchain = a live chain) needs a second slot for the same surface while
 * the app still holds the old one, so eight live chains plus eight replacements fit. Beyond that creation still
 * refuses cleanly (CR #323 T3/N8). */
static struct mali_wsi_x11_chain mali_wsi_x11_chains[16];

/* One recursive lock for the per-call mode and the chain table: two threads on different surfaces (one X11, one
 * Wayland) would otherwise race on wsi_device's device-wide fields, and add/remove/lookup would race on the table.
 * Recursive so a helper can take it while a wsi call already holds it. */
static pthread_mutex_t mali_wsi_lock;
static pthread_once_t mali_wsi_lock_once = PTHREAD_ONCE_INIT;

static void
mali_wsi_lock_init(void)
{
   pthread_mutexattr_t attr;
   pthread_mutexattr_init(&attr);
   pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
   pthread_mutex_init(&mali_wsi_lock, &attr);
   pthread_mutexattr_destroy(&attr);
}

static void
mali_wsi_lock_take(void)
{
   pthread_once(&mali_wsi_lock_once, mali_wsi_lock_init);
   pthread_mutex_lock(&mali_wsi_lock);
}

static void
mali_wsi_lock_release(void)
{
   pthread_mutex_unlock(&mali_wsi_lock);
}

/* The X connection behind an X11 surface: two xcb_connection_t or two Display* in one process are two servers. */
static uintptr_t
mali_wsi_x11_conn(VkIcdSurfaceBase *surface)
{
#if defined(VK_USE_PLATFORM_XCB_KHR)
   if (surface->platform == VK_ICD_WSI_PLATFORM_XCB)
      return (uintptr_t)((VkIcdSurfaceXcb *)surface)->connection;
#endif
#if defined(VK_USE_PLATFORM_XLIB_KHR)
   if (surface->platform == VK_ICD_WSI_PLATFORM_XLIB)
      return (uintptr_t)((VkIcdSurfaceXlib *)surface)->dpy;
#endif
   return 0;
}

/* The server's resource-id base for this connection: xcb_connection_t addresses are reused after a disconnect, the
 * base is not (it is allocated per client connection by the server). 0 for XLIB or when the probe cannot tell.
 * Only an XCB connection may be handed to libxcb: an XLIB surface carries a Display *, a libX11 object that libxcb
 * must never interpret (CR #323 T1). libX11's own connection can be reached with XGetXCBConnection(), but that
 * needs libX11-xcb in the ICD; without it the display pointer is the whole key and the base stays 0. */
static uint32_t
mali_wsi_x11_conn_base(uintptr_t conn, int platform)
{
   if (platform != VK_ICD_WSI_PLATFORM_XCB)
      return 0;
#if defined(VK_USE_PLATFORM_XCB_KHR)
   xcb_connection_t *c = (xcb_connection_t *)conn;
   if (!c || xcb_connection_has_error(c))
      return 0;
   const xcb_setup_t *setup = xcb_get_setup(c);
   return setup ? (uint32_t)setup->resource_id_base : 0;
#else
   (void)conn;
   return 0;
#endif
}

static bool
mali_wsi_x11_sw_decided(const struct mali_physical_device *pdev, uintptr_t conn, int platform)
{
   if (!conn)
      return false;
   for (unsigned i = 0; i < sizeof(pdev->wsi_x11_sw) / sizeof(pdev->wsi_x11_sw[0]); i++)
      if (pdev->wsi_x11_sw[i].conn == conn &&
          pdev->wsi_x11_sw[i].base == mali_wsi_x11_conn_base(conn, platform))
         return pdev->wsi_x11_sw[i].sw;
   return false;   /* not probed yet: dma-buf, the default for a server we have not seen (the query probes) */
}

/* Returns whether the table now carries the software decision for this connection. A full table keeps the decision
 * for the current call (the caller uses the return value of mali_wsi_x11_maybe_sw_conn), it only loses the cache for
 * later calls - which re-probe and decide the same way (CR #323 T2). */
static bool
mali_wsi_x11_sw_record(struct mali_physical_device *pdev, uintptr_t conn, int platform)
{
   const unsigned n = sizeof(pdev->wsi_x11_sw) / sizeof(pdev->wsi_x11_sw[0]);
   if (!conn)
      return false;
   const uint32_t base = mali_wsi_x11_conn_base(conn, platform);
   mali_wsi_lock_take();
   unsigned free_slot = n;
   for (unsigned i = 0; i < n; i++) {
      if (pdev->wsi_x11_sw[i].conn == conn && pdev->wsi_x11_sw[i].base == base) {
         pdev->wsi_x11_sw[i].sw = true;
         mali_wsi_lock_release();
         return true;
      }
      if (!pdev->wsi_x11_sw[i].conn && free_slot == n)
         free_slot = i;
   }
   if (free_slot < n) {
      pdev->wsi_x11_sw[free_slot].conn = conn;
      pdev->wsi_x11_sw[free_slot].base = base;
      pdev->wsi_x11_sw[free_slot].sw = true;
   }
   mali_wsi_lock_release();
   return free_slot < n;
}

/* Returns false when the table is full: the caller must destroy the chain it just created. */
static bool
mali_wsi_x11_chain_add(VkSwapchainKHR sc, bool sw)
{
   bool added = false;
   mali_wsi_lock_take();
   for (unsigned i = 0; i < sizeof(mali_wsi_x11_chains) / sizeof(mali_wsi_x11_chains[0]); i++)
      if (!mali_wsi_x11_chains[i].sc) {
         mali_wsi_x11_chains[i] = (struct mali_wsi_x11_chain){.sc = sc, .sw = sw};
         added = true;
         break;
      }
   pthread_mutex_unlock(&mali_wsi_lock);
   return added;
}

static void
mali_wsi_x11_chain_remove(VkSwapchainKHR sc)
{
   mali_wsi_lock_take();
   for (unsigned i = 0; i < sizeof(mali_wsi_x11_chains) / sizeof(mali_wsi_x11_chains[0]); i++)
      if (mali_wsi_x11_chains[i].sc == sc) mali_wsi_x11_chains[i] = (struct mali_wsi_x11_chain){0};
   pthread_mutex_unlock(&mali_wsi_lock);
}

static bool
mali_wsi_chain_is_x11(VkSwapchainKHR sc)
{
   bool found = false;
   mali_wsi_lock_take();
   for (unsigned i = 0; i < sizeof(mali_wsi_x11_chains) / sizeof(mali_wsi_x11_chains[0]); i++)
      if (mali_wsi_x11_chains[i].sc == sc) found = true;
   pthread_mutex_unlock(&mali_wsi_lock);
   return found;
}

/* The mode one X11 chain was created with, for its destroy. */
static bool
mali_wsi_x11_chain_mode(VkSwapchainKHR sc, bool *sw)
{
   bool found = false;
   mali_wsi_lock_take();
   for (unsigned i = 0; i < sizeof(mali_wsi_x11_chains) / sizeof(mali_wsi_x11_chains[0]); i++)
      if (mali_wsi_x11_chains[i].sc == sc) {
         if (sw) *sw = mali_wsi_x11_chains[i].sw;
         found = true;
      }
   pthread_mutex_unlock(&mali_wsi_lock);
   return found;
}

/* Runs one wsi call on the software or the dma-buf wsi_device and keeps the lock until mali_wsi_sw_leave(). Nothing
 * is mutated: both instances are immutable (built in mali_wsi_init), so the only shared state is the pointer the
 * physical device hands to Mesa, and the lock covers exactly that. Only short calls (queries, swapchain creation)
 * use it - acquire and present do not need a mode at all (CR #323 N4), and a chain keeps its instance for its life,
 * including the async present queue manager (CR #323 T7). */
static struct mali_wsi_mode
mali_wsi_sw_enter_value(struct mali_physical_device *pdev, bool sw)
{
   mali_wsi_lock_take();
   struct mali_wsi_mode saved = {.saved = pdev->vk.wsi_device};
   pdev->vk.wsi_device = sw ? &pdev->wsi_sw_dev : &pdev->wsi;
   return saved;
}

/* The path one call of this surface runs on. wsi_sw_base is what Mesa/the device was built for
 * (MESA_VK_WSI_DEBUG=sw, the fake backend, MALI_WSI=sw) and a floor: lowering it would put a device whose wsi
 * callbacks were fetched for software (GetMemoryFdKHR is NULL then, wsi_common.c:221) on the dma-buf path - the same
 * invariant stated in the one term that crashes when it is violated. The X11 decision is per connection (N2). */
static bool
mali_wsi_surface_sw(struct mali_physical_device *pdev, VkIcdSurfaceBase *surface, bool decided_sw)
{
   const bool x11 = surface->platform == VK_ICD_WSI_PLATFORM_XCB || surface->platform == VK_ICD_WSI_PLATFORM_XLIB;
   return pdev->wsi_sw_base || decided_sw ||
          (x11 && mali_wsi_x11_sw_decided(pdev, mali_wsi_x11_conn(surface), surface->platform)) ||
          pdev->wsi.GetMemoryFdKHR == NULL;
}

/* decided_sw: what this call's probe returned (mali_wsi_x11_maybe_sw*), so the selection does not depend on the
 * decision having fitted in the connection table (CR #323 T2). */
static struct mali_wsi_mode
mali_wsi_sw_enter(struct mali_physical_device *pdev, VkIcdSurfaceBase *surface, bool decided_sw)
{
   return mali_wsi_sw_enter_value(pdev, mali_wsi_surface_sw(pdev, surface, decided_sw));
}

static void
mali_wsi_sw_leave(struct mali_physical_device *pdev, struct mali_wsi_mode saved)
{
   pdev->vk.wsi_device = saved.saved;
   pthread_mutex_unlock(&mali_wsi_lock);
}

/* Surface queries in front of wsi_common. The first one decides the present path (mali_wsi_x11_maybe_sw above), so
 * wsi_common_x11's own DRI3 gate (wsi_common_x11.c:713-719 for support, :646 for the presentation-support queries)
 * sees the software device and answers "supported" where MIT-SHM can serve the surface, before zink gives up on the
 * answer (zink_kopper.c:130-132). The answers themselves are wsi's, unchanged. */
VKAPI_ATTR VkResult VKAPI_CALL
mali_wsi_get_surface_support(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, VkSurfaceKHR surface,
                             VkBool32 *supported)
{
   VK_FROM_HANDLE(vk_physical_device, vk_pdev, physicalDevice);
   struct mali_physical_device *pdev = container_of(vk_pdev, struct mali_physical_device, vk);
   const bool sw_now = mali_wsi_x11_maybe_sw_surface(pdev, surface);
   const struct mali_wsi_mode saved = mali_wsi_sw_enter(pdev, (VkIcdSurfaceBase *)(uintptr_t)surface, sw_now);
   VkResult r = wsi_GetPhysicalDeviceSurfaceSupportKHR(physicalDevice, queueFamilyIndex, surface, supported);
   mali_wsi_sw_leave(pdev, saved);
   return r;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_wsi_get_surface_capabilities(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
                                  VkSurfaceCapabilitiesKHR *caps)
{
   VK_FROM_HANDLE(vk_physical_device, vk_pdev, physicalDevice);
   struct mali_physical_device *pdev = container_of(vk_pdev, struct mali_physical_device, vk);
   const bool sw_now = mali_wsi_x11_maybe_sw_surface(pdev, surface);
   const struct mali_wsi_mode saved = mali_wsi_sw_enter(pdev, (VkIcdSurfaceBase *)(uintptr_t)surface, sw_now);
   VkResult r = wsi_GetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, caps);
   mali_wsi_sw_leave(pdev, saved);
   return r;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_wsi_get_surface_formats(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t *count,
                             VkSurfaceFormatKHR *formats)
{
   VK_FROM_HANDLE(vk_physical_device, vk_pdev, physicalDevice);
   struct mali_physical_device *pdev = container_of(vk_pdev, struct mali_physical_device, vk);
   const bool sw_now = mali_wsi_x11_maybe_sw_surface(pdev, surface);
   const struct mali_wsi_mode saved = mali_wsi_sw_enter(pdev, (VkIcdSurfaceBase *)(uintptr_t)surface, sw_now);
   VkResult r = wsi_GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, count, formats);
   mali_wsi_sw_leave(pdev, saved);
   return r;
}
/* The *2 forms answer from wsi_device directly and were unwrapped: wsi_common.c:1149/1244 take
 * `device->wsi_device` and wsi_common_x11 gates get_capabilities2/get_formats2 on wsi->sw like the non-2 forms
 * (wsi_common_x11.c:646), so on a server without DRI3 they answered the dma-buf story while
 * vkGetPhysicalDeviceSurface*KHR answered supported - a client that uses the 2 form (zink asks
 * vkGetPhysicalDeviceSurfaceCapabilities2KHR) saw the two disagree (CR #323 N5). Present rectangles are consumed by
 * the chain's own present (chain->base.wsi) and are not affected. */
VKAPI_ATTR VkResult VKAPI_CALL
mali_wsi_get_surface_capabilities2(VkPhysicalDevice physicalDevice,
                                   const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
                                   VkSurfaceCapabilities2KHR *pSurfaceCapabilities)
{
   VK_FROM_HANDLE(vk_physical_device, vk_pdev, physicalDevice);
   struct mali_physical_device *pdev = container_of(vk_pdev, struct mali_physical_device, vk);
   VkIcdSurfaceBase *surface = (VkIcdSurfaceBase *)(uintptr_t)pSurfaceInfo->surface;
   const bool sw_now = mali_wsi_x11_maybe_sw_surface(pdev, pSurfaceInfo->surface);
   const struct mali_wsi_mode saved = mali_wsi_sw_enter(pdev, surface, sw_now);
   VkResult r = wsi_GetPhysicalDeviceSurfaceCapabilities2KHR(physicalDevice, pSurfaceInfo, pSurfaceCapabilities);
   mali_wsi_sw_leave(pdev, saved);
   return r;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_wsi_get_surface_formats2(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
                              uint32_t *pSurfaceFormatCount, VkSurfaceFormat2KHR *pSurfaceFormats)
{
   VK_FROM_HANDLE(vk_physical_device, vk_pdev, physicalDevice);
   struct mali_physical_device *pdev = container_of(vk_pdev, struct mali_physical_device, vk);
   VkIcdSurfaceBase *surface = (VkIcdSurfaceBase *)(uintptr_t)pSurfaceInfo->surface;
   const bool sw_now = mali_wsi_x11_maybe_sw_surface(pdev, pSurfaceInfo->surface);
   const struct mali_wsi_mode saved = mali_wsi_sw_enter(pdev, surface, sw_now);
   VkResult r = wsi_GetPhysicalDeviceSurfaceFormats2KHR(physicalDevice, pSurfaceInfo, pSurfaceFormatCount,
                                                        pSurfaceFormats);
   mali_wsi_sw_leave(pdev, saved);
   return r;
}

/* wsi_common_x11 gates these on wsi_device->sw too (wsi_common_x11.c:646), and a client may ask them before any
 * surface query, so they take the same per-call switch as the queries. Without it, a zink client on the no-DRI3
 * server asked "presentation support" while wsi.sw was at rest (false, the dma-buf answer) and got FALSE. */
#if defined(VK_USE_PLATFORM_XCB_KHR)
VKAPI_ATTR VkBool32 VKAPI_CALL
mali_wsi_xcb_presentation_support(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex,
                                  xcb_connection_t *connection, xcb_visualid_t visual_id)
{
   VK_FROM_HANDLE(vk_physical_device, vk_pdev, physicalDevice);
   struct mali_physical_device *pdev = container_of(vk_pdev, struct mali_physical_device, vk);
   /* A client may ask this before any surface exists (CR #323 T6): probe the connection it hands us, so the answer
    * describes the server this window will be presented on, not "no decision yet". */
   struct mali_wsi_x11_probe probe;
   mali_wsi_x11_probe_conn(connection, &probe);
   const bool sw_now = mali_wsi_x11_maybe_sw_conn(pdev, VK_ICD_WSI_PLATFORM_XCB, (uintptr_t)connection, &probe);
   const struct mali_wsi_mode saved =
      mali_wsi_sw_enter_value(pdev, pdev->wsi_sw_base || sw_now ||
                                        mali_wsi_x11_sw_decided(pdev, (uintptr_t)connection,
                                                                VK_ICD_WSI_PLATFORM_XCB) ||
                                        pdev->wsi.GetMemoryFdKHR == NULL);
   VkBool32 r = wsi_GetPhysicalDeviceXcbPresentationSupportKHR(physicalDevice, queueFamilyIndex, connection, visual_id);
   mali_wsi_sw_leave(pdev, saved);
   return r;
}
#endif

#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
VKAPI_ATTR VkBool32 VKAPI_CALL
mali_wsi_wayland_presentation_support(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex,
                                      struct wl_display *display)
{
   VK_FROM_HANDLE(vk_physical_device, vk_pdev, physicalDevice);
   struct mali_physical_device *pdev = container_of(vk_pdev, struct mali_physical_device, vk);
   /* It initialises a throwaway wsi_wl_display with wsi_device->sw (wsi_common_wayland.c:1839), so it must see
    * Wayland's mode, not an X11 call's: same scope as the *2KHR queries (CR #323 N5). */
   const struct mali_wsi_mode saved = mali_wsi_sw_enter_value(pdev, pdev->wsi_sw_base ||
                                                                      pdev->wsi.GetMemoryFdKHR == NULL);
   VkBool32 r = wsi_GetPhysicalDeviceWaylandPresentationSupportKHR(physicalDevice, queueFamilyIndex, display);
   mali_wsi_sw_leave(pdev, saved);
   return r;
}
#endif

#if defined(VK_USE_PLATFORM_XLIB_KHR)
VKAPI_ATTR VkBool32 VKAPI_CALL
mali_wsi_xlib_presentation_support(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex,
                                   Display *dpy, VisualID visual_id)
{
   VK_FROM_HANDLE(vk_physical_device, vk_pdev, physicalDevice);
   struct mali_physical_device *pdev = container_of(vk_pdev, struct mali_physical_device, vk);
   /* No probe for XLIB (it would need libX11 in the ICD), so the XLIB decision is the software one; make it here
    * rather than depending on an earlier surface query (CR #323 T6). */
   struct mali_wsi_x11_probe probe;
   memset(&probe, 0, sizeof probe);
   const bool sw_now = mali_wsi_x11_maybe_sw_conn(pdev, VK_ICD_WSI_PLATFORM_XLIB, (uintptr_t)dpy, &probe);
   const struct mali_wsi_mode saved =
      mali_wsi_sw_enter_value(pdev, pdev->wsi_sw_base || sw_now ||
                                        mali_wsi_x11_sw_decided(pdev, (uintptr_t)dpy, VK_ICD_WSI_PLATFORM_XLIB) ||
                                        pdev->wsi.GetMemoryFdKHR == NULL);
   VkBool32 r = wsi_GetPhysicalDeviceXlibPresentationSupportKHR(physicalDevice, queueFamilyIndex, dpy, visual_id);
   mali_wsi_sw_leave(pdev, saved);
   return r;
}
#endif

/* Software present over MIT-SHM, our own segment: the frame leaves the driver as a memcpy into shared pages plus
 * XShmPutImage on the socket, instead of a ~w*h*4 byte PutImage request per frame (which capped the CMF at ~113 fps
 * whatever the resolution). We do not need EXT_external_memory_host for this: the segment is ours
 * (shmget/shmat), attached to the server with XShmAttach-equivalent xcb_shm_attach.
 *
 * One segment holds one buffer per swapchain image (offset = image_index * stride * height): a present writes the
 * new frame into its own buffer, so a server still reading the previous image cannot show a torn mix of two frames,
 * and nothing has to be copied twice. The whole segment is attached once per swapchain and released with
 * shmdt + shmctl(IPC_RMID) on destroy; a leaked segment is pinned memory that survives the process.
 * MALI_WSI=xput forces the old PutImage path (A/B). */
/* A present whose image may not be rewritten yet (design-shm.md A). After each PutImage we queue a GetGeometry on the
 * same connection; X answers in order, so the reply proves the server has processed our PutImage. The image is
 * released only then, which is what stops the app from acquiring it and memcpy-ing over a region the server is still
 * reading (Mesa's own acquire fence is not created for this chain shape: wsi_common_x11.c:2807). */
struct mali_x11_shm_gate {
   uint32_t image_index;
   unsigned int seq;    /* xcb_get_geometry cookie sequence, 0 when the slot is free */
};

struct mali_x11_shm {
   VkSwapchainKHR swapchain;
   bool active;
   struct mali_x11_shm_gate *gates;   /* n_images + 1 entries, FIFO */
   uint32_t gate_count, gate_head, gate_tail;
   unsigned gate_hold;                /* MALI_WSI_SHM_HOLD: test hook, replies to keep outstanding */
   unsigned reused_pending;           /* diagnostics: image reused while its gate was still open (must stay 0) */
   void *conn;          /* xcb_connection_t *: opaque so this file also compiles without XCB (t_cmd) */
   uint32_t window;
   uint32_t gc;
   uint8_t depth;
   uint32_t width, height, stride, n_images;
   uint32_t seg;
   int shmid;
   void *addr;
   size_t size;
};
static struct mali_x11_shm mali_x11_shm_slots[8];

static struct mali_x11_shm *
mali_x11_shm_find(VkSwapchainKHR sc)
{
   for (unsigned i = 0; i < sizeof(mali_x11_shm_slots) / sizeof(mali_x11_shm_slots[0]); i++)
      if (mali_x11_shm_slots[i].active && mali_x11_shm_slots[i].swapchain == sc)
         return &mali_x11_shm_slots[i];
   return NULL;
}

#define MALI_SHM_FAILED ((void *)-1)

static void
mali_x11_shm_destroy(VkSwapchainKHR sc)
{
   struct mali_x11_shm *st = mali_x11_shm_find(sc);
   if (!st)
      return;
#if defined(VK_USE_PLATFORM_XCB_KHR)
   mali_x11_shm_drain(st, true);   /* discard the gate cookies: never block, never release a dying chain */
   xcb_shm_detach((xcb_connection_t *)st->conn, st->seg);
   if (st->gc)
      xcb_free_gc((xcb_connection_t *)st->conn, st->gc);   /* created per swapchain in mali_x11_shm_create */
   xcb_flush((xcb_connection_t *)st->conn);
#endif
   if (st->addr && st->addr != MALI_SHM_FAILED)
      shmdt(st->addr);   /* the segment was already unlinked at attach; this is the last detach that frees it */
   if (mali_wsi_debug())
      fprintf(stderr, "mali_vk: x11 shm: released %zu B segment (shmid %d, %u region(s) reused with completion "
                      "pending)\n", st->size, st->shmid, st->reused_pending);
   free(st->gates);
   memset(st, 0, sizeof(*st));
   st->shmid = -1;
}

#if defined(VK_USE_PLATFORM_XCB_KHR)
static void
mali_x11_shm_create(VkSwapchainKHR sc, VkIcdSurfaceBase *surface, struct wsi_swapchain *chain,
                    const VkSwapchainCreateInfoKHR *ci)
{
   if (surface->platform != VK_ICD_WSI_PLATFORM_XCB)
      return;
   VkIcdSurfaceXcb *s = (VkIcdSurfaceXcb *)surface;
   const char *e = getenv("MALI_WSI");
   if (!s->connection || (e && !strcmp(e, "xput"))) {
      fprintf(stderr, "mali_vk: x11 present: shm=0 putimage=1 (%s)\n", e ? "MALI_WSI=xput" : "no connection");
      return;
   }
   const struct xcb_query_extension_reply_t *shm = xcb_get_extension_data(s->connection, &xcb_shm_id);
   if (!shm || !shm->present) {
      fprintf(stderr, "mali_vk: x11 present: shm=0 putimage=1 (server has no MIT-SHM)\n");
      return;
   }
   struct mali_x11_shm *st = NULL;
   for (unsigned i = 0; i < sizeof(mali_x11_shm_slots) / sizeof(mali_x11_shm_slots[0]); i++)
      if (!mali_x11_shm_slots[i].active) { st = &mali_x11_shm_slots[i]; break; }
   if (!st)
      return;

   struct wsi_image *img0 = chain->get_wsi_image(chain, 0);
   const uint32_t stride = img0->row_pitches[0];
   const uint32_t w = ci->imageExtent.width, h = ci->imageExtent.height;

   xcb_get_geometry_reply_t *geom =
      xcb_get_geometry_reply(s->connection, xcb_get_geometry(s->connection, s->window), NULL);
   if (!geom) {
      fprintf(stderr, "mali_vk: x11 present: shm=0 putimage=1 (no window geometry)\n");
      return;
   }
   const uint8_t depth = geom->depth;
   free(geom);

   const size_t size = (size_t)stride * h * chain->image_count;
   int shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | 0600);
   if (shmid < 0) {
      fprintf(stderr, "mali_vk: x11 present: shm=0 putimage=1 (shmget: %s)\n", strerror(errno));
      return;
   }
   void *addr = shmat(shmid, NULL, 0);
   if (addr == MALI_SHM_FAILED) {
      fprintf(stderr, "mali_vk: x11 present: shm=0 putimage=1 (shmat: %s)\n", strerror(errno));
      shmctl(shmid, IPC_RMID, NULL);
      return;
   }
   const xcb_shm_seg_t seg = xcb_generate_id(s->connection);
   xcb_void_cookie_t ck = xcb_shm_attach_checked(s->connection, seg, shmid, 0);
   if (xcb_request_check(s->connection, ck)) {
      fprintf(stderr, "mali_vk: x11 present: shm=0 putimage=1 (xcb_shm_attach rejected)\n");
      shmdt(addr);
      shmctl(shmid, IPC_RMID, NULL);
      return;
   }
   /* Unlink the segment now, not at destroy (CR #288/#289/#307, design-shm.md B): IPC_RMID only marks it, the pages
    * live until the last detach - ours at destroy and the server's own attach until it processes our xcb_shm_detach,
    * which is queued behind every PutImage on this connection. So the kernel reclaims a segment whose client is
    * SIGKILLed or calls exit() without vkDestroySwapchainKHR (today that leaks 196 KB at 128x96 to ~6 MB at 800x600
    * until reboot), and destroy-with-a-present-in-flight can no longer pull pages from under the server. */
   if (shmctl(shmid, IPC_RMID, NULL) == 0 && mali_wsi_debug())
      fprintf(stderr, "mali_vk: x11 shm: segment unlinked at attach (shmid %d, %zu B), reclaimed at our last detach\n",
              shmid, size);
   /* The gate ring is what makes a present safe: without it the slot would present and release an image the server
    * may still be reading. An allocation failure therefore unwinds the whole SHM setup (the present then falls back
    * to wsi's own PutImage) instead of publishing an ungated slot (CR thread, k vk/src/mali_wsi.c:969). */
   struct mali_x11_shm_gate *gates = calloc((size_t)chain->image_count + 1, sizeof *gates);
   if (!gates) {
      fprintf(stderr, "mali_vk: x11 present: shm=0 putimage=1 (no memory for %u gate slots)\n",
              chain->image_count + 1);
      xcb_shm_detach(s->connection, seg);
      shmdt(addr);
      shmctl(shmid, IPC_RMID, NULL);
      xcb_flush(s->connection);
      return;
   }
   const xcb_gcontext_t gc = xcb_generate_id(s->connection);
   xcb_create_gc(s->connection, gc, s->window, XCB_GC_GRAPHICS_EXPOSURES, (uint32_t []){0});
   xcb_flush(s->connection);

   *st = (struct mali_x11_shm){.gates = gates,
                              .swapchain = sc, .active = true, .conn = s->connection, .window = s->window,
                              .gc = gc, .depth = depth, .width = w, .height = h, .stride = stride,
                              .n_images = chain->image_count, .seg = seg, .shmid = shmid, .addr = addr,
                              .size = size};
   assert(stride % 4 == 0);   /* XShmPutImage takes the segment pitch in 4-byte pixels */
   fprintf(stderr, "mali_vk: x11 present: shm=1 putimage=0 (%ux%u, %u images, %zu B segment, stride %u)\n",
           w, h, chain->image_count, size, stride);
}
#else
static void mali_x11_shm_create(VkSwapchainKHR sc, VkIcdSurfaceBase *surface, struct wsi_swapchain *chain,
                                const VkSwapchainCreateInfoKHR *ci) { (void)sc; (void)surface; (void)chain; (void)ci; }
#endif

/* The CPU is about to read the presented image: wait for the GPU's writes (DMA_BUF_IOCTL_SYNC START|READ on the
 * image's dma-buf, kbase_bo_cache in mali_backend_kbase.c) and drop any stale cache lines. Without this the copy
 * below can read memory the GPU has not written yet: the client only waits its fence after presenting, and a
 * semaphore wait at present cannot stand in for it (see driver-xcb.md: vk_sync_wait on the present semaphore
 * blocks on an object only our submit path fills, and times out). */
static void
present_image_cpu_sync(struct mali_device *dev, struct mali_image *mi, uint32_t image_index)
{
   if (!dev || !mi || !mi->mem)
      return;
   const struct mali_backend_ops *be = mali_device_pdev(dev)->backend;
   if (be->bo_host_touch)
      be->bo_host_touch(dev->kdev, &mi->mem->bo);   /* a mapped BO: the next sync must cover it */
   if (be->bo_cache)
      be->bo_cache(dev->kdev, &mi->mem->bo, 0, mi->mem->bo.size, true);
   if (mali_wsi_debug())
      fprintf(stderr, "mali_vk: wsi present: image %u BO 0x%llx synced device->CPU before the copy\n",
              image_index, (unsigned long long)mi->va);
}

/* The copy must not read the image before the submission that wrote it has retired. There is no CPU-visible
 * semaphore to wait here (binary semaphores are not observable through vk_sync_wait in this driver: see
 * LESSONS / driver-xcb.md), so wait the queue's own submit fence, bounded (1 s: a lost render must not hang the
 * present). That wait ends in kbase's device->CPU MEM_SYNC (kbase_fence_wait -> mem_sync_bos).
 * Also used for a software chain that wsi presents itself (PutImage fallback): Mesa does that wait only when the
 * *device-wide* wsi is in software mode (wsi_common.c:2889), and after CR #323 N4 the present path holds no lock and
 * no mode, so doing it here is what makes present mode-free. */
static bool
present_wait_submit_fence(struct mali_device *dev, uint32_t image_index)
{
   int ffd = mali_queue_last_fence_dup(dev);
   if (ffd < 0)
      return true;
   const struct mali_backend_ops *be = dev ? mali_device_pdev(dev)->backend : NULL;
   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);
   int wr = be && be->fence_wait ? be->fence_wait(dev->kdev, ffd, 1000) : 0;
   clock_gettime(CLOCK_MONOTONIC, &t1);
   close(ffd);
   const long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
   if (wr != 0) {
      /* The frame's submit never completed (device lost or a fault): copying its buffer would present a partial
       * frame as success. Let wsi's own path report the error. */
      fprintf(stderr, "mali_vk: wsi present: image %u submit fence wait %d, not copying\n", image_index, wr);
      return false;
   }
   if (ms >= 20)
      fprintf(stderr, "mali_vk: wsi present: image %u waited %ld ms for the submit fence\n", image_index, ms);
   return true;
}

/* Release every presented image whose GetGeometry reply has already arrived. Non-blocking by contract: present calls
 * it at the top, so a present never waits for the server (the reply is normally there long before the next frame);
 * anything still outstanding is left in the ring. `force` (destroy) discards without releasing - the chain is dying. */
static void
mali_x11_shm_drain(struct mali_x11_shm *st, bool force)
{
#if defined(VK_USE_PLATFORM_XCB_KHR)
   if (!st || !st->active || !st->gates)
      return;
   xcb_connection_t *c = (xcb_connection_t *)st->conn;
   VK_FROM_HANDLE(wsi_swapchain, chain, st->swapchain);
   const char *hold_env = getenv("MALI_WSI_SHM_HOLD");
   st->gate_hold = hold_env ? (unsigned)strtoul(hold_env, NULL, 0) : 0;
   while (st->gate_count > 0) {
      struct mali_x11_shm_gate *g = &st->gates[st->gate_head % (st->n_images + 1)];
      if (!g->seq)
         break;
      if (force) {
         xcb_discard_reply(c, g->seq);
      } else {
         /* Keep the newest `gate_hold` replies open (test hook): the ring then behaves as a server that is behind. */
         if (st->gate_count <= st->gate_hold)
            break;
         xcb_generic_error_t *err = NULL;
         void *reply = NULL;
         /* Poll only: a 0 return with no error means "not answered yet". xcb_poll_for_reply frees the reply itself;
          * we only need to know that the PutImage before it was processed. */
         if (!xcb_poll_for_reply(c, g->seq, &reply, &err) && !err)
            break;   /* not answered yet: leave it and everything after it in the ring */
         free(reply);
         free(err);
         if (chain) {
            const uint32_t idx = g->image_index;
            struct wsi_image *img = chain->get_wsi_image(chain, idx);
            if (img) {
               img->acquired = false;
               img->present_serial = ++chain->present_serial;
            }
            if (chain->release_images)
               chain->release_images(chain, 1, &idx);
         }
      }
      g->seq = 0;
      st->gate_head++;
      st->gate_count--;
   }
#else
   (void)st; (void)force;
#endif
}

/* The one place that may wait on the server, and only because the app is asking for an image it would otherwise
 * reuse while the server is reading it: poll the connection and drain answered gates until the oldest one is in (or
 * the budget is spent). Bounded per call; -1 means the connection is gone. Never called from present. */
static int
mali_x11_shm_wait_gate(struct mali_x11_shm *st, uint64_t timeout_ns)
{
#if defined(VK_USE_PLATFORM_XCB_KHR)
   if (!st || !st->active || !st->gates || !st->gate_count)
      return 0;
   xcb_connection_t *c = (xcb_connection_t *)st->conn;
   const int fd = xcb_get_file_descriptor(c);
   uint64_t budget = timeout_ns == UINT64_MAX ? 1000000000ull : timeout_ns;   /* ns */
   if (budget > 1000000000ull)
      budget = 1000000000ull;   /* one call never sleeps longer than this; the caller repeats it against its own timeout */
   struct timespec t0;
   clock_gettime(CLOCK_MONOTONIC, &t0);
   while (st->gate_count > 0) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      const uint64_t spent = (uint64_t)(now.tv_sec - t0.tv_sec) * 1000000000ull +
                             (uint64_t)(now.tv_nsec - t0.tv_nsec);
      if (spent >= budget)
         break;
      uint64_t left_ms = (budget - spent + 999999) / 1000000;   /* never more than what is left */
      if (left_ms > 20)
         left_ms = 20;
      if (xcb_connection_has_error(c))
         return -1;
      struct pollfd p = {.fd = fd, .events = POLLIN};
      const int pr = poll(&p, 1, (int)left_ms);
      if (pr < 0 && errno != EINTR)
         return -1;
      /* EINTR is not an answer and not free: the clock, not a fixed 20 ms, accounts for it (CR thread, :1103). */
      mali_x11_shm_drain(st, false);
   }
   return st->gate_count == 0 ? 1 : 0;
#else
   (void)st; (void)timeout_ns;
   return 0;
#endif
}

/* Present one image through the shared segment. Returns false when the image cannot be copied (no CPU map), in
 * which case the caller falls back to wsi's PutImage present. */
static bool
mali_x11_shm_present(struct mali_device *dev, struct wsi_swapchain *chain, uint32_t image_index, bool *gated)
{
   if (gated)
      *gated = false;
#if !defined(VK_USE_PLATFORM_XCB_KHR)
   (void)chain; (void)image_index;
   return false;   /* built without XCB: wsi_common_x11's PutImage present handles it */
#else
   struct mali_x11_shm *st = mali_x11_shm_find((VkSwapchainKHR)(uintptr_t)chain);
   if (!st || !st->active)
      return false;
   struct wsi_image *img = chain->get_wsi_image(chain, image_index);
   if (!img || !img->cpu_map)
      return false;
   const size_t off = (size_t)st->stride * st->height * image_index;
   if (off + (size_t)st->stride * st->height > st->size)
      return false;
   if (!present_wait_submit_fence(dev, image_index))
      return false;
   present_image_cpu_sync(dev, mali_image_from_handle(img->image), image_index);
   memcpy((uint8_t *)st->addr + off, img->cpu_map, (size_t)st->stride * st->height);
   /* total_width is the *segment* pitch (the server reads rows of that width); src width/height are the image. */
   const uint16_t total_w = (uint16_t)(st->stride / 4);
   xcb_shm_put_image((xcb_connection_t *)st->conn, st->window, st->gc, total_w, st->height, 0, 0, st->width, st->height,
                     0, 0, st->depth, XCB_IMAGE_FORMAT_Z_PIXMAP, 0 /* send_event */, st->seg, off);
   /* Gate only the modes that promise a non-tearing, ordered presentation. IMMEDIATE is the A/B measurement path and
    * its contract allows tearing, so it keeps the immediate release (and the zero cost). MALI_WSI_SHM_GATE=0 is the
    * escape hatch to the old behaviour. */
   const char *gate_env = getenv("MALI_WSI_SHM_GATE");
   const bool gate_off = gate_env && gate_env[0] == '0';
   if (!gate_off && st->gates && chain->present_mode != VK_PRESENT_MODE_IMMEDIATE_KHR) {
      const uint32_t cap = st->n_images + 1;
      if (st->gate_count >= cap)
         mali_x11_shm_drain(st, false);
      if (st->gate_count >= cap) {   /* still full: the oldest image is about to be reused unchecked */
         struct mali_x11_shm_gate *g = &st->gates[st->gate_head % cap];
         xcb_discard_reply((xcb_connection_t *)st->conn, g->seq);
         g->seq = 0;
         st->gate_head++;
         st->gate_count--;
         st->reused_pending++;
         if (mali_wsi_debug())
            fprintf(stderr, "mali_vk: x11 shm: region reused with completion pending (image %u) - the server is "
                            "%u frames behind\n", g->image_index, cap);
      }
      struct mali_x11_shm_gate *g = &st->gates[st->gate_tail % cap];
      g->image_index = image_index;
      /* 16-byte request, in the buffer xcb_flush writes: no extra syscall. The reply is read in a later drain. */
      g->seq = xcb_get_geometry((xcb_connection_t *)st->conn, st->window).sequence;
      st->gate_tail++;
      st->gate_count++;
      if (gated)
         *gated = true;
   }
   xcb_flush((xcb_connection_t *)st->conn);
   return true;
#endif
}

/* The wsi_device an operation of this swapchain runs on: the one the chain was created with (wsi_CreateSwapchainKHR
 * stores it in chain->wsi), never the device-wide pdev->vk.wsi_device, which an X11 surface query on another thread
 * points at the software instance for the duration of that call. Mesa's generic entrypoints read the device-wide
 * pointer without the lock (wsi_common.c:2889 waits the image fence when it sees sw), so acquire and present pass the
 * chain's instance explicitly instead (CR #323 T3). */
static const struct wsi_device *
chain_wsi_device(struct mali_physical_device *pdev, VkSwapchainKHR swapchain)
{
   VK_FROM_HANDLE(wsi_swapchain, sc, swapchain);
   return sc && sc->wsi ? sc->wsi : &pdev->wsi;
}

/* MALI_WSI_DEBUG=1: one line per acquire/present so a stall shows as an "enter" without its matching result
 * (the software present path copies the whole image and PutImage is fire-and-forget, so this is also the only way
 * to see how far a client got). Both are thin wrappers around the runtime's wsi entrypoints. */
VKAPI_ATTR VkResult VKAPI_CALL
mali_wsi_acquire_next_image2(VkDevice device, const VkAcquireNextImageInfoKHR *info, uint32_t *image_index)
{
   VK_FROM_HANDLE(mali_device, dev, device);
   struct mali_physical_device *pdev = mali_device_pdev(dev);
   return wsi_common_acquire_next_image2(chain_wsi_device(pdev, info->swapchain), device, info, image_index);
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_wsi_acquire_next_image(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore,
                            VkFence fence, uint32_t *image_index)
{
   VK_FROM_HANDLE(mali_device, dev, device);
   struct mali_physical_device *pdev = mali_device_pdev(dev);
   static uint64_t seq;
   uint64_t n = p_atomic_inc_return(&seq);
   bool acq_sw = pdev->wsi.sw;
   (void)mali_wsi_x11_chain_mode(swapchain, &acq_sw);   /* the chain's creation mode: acquire itself needs none (N4) */
   if (mali_wsi_debug())
      fprintf(stderr, "mali_vk: wsi acquire #%llu: timeout=%llu sem=%d fence=%d sw=%d\n", (unsigned long long)n,
              (unsigned long long)timeout, semaphore != VK_NULL_HANDLE, fence != VK_NULL_HANDLE, (int)acq_sw);
   /* No mode and no lock: vkAcquireNextImageKHR blocks for the whole `timeout` (UINT64_MAX for an app that waits),
    * and a mode is only consulted when images are created (wsi_common_x11.c:2654/2665) or presented (2101, 2807) -
    * never by acquire. Holding the process-wide wsi lock across it starved every other thread of the same device:
    * surface queries, swapchain creation and present on another swapchain all waited behind the blocked acquire,
    * which on a multi-threaded client (Steam's client + web helper) looks like a frozen UI (CR #323 N4). */
   struct mali_x11_shm *st = mali_x11_shm_find(swapchain);
   mali_x11_shm_drain(st, false);   /* release what the server has already read: never blocks */
   VkAcquireNextImageInfoKHR ai = {
      .sType = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR,
      .swapchain = swapchain,
      .timeout = timeout,
      .semaphore = semaphore,
      .fence = fence,
      .deviceMask = 0,
   };
   /* A pending gate means an image may be about to come free through the server's reply, and Mesa's own acquire
    * queue is only refilled by wsi's release_images - a GetGeometry reply alone does not refill it. Entering that
    * wait with every image gated would therefore block for the whole `timeout` (forever for UINT64_MAX) and never
    * reach the gate wait that frees one. So probe with timeout 0 first and wait on the gates instead (CR thread,
    * vk/src/mali_wsi.c:1233). */
   const bool gated = st && st->gate_count && timeout != 0;
   if (gated)
      ai.timeout = 0;
   VkResult r = mali_wsi_acquire_next_image2(device, &ai, image_index);
   if (gated && (r == VK_NOT_READY || r == VK_TIMEOUT)) {
      struct timespec t0;
      clock_gettime(CLOCK_MONOTONIC, &t0);
      for (;;) {
         uint64_t remain = UINT64_MAX;
         if (timeout != UINT64_MAX) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            const uint64_t spent = (uint64_t)(now.tv_sec - t0.tv_sec) * 1000000000ull +
                                   (uint64_t)(now.tv_nsec - t0.tv_nsec);
            if (spent >= timeout) {
               r = VK_TIMEOUT;
               break;
            }
            remain = timeout - spent;
         }
         /* No image is free because every one of them is still being read by the server: that is the only wait in
          * this design, and it is bounded by what is left of the caller's own timeout (design-shm.md A). */
         const int wr = mali_x11_shm_wait_gate(st, remain);
         if (wr < 0)
            return VK_ERROR_SURFACE_LOST_KHR;
         ai.timeout = 0;
         r = mali_wsi_acquire_next_image2(device, &ai, image_index);
         if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)
            break;
         if (!(r == VK_NOT_READY || r == VK_TIMEOUT))
            break;
         if (st->gate_count == 0)
            break;   /* nothing left to wait for: hand Mesa's own answer back */
      }
   }
   if (mali_wsi_debug())
      fprintf(stderr, "mali_vk: wsi acquire #%llu -> %s image=%u\n", (unsigned long long)n,
              vk_Result_to_str(r), r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR ? *image_index : 0);
   return r;
}

/* The swapchain image of a VkPresentInfoKHR entry as our object (NULL for a swapchain we do not know, or an
 * image wsi did not create through us). Shared by the dump and the debug line so both name the same BO. */
static struct mali_image *
present_image(struct wsi_swapchain *chain, uint32_t image_index, struct wsi_image **wsi_out)
{
   struct wsi_image *wi = chain ? chain->get_wsi_image(chain, image_index) : NULL;
   if (wsi_out)
      *wsi_out = wi;
   return wi && wi->image != VK_NULL_HANDLE ? mali_image_from_handle(wi->image) : NULL;
}

/* ---- MALI_WSI_DUMP / MALI_WSI_DEBUG: what the client presented, for every swapchain kind -------------------
 *
 * The dump used to live inside the X11 MIT-SHM present shortcut, which is exactly the path a phone does not take:
 * the product path hands a Wayland dma-buf to the gamescope WSI layer and never runs that code, so black frames
 * could only be looked at by disabling the layer and forcing software present - a different path under test. Every
 * kind of swapchain (X11 shm, X11 DRI3, Wayland dma-buf) gets its images through vkCreateImage on a BO of ours, so
 * vkQueuePresentKHR can read the frame back before wsi_common is handed it. That is where mali_wsi_present_trace()
 * runs.
 *
 * Nothing here costs anything unless the environment is set: one cached getenv for the prefix, one for the debug
 * cadence.
 */
#define MALI_WSI_INFO_SLOTS 32

/* Which swapchain a presented frame belongs to: struct wsi_swapchain carries neither the surface nor the requested
 * format back to us, so the creation site records them and the present/dump lines name the process and the surface. */
struct mali_wsi_chain_info {
   bool active;
   VkSwapchainKHR swapchain;
   int platform;
   uint64_t surface;            /* xcb/xlib window id, or the wl_surface pointer */
   uint32_t width, height;
   uint32_t images, min_images;
   VkFormat format;
   VkColorSpaceKHR colorspace;
   VkImageUsageFlags usage;
   VkPresentModeKHR mode;
   int slot;
   bool sw;
   uint64_t presents;           /* frames presented through this chain (atomic) */
};

static struct mali_wsi_chain_info mali_wsi_infos[MALI_WSI_INFO_SLOTS];
static pthread_mutex_t mali_wsi_infos_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *
mali_wsi_comm(void)
{
   static char comm[32];
   static bool once;
   if (!once) {
      char buf[64] = {0};
      FILE *f = fopen("/proc/self/comm", "r");
      if (f) {
         if (!fgets(buf, sizeof buf, f))
            buf[0] = 0;
         fclose(f);
      }
      buf[strcspn(buf, "\r\n")] = 0;
      for (char *p = buf; *p; p++)
         if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-'))
            *p = '_';   /* a comm with a '/' or a space must not break the file name */
      snprintf(comm, sizeof comm, "%s", buf[0] ? buf : "unknown");
      once = true;
   }
   return comm;
}

static uint64_t
mali_wsi_surface_of(VkIcdSurfaceBase *surface)
{
   if (!surface)
      return 0;
   switch (surface->platform) {
#if defined(VK_USE_PLATFORM_XCB_KHR)
   case VK_ICD_WSI_PLATFORM_XCB:     return (uint64_t)((VkIcdSurfaceXcb *)surface)->window;
#endif
#if defined(VK_USE_PLATFORM_XLIB_KHR)
   case VK_ICD_WSI_PLATFORM_XLIB:    return (uint64_t)((VkIcdSurfaceXlib *)surface)->window;
#endif
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
   case VK_ICD_WSI_PLATFORM_WAYLAND: return (uint64_t)(uintptr_t)((VkIcdSurfaceWayland *)surface)->surface;
#endif
   default:                          return 0;
   }
}

static struct mali_wsi_chain_info *
mali_wsi_info_find(VkSwapchainKHR sc)
{
   if (!sc)
      return NULL;
   /* Read-only: an entry is added and removed around the chain's own lifetime, and a present of that chain cannot
    * race its destroy (Vulkan's external synchronisation of vkDestroySwapchainKHR). */
   for (unsigned i = 0; i < MALI_WSI_INFO_SLOTS; i++)
      if (mali_wsi_infos[i].active && mali_wsi_infos[i].swapchain == sc)
         return &mali_wsi_infos[i];
   return NULL;
}

static void
mali_wsi_info_add(VkSwapchainKHR sc, VkIcdSurfaceBase *surface, const VkSwapchainCreateInfoKHR *ci,
                  struct wsi_swapchain *chain, bool sw)
{
   pthread_mutex_lock(&mali_wsi_infos_lock);
   struct mali_wsi_chain_info *e = NULL;
   for (unsigned i = 0; i < MALI_WSI_INFO_SLOTS; i++)
      if (!mali_wsi_infos[i].active) { e = &mali_wsi_infos[i]; break; }
   if (e) {
      memset(e, 0, sizeof *e);
      e->active = true;
      e->swapchain = sc;
      e->platform = surface ? surface->platform : -1;
      e->surface = mali_wsi_surface_of(surface);
      e->width = ci->imageExtent.width;
      e->height = ci->imageExtent.height;
      e->min_images = ci->minImageCount;
      e->images = chain ? chain->image_count : ci->minImageCount;
      e->format = ci->imageFormat;
      e->colorspace = ci->imageColorSpace;
      e->usage = ci->imageUsage;
      e->mode = ci->presentMode;
      e->sw = sw;
      e->slot = (int)(e - mali_wsi_infos);
   }
   pthread_mutex_unlock(&mali_wsi_infos_lock);
}

static void
mali_wsi_info_remove(VkSwapchainKHR sc)
{
   pthread_mutex_lock(&mali_wsi_infos_lock);
   struct mali_wsi_chain_info *e = mali_wsi_info_find(sc);
   if (e) {
      memset(e, 0, sizeof *e);   /* clears `active` and frees the slot */
      e->swapchain = VK_NULL_HANDLE;
   }
   pthread_mutex_unlock(&mali_wsi_infos_lock);
}

/* MALI_WSI_DUMP=<prefix>, cached: the present path must not call getenv per frame when the dump is off. */
static const char *
mali_wsi_dump_prefix(void)
{
   static int on = -1;
   static const char *prefix;
   if (on < 0) {
      prefix = getenv("MALI_WSI_DUMP");
      on = prefix && prefix[0] ? 1 : 0;
   }
   return on ? prefix : NULL;
}

static unsigned
mali_wsi_env_every(const char *name, unsigned dflt)
{
   const char *e = getenv(name);
   if (!e || !e[0])
      return dflt;
   const unsigned long v = strtoul(e, NULL, 10);
   return v ? (unsigned)v : 1;
}

/* The CPU decode of one texel, as 0..255 rgb. false = no decode for this format (no picture is written, only the
 * .txt line: a PPM built from the raw bytes of a 10:10:10:2 or 16-bit image would be a wrong picture, and a wrong
 * picture is worse than none). */
enum mali_wsi_px { MALI_WSI_PX_NONE, MALI_WSI_PX_RGBA8, MALI_WSI_PX_PACK32, MALI_WSI_PX_565 };

static enum mali_wsi_px
mali_wsi_px_kind(VkFormat f, bool *bgr)
{
   *bgr = false;
   switch (f) {
   case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
      *bgr = true; return MALI_WSI_PX_RGBA8;
   case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
      return MALI_WSI_PX_RGBA8;
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      *bgr = true; return MALI_WSI_PX_PACK32;
   case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
      return MALI_WSI_PX_PACK32;
   case VK_FORMAT_B5G6R5_UNORM_PACK16: case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
      *bgr = true; return MALI_WSI_PX_565;
   case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
      return MALI_WSI_PX_565;
   default:
      return MALI_WSI_PX_NONE;
   }
}

static void
mali_wsi_px_rgb(enum mali_wsi_px kind, bool bgr, const uint8_t *p, uint8_t out[3])
{
   unsigned r, g, b;
   switch (kind) {
   case MALI_WSI_PX_RGBA8:
      r = p[bgr ? 2 : 0]; g = p[1]; b = p[bgr ? 0 : 2];
      break;
   case MALI_WSI_PX_PACK32: {
      const uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
      const unsigned c0 = v & 0x3ffu, c1 = (v >> 10) & 0x3ffu, c2 = (v >> 20) & 0x3ffu;
      r = (bgr ? c2 : c0) * 255u / 1023u; g = c1 * 255u / 1023u; b = (bgr ? c0 : c2) * 255u / 1023u;
      break;
   }
   default: {
      const uint16_t v = (uint16_t)((uint32_t)p[0] | (uint32_t)p[1] << 8);
      const unsigned c0 = (v >> 11) & 0x1fu, c1 = (v >> 5) & 0x3fu, c2 = v & 0x1fu;
      r = (bgr ? c2 : c0) * 255u / 31u; g = c1 * 255u / 63u; b = (bgr ? c0 : c2) * 255u / 31u;
      break;
   }
   }
   out[0] = (uint8_t)r; out[1] = (uint8_t)g; out[2] = (uint8_t)b;
}

/* MALI_WSI_DUMP=<prefix>: every MALI_WSI_DUMP_EVERY-th presented frame (default 30) of EVERY swapchain kind, as
 *   <prefix>-<comm>-<pid>-sc<N>-<frame>.ppm   (P6, the presented pixels)
 * plus one line in <prefix>.txt: comm, pid, platform, surface, WxH, VkFormat number and name, layout, mean, file.
 * The pid and comm in the name are what stop Steam's UI and Megabonk (or two runs of one game) from overwriting each
 * other: the old <prefix>-NNNNN.ppm was a per-process counter with nothing identifying the process.
 * Cost: one CPU pass over the presented BO plus a cache invalidate every N frames, on the presenting thread. */
static void
mali_wsi_dump_present(struct mali_device *dev, struct wsi_swapchain *chain, uint32_t image_index,
                      struct mali_wsi_chain_info *info, uint64_t frame)
{
   const char *prefix = mali_wsi_dump_prefix();
   if (!prefix)
      return;
   if (frame % mali_wsi_env_every("MALI_WSI_DUMP_EVERY", 30))
      return;

   struct wsi_image *wi = NULL;
   struct mali_image *mi = present_image(chain, image_index, &wi);
   char why[128] = "";
   const char *layout = "unknown";
   const struct mali_plane_layout *pl = NULL;
   enum mali_wsi_px px = MALI_WSI_PX_NONE;
   bool bgr = false;
   unsigned w = 0, h = 0, stride = 0, bpp = 0;
   const uint8_t *base = NULL;

   if (!mi) {
      snprintf(why, sizeof why, "the swapchain image is not one of our VkImages (wsi did not create it through us)");
   } else if (!mi->mem || !mi->mem->bo.cpu) {
      snprintf(why, sizeof why, "the image's BO has no CPU mapping (unbound, or memory we did not allocate)");
   } else {
      pl = mali_image_plane_for_aspect(&mi->lay, VK_IMAGE_ASPECT_COLOR_BIT);
      w = mi->lay.d.extent.width;
      h = mi->lay.d.extent.height;
      stride = pl ? pl->slice[0].row_stride : 0;
      bpp = pl && pl->bs ? pl->bs : 4;
      layout = mi->lay.afbc ? "afbc" : mi->lay.d.tiled ? "tiled" : "linear";
      if (!pl || !stride || !w || !h)
         snprintf(why, sizeof why, "no colour plane layout");
      else if (mi->lay.afbc)
         snprintf(why, sizeof why, "AFBC-compressed layout: the header blocks are not a pixel grid (a GPU copy into a "
                                   "linear image is needed)");
      else if (mi->lay.d.tiled)
         snprintf(why, sizeof why, "tiled (non-linear) layout: rows are swizzled (a GPU copy into a linear image is needed)");
      else if (pl->bw > 1 || pl->bh > 1)
         snprintf(why, sizeof why, "block-compressed format: no CPU decode");
      else {
         px = mali_wsi_px_kind(mi->vk.format, &bgr);
         if (px == MALI_WSI_PX_NONE)
            snprintf(why, sizeof why, "no CPU decode for this VkFormat (a GPU copy or a format-table entry is needed)");
      }
      if (!why[0]) {
         const uint64_t off = mi->mem_off + pl->offset;
         base = (const uint8_t *)mi->mem->bo.cpu + off;
         if (off + (uint64_t)stride * h > mi->mem->bo.size)
            snprintf(why, sizeof why, "the plane (%ux%u, stride %u) runs past the BO (%llu B)", w, h, stride,
                     (unsigned long long)mi->mem->bo.size);
      }
   }

   char path[1200];
   uint8_t *row = NULL;
   double sum[3] = {0, 0, 0};
   const char *file = "none";
   if (!why[0]) {
      /* The GPU wrote this BO: invalidate the CPU's view before reading it, exactly as the shm copy does. */
      const struct mali_backend_ops *be = dev ? mali_device_pdev(dev)->backend : NULL;
      if (be && be->bo_host_touch)
         be->bo_host_touch(dev->kdev, &mi->mem->bo);
      if (be && be->bo_cache)
         be->bo_cache(dev->kdev, &mi->mem->bo, 0, mi->mem->bo.size, true);

      snprintf(path, sizeof path, "%s-%s-%d-sc%d-%llu.ppm", prefix, mali_wsi_comm(), (int)getpid(),
               info ? info->slot : 0, (unsigned long long)frame);
      row = malloc((size_t)w * 3);
      FILE *f = row ? fopen(path, "wb") : NULL;
      if (!f) {
         snprintf(why, sizeof why, "cannot write %s: %s", path, strerror(errno));
      } else {
         fprintf(f, "P6\n%u %u\n255\n", w, h);
         for (unsigned y = 0; y < h; y++) {
            const uint8_t *src = base + (size_t)y * stride;
            for (unsigned x = 0; x < w; x++) {
               uint8_t rgb[3];
               mali_wsi_px_rgb(px, bgr, src + (size_t)x * bpp, rgb);
               row[x * 3 + 0] = rgb[0]; row[x * 3 + 1] = rgb[1]; row[x * 3 + 2] = rgb[2];
               sum[0] += rgb[0]; sum[1] += rgb[1]; sum[2] += rgb[2];
            }
            fwrite(row, 1, (size_t)w * 3, f);
         }
         fclose(f);
         file = path;
      }
   }
   free(row);

   snprintf(path, sizeof path, "%s.txt", prefix);
   FILE *t = fopen(path, "a");
   if (t) {
      fprintf(t, "dump frame=%llu sc=%d comm=%s pid=%d platform=%s surface=0x%llx %ux%u format=%d(%s) "
                 "layout=%s samples=%u images=%u usage=0x%x",
              (unsigned long long)frame, info ? info->slot : 0, mali_wsi_comm(), (int)getpid(),
              info ? mali_wsi_platform_name(info->platform) : "unknown", (unsigned long long)(info ? info->surface : 0),
              w, h, (int)(mi ? mi->vk.format : VK_FORMAT_UNDEFINED), mi ? vk_Format_to_str(mi->vk.format) : "no image",
              layout, mi ? mi->lay.d.samples : 0u, info ? info->images : 0u, info ? (unsigned)info->usage : 0u);
      if (file[0] != 'n' || strcmp(file, "none"))
         fprintf(t, " mean=%.1f,%.1f,%.1f file=%s\n", sum[0] / ((double)w * h), sum[1] / ((double)w * h),
                 sum[2] / ((double)w * h), file);
      else
         fprintf(t, " file=none reason=\"%s\"\n", why);
      fclose(t);
   }
   if (!strcmp(file, "none"))
      fprintf(stderr, "mali_vk: wsi dump #%llu: sc%d image %u %ux%u %s (%s): no picture, %s\n",
              (unsigned long long)frame, info ? info->slot : 0, image_index, w, h,
              mi ? vk_Format_to_str(mi->vk.format) : "no image", layout, why);
   else
      fprintf(stderr, "mali_vk: wsi dump #%llu: sc%d image %u %ux%u %s %s -> %s\n", (unsigned long long)frame,
              info ? info->slot : 0, image_index, w, h, mi ? vk_Format_to_str(mi->vk.format) : "no image", layout, file);
}

/* One present of one swapchain: the dump, and the per-chain summary line every N frames (never per frame). The frame
 * number is the chain's own present counter, so "frame" in a dump name means "the Nth frame this swapchain put". */
static void
mali_wsi_present_trace(struct mali_device *dev, VkSwapchainKHR handle, struct wsi_swapchain *chain, uint32_t image_index)
{
   struct mali_wsi_chain_info *info = mali_wsi_info_find(handle);
   if (!info)
      return;
   struct mali_image *mi = present_image(chain, image_index, NULL);
   const uint64_t frame = __atomic_add_fetch(&info->presents, 1, __ATOMIC_RELAXED);
   if (mali_wsi_dump_prefix())
      mali_wsi_dump_present(dev, chain, image_index, info, frame);
   if (mali_wsi_debug()) {
      static int every = -1;
      if (every < 0)
         every = (int)mali_wsi_env_every("MALI_WSI_DEBUG_EVERY", 60);
      if (every && !(frame % (uint64_t)every))
         fprintf(stderr, "mali_vk: wsi present: sc%d %s comm=%s pid=%d surface=0x%llx %ux%u %s images=%u usage=0x%x "
                         "mode=%u sw=%d frames=%llu image=%u\n",
                 info->slot, mali_wsi_platform_name(info->platform), mali_wsi_comm(), (int)getpid(),
                 (unsigned long long)info->surface, info->width, info->height,
                 mi ? vk_Format_to_str(mi->vk.format) : "no image", info->images, (unsigned)info->usage,
                 (unsigned)info->mode, (int)info->sw, (unsigned long long)frame, image_index);
   }
}

/* MALI_WSI_DEBUG=1: the image, BO and present path of the frame being put (shm = our segment, dma-buf = wsi). */
static void
mali_wsi_debug_present(struct mali_device *dev, struct wsi_swapchain *chain, uint32_t image_index, const char *path,
                       VkResult res)
{
   if (!mali_wsi_debug())
      return;
   struct wsi_image *wi = NULL;
   struct mali_image *mi = present_image(chain, image_index, &wi);
   fprintf(stderr, "mali_vk: wsi present image %u path %s result %s: %s %ux%u va=0x%llx stride %u\n",
           image_index, path ? path : "?", vk_Result_to_str(res),
           mi ? vk_Format_to_str(mi->vk.format) : "no image", mi ? mi->lay.d.extent.width : 0,
           mi ? mi->lay.d.extent.height : 0, mi ? (unsigned long long)mi->va : 0ull,
           mi && mi->lay.plane[0].slice[0].row_stride ? mi->lay.plane[0].slice[0].row_stride : 0u);
   (void)dev; (void)wi;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_wsi_queue_present(VkQueue queue, const VkPresentInfoKHR *info)
{
   static uint64_t seq;
   uint64_t n = p_atomic_inc_return(&seq);
   VK_FROM_HANDLE(vk_queue, vq, queue);
   struct mali_device *dev = container_of(vq->base.device, struct mali_device, vk);
   if (mali_wsi_debug()) {
      const struct mali_physical_device *qpdev = mali_device_pdev(dev);
      fprintf(stderr, "mali_vk: wsi present #%llu: swapchains=%u wait_sems=%u dma_buf_sync=%d sw=%d\n",
              (unsigned long long)n, info->swapchainCount, info->waitSemaphoreCount,
              __atomic_load_n(&qpdev->wsi.cached_sync_file_import_export_result, __ATOMIC_RELAXED) == UINT32_MAX,
              (int)qpdev->wsi.sw);
   }
   /* MALI_WSI_DUMP and the per-chain MALI_WSI_DEBUG summary, for every kind of swapchain (Wayland dma-buf and X11
    * DRI3 included): after the present semaphores are waited on, before wsi_common is handed the frame (#329). */
   for (uint32_t i = 0; i < info->swapchainCount; i++) {
      VK_FROM_HANDLE(wsi_swapchain, sc, info->pSwapchains[i]);
      if (sc)
         mali_wsi_present_trace(dev, info->pSwapchains[i], sc, info->pImageIndices[i]);
   }
   /* Release the images of the previous presents whose GetGeometry reply has arrived: never blocks (design-shm.md A). */
   for (uint32_t i = 0; i < info->swapchainCount; i++)
      mali_x11_shm_drain(mali_x11_shm_find(info->pSwapchains[i]), false);
   /* Software present through our own shared segment: memcpy + XShmPutImage, no image bytes on the socket. */
   {
      bool all_shm = info->swapchainCount > 0;
      for (uint32_t i = 0; i < info->swapchainCount; i++)
         if (!mali_x11_shm_find(info->pSwapchains[i])) {
            all_shm = false;      /* not all of them are in our segment: let wsi present normally */
            break;
         }
      /* The shortcut copies the image itself and returns before wsi's present, so the app's present semaphores
       * would never be waited on: do it here, with the same CPU-wait semantics the queue submit uses
       * (mali_queue.c:618), and only for the shortcut (the dma-buf path hands the semaphores to wsi). */
      if (all_shm && info->waitSemaphoreCount) {
         VK_FROM_HANDLE(vk_queue, q, queue);
         struct mali_device *qdev = container_of(q->base.device, struct mali_device, vk);
         const VkTimelineSemaphoreSubmitInfo *tl =
            vk_find_struct_const(info->pNext, TIMELINE_SEMAPHORE_SUBMIT_INFO);
         for (uint32_t i = 0; i < info->waitSemaphoreCount; i++) {
            VK_FROM_HANDLE(vk_semaphore, sem, info->pWaitSemaphores[i]);
            if (!sem)
               continue;
            const uint64_t val = (tl && tl->pWaitSemaphoreValues) ? tl->pWaitSemaphoreValues[i] : 0;
            VkResult wr = vk_sync_wait(&qdev->vk, vk_semaphore_get_active_sync(sem), val,
                                       VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
            if (wr != VK_SUCCESS)
               return wr;
            /* wsi_QueuePresentKHR would have consumed a binary semaphore; this shortcut returns before it, so reset
             * it here or the next submit that waits on it would see a still-signalled semaphore. */
            if (sem->type == VK_SEMAPHORE_TYPE_BINARY)
               vk_semaphore_reset_temporary(&qdev->vk, sem);
         }
      }
      for (uint32_t i = 0; all_shm && i < info->swapchainCount; i++) {
         VK_FROM_HANDLE(wsi_swapchain, sc, info->pSwapchains[i]);
         const uint32_t idx = info->pImageIndices[i];
         bool gated = false;
         const bool shm = sc && mali_x11_shm_find(info->pSwapchains[i]) &&
                          mali_x11_shm_present(dev, sc, idx, &gated);
         if (mali_wsi_debug() && sc)
            mali_wsi_debug_present(dev, sc, idx, shm ? (gated ? "shm+gated" : "shm") : "dma-buf", VK_SUCCESS);
         if (!shm) {
            all_shm = false;
            break;
         }
         if (gated)
            continue;   /* released by mali_x11_shm_drain() once the server has read the region (design-shm.md A) */
         struct wsi_image *img = sc->get_wsi_image(sc, idx);
         img->acquired = false;
         img->present_serial = ++sc->present_serial;
         if (sc->release_images)
            sc->release_images(sc, 1, &idx);
      }
      if (all_shm) {
         if (info->pResults)
            for (uint32_t i = 0; i < info->swapchainCount; i++)
               info->pResults[i] = VK_SUCCESS;   /* the caller asked for per-swapchain results */
         /* VK_EXT_swapchain_maintenance1 present fences: wsi_common signals them at the end of the per-image GPU
          * work (wsi_common.c:2679), which this shortcut never reaches - the frame is copied and the XShmPutImage
          * is queued here, so signal them here too (#325). Without it a client that waits a present fence (the
          * pattern t_wsi_maint.c:453 checks) times out on the X11 shm path. Signalled exactly once: this is the only
          * place in the driver that touches SWAPCHAIN_PRESENT_FENCE_INFO_EXT. */
         const VkSwapchainPresentFenceInfoEXT *pf =
            vk_find_struct_const(info->pNext, SWAPCHAIN_PRESENT_FENCE_INFO_EXT);
         if (pf && pf->pFences) {
            for (uint32_t i = 0; i < info->swapchainCount && i < pf->swapchainCount; i++) {
               if (pf->pFences[i] == VK_NULL_HANDLE)
                  continue;
               VK_FROM_HANDLE(vk_fence, f, pf->pFences[i]);
               if (f) {
                  VkResult sr = vk_sync_signal(&dev->vk, vk_fence_get_active_sync(f), 0);
                  (void)sr;   /* a lost device is reported by the next wait, not by present */
               }
            }
         }
         if (mali_wsi_debug()) {
            static int shm_logged;
            if (!shm_logged) {
               shm_logged = 1;
               fprintf(stderr, "mali_vk: wsi present: shared-memory segment (no image bytes on the socket)\n");
            }
         }
         return VK_SUCCESS;
      }
   }
   if (mali_wsi_debug()) {
      for (uint32_t i = 0; i < info->swapchainCount; i++) {
         VK_FROM_HANDLE(wsi_swapchain, sc, info->pSwapchains[i]);
         if (sc && sc->present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            static int imm_logged;
            if (!imm_logged) {
               imm_logged = 1;
               fprintf(stderr, "mali_vk: wsi present: IMMEDIATE - put and return, no wait on the previous frame "
                       "(drop-oldest if the X server is still busy with it)\n");
            }
         }
      }
   }
   /* No mode and no lock: the chain carries the wsi_device it was created with, so wsi_common_x11's present branch
    * (chain->base.wsi->sw, :2101) is right on the app's thread and on the async present queue manager that reads it
    * later (:2235 -> :2101) - the decision is immutable per chain (CR #323 T7). The one device-wide read left in the
    * generic path (wsi_common.c:2889 waits the image fence when wsi->sw) is covered by waiting it here for a
    * software chain, so presenting never takes the wsi lock (CR #323 N4). */
   for (uint32_t i = 0; i < info->swapchainCount; i++) {
      bool sc_sw = false;
      if (!mali_wsi_x11_chain_mode(info->pSwapchains[i], &sc_sw) || !sc_sw)
         continue;
      VK_FROM_HANDLE(wsi_swapchain, sc, info->pSwapchains[i]);
      struct mali_image *mi = sc ? present_image(sc, info->pImageIndices[i], NULL) : NULL;
      if (!mi)
         continue;   /* not an image of ours: wsi's own present is the right path */
      if (!present_wait_submit_fence(dev, info->pImageIndices[i])) {
         /* The frame's submit never completed (device lost or a fault): the CPU copy or PutImage that follows would
          * hand the compositor a partial frame and report success. Do not delegate this present either - wsi's
          * generic path skips the software fence wait for this chain (CR #323 T4). */
         if (info->pResults)
            for (uint32_t j = 0; j < info->swapchainCount; j++)
               info->pResults[j] = VK_ERROR_DEVICE_LOST;
         if (mali_wsi_debug())
            fprintf(stderr, "mali_vk: wsi present #%llu: image %u submit fence wait failed -> VK_ERROR_DEVICE_LOST\n",
                    (unsigned long long)n, info->pImageIndices[i]);
         return VK_ERROR_DEVICE_LOST;
      }
      present_image_cpu_sync(dev, mi, info->pImageIndices[i]);
   }
   /* The instance a present runs on comes from the swapchains it presents, not from the device-wide pointer: another
    * thread's X11 surface query points that at the software instance for one call, and the generic entrypoint would
    * then read sw=true for a wayland dma-buf chain and wait an image fence only the software path creates
    * (wsi_common.c:2889; CR #323 T3). Mixed modes in one present fall back to the device's own instance. */
   struct mali_physical_device *pdev = mali_device_pdev(dev);
   const struct wsi_device *wsi_sel = &pdev->wsi;
   for (uint32_t i = 0; i < info->swapchainCount; i++) {
      const struct wsi_device *one = chain_wsi_device(pdev, info->pSwapchains[i]);
      if (i == 0)
         wsi_sel = one;
      else if (one != wsi_sel) {
         wsi_sel = &pdev->wsi;
         break;
      }
   }
   VkResult r = wsi_common_queue_present(wsi_sel, vq, info);
   /* wsi_common_wayland only pumps its per-swapchain present-id queue inside dispatch_present_id_queue(), i.e. at
    * the next present or inside vkWaitForPresentKHR. An app that presents its last frame and destroys the
    * swapchain never gets that pump, so the frame callback/presentation feedback of that commit is still
    * outstanding and wsi_wl_swapchain_chain_free() asserts (!chain->present_ids.dispatch_in_progress)
    * (wsi_common_wayland.c:3782, and the same in the retire path at :2779). Drain it here, bounded: a slow
    * compositor costs at most the cap and never blocks the frame. Only when the swapchain asked for present-id
    * waiting: without that, wsi does not queue an id at all and there is nothing to drain. */
   const VkPresentIdKHR *ids = vk_find_struct_const(info->pNext, PRESENT_ID_KHR);
   if (r == VK_SUCCESS && ids) {
      for (uint32_t i = 0; i < info->swapchainCount && i < ids->swapchainCount; i++) {
         VK_FROM_HANDLE(wsi_swapchain, chain, info->pSwapchains[i]);
         if (chain && chain->present_wait_enabled && ids->pPresentIds && ids->pPresentIds[i])
            mali_wsi_present_id_set(info->pSwapchains[i], ids->pPresentIds[i]);
      }
   }
   /* SUBOPTIMAL/OUT_OF_DATE are normal swapchain states, not errors: one line per frame would be noise. */
   bool quiet = r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR;
   if (mali_wsi_debug() || !quiet)
      fprintf(stderr, "mali_vk: wsi present #%llu -> %s (%d)\n", (unsigned long long)n, vk_Result_to_str(r), (int)r);
   return r;
}

/* The last VkPresentIdKHR seen per swapchain. Present records it and returns - waiting for the compositor's
 * presentation feedback here (the 20 ms vkWaitForPresentKHR cap this used to call on every successful present) cost
 * up to 20 ms per frame whenever the compositor was late, i.e. a 50 fps ceiling enforced by our own present path on
 * a client that passes present ids (Steam/gamescope do). The only place that feedback has to be pumped is
 * vkDestroySwapchainKHR: wsi_common_wayland asserts (!chain->present_ids.dispatch_in_progress) while freeing
 * (wsi_common_wayland.c:3782, :2779) and the queue is otherwise pumped by the next present. */
struct mali_wsi_present_id {
   VkSwapchainKHR sc;
   uint64_t last;
};
static struct mali_wsi_present_id mali_wsi_present_ids[16];
/* Its own lock: the table is written by the app's present thread and read by destroy. */
static pthread_mutex_t mali_wsi_present_id_lock = PTHREAD_MUTEX_INITIALIZER;

static void
mali_wsi_present_id_set(VkSwapchainKHR sc, uint64_t id)
{
   pthread_mutex_lock(&mali_wsi_present_id_lock);
   unsigned n = sizeof(mali_wsi_present_ids) / sizeof(mali_wsi_present_ids[0]);
   unsigned free_slot = n;
   for (unsigned i = 0; i < n; i++) {
      if (mali_wsi_present_ids[i].sc == sc) { mali_wsi_present_ids[i].last = id; pthread_mutex_unlock(&mali_wsi_present_id_lock); return; }
      if (!mali_wsi_present_ids[i].sc && free_slot == n) free_slot = i;
   }
   if (free_slot < n)
      mali_wsi_present_ids[free_slot] = (struct mali_wsi_present_id){.sc = sc, .last = id};
   pthread_mutex_unlock(&mali_wsi_present_id_lock);
}

static uint64_t
mali_wsi_present_id_take(VkSwapchainKHR sc)
{
   uint64_t id = 0;
   pthread_mutex_lock(&mali_wsi_present_id_lock);
   for (unsigned i = 0; i < sizeof(mali_wsi_present_ids) / sizeof(mali_wsi_present_ids[0]); i++)
      if (mali_wsi_present_ids[i].sc == sc) {
         id = mali_wsi_present_ids[i].last;
         mali_wsi_present_ids[i] = (struct mali_wsi_present_id){0};
         break;
      }
   pthread_mutex_unlock(&mali_wsi_present_id_lock);
   return id;
}

/* Device entrypoint (installed in mali_device.c) in front of wsi_CreateSwapchainKHR: it only observes and, for an
 * X11 surface on a server without DRI3, flips the device to the software path before Mesa allocates images. The
 * real wsi_common_x11 interface (and the struct wsi_x11 it is embedded in) is left exactly as Mesa set it up:
 * wsi_x11_get_connection() casts wsi_device->wsi[XCB] back to struct wsi_x11, so it must not be replaced. */
VKAPI_ATTR VkResult VKAPI_CALL
mali_wsi_create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *ci,
                          const VkAllocationCallbacks *alloc, VkSwapchainKHR *out)
{
   VK_FROM_HANDLE(mali_device, dev, device);
   struct mali_physical_device *pdev = mali_device_pdev(dev);
   struct wsi_device *wsi_dev = &pdev->wsi;
   VkIcdSurfaceBase *surface = (VkIcdSurfaceBase *)(uintptr_t)ci->surface;
   struct mali_wsi_x11_probe probe;

   mali_wsi_x11_probe(surface, &probe);

   /* X11 surfaces on a server without DRI3+Present take the software (PutImage) path. The decision is made by
    * mali_wsi_x11_maybe_sw(), which the surface queries already took for a zink client (it asks surface support
    * first and would never get here), and is repeated here for a client that went straight to vkCreateSwapchainKHR;
    * the same function, so the two can never disagree. MALI_WSI=dmabuf opts back into dma-buf, where a broken import
    * fails loudly instead of silently dropping presents (the hang in wsi_common_x11.c:2202). */
   const bool sw_now = mali_wsi_x11_maybe_sw(pdev, surface, &probe);

   /* The chain is created on the instance its surface's path needs, and wsi_CreateSwapchainKHR stores that pointer in
    * chain->base.wsi: from here on the chain's mode is immutable, for the async present queue manager
    * (wsi_common_x11.c:2235 -> :2101) and for destroy (:2807) alike (CR #323 T7). */
   struct mali_wsi_mode saved_sw = mali_wsi_sw_enter(pdev, surface, sw_now);   /* reassigned by #328's retry */

   if (mali_wsi_debug() && surface->platform != VK_ICD_WSI_PLATFORM_WAYLAND)
      fprintf(stderr, "mali_vk: wsi hooks: GetSemaphoreFdKHR=%s GetMemoryFdKHR=%s cached_sync_file=%u (dma_buf_sync=%d)\n",
              wsi_dev->GetSemaphoreFdKHR ? "set" : "NULL", wsi_dev->GetMemoryFdKHR ? "set" : "NULL",
              __atomic_load_n(&pdev->vk.wsi_device->cached_sync_file_import_export_result, __ATOMIC_RELAXED),
              __atomic_load_n(&pdev->vk.wsi_device->cached_sync_file_import_export_result, __ATOMIC_RELAXED) == UINT32_MAX);
   if (mali_wsi_debug() && surface->platform != VK_ICD_WSI_PLATFORM_WAYLAND)
      fprintf(stderr, "mali_vk: x11 present path: shm=%d dri3=%d%s\n",
              (int)(probe.mit_shm && pdev->vk.wsi_device->has_import_memory_host), (int)probe.dri3,
              pdev->vk.wsi_device->sw ? " (software)" : " (dma-buf)");

   VkResult r = wsi_CreateSwapchainKHR(device, ci, alloc, out);
   const bool x11_surface = surface->platform == VK_ICD_WSI_PLATFORM_XCB ||
                            surface->platform == VK_ICD_WSI_PLATFORM_XLIB;
   const char *wsi_env = getenv("MALI_WSI");
   const bool strict_dmabuf = wsi_env && !strcmp(wsi_env, "dmabuf-strict");   /* same run without the retry (A/B) */
   if (r != VK_SUCCESS && !pdev->vk.wsi_device->sw && x11_surface && !mali_wsi_x11_forced_shm() && !strict_dmabuf) {
      /* #328: the server advertised DRI3 and Present and still refused every image (xcb_dri3_pixmap_from_buffer of
       * our dma-buf fails), so the whole vkCreateSwapchainKHR returns VK_ERROR_INITIALIZATION_FAILED and zink reports
       * "could not create swapchain". Retry the same chain once on the software device through #323's switch. This is
       * a per-chain fact, not a per-connection one: the connection's decision is NOT recorded as software, so another
       * chain on the same connection still starts on dma-buf (CR #323 N2). MALI_WSI=dmabuf (dma-buf first) retries;
       * MALI_WSI=dmabuf-strict keeps the loud failure for diagnostics. */
      fprintf(stderr, "mali_vk: wsi: %s surface: dma-buf swapchain failed (%s), retrying on the software "
                      "(MIT-SHM/PutImage) present path\n",
              mali_wsi_platform_name(surface->platform), vk_Result_to_str(r));
      mali_wsi_sw_leave(pdev, saved_sw);
      saved_sw = mali_wsi_sw_enter_value(pdev, true);
      r = wsi_CreateSwapchainKHR(device, ci, alloc, out);
   }
   if (r == VK_SUCCESS && x11_surface && !mali_wsi_x11_chain_add(*out, pdev->vk.wsi_device->sw)) {
      /* The table is full: an untracked X11 chain would be presented as a Wayland one (sw=false, dma-buf) with no
       * fallback, so refuse the creation and destroy the chain wsi just built (CR #323 T3). */
      wsi_DestroySwapchainKHR(device, *out, alloc);
      *out = VK_NULL_HANDLE;
      r = VK_ERROR_TOO_MANY_OBJECTS;
      fprintf(stderr, "mali_vk: wsi: no free X11 chain slot (%u); swapchain creation refused\n",
              (unsigned)(sizeof(mali_wsi_x11_chains) / sizeof(mali_wsi_x11_chains[0])));
   }
   if (r == VK_SUCCESS && pdev->vk.wsi_device->sw) {
      VK_FROM_HANDLE(wsi_swapchain, chain, *out);
      mali_x11_shm_create(*out, surface, chain, ci);
   }
   struct wsi_swapchain *created = NULL;
   if (r == VK_SUCCESS) {
      /* #329: struct wsi_swapchain carries neither the surface nor the requested format back to vkQueuePresentKHR. */
      VK_FROM_HANDLE(wsi_swapchain, chain, *out);
      created = chain;
      mali_wsi_info_add(*out, surface, ci, chain, pdev->vk.wsi_device->sw);
   }
   if (r == VK_SUCCESS && !wsi_dev->sw && !wsi_dev->GetMemoryFdKHR) {
      /* Refuse cleanly instead of crashing later: this device's wsi callbacks were fetched for the software path
       * (wsi_common.c:221), so a dma-buf chain cannot present - wsi_common_x11 dereferences the NULL export
       * callback (the #323 crash on the CMF and X8). mali_wsi_sw_enter() above should already make this
       * impossible; this is the guard that keeps a future mistake a failed vkCreateSwapchainKHR, not a SIGSEGV. */
      fprintf(stderr, "mali_vk: wsi: refusing a dma-buf swapchain on a device built for the software path "
              "(GetMemoryFdKHR is NULL); MALI_WSI=dmabuf cannot work here\n");
      if (x11_surface)
         mali_wsi_x11_chain_remove(*out);
      wsi_DestroySwapchainKHR(device, *out, alloc);
      *out = VK_NULL_HANDLE;
      r = VK_ERROR_INITIALIZATION_FAILED;
   }

   if (mali_wsi_debug() || r != VK_SUCCESS) {
      /* Every platform, Wayland included: the product path (gamescope's WSI layer hands a Wayland dma-buf) is the
       * one these diagnostics exist for. comm and pid say which process the swapchain belongs to, surface which
       * window (xcb/xlib id, or the wl_surface pointer), formatName what the app can actually have asked for (#329). */
      fprintf(stderr, "mali_vk: wsi create_swapchain: platform=%s comm=%s pid=%d surface=0x%llx format=%u(%s) "
              "colorSpace=%u usage=0x%x mode=%u extent=%ux%u minImageCount=%u images=%u compositeAlpha=0x%x flags=0x%x "
              "oldSwapchain=%d sw=%d dri3=%d present=%d mit_shm=%d -> %s (%d)\n",
              mali_wsi_platform_name(surface->platform), mali_wsi_comm(), (int)getpid(),
              (unsigned long long)mali_wsi_surface_of(surface), ci->imageFormat, vk_Format_to_str(ci->imageFormat),
              ci->imageColorSpace, ci->imageUsage, ci->presentMode, ci->imageExtent.width, ci->imageExtent.height,
              ci->minImageCount, created ? created->image_count : 0u,
              ci->compositeAlpha, ci->flags, ci->oldSwapchain != VK_NULL_HANDLE,
              (int)pdev->vk.wsi_device->sw, (int)probe.dri3, (int)probe.present, (int)probe.mit_shm,
              vk_Result_to_str(r), (int)r);
      if (r != VK_SUCCESS && !probe.have_conn)
         fprintf(stderr, "mali_vk: wsi: %s surface: the X connection was not probed (xlib), see the X server's "
                 "extensions (xdpyinfo -queryExtensions)\n", mali_wsi_platform_name(surface->platform));
      if (r != VK_SUCCESS && !probe.present && probe.have_conn)
         fprintf(stderr, "mali_vk: wsi: the X server has no Present extension: wsi_common_x11 cannot present at "
                 "all there, so no swapchain can be created\n");
      mali_wsi_x11_log_surface(pdev, surface);
   }

   mali_wsi_sw_leave(pdev, saved_sw);
   return r;
}

VkResult
mali_wsi_init(struct mali_physical_device *pdev)
{
   bool sw = mali_wsi_use_sw(pdev);
   VkResult r = wsi_device_init(&pdev->wsi, mali_physical_device_to_handle(pdev), mali_wsi_proc_addr,
                                &pdev->vk.instance->alloc, -1, NULL,
                                &(struct wsi_device_options){.sw_device = sw});
   if (r != VK_SUCCESS)
      return r;
   /* What Mesa decided (sw_device or MESA_VK_WSI_DEBUG=sw) is an input, not something mali_wsi_sw_enter() may
    * lower: wsi_common.c:221 fetches GetMemoryFdKHR only for the dma-buf path, so flipping wsi.sw to false on such
    * a device makes wsi_common_x11 call a NULL pointer at swapchain create/present (#323 crash, exit 139 on the CMF
    * and X8). Kept even when our own X11 decision later asks for dma-buf. */
   pdev->wsi_sw_base = pdev->wsi.sw;
   /* No DRM node behind kbase: wsi sees no render/primary device and assumes the compositor is on our GPU.
    * Modifiers (linear only) exist only on the dma-buf path; the software path copies into CPU-visible buffers. */
   pdev->wsi.supports_modifiers = !sw;
   /* Capability rule for the software present path: wsi_cpu_image_needs_buffer_blit() (wsi_common.c:3599) returns
    * true unless wsi->wants_linear, and a buffer blit makes wsi record vkCmdBlitImage/vkCmdCopyImage between a
    * CPU buffer image and the presentable one. We do not implement those transfer commands, so vkEndCommandBuffer
    * failed with VK_ERROR_FEATURE_NOT_PRESENT inside the present and vkQueuePresentKHR returned -8 *after* the
    * frame had been copied (mali_cmd.c:1330/1370/1386). wants_linear makes wsi allocate the linear CPU-visible
    * image itself: no blit, the present is a plain PutImage of that memory, and the pixels the app rendered are the
    * pixels that get displayed. It also removes the SHM/blit branch that needs EXT_external_memory_host, which we
    * do not implement: SHM is only used where the extension exists, otherwise PutImage, and both report VK_SUCCESS. */
   pdev->wsi.wants_linear = true;
   if (sw)
      mali_wsi_disable_dmabuf_sync(&pdev->wsi);
   pdev->wsi_real_destroy_query_pool = pdev->wsi.DestroyQueryPool;
   pdev->wsi.DestroyQueryPool = mali_wsi_destroy_query_pool;
   /* The software instance: an immutable copy, so any call (and any swapchain, for its whole life) can run on it
    * while another runs on the dma-buf one. A swapchain's mode is then fixed at creation - Mesa reads
    * chain->base.wsi->sw at present (wsi_common_x11.c:2101, from the async queue manager) and at destroy (:2807). */
   pdev->wsi_sw_dev = pdev->wsi;
   pdev->wsi_sw_dev.sw = true;
   pdev->wsi_sw_dev.supports_modifiers = false;   /* software images are linear CPU buffers */
   mali_wsi_disable_dmabuf_sync(&pdev->wsi_sw_dev);
   pdev->vk.wsi_device = &pdev->wsi;

   return VK_SUCCESS;
}

void
mali_wsi_finish(struct mali_physical_device *pdev)
{
   pdev->vk.wsi_device = NULL;
   wsi_device_finish(&pdev->wsi, &pdev->vk.instance->alloc);
}
