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
#include "vk_fence.h"             /* VkSwapchainPresentFenceInfoEXT in the shm shortcut below */

/* wsi_common.c: what the device dispatch would have called; not in wsi_common.h in this tree. */
VKAPI_ATTR VkResult VKAPI_CALL wsi_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
                                                     const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain);
VKAPI_ATTR VkResult VKAPI_CALL wsi_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                                       VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex);
VKAPI_ATTR VkResult VKAPI_CALL wsi_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo);
VKAPI_ATTR void VKAPI_CALL wsi_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                  const VkAllocationCallbacks *pAllocator);

#include "vk_alloc.h"
#include "vk_queue.h"      /* VK_FROM_HANDLE(vk_queue) */
#include "vk_semaphore.h"  /* struct vk_semaphore -> &sem->sync */
#include "vk_log.h"
#include "vk_util.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#if defined(VK_USE_PLATFORM_XCB_KHR)
#include <xcb/xcb.h>
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
static bool mali_x11_shm_present(struct mali_device *dev, struct wsi_swapchain *chain, uint32_t image_index);

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
   if (e && !strcmp(e, "dmabuf"))
      return false;
   /* No X probe here (see above): the software path is chosen per surface in mali_wsi_create_swapchain(). */
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
static void
mali_wsi_x11_probe(VkIcdSurfaceBase *surface, struct mali_wsi_x11_probe *p)
{
   memset(p, 0, sizeof(*p));
#if defined(VK_USE_PLATFORM_XCB_KHR)
   if (surface->platform != VK_ICD_WSI_PLATFORM_XCB)
      return;
   VkIcdSurfaceXcb *s = (VkIcdSurfaceXcb *)surface;
   if (!s->connection)
      return;
   const struct xcb_query_extension_reply_t *dri3 = xcb_get_extension_data(s->connection, &xcb_dri3_id);
   const struct xcb_query_extension_reply_t *present = xcb_get_extension_data(s->connection, &xcb_present_id);
   const struct xcb_query_extension_reply_t *shm = xcb_get_extension_data(s->connection, &xcb_shm_id);
   p->have_conn = true;
   p->dri3 = dri3 && dri3->present && !mali_wsi_force_no_dri3();
   p->present = present && present->present;
   p->mit_shm = shm && shm->present;
#else
   (void)surface;
#endif
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
       iface->get_capabilities2(surface, &pdev->wsi, NULL, &caps) == VK_SUCCESS) {
      const VkSurfaceCapabilitiesKHR *c = &caps.surfaceCapabilities;
      fprintf(stderr, "mali_vk: wsi surface caps: currentExtent=%ux%u minImageCount=%u maxImageCount=%u "
              "supportedUsage=0x%x compositeAlpha=0x%x transform=0x%x\n",
              c->currentExtent.width, c->currentExtent.height, c->minImageCount, c->maxImageCount,
              c->supportedUsageFlags, c->supportedCompositeAlpha, c->currentTransform);
   }
   VkSurfaceFormatKHR fmt[16];
   uint32_t n = 16;
   if (iface->get_formats && iface->get_formats(surface, &pdev->wsi, &n, fmt) == VK_SUCCESS) {
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

/* wsi_common_x11 advertises all four present modes, but a software (PutImage) swapchain is only useful to a
 * measurement when IMMEDIATE is really offered: with FIFO/MAILBOX the producer rate equals the displayed rate by
 * construction, which is what hid the GPU headroom (1600x720 and 2400x1080 both 118 fps). IMMEDIATE is additive,
 * FIFO/MAILBOX stay first and unchanged for real apps. */
static void mali_wsi_present_id_set(VkSwapchainKHR sc, uint64_t id);
static uint64_t mali_wsi_present_id_take(VkSwapchainKHR sc);

VKAPI_ATTR void VKAPI_CALL
mali_wsi_destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *alloc)
{
   /* The one place the presentation feedback of the last frame must be pumped: wsi_common_wayland asserts that no
    * present id is still queued while it frees the chain (wsi_common_wayland.c:3782, :2779). Bounded, once per chain
    * and outside the frame loop, instead of 20 ms per frame in present (see struct mali_wsi_present_id). */
   VK_FROM_HANDLE(wsi_swapchain, chain, swapchain);
   const uint64_t last_id = mali_wsi_present_id_take(swapchain);
   if (chain && last_id && chain->present_wait_enabled && chain->wait_for_present2)
      chain->wait_for_present2(chain, last_id, 20ull * 1000 * 1000);   /* ns, bounded */
   mali_x11_shm_destroy(swapchain);
   wsi_DestroySwapchainKHR(device, swapchain, alloc);
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
   if (!modes) {
      VkResult r = iface->get_present_modes((VkIcdSurfaceBase *)(uintptr_t)surface, &pdev->wsi, count, NULL);
      if (r == VK_SUCCESS && pdev->wsi.sw && *count < UINT32_MAX)
         (*count)++;   /* upper bound: the enumeration below decides whether IMMEDIATE is really missing */
      return r;
   }
   /* Enumerate, then return the list unchanged except for IMMEDIATE appended on the software path. */
   VkPresentModeKHR all[8];
   uint32_t n = 8;
   VkResult r = iface->get_present_modes((VkIcdSurfaceBase *)(uintptr_t)surface, &pdev->wsi, &n, all);
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
struct mali_x11_shm {
   VkSwapchainKHR swapchain;
   bool active;
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
   xcb_shm_detach((xcb_connection_t *)st->conn, st->seg);
   if (st->gc)
      xcb_free_gc((xcb_connection_t *)st->conn, st->gc);   /* created per swapchain in mali_x11_shm_create */
   xcb_flush((xcb_connection_t *)st->conn);
#endif
   if (st->addr && st->addr != MALI_SHM_FAILED)
      shmdt(st->addr);
   if (st->shmid >= 0)
      shmctl(st->shmid, IPC_RMID, NULL);
   if (mali_wsi_debug())
      fprintf(stderr, "mali_vk: x11 shm: released %zu B segment (shmid %d)\n", st->size, st->shmid);
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
   const xcb_gcontext_t gc = xcb_generate_id(s->connection);
   xcb_create_gc(s->connection, gc, s->window, XCB_GC_GRAPHICS_EXPOSURES, (uint32_t []){0});
   xcb_flush(s->connection);

   *st = (struct mali_x11_shm){.swapchain = sc, .active = true, .conn = s->connection, .window = s->window,
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

/* Present one image through the shared segment. Returns false when the image cannot be copied (no CPU map), in
 * which case the caller falls back to wsi's PutImage present. */
static bool
mali_x11_shm_present(struct mali_device *dev, struct wsi_swapchain *chain, uint32_t image_index)
{
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
   /* The copy must not read the image before the submission that wrote it has retired. There is no CPU-visible
    * semaphore to wait here (binary semaphores are not observable through vk_sync_wait in this driver: see
    * LESSONS / driver-xcb.md), so wait the queue's own submit fence, bounded (1 s: a lost render must not hang the
    * present). That wait ends in kbase's device->CPU MEM_SYNC (kbase_fence_wait -> mem_sync_bos), and
    * present_image_cpu_sync() below covers the mapping itself. */
   int ffd = mali_queue_last_fence_dup(dev);
   if (ffd >= 0) {
      const struct mali_backend_ops *be = dev ? mali_device_pdev(dev)->backend : NULL;
      struct timespec t0, t1;
      clock_gettime(CLOCK_MONOTONIC, &t0);
      int wr = be && be->fence_wait ? be->fence_wait(dev->kdev, ffd, 1000) : 0;
      clock_gettime(CLOCK_MONOTONIC, &t1);
      close(ffd);
      const long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
      if (wr != 0) {
         /* The frame's submit never completed (device lost or a fault): copying its buffer would present a
          * partial frame as success. Fall back to wsi's own path, which reports the error. */
         fprintf(stderr, "mali_vk: wsi present: image %u submit fence wait %d, not copying\n", image_index, wr);
         return false;
      }
      else if (ms >= 20)
         fprintf(stderr, "mali_vk: wsi present: image %u waited %ld ms for the submit fence\n", image_index, ms);
   }
   present_image_cpu_sync(dev, mali_image_from_handle(img->image), image_index);
   memcpy((uint8_t *)st->addr + off, img->cpu_map, (size_t)st->stride * st->height);
   /* total_width is the *segment* pitch (the server reads rows of that width); src width/height are the image. */
   const uint16_t total_w = (uint16_t)(st->stride / 4);
   xcb_shm_put_image((xcb_connection_t *)st->conn, st->window, st->gc, total_w, st->height, 0, 0, st->width, st->height,
                     0, 0, st->depth, XCB_IMAGE_FORMAT_Z_PIXMAP, 0 /* send_event */, st->seg, off);
   xcb_flush((xcb_connection_t *)st->conn);
   return true;
#endif
}

/* MALI_WSI_DEBUG=1: one line per acquire/present so a stall shows as an "enter" without its matching result
 * (the software present path copies the whole image and PutImage is fire-and-forget, so this is also the only way
 * to see how far a client got). Both are thin wrappers around the runtime's wsi entrypoints. */
VKAPI_ATTR VkResult VKAPI_CALL
mali_wsi_acquire_next_image(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore,
                            VkFence fence, uint32_t *image_index)
{
   VK_FROM_HANDLE(mali_device, dev, device);
   struct mali_physical_device *pdev = mali_device_pdev(dev);
   static uint64_t seq;
   uint64_t n = p_atomic_inc_return(&seq);
   if (mali_wsi_debug())
      fprintf(stderr, "mali_vk: wsi acquire #%llu: timeout=%llu sem=%d fence=%d sw=%d\n", (unsigned long long)n,
              (unsigned long long)timeout, semaphore != VK_NULL_HANDLE, fence != VK_NULL_HANDLE,
              (int)pdev->wsi.sw);
   VkResult r = wsi_AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, image_index);
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

/* MALI_WSI_DUMP=<prefix>: write every MALI_WSI_DUMP_EVERY-th presented frame (default 30) as a PPM (P6) at
 * <prefix>-NNNNN.ppm and one line per dump into <prefix>.txt (frame, image, extent, stride, VkFormat, BO va).
 * Debug only, default off. Cost: one CPU read of the presented BO plus a cache invalidate, i.e. a full
 * w*h*bpp pass on the presenting thread every N frames (and it drops the BO's HOSTLAZY flag, so the next
 * fence wait MEM_SYNCs it once). Only 8-bit RGBA/BGRA rows are reordered to RGB; anything else is written
 * with the raw first three bytes of the texel and the format named in the sidecar. */
static void
mali_wsi_dump_frame(struct mali_device *dev, struct wsi_swapchain *chain, uint32_t image_index)
{
   const char *prefix = getenv("MALI_WSI_DUMP");
   if (!prefix || !prefix[0])
      return;
   unsigned every = 30;
   const char *e = getenv("MALI_WSI_DUMP_EVERY");
   if (e && e[0])
      every = (unsigned)strtoul(e, NULL, 10);
   if (!every)
      every = 1;
   static uint64_t seq;
   const uint64_t n = p_atomic_inc_return(&seq);
   if (n % every)
      return;

   struct wsi_image *wi = NULL;
   struct mali_image *mi = present_image(chain, image_index, &wi);
   if (!mi || !mi->mem || !mi->mem->bo.cpu) {
      fprintf(stderr, "mali_vk: wsi dump #%llu: image %u has no CPU mapping (unbound or not ours)\n",
              (unsigned long long)n, image_index);
      return;
   }
   const struct mali_backend_ops *be = dev ? mali_device_pdev(dev)->backend : NULL;
   if (be && be->bo_host_touch)
      be->bo_host_touch(dev->kdev, &mi->mem->bo);   /* device wrote it: the next sync must cover it */
   if (be && be->bo_cache)
      be->bo_cache(dev->kdev, &mi->mem->bo, 0, mi->mem->bo.size, true);

   const struct mali_plane_layout *pl = mali_image_plane_for_aspect(&mi->lay, VK_IMAGE_ASPECT_COLOR_BIT);
   if (!pl || !pl->slice[0].row_stride || !mi->lay.d.extent.width || !mi->lay.d.extent.height) {
      fprintf(stderr, "mali_vk: wsi dump #%llu: image %u has no colour plane layout\n", (unsigned long long)n, image_index);
      return;
   }
   const unsigned w = mi->lay.d.extent.width, h = mi->lay.d.extent.height;
   const unsigned stride = pl->slice[0].row_stride, bpp = pl->bs ? pl->bs : 4;
   const int fmt = (int)mi->vk.format;
   const char *fname = vk_Format_to_str(mi->vk.format);
   const uint8_t *base = (const uint8_t *)mi->mem->bo.cpu + mi->mem_off + pl->offset;
   const bool bgra = mi->vk.format == VK_FORMAT_B8G8R8A8_UNORM || mi->vk.format == VK_FORMAT_B8G8R8A8_SRGB;
   const bool rgba = mi->vk.format == VK_FORMAT_R8G8B8A8_UNORM || mi->vk.format == VK_FORMAT_R8G8B8A8_SRGB ||
                     mi->vk.format == VK_FORMAT_A8B8G8R8_UNORM_PACK32 || mi->vk.format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;

   char path[1024];
   snprintf(path, sizeof path, "%s-%05llu.ppm", prefix, (unsigned long long)n);
   FILE *f = fopen(path, "wb");
   if (!f) {
      fprintf(stderr, "mali_vk: wsi dump #%llu: open %s: %s\n", (unsigned long long)n, path, strerror(errno));
      return;
   }
   fprintf(f, "P6\n%u %u\n255\n", w, h);
   uint8_t *row = malloc((size_t)w * 3);
   if (!row) { fclose(f); return; }
   for (unsigned y = 0; y < h; y++) {
      const uint8_t *src = base + (size_t)y * stride;
      for (unsigned x = 0; x < w; x++) {
         const uint8_t *p = src + (size_t)x * bpp;
         if (bgra)      { row[x * 3 + 0] = p[2]; row[x * 3 + 1] = p[1]; row[x * 3 + 2] = p[0]; }
         else if (rgba) { row[x * 3 + 0] = p[0]; row[x * 3 + 1] = p[1]; row[x * 3 + 2] = p[2]; }
         else           { row[x * 3 + 0] = p[0]; row[x * 3 + 1] = p[1]; row[x * 3 + 2] = p[2]; }   /* raw, see the sidecar */
      }
      fwrite(row, 1, (size_t)w * 3, f);
   }
   free(row);
   fclose(f);

   snprintf(path, sizeof path, "%s.txt", prefix);
   FILE *t = fopen(path, "a");
   if (t) {
      fprintf(t, "frame %llu: image %u %ux%u stride %u bpp %u samples %u format %s (%d)%s%s%s bo 0x%llx\n",
              (unsigned long long)n, image_index, w, h, stride, bpp, mi->lay.d.samples, fname, fmt,
              bgra ? " bgra->rgb" : rgba ? " rgba->rgb" : " raw", pl->bw == 1 && pl->bh == 1 ? "" : " (block format)",
              wi && wi->cpu_map ? " cpu_map" : "", (unsigned long long)mi->va);
      fclose(t);
   }
   fprintf(stderr, "mali_vk: wsi dump #%llu: image %u %ux%u stride %u %s -> %s-%05llu.ppm\n", (unsigned long long)n,
           image_index, w, h, stride, fname, prefix, (unsigned long long)n);
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
         if (sc)
            mali_wsi_dump_frame(dev, sc, idx);   /* MALI_WSI_DUMP: the BO as it is handed to the compositor */
         const bool shm = sc && mali_x11_shm_find(info->pSwapchains[i]) && mali_x11_shm_present(dev, sc, idx);
         if (mali_wsi_debug() && sc)
            mali_wsi_debug_present(dev, sc, idx, shm ? "shm" : "dma-buf", VK_SUCCESS);
         if (!shm) {
            all_shm = false;
            break;
         }
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
          * is queued here, so signal them here too. Without it a client that waits a present fence (the pattern
          * t_wsi_maint.c:453 checks) hangs until its timeout on the X11 shm path. */
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
   VkResult r = wsi_QueuePresentKHR(queue, info);
   /* Record the present id and return: see struct mali_wsi_present_id. The feedback is pumped by the next present
    * (Mesa's own dispatch_present_id_queue) and drained once in destroy, so nothing here waits on the compositor. */
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

   /* X11 surfaces default to the software (PutImage) path, decided here, per surface, so Wayland clients never
    * touch the X server: the old unconditional probe ran inside vkEnumeratePhysicalDevices and its xcb_connect()
    * hung them before their first log line. MALI_WSI=dmabuf opts back into dma-buf, where a broken import fails
    * loudly instead of silently dropping presents (the hang in wsi_common_x11.c:2202). The fake backend stays
    * software from init and its tests use MALI_WSI=dmabuf to see the dma-buf path fail on purpose. */
   const char *forced = getenv("MALI_WSI");
   const bool forced_dmabuf = forced && !strcmp(forced, "dmabuf");
   const bool forced_shm = forced && (!strcmp(forced, "shm") || !strcmp(forced, "xput") || !strcmp(forced, "sw"));
   bool x11_surface = surface->platform == VK_ICD_WSI_PLATFORM_XCB ||
                      surface->platform == VK_ICD_WSI_PLATFORM_XLIB;
   /* wsi_common_x11's dma-buf path needs DRI3 (x11_image_init -> xcb_dri3_pixmap_from_buffer) and Present
    * (x11_present_image -> xcb_present_pixmap); mali_wsi_x11_probe answers both (mali_wsi.c:169-176). Prefer it
    * whenever the server has them: it hands the compositor the frame's own fence, while the shared-memory path
    * has to wait for the GPU from the CPU on every present and was losing 20 % of frames to stale copies.
    * MALI_WSI=shm/xput/sw forces the fallback, MALI_WSI=dmabuf is now the default where the server allows it. */
   const bool server_dmabuf = probe.dri3 && probe.present;
   if (!wsi_dev->sw && !forced_dmabuf && x11_surface && (forced_shm || !server_dmabuf)) {
      wsi_dev->sw = true;
      wsi_dev->supports_modifiers = false;
      mali_wsi_disable_dmabuf_sync(wsi_dev);
      fprintf(stderr, "mali_vk: wsi: %s surface: software (PutImage) present path (%s) (MALI_WSI=dmabuf for dma-buf)\n",
              mali_wsi_platform_name(surface->platform),
              forced_shm ? "forced" : !probe.present ? "server has no Present" : "server has no DRI3");
   }

   if (mali_wsi_debug() && surface->platform != VK_ICD_WSI_PLATFORM_WAYLAND)
      fprintf(stderr, "mali_vk: wsi hooks: GetSemaphoreFdKHR=%s GetMemoryFdKHR=%s cached_sync_file=%u (dma_buf_sync=%d)\n",
              wsi_dev->GetSemaphoreFdKHR ? "set" : "NULL", wsi_dev->GetMemoryFdKHR ? "set" : "NULL",
              __atomic_load_n(&wsi_dev->cached_sync_file_import_export_result, __ATOMIC_RELAXED),
              __atomic_load_n(&wsi_dev->cached_sync_file_import_export_result, __ATOMIC_RELAXED) == UINT32_MAX);
   if (mali_wsi_debug() && surface->platform != VK_ICD_WSI_PLATFORM_WAYLAND)
      fprintf(stderr, "mali_vk: x11 present path: shm=%d dri3=%d%s\n",
              (int)(probe.mit_shm && wsi_dev->has_import_memory_host), (int)probe.dri3,
              wsi_dev->sw ? " (software)" : " (dma-buf)");

   VkResult r = wsi_CreateSwapchainKHR(device, ci, alloc, out);
   if (r == VK_SUCCESS && wsi_dev->sw) {
      VK_FROM_HANDLE(wsi_swapchain, chain, *out);
      mali_x11_shm_create(*out, surface, chain, ci);
   }

   if (mali_wsi_debug() || r != VK_SUCCESS) {
      fprintf(stderr, "mali_vk: wsi create_swapchain: platform=%s format=%u colorSpace=%u usage=0x%x mode=%u "
              "extent=%ux%u minImageCount=%u compositeAlpha=0x%x flags=0x%x oldSwapchain=%d sw=%d "
              "dri3=%d present=%d mit_shm=%d -> %s (%d)\n",
              mali_wsi_platform_name(surface->platform), ci->imageFormat, ci->imageColorSpace,
              ci->imageUsage, ci->presentMode, ci->imageExtent.width, ci->imageExtent.height,
              ci->minImageCount, ci->compositeAlpha, ci->flags, ci->oldSwapchain != VK_NULL_HANDLE,
              (int)wsi_dev->sw, (int)probe.dri3, (int)probe.present, (int)probe.mit_shm,
              vk_Result_to_str(r), (int)r);
      if (r != VK_SUCCESS && !probe.have_conn)
         fprintf(stderr, "mali_vk: wsi: %s surface: the X connection was not probed (xlib), see the X server's "
                 "extensions (xdpyinfo -queryExtensions)\n", mali_wsi_platform_name(surface->platform));
      if (r != VK_SUCCESS && !probe.present && probe.have_conn)
         fprintf(stderr, "mali_vk: wsi: the X server has no Present extension: wsi_common_x11 cannot present at "
                 "all there, so no swapchain can be created\n");
      mali_wsi_x11_log_surface(pdev, surface);
   }

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
   pdev->vk.wsi_device = &pdev->wsi;

   return VK_SUCCESS;
}

void
mali_wsi_finish(struct mali_physical_device *pdev)
{
   pdev->vk.wsi_device = NULL;
   wsi_device_finish(&pdev->wsi, &pdev->vk.instance->alloc);
}
