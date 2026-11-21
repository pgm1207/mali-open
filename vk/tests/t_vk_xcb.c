/* SPDX-License-Identifier: MIT */
/* X11 WSI test (STEAM-GAPS.md "X11 WSI"): xcb window -> VK_KHR_xcb_surface -> swapchain -> acquire -> present, 3 frames.
 *   t_vk_xcb [--direct <icd.so>] [--pixels] [--draw|--textured|--depth|--blend [--spv-dir d]] [--expect-name <substr>]
 *   Each draw mode renders a full-screen quad into the *swapchain image* with vkCmdBeginRendering + vkCmdDraw (the path
 *   HL2 takes: dynamic rendering, loadOp CLEAR, storeOp STORE) instead of vkCmdClearColorImage, and with --pixels the
 *   window must read back that mode's colour. Together they bisect a client that draws nothing, one path at a time:
 *   --draw      solid fragment colour, no texture, no depth, no blend      -> red   (BGR 0,0,255)
 *   --textured  sample a 2x2 magenta texture in the fragment shader        -> magenta (BGR 255,0,255)
 *   --depth     D32 attachment cleared to 1.0, depth test+write, quad z=0.5 -> red   (BGR 0,0,255)
 *   --blend     red 50% alpha over a blue clear (srcAlpha/1-srcAlpha)      -> purple (BGR 128,0,128)
 *   SPIR-V comes from --spv-dir (default "shader"): quad.vert.spv + solid.frag.spv, wsitex.frag.spv (--textured),
 *   wsiblend.frag.spv (--blend).
 * Needs an X server ($DISPLAY). Exit 0 = pass, 77 = skipped (no X, or the ICD does not advertise VK_KHR_xcb_surface: Mesa configured
 * without -Dplatforms=x11), 1 = fail.
 *   --pixels  read the window back with xcb_get_image and require the clear colour (hardware only: the fake backend executes
 *             nothing, so its images stay zero).
 * Without --direct the Vulkan loader is used (VK_DRIVER_FILES selects the ICD). */
#define _GNU_SOURCE
#define VK_USE_PLATFORM_XCB_KHR
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include <xcb/xcb.h>

#define W 128
#define H 96
#define FRAMES 3

#define CK(x)                                                                                    \
   do {                                                                                          \
      VkResult r_ = (x);                                                                         \
      if (r_ != VK_SUCCESS) {                                                                    \
         fprintf(stderr, "FAIL %s:%d %s = %d\n", __FILE__, __LINE__, #x, (int)r_);               \
         return 1;                                                                               \
      }                                                                                          \
   } while (0)
#define REQ(c, m)                                                                                \
   do {                                                                                          \
      if (!(c)) {                                                                                \
         fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, m);                              \
         return 1;                                                                               \
      }                                                                                          \
   } while (0)

static PFN_vkGetInstanceProcAddr gipa;

/* A memory type index for (typeBits, required property bits), -1 if the device has none. */
static int
mem_type(VkPhysicalDevice pd, uint32_t type_bits, VkMemoryPropertyFlags want, PFN_vkGetPhysicalDeviceMemoryProperties gpmp)
{
   VkPhysicalDeviceMemoryProperties mp;
   gpmp(pd, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount && i < 32; i++)
      if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
         return (int)i;
   return -1;
}

