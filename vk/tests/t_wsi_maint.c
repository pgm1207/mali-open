/* SPDX-License-Identifier: MIT */
/* WSI contract test: VK_EXT_swapchain_maintenance1 and DRM format modifiers.
 *
 *   t_wsi_maint [--direct <icd.so>] [--expect-name <substr>]
 *
 * Needs an X server ($DISPLAY) for the swapchain half. Exit 0 = pass, 77 = skipped (no X, no xcb surface, or the
 * ICD does not advertise VK_EXT_swapchain_maintenance1), 1 = fail.
 *
 * 1. maintenance1 is advertised AND enabled (extension + VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT): creating a
 *    swapchain with VkSwapchainPresentModesCreateInfoEXT and VkSwapchainPresentScalingCreateInfoEXT chained succeeds
 *    (an app that enables the feature may chain them; the gamescope WSI layer forces both on its client device).
 * 2. the per-present switch is honoured: VkSwapchainPresentModeInfoKHR changes the mode on vkQueuePresentKHR, the
 *    VkSwapchainPresentFenceInfoEXT fence signals, and vkReleaseSwapchainImagesEXT releases an acquired image.
 * 3. DRM modifiers: NV12/BGRA8/RGBA8 report LINEAR with a real plane count and tiling features; an NV12 LINEAR
 *    modifier image gets both memory planes' subresource layouts (plane 1 offset differs from plane 0's).
 */
#define _GNU_SOURCE
#define VK_USE_PLATFORM_XCB_KHR
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include <xcb/xcb.h>

#define W 128
#define H 96
#define FRAMES 2

#define CK(x)                                                                                    \
   do {                                                                                          \
      VkResult r_ = (x);                                                                         \
      if (r_ != VK_SUCCESS) {                                                                    \
         fprintf(stderr, "FAIL %s:%d %s = %d\n", __FILE__, __LINE__, #x, (int)r_);               \
         return 1;                                                                               \
      }                                                                                          \
   } while (0)
#define REQ(c, ...)                                                                              \
   do {                                                                                          \
      if (!(c)) {                                                                                \
         fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__);                                     \
         fprintf(stderr, __VA_ARGS__);                                                           \
         fprintf(stderr, "\n");                                                                  \
         return 1;                                                                               \
      }                                                                                          \
   } while (0)

static PFN_vkGetInstanceProcAddr gipa;

#define IPFN(i, n) PFN_vk##n n = (PFN_vk##n)gipa(i, "vk" #n)

static int
modifier_queries(VkPhysicalDevice pd, PFN_vkGetPhysicalDeviceFormatProperties2 fp2)
{
   struct { VkFormat fmt; uint32_t planes; const char *name; } cases[] = {
      {VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, 2, "NV12"},
      {VK_FORMAT_B8G8R8A8_UNORM, 1, "BGRA8"},
      {VK_FORMAT_R8G8B8A8_UNORM, 1, "RGBA8"},
   };
   for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
      VkFormatProperties2 bare = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
      fp2(pd, cases[i].fmt, &bare);
      VkDrmFormatModifierPropertiesListEXT ml = {.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
      VkFormatProperties2 fp = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &ml};
      fp2(pd, cases[i].fmt, &fp);
      REQ(ml.drmFormatModifierCount >= 1, "%s: zero DRM modifiers", cases[i].name);
      VkDrmFormatModifierPropertiesEXT mp[4] = {0};
      ml.pDrmFormatModifierProperties = mp;
      ml.drmFormatModifierCount = 4;
      fp2(pd, cases[i].fmt, &fp);
      REQ(ml.drmFormatModifierCount >= 1, "%s: zero modifiers on the filled query", cases[i].name);
      REQ(mp[0].drmFormatModifier == 0, "%s: first modifier %#llx is not LINEAR", cases[i].name,
          (unsigned long long)mp[0].drmFormatModifier);
      REQ(mp[0].drmFormatModifierPlaneCount == cases[i].planes, "%s: %u memory planes, wanted %u", cases[i].name,
          mp[0].drmFormatModifierPlaneCount, cases[i].planes);
      REQ(mp[0].drmFormatModifierTilingFeatures != 0, "%s: modifier reports no tiling features", cases[i].name);
      printf("modifier: %s LINEAR planes=%u features=%#x\n", cases[i].name, mp[0].drmFormatModifierPlaneCount,
             mp[0].drmFormatModifierTilingFeatures);
   }
   return 0;
}

