/* SPDX-License-Identifier: MIT */
/* VkDeviceMemory / instance edge cases on the fake backend (mali_memory.c, mali_device.c, mali_instance.c error paths).
 *   t_vk_mem --direct <icd.so>
 * Checks: a layer-name extension query is LAYER_NOT_PRESENT; map twice and map past the end fail with MEMORY_MAP_FAILED and the
 * failed second map does not unmap the first; map after unmap works and offset maps are offset from the base; heap 0's size is a
 * size this driver grants (an allocation of exactly it succeeds) and the VK_EXT_memory_budget answers add up to it
 * (heapUsage + heapBudget == heap, maxMemoryAllocationSize <= heap); the cached memory
 * type accepts flush/invalidate (explicit size, WHOLE_SIZE, unaligned range); an out-of-range memory type and an oversized
 * allocation are OUT_OF_DEVICE_MEMORY; the 4096-allocation cap returns TOO_MANY_OBJECTS and frees make room again;
 * vkGetDeviceMemoryCommitment is 0 (no lazily allocated type); the opaque capture address queries are 0 (no
 * bufferDeviceAddressCaptureReplay). Exit 0 = pass. */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...)                                                                              \
   do {                                                                                            \
      if (!(c)) {                                                                                  \
         printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #c);                                       \
         printf(__VA_ARGS__);                                                                      \
         printf("\n");                                                                             \
         g_fail = 1;                                                                               \
      }                                                                                            \
   } while (0)

#define MAXALLOC 4096 /* MALI_MAX_ALLOCS */

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   for (int i = 1; i < argc; i++)
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
   if (!icd) { printf("usage: t_vk_mem --direct <icd.so>\n"); return 2; }
   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   if (!gipa) { printf("no vk_icdGetInstanceProcAddr\n"); return 1; }

   PFN_vkEnumerateInstanceExtensionProperties EIEP =
      (PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL, "vkEnumerateInstanceExtensionProperties");
   uint32_t cnt = 0;
   CHECK(EIEP("VK_LAYER_does_not_exist", &cnt, NULL) == VK_ERROR_LAYER_NOT_PRESENT, "layer query");
   CHECK(EIEP(NULL, &cnt, NULL) == VK_SUCCESS, "plain instance ext query");

   VkInstance inst;
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   if (CreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("FAIL CreateInstance\n"); return 1; }
#define I(n) PFN_vk##n n = (PFN_vk##n)gipa(inst, "vk" #n)
   I(EnumeratePhysicalDevices); I(GetPhysicalDeviceMemoryProperties); I(CreateDevice); I(GetDeviceProcAddr); I(DestroyInstance);
   I(GetPhysicalDeviceExternalBufferProperties);
   VkPhysicalDevice pd; uint32_t n = 1;
   if (EnumeratePhysicalDevices(inst, &n, &pd) != VK_SUCCESS || n != 1) { printf("FAIL enumerate\n"); return 1; }
   VkPhysicalDeviceMemoryProperties mp;
   GetPhysicalDeviceMemoryProperties(pd, &mp);
   int t_coh = -1, t_cached = -1;
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
      VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
      if ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) && t_coh < 0) t_coh = i;
      if ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) && t_cached < 0) t_cached = i;
   }
   CHECK(t_coh >= 0 && t_cached >= 0, "memory types coherent=%d cached=%d", t_coh, t_cached);
   if (t_coh < 0 || t_cached < 0) return 1;

   /* ---- heap 0 is a size this driver will commit; the budget is the total the process may use ---- */
   /* (zink checks a request against memoryHeaps[].size and then calls vkAllocateMemory: a heap that is bigger than
    * mem_reserve()'s cap turns that into "zink: couldn't allocate memory: heap=0 size=...".) */
   PFN_vkGetPhysicalDeviceMemoryProperties2 GDPMP2 =
      (PFN_vkGetPhysicalDeviceMemoryProperties2)gipa(inst, "vkGetPhysicalDeviceMemoryProperties2");
   PFN_vkGetPhysicalDeviceProperties2 GDPP2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2");
   VkPhysicalDeviceMemoryBudgetPropertiesEXT bud = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
   VkPhysicalDeviceMemoryProperties2 mp2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &bud};
   GDPMP2(pd, &mp2);
   const uint64_t heap = mp2.memoryProperties.memoryHeaps[0].size;
   VkPhysicalDeviceMaintenance3Properties m3 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
   VkPhysicalDeviceProperties2 props2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &m3};
   GDPP2(pd, &props2);
   CHECK(heap > 0, "heap 0 size %llu", (unsigned long long)heap);
   CHECK(m3.maxMemoryAllocationSize <= heap,
         "maxMemoryAllocationSize %llu must fit the heap 0 it is reported with (%llu)",
         (unsigned long long)m3.maxMemoryAllocationSize, (unsigned long long)heap);
   CHECK(bud.heapUsage[0] == 0 && bud.heapBudget[0] == heap,
         "fresh heap: usage %llu budget %llu, want 0/%llu", (unsigned long long)bud.heapUsage[0],
         (unsigned long long)bud.heapBudget[0], (unsigned long long)heap);

   /* VK_KHR_external_memory_fd: the import/export paths take OPAQUE_FD and DMA_BUF as the same kbase dma-buf, so the
    * capability query must say so for both. A client that queries first (zink for gamescope, ANGLE) otherwise reads
    * "not exportable" and skips the path the driver would have accepted. */
   for (int h = 0; h < 2; h++) {
      VkExternalMemoryHandleTypeFlagBits ht = h ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT
                                                : VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      VkPhysicalDeviceExternalBufferInfo bi = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
                                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .handleType = ht};
      VkExternalBufferProperties bo = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
      GetPhysicalDeviceExternalBufferProperties(pd, &bi, &bo);
      const VkExternalMemoryHandleTypeFlags ours = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT |
                                                   VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
      CHECK((bo.externalMemoryProperties.externalMemoryFeatures &
             (VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) ==
            (VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT),
            "handle type 0x%x is importable+exportable (features 0x%x)", (unsigned)ht,
            (unsigned)bo.externalMemoryProperties.externalMemoryFeatures);
      CHECK(bo.externalMemoryProperties.compatibleHandleTypes == ours,
            "handle type 0x%x: compatible types 0x%x, want DMA_BUF|OPAQUE_FD 0x%x", (unsigned)ht,
            (unsigned)bo.externalMemoryProperties.compatibleHandleTypes, (unsigned)ours);
   }

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
   VkDevice dev;
   if (CreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) { printf("FAIL CreateDevice\n"); return 1; }