/* A SPIR-V file from --spv-dir (the tests' shader directory; the driver itself never reads files). */
static void *
read_spv(const char *dir, const char *name, size_t *sz)
{
   char path[512];
   snprintf(path, sizeof path, "%s/%s", dir, name);
   FILE *f = fopen(path, "rb");
   if (!f) { fprintf(stderr, "FAIL open %s: %s\n", path, strerror(errno)); return NULL; }
   if (fseek(f, 0, SEEK_END) || (*sz = (size_t)ftell(f)) == 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
   void *p = malloc(*sz);
   if (!p || fread(p, 1, *sz, f) != *sz) { fclose(f); free(p); return NULL; }
   fclose(f);
   return p;
}

#define IPFN(i, n) PFN_vk##n n = (PFN_vk##n)gipa(i, "vk" #n)

int
main(int argc, char **argv)
{
   const char *direct = NULL, *expect = NULL, *spv_dir = "shader";
   int pixels = 0, draw = 0, textured = 0, depth = 0, blend = 0;
   unsigned win_w = W, win_h = H;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--size") && i + 1 < argc) {
         /* The window size is the swapchain size on X11 (caps.currentExtent is the window). Small windows are what
          * Steam creates (32x32); 800x600 and 33x17 cover an ordinary and an odd size. */
         if (sscanf(argv[++i], "%ux%u", &win_w, &win_h) != 2 || !win_w || !win_h) {
            fprintf(stderr, "usage: --size WxH\n");
            return 2;
         }
      } else if (!strcmp(argv[i], "--direct") && i + 1 < argc) direct = argv[++i];
      else if (!strcmp(argv[i], "--pixels")) pixels = 1;
      else if (!strcmp(argv[i], "--draw")) draw = 1;
      else if (!strcmp(argv[i], "--textured")) textured = 1;
      else if (!strcmp(argv[i], "--depth")) depth = 1;
      else if (!strcmp(argv[i], "--blend")) blend = 1;
      else if (!strcmp(argv[i], "--spv-dir") && i + 1 < argc) spv_dir = argv[++i];
      else if (!strcmp(argv[i], "--expect-name") && i + 1 < argc) expect = argv[++i];
      else { fprintf(stderr, "usage: t_vk_xcb [--direct icd.so] [--size WxH] [--pixels] [--draw|--textured|--depth|--blend] "
                            "[--spv-dir d] [--expect-name s]\n"); return 2; }
   }

   void *lib = direct ? dlopen(direct, RTLD_NOW | RTLD_LOCAL) : dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
   if (!lib) { fprintf(stderr, "FAIL dlopen: %s\n", dlerror()); return 1; }
   gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, direct ? "vk_icdGetInstanceProcAddr" : "vkGetInstanceProcAddr");
   REQ(gipa, "no GetInstanceProcAddr");

   int scr;
   xcb_connection_t *c = xcb_connect(NULL, &scr);
   if (xcb_connection_has_error(c)) { printf("skipped: no X server ($DISPLAY)\n"); return 77; }

   IPFN(NULL, EnumerateInstanceExtensionProperties);
   uint32_t n = 0;
   CK(EnumerateInstanceExtensionProperties(NULL, &n, NULL));
   VkExtensionProperties *ep = calloc(n + 1, sizeof *ep);
   CK(EnumerateInstanceExtensionProperties(NULL, &n, ep));
   int has_surf = 0, has_xcb = 0;
   for (uint32_t i = 0; i < n; i++) {
      has_surf |= !strcmp(ep[i].extensionName, "VK_KHR_surface");
      has_xcb |= !strcmp(ep[i].extensionName, "VK_KHR_xcb_surface");
   }
   free(ep);
   if (!has_surf || !has_xcb) { printf("skipped: ICD does not advertise VK_KHR_xcb_surface\n"); return 77; }

   const xcb_setup_t *setup = xcb_get_setup(c);
   xcb_screen_iterator_t sit = xcb_setup_roots_iterator(setup);
   for (int i = 0; i < scr; i++) xcb_screen_next(&sit);
   xcb_screen_t *s = sit.data;
   xcb_window_t win = xcb_generate_id(c);
   uint32_t vals[2] = {s->black_pixel, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY};
   xcb_create_window(c, XCB_COPY_FROM_PARENT, win, s->root, 0, 0, win_w, win_h, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                     s->root_visual,
                     XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, vals);
   xcb_map_window(c, win);
   xcb_flush(c);

   const char *iexts[] = {"VK_KHR_surface", "VK_KHR_xcb_surface"};
   VkInstance inst;
   CK(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(
      &(VkInstanceCreateInfo){.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                              .pApplicationInfo = &(VkApplicationInfo){.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                                                       .pApplicationName = "t_vk_xcb",
                                                                       .apiVersion = VK_API_VERSION_1_3},
                              .enabledExtensionCount = 2,
                              .ppEnabledExtensionNames = iexts},
      NULL, &inst));
   IPFN(inst, EnumeratePhysicalDevices);
   IPFN(inst, GetPhysicalDeviceProperties);
   IPFN(inst, GetPhysicalDeviceQueueFamilyProperties);
   IPFN(inst, GetPhysicalDeviceMemoryProperties);
   IPFN(inst, CreateXcbSurfaceKHR);
   IPFN(inst, GetPhysicalDeviceSurfaceSupportKHR);
   IPFN(inst, GetPhysicalDeviceSurfaceCapabilitiesKHR);
   IPFN(inst, GetPhysicalDeviceSurfaceFormatsKHR);
   IPFN(inst, GetPhysicalDeviceSurfacePresentModesKHR);
   IPFN(inst, GetPhysicalDeviceXcbPresentationSupportKHR);
   IPFN(inst, DestroySurfaceKHR);
   IPFN(inst, CreateDevice);
   IPFN(inst, GetDeviceProcAddr);
   IPFN(inst, DestroyInstance);

   uint32_t np = 1;
   VkPhysicalDevice pd;
   VkResult r = EnumeratePhysicalDevices(inst, &np, &pd);
   REQ((r == VK_SUCCESS || r == VK_INCOMPLETE) && np >= 1, "no physical device");
   VkPhysicalDeviceProperties props;
   GetPhysicalDeviceProperties(pd, &props);
   printf("device: %s\n", props.deviceName);
   if (expect) REQ(strstr(props.deviceName, expect), "device name mismatch");

   uint32_t nq = 0;
   GetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
   VkQueueFamilyProperties *qf = calloc(nq, sizeof *qf);
   GetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
   uint32_t fam = ~0u;
   for (uint32_t i = 0; i < nq; i++)
      if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { fam = i; break; }
   REQ(fam != ~0u, "no graphics queue");
   free(qf);

   VkSurfaceKHR surf;
   CK(CreateXcbSurfaceKHR(inst, &(VkXcbSurfaceCreateInfoKHR){.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
                                                             .connection = c, .window = win}, NULL, &surf));
   VkBool32 sup = VK_FALSE;
   CK(GetPhysicalDeviceSurfaceSupportKHR(pd, fam, surf, &sup));
   REQ(sup, "surface not supported by the graphics queue");
   REQ(GetPhysicalDeviceXcbPresentationSupportKHR(pd, fam, c, s->root_visual), "xcb presentation support is false");

   VkSurfaceCapabilitiesKHR caps;
   CK(GetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));
   printf("caps: images %u..%u extent %ux%u usage 0x%x\n", caps.minImageCount, caps.maxImageCount, caps.currentExtent.width,
          caps.currentExtent.height, caps.supportedUsageFlags);
   REQ(caps.minImageCount >= 1, "minImageCount 0");
   REQ(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT, "no TRANSFER_DST usage");
   REQ(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, "no COLOR_ATTACHMENT usage");

   uint32_t nf = 0;
   CK(GetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, NULL));
   REQ(nf > 0, "no surface formats");
   VkSurfaceFormatKHR *fm = calloc(nf, sizeof *fm);
   CK(GetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, fm));
   VkSurfaceFormatKHR fmt = fm[0];
   for (uint32_t i = 0; i < nf; i++)
      if (fm[i].format == VK_FORMAT_B8G8R8A8_UNORM) fmt = fm[i];
   free(fm);
   uint32_t nm = 0;
   CK(GetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &nm, NULL));
   VkPresentModeKHR modes[8];
   if (nm > 8) nm = 8;
   CK(GetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &nm, modes));
   int fifo = 0;
   for (uint32_t i = 0; i < nm; i++) fifo |= modes[i] == VK_PRESENT_MODE_FIFO_KHR;
   REQ(fifo, "FIFO present mode missing");
   printf("format %d, %u present modes\n", (int)fmt.format, nm);

   const char *dexts[] = {"VK_KHR_swapchain"};
   float prio = 1.0f;
   /* --draw: the dynamic-rendering path (core 1.3 here, same as the zink/HL2 command streams). */
   VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                           .dynamicRendering = (draw || textured || depth || blend) ? VK_TRUE : VK_FALSE};
   VkDevice dev;
   CK(CreateDevice(pd, &(VkDeviceCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13,
                                             .queueCreateInfoCount = 1,
                                             .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
                                                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                .queueFamilyIndex = fam, .queueCount = 1, .pQueuePriorities = &prio},
                                             .enabledExtensionCount = 1, .ppEnabledExtensionNames = dexts}, NULL, &dev));