static int
nv12_modifier_image(VkDevice dev, VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties mp,
                    PFN_vkCreateImage ci_, PFN_vkDestroyImage di_, PFN_vkGetImageMemoryRequirements2 gm_,
                    PFN_vkGetImageSubresourceLayout gl_, PFN_vkAllocateMemory am_, PFN_vkBindImageMemory2 bm_,
                    PFN_vkFreeMemory fm_, PFN_vkGetImageDrmFormatModifierPropertiesEXT gmprop_)
{
   const uint64_t linear = 0;
   VkImageDrmFormatModifierListCreateInfoEXT ml = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
      .drmFormatModifierCount = 1, .pDrmFormatModifiers = &linear};
   VkImageCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &ml,
      .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
      .extent = {64, 64, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   VkImage img;
   CK(ci_(dev, &ci, NULL, &img));
   VkMemoryRequirements req;
   VkMemoryRequirements2 req2 = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
   gm_(dev, &(VkImageMemoryRequirementsInfo2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = img}, &req2);
   req = req2.memoryRequirements;
   uint32_t type = 0;
   for (; type < mp.memoryTypeCount; type++)
      if (req.memoryTypeBits & (1u << type)) break;
   REQ(type < mp.memoryTypeCount, "NV12 modifier image: no memory type");
   VkDeviceMemory mem;
   CK(am_(dev, &(VkMemoryAllocateInfo){.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                       .memoryTypeIndex = type}, NULL, &mem));
   CK(bm_(dev, 1, &(VkBindImageMemoryInfo){.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO, .image = img, .memory = mem,
                                           .memoryOffset = 0}));

   uint64_t off[2] = {0}, size[2] = {0};
   for (unsigned p = 0; p < 2; p++) {
      VkSubresourceLayout sl;
      gl_(dev, img, &(VkImageSubresource){.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << p,
                                          .mipLevel = 0, .arrayLayer = 0}, &sl);
      off[p] = sl.offset;
      size[p] = sl.size;
      REQ(size[p] > 0, "NV12 modifier image: memory plane %u has size 0", p);
      REQ(sl.rowPitch > 0, "NV12 modifier image: memory plane %u has rowPitch 0", p);
   }
   REQ(off[1] != off[0], "NV12 modifier image: memory planes 0 and 1 share offset %llu", (unsigned long long)off[0]);
   VkImageDrmFormatModifierPropertiesEXT got = {.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
   CK(gmprop_(dev, img, &got));
   REQ(got.drmFormatModifier == 0, "NV12 modifier image: modifier came back as %#llx", (unsigned long long)got.drmFormatModifier);
   printf("image: NV12 LINEAR planes at %llu+%llu (sizes %llu+%llu)\n", (unsigned long long)off[0],
          (unsigned long long)off[1], (unsigned long long)size[0], (unsigned long long)size[1]);
   di_(dev, img, NULL);
   fm_(dev, mem, NULL);
   return 0;
}

int
main(int argc, char **argv)
{
   const char *direct = NULL, *expect = NULL;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) direct = argv[++i];
      else if (!strcmp(argv[i], "--expect-name") && i + 1 < argc) expect = argv[++i];
      else { fprintf(stderr, "usage: t_wsi_maint [--direct icd.so] [--expect-name s]\n"); return 2; }
   }

   void *lib = direct ? dlopen(direct, RTLD_NOW | RTLD_LOCAL) : dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
   if (!lib) { fprintf(stderr, "FAIL dlopen: %s\n", dlerror()); return 1; }
   gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, direct ? "vk_icdGetInstanceProcAddr" : "vkGetInstanceProcAddr");
   REQ(gipa, "no GetInstanceProcAddr");

   IPFN(NULL, EnumerateInstanceExtensionProperties);
   uint32_t n = 0;
   CK(EnumerateInstanceExtensionProperties(NULL, &n, NULL));
   VkExtensionProperties *ep = calloc(n + 1, sizeof *ep);
   CK(EnumerateInstanceExtensionProperties(NULL, &n, ep));
   int has_surf = 0, has_xcb = 0, has_sm1 = 0, has_gsc2 = 0;
   for (uint32_t i = 0; i < n; i++) {
      has_surf |= !strcmp(ep[i].extensionName, "VK_KHR_surface");
      has_xcb |= !strcmp(ep[i].extensionName, "VK_KHR_xcb_surface");
      has_sm1 |= !strcmp(ep[i].extensionName, "VK_EXT_surface_maintenance1");
      has_gsc2 |= !strcmp(ep[i].extensionName, "VK_KHR_get_surface_capabilities2");
   }
   free(ep);
   if (!has_surf || !has_xcb) { printf("skipped: ICD does not advertise VK_KHR_xcb_surface\n"); return 77; }

   int scr;
   xcb_connection_t *c = xcb_connect(NULL, &scr);
   if (xcb_connection_has_error(c)) { printf("skipped: no X server ($DISPLAY)\n"); return 77; }
   const xcb_setup_t *setup = xcb_get_setup(c);
   xcb_screen_iterator_t sit = xcb_setup_roots_iterator(setup);
   for (int i = 0; i < scr; i++) xcb_screen_next(&sit);
   xcb_screen_t *s = sit.data;
   xcb_window_t win = xcb_generate_id(c);
   uint32_t vals[2] = {s->black_pixel, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY};
   xcb_create_window(c, XCB_COPY_FROM_PARENT, win, s->root, 0, 0, W, H, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual,
                     XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, vals);
   xcb_map_window(c, win);
   xcb_flush(c);

   const char *iexts[4];
   uint32_t niext = 0;
   iexts[niext++] = "VK_KHR_surface";
   iexts[niext++] = "VK_KHR_xcb_surface";
   if (has_sm1) iexts[niext++] = "VK_EXT_surface_maintenance1";
   if (has_gsc2) iexts[niext++] = "VK_KHR_get_surface_capabilities2";
   VkInstance inst;
   CK(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(
      &(VkInstanceCreateInfo){.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                              .pApplicationInfo = &(VkApplicationInfo){.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                                                       .pApplicationName = "t_wsi_maint",
                                                                       .apiVersion = VK_API_VERSION_1_3},
                              .enabledExtensionCount = niext, .ppEnabledExtensionNames = iexts},
      NULL, &inst));
   IPFN(inst, EnumeratePhysicalDevices);
   IPFN(inst, GetPhysicalDeviceProperties);
   IPFN(inst, EnumerateDeviceExtensionProperties);
   IPFN(inst, GetPhysicalDeviceQueueFamilyProperties);
   IPFN(inst, GetPhysicalDeviceFeatures2);
   IPFN(inst, GetPhysicalDeviceFormatProperties2);
   IPFN(inst, GetPhysicalDeviceMemoryProperties);
   IPFN(inst, GetPhysicalDeviceSurfaceCapabilities2KHR);
   IPFN(inst, CreateXcbSurfaceKHR);
   IPFN(inst, GetPhysicalDeviceSurfaceSupportKHR);
   IPFN(inst, GetPhysicalDeviceSurfaceCapabilitiesKHR);
   IPFN(inst, GetPhysicalDeviceSurfaceFormatsKHR);
   IPFN(inst, GetPhysicalDeviceSurfacePresentModesKHR);
   IPFN(inst, CreateDevice);
   IPFN(inst, GetDeviceProcAddr);
   IPFN(inst, DestroySurfaceKHR);
   IPFN(inst, DestroyInstance);

   uint32_t np = 1;
   VkPhysicalDevice pd;
   VkResult r = EnumeratePhysicalDevices(inst, &np, &pd);
   REQ((r == VK_SUCCESS || r == VK_INCOMPLETE) && np >= 1, "no physical device");
   VkPhysicalDeviceProperties props;
   GetPhysicalDeviceProperties(pd, &props);
   printf("device: %s\n", props.deviceName);
   if (expect) REQ(strstr(props.deviceName, expect), "device name mismatch");

   /* 1. the extension must be advertised, the feature must be true, and the extension must be enableable. */
   uint32_t nd = 0;
   CK(EnumerateDeviceExtensionProperties(pd, NULL, &nd, NULL));
   VkExtensionProperties *dep = calloc(nd + 1, sizeof *dep);
   CK(EnumerateDeviceExtensionProperties(pd, NULL, &nd, dep));
   int dev_sm1 = 0;
   for (uint32_t i = 0; i < nd; i++) dev_sm1 |= !strcmp(dep[i].extensionName, "VK_EXT_swapchain_maintenance1");
   free(dep);
   REQ(dev_sm1, "ICD does not advertise VK_EXT_swapchain_maintenance1 (the devices below chain its structs)");

   VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT sm1 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
   VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &sm1};
   GetPhysicalDeviceFeatures2(pd, &f2);
   REQ(sm1.swapchainMaintenance1, "VK_EXT_swapchain_maintenance1 advertised but swapchainMaintenance1 is false");

   if (modifier_queries(pd, GetPhysicalDeviceFormatProperties2)) return 1;

   VkPhysicalDeviceMemoryProperties mp;
   GetPhysicalDeviceMemoryProperties(pd, &mp);

   uint32_t nq = 0;
   GetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
   VkQueueFamilyProperties *qf = calloc(nq + 1, sizeof *qf);
   GetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
   uint32_t fam = UINT32_MAX;
   for (uint32_t i = 0; i < nq; i++)
      if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { fam = i; break; }
   REQ(fam != UINT32_MAX, "no graphics queue");
   free(qf);

   VkXcbSurfaceCreateInfoKHR sci = {.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR, .connection = c, .window = win};
   VkSurfaceKHR surf;
   CK(CreateXcbSurfaceKHR(inst, &sci, NULL, &surf));
   VkBool32 sup = 0;
   CK(GetPhysicalDeviceSurfaceSupportKHR(pd, fam, surf, &sup));
   REQ(sup, "queue family does not present to the xcb surface");

   VkSurfaceCapabilitiesKHR caps;
   CK(GetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));
   uint32_t nf = 0;
   CK(GetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, NULL));
   VkSurfaceFormatKHR *fmts = calloc(nf + 1, sizeof *fmts);
   CK(GetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, fmts));
   REQ(nf >= 1, "no surface formats");
   VkSurfaceFormatKHR fmt = fmts[0];
   for (uint32_t i = 0; i < nf; i++)
      if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) fmt = fmts[i];
   free(fmts);

   uint32_t npm = 0;
   CK(GetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &npm, NULL));
   VkPresentModeKHR *pms = calloc(npm + 1, sizeof *pms);
   CK(GetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &npm, pms));
   REQ(npm >= 1, "no surface present modes");
   int have_fifo = 0;
   for (uint32_t i = 0; i < npm; i++) have_fifo |= pms[i] == VK_PRESENT_MODE_FIFO_KHR;
   free(pms);
   REQ(have_fifo, "surface does not list VK_PRESENT_MODE_FIFO_KHR");
   VkPresentModeKHR mode_a = VK_PRESENT_MODE_FIFO_KHR, mode_b = VK_PRESENT_MODE_FIFO_KHR;

   /* Companion VK_EXT_surface_maintenance1: per-present-mode capabilities must accept the struct and keep mode_a. */
   if (!has_sm1 || !has_gsc2 || !GetPhysicalDeviceSurfaceCapabilities2KHR) {
      printf("skipped: no VK_EXT_surface_maintenance1 + VK_KHR_get_surface_capabilities2 for the per-mode queries\n");
      return 77;
   }
   VkSurfacePresentModeEXT spm = {.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT, .presentMode = mode_a};
   VkPhysicalDeviceSurfaceInfo2KHR sinfo = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
                                            .surface = surf, .pNext = &spm};
   VkSurfaceCapabilities2KHR caps2 = {.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
   CK(GetPhysicalDeviceSurfaceCapabilities2KHR(pd, &sinfo, &caps2));
   printf("surface maintenance1: capabilities2 for FIFO ok (minImageCount %u)\n", caps2.surfaceCapabilities.minImageCount);

   /* pPresentModes must be a subset of the modes the surface reports as compatible with presentMode (mode_a). */
   VkPresentModeKHR compat[8];
   VkSurfacePresentModeCompatibilityEXT pmc = {.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_EXT,
                                               .presentModeCount = 8, .pPresentModes = compat};
   VkSurfaceCapabilities2KHR compat_caps = {.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR, .pNext = &pmc};
   CK(GetPhysicalDeviceSurfaceCapabilities2KHR(pd, &sinfo, &compat_caps));
   REQ(pmc.presentModeCount >= 1 && pmc.presentModeCount <= 8, "compatibility query returned %u modes",
       pmc.presentModeCount);
   int fifo_self = 0;
   for (uint32_t i = 0; i < pmc.presentModeCount; i++) {
      fifo_self |= compat[i] == mode_a;
      if (compat[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode_b = VK_PRESENT_MODE_MAILBOX_KHR;
      else if (compat[i] == VK_PRESENT_MODE_IMMEDIATE_KHR && mode_b == VK_PRESENT_MODE_FIFO_KHR)
         mode_b = VK_PRESENT_MODE_IMMEDIATE_KHR;
   }
   /* The queried mode must list itself as compatible with itself: a valid list that omits it is a driver
    * defect, not a skip. Only a list with no alternate mode is a legitimate skip. */
   REQ(fifo_self, "compatibility query for mode %d omitted the queried mode itself", (int)mode_a);
   if (mode_b == mode_a) {
      printf("skipped: FIFO reports no alternate compatible present mode, cannot test a per-present switch\n");
      return 77;
   }
   printf("surface maintenance1: FIFO compatible with %u mode(s), switching to %d\n", pmc.presentModeCount, (int)mode_b);

   /* Ask only for scaling/gravity the surface reports for mode_a; 0 means "driver default" and is always valid. */
   VkSurfacePresentScalingCapabilitiesEXT psc = {.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_EXT};
   VkSurfaceCapabilities2KHR scaling_caps = {.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR, .pNext = &psc};
   CK(GetPhysicalDeviceSurfaceCapabilities2KHR(pd, &sinfo, &scaling_caps));
   VkPresentScalingFlagsEXT scale_bits = 0;
   if (psc.supportedPresentScaling & VK_PRESENT_SCALING_ONE_TO_ONE_BIT_EXT)
      scale_bits = VK_PRESENT_SCALING_ONE_TO_ONE_BIT_EXT;
   VkPresentGravityFlagsEXT grav_x = 0, grav_y = 0;
   if (scale_bits && (psc.supportedPresentGravityX & VK_PRESENT_GRAVITY_MIN_BIT_EXT))
      grav_x = VK_PRESENT_GRAVITY_MIN_BIT_EXT;
   if (grav_x && (psc.supportedPresentGravityY & VK_PRESENT_GRAVITY_MIN_BIT_EXT))
      grav_y = VK_PRESENT_GRAVITY_MIN_BIT_EXT;
   if (!scale_bits)
      printf("surface maintenance1: no ONE_TO_ONE scaling for FIFO, requesting scaling/gravity 0\n");

   const char *dexts[] = {"VK_KHR_swapchain", "VK_EXT_swapchain_maintenance1", "VK_EXT_image_drm_format_modifier"};
   float prio = 1.0f;
   VkDevice dev;
   CK(CreateDevice(pd, &(VkDeviceCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &sm1,
                                             .queueCreateInfoCount = 1,
                                             .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
                                                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                .queueFamilyIndex = fam, .queueCount = 1, .pQueuePriorities = &prio},
                                             .enabledExtensionCount = 3, .ppEnabledExtensionNames = dexts}, NULL, &dev));
#define DPFN(n) PFN_vk##n n = (PFN_vk##n)GetDeviceProcAddr(dev, "vk" #n)
   DPFN(GetDeviceQueue); DPFN(CreateSwapchainKHR); DPFN(GetSwapchainImagesKHR); DPFN(AcquireNextImageKHR);
   DPFN(QueuePresentKHR); DPFN(DestroySwapchainKHR); DPFN(CreateSemaphore); DPFN(DestroySemaphore);
   DPFN(CreateFence); DPFN(WaitForFences); DPFN(ResetFences); DPFN(QueueWaitIdle); DPFN(DeviceWaitIdle);
   DPFN(DestroyFence); DPFN(DestroyDevice);
   DPFN(QueueSubmit); DPFN(CreateCommandPool); DPFN(AllocateCommandBuffers);
   DPFN(BeginCommandBuffer); DPFN(EndCommandBuffer); DPFN(CmdPipelineBarrier); DPFN(DestroyCommandPool);
   DPFN(ReleaseSwapchainImagesEXT);
   DPFN(CreateImage); DPFN(DestroyImage); DPFN(GetImageMemoryRequirements2); DPFN(GetImageSubresourceLayout);
   DPFN(AllocateMemory); DPFN(BindImageMemory2); DPFN(FreeMemory); DPFN(GetImageDrmFormatModifierPropertiesEXT);
   VkQueue q;
   GetDeviceQueue(dev, fam, 0, &q);

   if (nv12_modifier_image(dev, pd, mp, CreateImage, DestroyImage, GetImageMemoryRequirements2,
                           GetImageSubresourceLayout, AllocateMemory, BindImageMemory2, FreeMemory,
                           GetImageDrmFormatModifierPropertiesEXT))
      return 1;

   /* 2. create with both maintenance1 create-time structs chained. */
   VkPresentModeKHR create_modes[2] = {mode_a, mode_b};
   VkSwapchainPresentModesCreateInfoEXT pmi_create = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT,
      .presentModeCount = 2, .pPresentModes = create_modes};
   VkSwapchainPresentScalingCreateInfoEXT scaling = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT,
      .scalingBehavior = scale_bits,
      .presentGravityX = grav_x, .presentGravityY = grav_y};
   pmi_create.pNext = &scaling;
   VkExtent2D ext = caps.currentExtent;
   if (ext.width == 0xFFFFFFFFu) ext = (VkExtent2D){W, H};
   uint32_t want = caps.minImageCount + 1;
   if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
   VkSwapchainKHR sc;
   VkResult cr = CreateSwapchainKHR(dev, &(VkSwapchainCreateInfoKHR){
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .pNext = &pmi_create, .surface = surf,
      .minImageCount = want, .imageFormat = fmt.format, .imageColorSpace = fmt.colorSpace, .imageExtent = ext,
      .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = mode_a, .clipped = VK_TRUE}, NULL, &sc);
   REQ(cr == VK_SUCCESS, "CreateSwapchainKHR with PresentModes/PresentScaling chained = %d", (int)cr);
   printf("swapchain: created with maintenance1 structs chained (modes %d,%d)\n", (int)mode_a, (int)mode_b);

   uint32_t ni = 0;
   CK(GetSwapchainImagesKHR(dev, sc, &ni, NULL));
   REQ(ni >= want, "fewer swapchain images than requested");
   VkImage imgs[16];
   if (ni > 16) ni = 16;
   CK(GetSwapchainImagesKHR(dev, sc, &ni, imgs));

   VkSemaphoreCreateInfo semci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   VkSemaphore acq, render_done;
   CK(CreateSemaphore(dev, &semci, NULL, &acq));
   CK(CreateSemaphore(dev, &semci, NULL, &render_done));
   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence present_fence, acq_fence;
   CK(CreateFence(dev, &fci, NULL, &present_fence));
   CK(CreateFence(dev, &fci, NULL, &acq_fence));
   VkCommandPool cp;
   CK(CreateCommandPool(dev, &(VkCommandPoolCreateInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                                        .queueFamilyIndex = fam}, NULL, &cp));
   VkCommandBuffer cb[FRAMES];
   CK(AllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){
          .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = FRAMES}, cb));

   /* Per-present mode switch + present fence: the fence must signal, i.e. the maintenance1 present path ran.
    * Each frame submits a barrier to PRESENT_SRC that waits on `acq` (consuming the acquire signal before the
    * semaphore is reused) and signals `render_done`, which the present waits on. */
   for (int f = 0; f < FRAMES; f++) {
      uint32_t idx;
      /* VK_SUBOPTIMAL_KHR is still a successful acquisition: the image is usable for this frame. */
      VkResult ar = AcquireNextImageKHR(dev, sc, UINT64_MAX, acq, VK_NULL_HANDLE, &idx);
      CK(ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR ? VK_SUCCESS : ar);
      REQ(idx < ni, "acquired image index %u outside the %u images the test got", idx, ni);
      CK(BeginCommandBuffer(cb[f], &(VkCommandBufferBeginInfo){
             .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
             .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}));
      CmdPipelineBarrier(cb[f], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, NULL, 0, NULL, 1,
                         &(VkImageMemoryBarrier){
                            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                            .srcAccessMask = 0, .dstAccessMask = 0,
                            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                            .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = imgs[idx],
                            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0,
                                                 .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1}});
      CK(EndCommandBuffer(cb[f]));
      VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      CK(QueueSubmit(q, 1, &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                           .waitSemaphoreCount = 1, .pWaitSemaphores = &acq,
                                           .pWaitDstStageMask = &wait_stage,
                                           .commandBufferCount = 1, .pCommandBuffers = &cb[f],
                                           .signalSemaphoreCount = 1, .pSignalSemaphores = &render_done},
                     VK_NULL_HANDLE));
      VkSwapchainPresentModeInfoKHR pmi = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR,
                                           .swapchainCount = 1, .pPresentModes = &mode_b};
      VkSwapchainPresentFenceInfoEXT pfi = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
                                             .swapchainCount = 1, .pFences = &present_fence};
      pmi.pNext = &pfi;
      VkResult pres = VK_SUCCESS;
      VkResult pr = QueuePresentKHR(q, &(VkPresentInfoKHR){.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = &pmi,
                                                           .waitSemaphoreCount = 1, .pWaitSemaphores = &render_done,
                                                           .swapchainCount = 1, .pSwapchains = &sc, .pImageIndices = &idx,
                                                           .pResults = &pres});
      REQ(pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR, "present frame %d with PresentModeInfo = %d", f, (int)pr);
      REQ(pres == VK_SUCCESS || pres == VK_SUBOPTIMAL_KHR, "present frame %d per-swapchain result = %d", f, (int)pres);
      VkResult wf = WaitForFences(dev, 1, &present_fence, VK_TRUE, 2000ull * 1000 * 1000);
      REQ(wf == VK_SUCCESS, "present fence did not signal for frame %d (%d)", f, (int)wf);
      CK(ResetFences(dev, 1, &present_fence));
      printf("frame %d: image %u presented with mode %d, fence signalled\n", f, idx, (int)mode_b);
   }

   /* vkReleaseSwapchainImagesEXT: acquire, then hand the image back without presenting. Acquire with a fence so
    * no semaphore signal is left pending (releasing the image does not consume an acquisition semaphore). */
   uint32_t ridx;
   VkResult ar2 = AcquireNextImageKHR(dev, sc, UINT64_MAX, VK_NULL_HANDLE, acq_fence, &ridx);
   CK(ar2 == VK_SUCCESS || ar2 == VK_SUBOPTIMAL_KHR ? VK_SUCCESS : ar2);
   CK(WaitForFences(dev, 1, &acq_fence, VK_TRUE, 2000ull * 1000 * 1000));
   CK(ResetFences(dev, 1, &acq_fence));
   CK(ReleaseSwapchainImagesEXT(dev, &(VkReleaseSwapchainImagesInfoKHR){
          .sType = VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_KHR, .swapchain = sc,
          .imageIndexCount = 1, .pImageIndices = &ridx}));
   printf("release: image %u released\n", ridx);

   CK(QueueWaitIdle(q));
   DeviceWaitIdle(dev);
   DestroyCommandPool(dev, cp, NULL);
   DestroyFence(dev, present_fence, NULL);
   DestroyFence(dev, acq_fence, NULL);
   DestroySemaphore(dev, render_done, NULL);
   DestroySemaphore(dev, acq, NULL);
   DestroySwapchainKHR(dev, sc, NULL);
   DestroyDevice(dev, NULL);
   DestroySurfaceKHR(inst, surf, NULL);
   DestroyInstance(inst, NULL);
   xcb_destroy_window(c, win);
   xcb_disconnect(c);
   printf("PASS\n");
   return 0;
}