#define D(n) PFN_vk##n n = (PFN_vk##n)GetDeviceProcAddr(dev, "vk" #n)
   D(AllocateMemory); D(FreeMemory); D(MapMemory); D(UnmapMemory); D(FlushMappedMemoryRanges); D(InvalidateMappedMemoryRanges);
   D(GetDeviceMemoryCommitment); D(GetDeviceMemoryOpaqueCaptureAddress); D(DestroyDevice);

   /* ---- map state machine ---- */
   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 65536, .memoryTypeIndex = t_coh};
   VkDeviceMemory m;
   CHECK(AllocateMemory(dev, &mai, NULL, &m) == VK_SUCCESS, "alloc coherent");
   uint8_t *p = NULL, *q = NULL;
   CHECK(MapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, (void **)&p) == VK_SUCCESS && p, "map");
   CHECK(MapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, (void **)&q) == VK_ERROR_MEMORY_MAP_FAILED, "second map must fail");
   if (g_fail || !p) { printf("FAIL: alloc/map failed, remaining map tests need the mapping\n"); return 1; }   /* p[] below would dereference NULL */
   p[0] = 0x5a; p[65535] = 0xa5;
   /* VK_EXT_memory_budget: the client subtracts usage from the budget, so the budget stays the total heap and
    * what is left is budget - usage (a budget of heap - usage would double-count the live 64 KiB). */
   bud.heapUsage[0] = 0;
   bud.heapBudget[0] = 0;
   mp2.pNext = &bud;
   GDPMP2(pd, &mp2);
   CHECK(bud.heapUsage[0] >= 65536, "heapUsage %llu must count the live 64 KiB", (unsigned long long)bud.heapUsage[0]);
   CHECK(bud.heapUsage[0] <= heap && bud.heapBudget[0] == heap && bud.heapBudget[0] > bud.heapUsage[0],
         "heap 0 %llu: budget %llu must be the total and stay above usage %llu (left = %llu)",
         (unsigned long long)heap, (unsigned long long)bud.heapBudget[0], (unsigned long long)bud.heapUsage[0],
         (unsigned long long)(bud.heapBudget[0] - bud.heapUsage[0]));
   UnmapMemory(dev, m);
   CHECK(MapMemory(dev, m, 65536, VK_WHOLE_SIZE, 0, (void **)&q) == VK_ERROR_MEMORY_MAP_FAILED, "offset == size must fail");
   CHECK(MapMemory(dev, m, 4096, 4096, 0, (void **)&q) == VK_SUCCESS && q == p + 4096, "offset map is base+offset (%p vs %p)", (void *)q, (void *)p);
   UnmapMemory(dev, m);
   CHECK(MapMemory(dev, m, 0, 16, 0, (void **)&q) == VK_SUCCESS && q == p && q[0] == 0x5a && q[65535] == 0xa5,
         "map after unmap keeps contents");
   UnmapMemory(dev, m);
   CHECK(MapMemory(dev, VK_NULL_HANDLE, 0, 16, 0, (void **)&q) == VK_SUCCESS && q == NULL, "map of VK_NULL_HANDLE gives NULL");
   uint64_t committed = 123;
   GetDeviceMemoryCommitment(dev, m, &committed);
   CHECK(committed == 0, "commitment %llu", (unsigned long long)committed);
   VkDeviceMemoryOpaqueCaptureAddressInfo oci = {.sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_OPAQUE_CAPTURE_ADDRESS_INFO, .memory = m};
   CHECK(GetDeviceMemoryOpaqueCaptureAddress(dev, &oci) == 0, "opaque capture address");
   FreeMemory(dev, m, NULL);

   /* ---- cached type: flush/invalidate are legal and keep the data ---- */
   mai.memoryTypeIndex = t_cached;
   VkDeviceMemory c;
   CHECK(AllocateMemory(dev, &mai, NULL, &c) == VK_SUCCESS, "alloc cached");
   CHECK(MapMemory(dev, c, 0, VK_WHOLE_SIZE, 0, (void **)&p) == VK_SUCCESS, "map cached");
   memset(p, 0x77, 65536);
   VkMappedMemoryRange r[3] = {
      {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = c, .offset = 0, .size = 128},
      {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = c, .offset = 64, .size = VK_WHOLE_SIZE},
      {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = c, .offset = 65536 - 64, .size = 64},
   };
   CHECK(FlushMappedMemoryRanges(dev, 3, r) == VK_SUCCESS, "flush");
   CHECK(InvalidateMappedMemoryRanges(dev, 3, r) == VK_SUCCESS, "invalidate");
   CHECK(p[0] == 0x77 && p[65535] == 0x77, "data after flush/invalidate");
   UnmapMemory(dev, c);
   FreeMemory(dev, c, NULL);

   /* ---- allocation limits ---- */
   VkDeviceMemory bad = VK_NULL_HANDLE;
   mai.memoryTypeIndex = 2; /* MALI_MEM_TYPE_COUNT */
   CHECK(AllocateMemory(dev, &mai, NULL, &bad) == VK_ERROR_OUT_OF_DEVICE_MEMORY, "memory type out of range");
   mai.memoryTypeIndex = t_coh; mai.allocationSize = mp.memoryHeaps[0].size + 1;
   CHECK(AllocateMemory(dev, &mai, NULL, &bad) == VK_ERROR_OUT_OF_DEVICE_MEMORY, "alloc larger than the heap");
   /* the other half of the promise: a single allocation of exactly the reported heap size is granted (a heap bigger
    * than mem_reserve()'s cap would answer OUT_OF_DEVICE_MEMORY here, which is zink's "couldn't allocate memory") */
   mai.allocationSize = heap;
   VkDeviceMemory whole = VK_NULL_HANDLE;
   CHECK(AllocateMemory(dev, &mai, NULL, &whole) == VK_SUCCESS,
         "an allocation of heap 0's size (%llu) must be granted", (unsigned long long)heap);
   if (whole) FreeMemory(dev, whole, NULL);

   mai.allocationSize = 64;
   VkDeviceMemory *all = calloc(MAXALLOC + 1, sizeof *all);
   int got = 0;
   for (; got < MAXALLOC + 1; got++) {
      VkResult res = AllocateMemory(dev, &mai, NULL, &all[got]);
      if (res != VK_SUCCESS) { CHECK(res == VK_ERROR_TOO_MANY_OBJECTS, "cap result %d", res); break; }
   }
   CHECK(got == MAXALLOC, "allocation cap at %d, want %d", got, MAXALLOC);
   if (got > 0) {
      FreeMemory(dev, all[--got], NULL);
      VkDeviceMemory again;
      CHECK(AllocateMemory(dev, &mai, NULL, &again) == VK_SUCCESS, "a free makes room under the cap");
      if (again) all[got++] = again;
   }
   for (int i = 0; i < got; i++) FreeMemory(dev, all[i], NULL);
   free(all);

   DestroyDevice(dev, NULL);
   DestroyInstance(inst, NULL);
   printf(g_fail ? "FAILED\n" : "OK\n");
   return g_fail;
}