#define DPFN(n) PFN_vk##n n = (PFN_vk##n)GetDeviceProcAddr(dev, "vk" #n)
   DPFN(GetDeviceQueue); DPFN(CreateSwapchainKHR); DPFN(GetSwapchainImagesKHR); DPFN(AcquireNextImageKHR);
   DPFN(QueuePresentKHR); DPFN(DestroySwapchainKHR); DPFN(CreateCommandPool); DPFN(AllocateCommandBuffers);
   DPFN(BeginCommandBuffer); DPFN(EndCommandBuffer); DPFN(CmdPipelineBarrier); DPFN(CmdClearColorImage);
   DPFN(CreateSemaphore); DPFN(CreateFence); DPFN(WaitForFences); DPFN(ResetFences); DPFN(QueueSubmit);
   DPFN(QueueWaitIdle); DPFN(DeviceWaitIdle); DPFN(DestroyDevice); DPFN(ResetCommandBuffer);
   DPFN(DestroySemaphore); DPFN(DestroyFence); DPFN(DestroyCommandPool);
   DPFN(CreateShaderModule); DPFN(DestroyShaderModule); DPFN(CreatePipelineLayout); DPFN(DestroyPipelineLayout);
   DPFN(CreateGraphicsPipelines); DPFN(DestroyPipeline); DPFN(CreateImageView); DPFN(DestroyImageView);
   DPFN(CmdBeginRendering); DPFN(CmdEndRendering); DPFN(CmdBindPipeline); DPFN(CmdSetViewport); DPFN(CmdSetScissor);
   DPFN(CmdPushConstants); DPFN(CmdDraw); DPFN(CmdBindDescriptorSets);
   DPFN(CreateImage); DPFN(DestroyImage); DPFN(BindImageMemory); DPFN(GetImageMemoryRequirements);
   DPFN(GetImageSubresourceLayout); DPFN(AllocateMemory); DPFN(FreeMemory); DPFN(MapMemory); DPFN(UnmapMemory);
   DPFN(CreateSampler); DPFN(DestroySampler);
   DPFN(CreateDescriptorSetLayout); DPFN(DestroyDescriptorSetLayout); DPFN(CreateDescriptorPool);
   DPFN(DestroyDescriptorPool); DPFN(AllocateDescriptorSets); DPFN(UpdateDescriptorSets);
   VkQueue q;
   GetDeviceQueue(dev, fam, 0, &q);

   VkExtent2D ext = caps.currentExtent;
   if (ext.width == 0xFFFFFFFFu) ext = (VkExtent2D){win_w, win_h};
   uint32_t want = caps.minImageCount + 1;
   if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
   VkSwapchainKHR sc;
   CK(CreateSwapchainKHR(dev, &(VkSwapchainCreateInfoKHR){.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                                          .surface = surf, .minImageCount = want,
                                                          .imageFormat = fmt.format, .imageColorSpace = fmt.colorSpace,
                                                          .imageExtent = ext, .imageArrayLayers = 1,
                                                          .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                                          .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                                          .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                                          .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                                          .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE}, NULL, &sc));
   uint32_t ni = 0;
   CK(GetSwapchainImagesKHR(dev, sc, &ni, NULL));
   REQ(ni >= want, "fewer swapchain images than requested");
   VkImage imgs[16];
   if (ni > 16) ni = 16;
   CK(GetSwapchainImagesKHR(dev, sc, &ni, imgs));
   printf("swapchain: %u images %ux%u\n", ni, ext.width, ext.height);

   /* Draw modes: one 2D colour view per swapchain image + quad.vert (full-screen quad from gl_VertexIndex, push constant
    * s = (1,1,0,0) so uv = q) with a per-mode fragment shader. --textured adds a 2x2 magenta texture + sampler,
    * --depth a D32 attachment, --blend a srcAlpha/1-srcAlpha blend over a blue clear. */
   const int draw_like = draw || textured || depth || blend;
   VkImageView views[16] = {0};
   VkPipeline pipe = VK_NULL_HANDLE;
   VkPipelineLayout play = VK_NULL_HANDLE;
   VkDescriptorSet dset = VK_NULL_HANDLE;
   VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
   VkDescriptorPool dpool = VK_NULL_HANDLE;
   VkImage tex = VK_NULL_HANDLE, dimg = VK_NULL_HANDLE;
   VkImageView texview = VK_NULL_HANDLE, dview = VK_NULL_HANDLE;
   VkSampler samp = VK_NULL_HANDLE;
   VkDeviceMemory texmem = VK_NULL_HANDLE, dmem = VK_NULL_HANDLE;
   if (draw_like) {
      for (uint32_t i = 0; i < ni; i++)
         CK(CreateImageView(dev, &(VkImageViewCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = imgs[i],
                                                          .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt.format,
                                                          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}, NULL, &views[i]));
      if (textured) {
         /* 2x2 R8G8B8A8 magenta, LINEAR tiling in host-visible memory: the sampler must return exactly (1,0,1). */
         CK(CreateImage(dev, &(VkImageCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                                  .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {2, 2, 1}, .mipLevels = 1,
                                                  .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                                  .tiling = VK_IMAGE_TILING_LINEAR, .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
                                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED}, NULL, &tex));
         VkMemoryRequirements mr;
         GetImageMemoryRequirements(dev, tex, &mr);
         const int mt = mem_type(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, GetPhysicalDeviceMemoryProperties);
         REQ(mt >= 0, "no host-visible memory type for the texture");
         CK(AllocateMemory(dev, &(VkMemoryAllocateInfo){.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                        .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt},
                          NULL, &texmem));
         VkSubresourceLayout sl = {0};
         GetImageSubresourceLayout(dev, tex, &(VkImageSubresource){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0}, &sl);
         void *tp = NULL;
         CK(MapMemory(dev, texmem, 0, VK_WHOLE_SIZE, 0, &tp));
         for (unsigned y = 0; y < 2; y++)
            for (unsigned x = 0; x < 2; x++)
               memcpy((uint8_t *)tp + sl.offset + y * sl.rowPitch + x * 4, (const uint8_t [4]){255, 0, 255, 255}, 4);
         UnmapMemory(dev, texmem);
         CK(BindImageMemory(dev, tex, texmem, 0));
         CK(CreateImageView(dev, &(VkImageViewCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = tex,
                                                          .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                                          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}, NULL, &texview));
         CK(CreateSampler(dev, &(VkSamplerCreateInfo){.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                                      .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                                                      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                                      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                                      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                                      .maxLod = 0.0f}, NULL, &samp));
         CK(CreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
               .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1,
               .pBindings = &(VkDescriptorSetLayoutBinding){.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                            .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT}},
             NULL, &dsl));
         CK(CreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                                    .maxSets = 1, .poolSizeCount = 1,
                                                                    .pPoolSizes = &(VkDescriptorPoolSize){
                                                                       .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                                       .descriptorCount = 1}}, NULL, &dpool));
         CK(AllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                                      .descriptorPool = dpool, .descriptorSetCount = 1,
                                                                      .pSetLayouts = &dsl}, &dset));
         UpdateDescriptorSets(dev, 1, &(VkWriteDescriptorSet){
                                 .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0,
                                 .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                 .pImageInfo = &(VkDescriptorImageInfo){
                                    .sampler = samp, .imageView = texview,
                                    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}, 0, NULL);
      }
      if (depth) {
         CK(CreateImage(dev, &(VkImageCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                                  .format = VK_FORMAT_D32_SFLOAT, .extent = {ext.width, ext.height, 1},
                                                  .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                                  .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED}, NULL, &dimg));
         VkMemoryRequirements mr;
         GetImageMemoryRequirements(dev, dimg, &mr);
         const int mt = mem_type(pd, mr.memoryTypeBits, 0, GetPhysicalDeviceMemoryProperties);
         REQ(mt >= 0, "no memory type for the depth attachment");
         CK(AllocateMemory(dev, &(VkMemoryAllocateInfo){.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                        .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt},
                          NULL, &dmem));
         CK(BindImageMemory(dev, dimg, dmem, 0));
         CK(CreateImageView(dev, &(VkImageViewCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = dimg,
                                                          .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT,
                                                          .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}}, NULL, &dview));
      }
      const char *fname = textured ? "wsitex.frag.spv" : blend ? "wsiblend.frag.spv" : "solid.frag.spv";
      size_t vsz = 0, fsz = 0;
      void *vcode = read_spv(spv_dir, "quad.vert.spv", &vsz), *fcode = read_spv(spv_dir, fname, &fsz);
      REQ(vcode && fcode, "SPIR-V not found (--spv-dir)");
      VkShaderModule vm, fm;
      CK(CreateShaderModule(dev, &(VkShaderModuleCreateInfo){.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                             .codeSize = vsz, .pCode = vcode}, NULL, &vm));
      CK(CreateShaderModule(dev, &(VkShaderModuleCreateInfo){.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                             .codeSize = fsz, .pCode = fcode}, NULL, &fm));
      free(vcode); free(fcode);
      CK(CreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                                 .setLayoutCount = textured ? 1 : 0, .pSetLayouts = &dsl,
                                                                 .pushConstantRangeCount = 1,
                                                                 .pPushConstantRanges = &(VkPushConstantRange){
                                                                    .stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .size = 16}},
                             NULL, &play));
      VkPipelineShaderStageCreateInfo st[2] = {
         {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vm, .pName = "main"},
         {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = fm, .pName = "main"},
      };
      VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
      if (blend) {   /* classic alpha blend: the fragment shader emits red at alpha 0.5 */
         cba.blendEnable = VK_TRUE;
         cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
         cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
         cba.colorBlendOp = VK_BLEND_OP_ADD;
         cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
         cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
         cba.alphaBlendOp = VK_BLEND_OP_ADD;
      }
      VkPipelineDepthStencilStateCreateInfo dss = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                   .depthTestEnable = depth ? VK_TRUE : VK_FALSE,
                                                   .depthWriteEnable = depth ? VK_TRUE : VK_FALSE,
                                                   .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL};
      VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
      VkFormat dfmt = VK_FORMAT_D32_SFLOAT;
      VkGraphicsPipelineCreateInfo gpci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
         .stageCount = 2, .pStages = st,
         .pVertexInputState = &(VkPipelineVertexInputStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO},
         .pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                                         .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST},
         .pViewportState = &(VkPipelineViewportStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                               .viewportCount = 1, .scissorCount = 1},
         .pRasterizationState = &(VkPipelineRasterizationStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                                          .polygonMode = VK_POLYGON_MODE_FILL,
                                                                          .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                                                                          .lineWidth = 1.0f},
         .pMultisampleState = &(VkPipelineMultisampleStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                                      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT},
         .pDepthStencilState = &dss,
         .pColorBlendState = &(VkPipelineColorBlendStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                                    .attachmentCount = 1, .pAttachments = &cba},
         .pDynamicState = &(VkPipelineDynamicStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                              .dynamicStateCount = 2, .pDynamicStates = dyn},
         .layout = play,
         .pNext = &(VkPipelineRenderingCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                                   .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt.format,
                                                   .depthAttachmentFormat = depth ? dfmt : VK_FORMAT_UNDEFINED},
         .renderPass = VK_NULL_HANDLE};
      CK(CreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));
      DestroyShaderModule(dev, vm, NULL);
      DestroyShaderModule(dev, fm, NULL);
      printf("mode: %s (full-screen quad into the swapchain image, dynamic rendering, loadOp CLEAR, STORE)\n",
             textured ? "textured 2x2 magenta" : depth ? "depth-tested, D32 clear 1.0" :
             blend ? "alpha blend over a blue clear" : "solid red");
   }

   VkCommandPool pool;
   CK(CreateCommandPool(dev, &(VkCommandPoolCreateInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                                        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                                        .queueFamilyIndex = fam}, NULL, &pool));
   VkCommandBuffer cb;
   CK(AllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                                 .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                                 .commandBufferCount = 1}, &cb));
   VkSemaphore acq, done;
   VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   CK(CreateSemaphore(dev, &sci, NULL, &acq));
   CK(CreateSemaphore(dev, &sci, NULL, &done));
   VkFence fence;
   CK(CreateFence(dev, &(VkFenceCreateInfo){.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}, NULL, &fence));

   const VkClearColorValue clr = {.float32 = {0.2f, 0.4f, 0.8f, 1.0f}};   /* BGRA8: 51,102,204 */
   for (int f = 0; f < FRAMES; f++) {
      uint32_t idx;
      CK(AcquireNextImageKHR(dev, sc, UINT64_MAX, acq, VK_NULL_HANDLE, &idx));
      REQ(idx < ni, "acquired index out of range");
      CK(ResetCommandBuffer(cb, 0));
      CK(BeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                                            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}));
      VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      if (draw_like) {
         VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                   .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = imgs[idx], .subresourceRange = rng};
         CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b);
         /* blend: a blue field the half-alpha red quad is blended onto; every other mode: black, so a lost draw
          * reads back as 0,0,0 rather than as the clear. */
         VkClearValue clearv = {.color = {.float32 = {0.0f, 0.0f, blend ? 1.0f : 0.0f, 1.0f}}};
         VkRenderingAttachmentInfo ra = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = views[idx],
                                        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                        .clearValue = clearv};
         VkRenderingAttachmentInfo da = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dview,
                                        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                        .clearValue = {.depthStencil = {1.0f, 0}}};
         VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                               .renderArea = {{0, 0}, {ext.width, ext.height}}, .layerCount = 1,
                               .colorAttachmentCount = 1, .pColorAttachments = &ra,
                               .pDepthAttachment = depth ? &da : NULL};
         CmdBeginRendering(cb, &ri);
         CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
         if (textured)
            CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, play, 0, 1, &dset, 0, NULL);
         VkViewport vp = {0.0f, 0.0f, (float)ext.width, (float)ext.height, 0.0f, 1.0f};
         VkRect2D sr = {{0, 0}, {ext.width, ext.height}};
         CmdSetViewport(cb, 0, 1, &vp);
         CmdSetScissor(cb, 0, 1, &sr);
         CmdPushConstants(cb, play, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, (const float [4]){1.0f, 1.0f, 0.0f, 0.0f});
         CmdDraw(cb, 6, 1, 0, 0);
         CmdEndRendering(cb);
         b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
         b.dstAccessMask = 0;
         b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
         b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
         CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
      } else {
      VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = imgs[idx], .subresourceRange = rng};
      CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
      CmdClearColorImage(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clr, 1, &rng);
      b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      b.dstAccessMask = 0;
      b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
      }
      CK(EndCommandBuffer(cb));
      VkPipelineStageFlags ws = draw_like ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT;
      CK(QueueSubmit(q, 1, &(VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                                           .pWaitSemaphores = &acq, .pWaitDstStageMask = &ws, .commandBufferCount = 1,
                                           .pCommandBuffers = &cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &done},
                     fence));
      VkResult pres = VK_SUCCESS;
      r = QueuePresentKHR(q, &(VkPresentInfoKHR){.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                                 .pWaitSemaphores = &done, .swapchainCount = 1, .pSwapchains = &sc,
                                                 .pImageIndices = &idx, .pResults = &pres});
      if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { fprintf(stderr, "FAIL present frame %d = %d\n", f, (int)r); return 1; }
      REQ(pres == VK_SUCCESS || pres == VK_SUBOPTIMAL_KHR, "per-swapchain present result");
      CK(WaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
      CK(ResetFences(dev, 1, &fence));
      printf("frame %d: image %u acquired, cleared, presented\n", f, idx);
   }
   CK(QueueWaitIdle(q));
   xcb_flush(c);

   int rc = 0;
   if (pixels) {
      /* the present may complete asynchronously (Present extension / the copy of the software path): give it a few round trips */
      uint8_t px[4] = {0};
      int ok = 0;
      for (int t = 0; t < 50 && !ok; t++) {
         xcb_get_image_reply_t *im = xcb_get_image_reply(
            c, xcb_get_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, win, ext.width / 2, ext.height / 2, 1, 1, ~0u), NULL);
         if (im && xcb_get_image_data_length(im) >= 3) {
            memcpy(px, xcb_get_image_data(im), 3);
            /* X pixel = B,G,R. --textured: magenta (255,0,255); --blend: 50% red over 50% blue (128,0,128);
             * --depth / --draw: opaque red (0,0,255); the clear path: 0.2,0.4,0.8 (204,102,51). */
            if (textured)
               ok = px[0] >= 253 && px[1] <= 2 && px[2] >= 253;
            else if (blend)
               ok = abs(px[0] - 128) <= 8 && px[1] <= 8 && abs(px[2] - 128) <= 8;
            else if (draw_like)
               ok = px[0] <= 2 && px[1] <= 2 && px[2] >= 253;
            else
               ok = abs(px[0] - 204) <= 2 && abs(px[1] - 102) <= 2 && abs(px[2] - 51) <= 2;
         }
         free(im);
         if (!ok) {
            struct timespec ts = {0, 20 * 1000 * 1000};
            nanosleep(&ts, NULL);
         }
      }
      printf("window pixel BGR = %u,%u,%u (want %s): %s\n", px[0], px[1], px[2],
             textured ? "255,0,255 (textured quad)" : blend ? "128,0,128 (blended quad)" :
             depth ? "0,0,255 (depth-tested quad)" : draw ? "0,0,255 (the drawn quad)" : "204,102,51 (the clear)",
             ok ? "ok" : "MISMATCH");
      if (!ok) rc = 1;
   }

   DeviceWaitIdle(dev);
   if (draw_like) {
      DestroyPipeline(dev, pipe, NULL);
      DestroyPipelineLayout(dev, play, NULL);
      for (uint32_t i = 0; i < ni; i++)
         DestroyImageView(dev, views[i], NULL);
      if (textured) {
         DestroyDescriptorPool(dev, dpool, NULL);
         DestroyDescriptorSetLayout(dev, dsl, NULL);
         DestroySampler(dev, samp, NULL);
         DestroyImageView(dev, texview, NULL);
         DestroyImage(dev, tex, NULL);
         FreeMemory(dev, texmem, NULL);
      }
      if (depth) {
         DestroyImageView(dev, dview, NULL);
         DestroyImage(dev, dimg, NULL);
         FreeMemory(dev, dmem, NULL);
      }
   }
   DestroyFence(dev, fence, NULL);
   DestroySemaphore(dev, acq, NULL);
   DestroySemaphore(dev, done, NULL);
   DestroyCommandPool(dev, pool, NULL);
   DestroySwapchainKHR(dev, sc, NULL);
   DestroyDevice(dev, NULL);
   DestroySurfaceKHR(inst, surf, NULL);
   DestroyInstance(inst, NULL);
   xcb_destroy_window(c, win);
   xcb_disconnect(c);
   printf(rc ? "FAIL\n" : "PASS\n");
   return rc;
}
