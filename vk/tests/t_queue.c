/* SPDX-License-Identifier: MIT */
/* M4 test: memory (map/write/read, cache maintenance, dma-buf export/import), fences, binary and
 * timeline semaphores, sync_file export/import, many empty submits, device loss. Loader-free: dlopens
 * the ICD like t_icd --direct. One JSON line per case; exit 0 = all pass.
 *
 *   t_queue --direct <icd.so> [--submits N] [--burst N] [--only mem|dmabuf|submits|binary|timeline|syncfile|fault|idle|teardown]
 *   fault mode needs MALI_FAULT_INJECT=n in the environment (n-th submission fails).
 */
#define _DEFAULT_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dirent.h>
#include <dlfcn.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

static int g_fail;
static const char *g_only;
static int g_burst = 300; /* --burst N: unwaited submits of the idle case */

#define CASE_ON(n) (!g_only || !strcmp(g_only, n))
#define CHECK(c, ...)                                                                              \
   do {                                                                                            \
      if (!(c)) {                                                                                  \
         printf("{\"fail\":\"%s:%d\",\"expr\":\"%s\",\"detail\":\"", __FILE__, __LINE__, #c);     \
         printf(__VA_ARGS__);                                                                      \
         printf("\"}\n");                                                                          \
         g_fail = 1;                                                                               \
         return 0;                                                                                 \
      }                                                                                            \
   } while (0)

static int
count_fds(void)
{
   int n = 0;
   DIR *d = opendir("/proc/self/fd");
   if (!d)
      return -1;
   while (readdir(d))
      n++;
   closedir(d);
   return n;
}

static double
now_us(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

#define DEV_FNS(X)                                                                                 \
   X(DestroyDevice) X(GetDeviceQueue) X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory)  \
   X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges) X(CreateFence) X(DestroyFence)       \
   X(ResetFences) X(WaitForFences) X(GetFenceStatus) X(CreateSemaphore) X(DestroySemaphore)        \
   X(QueueSubmit) X(QueueWaitIdle) X(DeviceWaitIdle) X(GetSemaphoreCounterValueKHR)                   \
   X(WaitSemaphoresKHR) X(SignalSemaphoreKHR) X(GetMemoryFdKHR) X(GetSemaphoreFdKHR)                     \
   X(ImportSemaphoreFdKHR) X(GetFenceFdKHR) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) \
   X(BindBufferMemory) X(GetMemoryFdPropertiesKHR) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(GetImageSubresourceLayout) X(CreateCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdFillBuffer) X(CmdCopyBuffer)

#define DECL(n) static PFN_vk##n n;
DEV_FNS(DECL)
static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memprops;

/* ------------------------------------------------------------------ memory */
static int
test_mem(int type, uint64_t size)
{
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = size,
                              .memoryTypeIndex = type};
   VkDeviceMemory m;
   CHECK(AllocateMemory(dev, &ai, NULL, &m) == VK_SUCCESS, "alloc");
   uint8_t *p = NULL;
   CHECK(MapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, (void **)&p) == VK_SUCCESS && p, "map");
   void *p2;
   CHECK(MapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, &p2) == VK_ERROR_MEMORY_MAP_FAILED, "double map");
   int cached = !!(memprops.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
   VkMappedMemoryRange rg = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = m, .offset = 0,
                             .size = VK_WHOLE_SIZE};
   /* Vulkan does not promise zeroed memory, but applications rely on it and kbase recycles cached
    * pages uncleared, so the driver zeroes them: both types must read zero (second allocations of
    * this test get the pages the first ones dirtied). */
   uint64_t nz = 0;
   if (cached)
      InvalidateMappedMemoryRanges(dev, 1, &rg);
   for (uint64_t i = 0; i < size; i++)
      nz += p[i] != 0;
   CHECK(nz == 0, "%s memory not zeroed (%llu bytes, first %u)", cached ? "cached" : "uncached",
         (unsigned long long)nz, p[0]);
   for (int round = 0; round < 4; round++) {
      for (uint64_t i = 0; i < size; i++)
         p[i] = (uint8_t)(i * 7 + round);
      if (cached) {
         CHECK(FlushMappedMemoryRanges(dev, 1, &rg) == VK_SUCCESS, "flush");
         CHECK(InvalidateMappedMemoryRanges(dev, 1, &rg) == VK_SUCCESS, "invalidate");
      }
      for (uint64_t i = 0; i < size; i++)
         CHECK(p[i] == (uint8_t)(i * 7 + round), "readback %llu round %d", (unsigned long long)i, round);
   }
   if (cached) { /* partial, unaligned range */
      VkMappedMemoryRange r2 = rg;
      r2.offset = 64;
      r2.size = 128;
      CHECK(FlushMappedMemoryRanges(dev, 1, &r2) == VK_SUCCESS, "flush partial");
      CHECK(InvalidateMappedMemoryRanges(dev, 1, &r2) == VK_SUCCESS, "inval partial");
   }
   UnmapMemory(dev, m);
   CHECK(MapMemory(dev, m, 64, 64, 0, (void **)&p2) == VK_SUCCESS && p2 == p + 64, "remap offset");
   UnmapMemory(dev, m);
   FreeMemory(dev, m, NULL);
   printf("{\"test\":\"mem\",\"type\":%d,\"size\":%llu,\"cached\":%d,\"ok\":true}\n", type,
          (unsigned long long)size, cached);
   return 1;
}

static int
test_many_allocs(void)
{
   VkDeviceMemory m[256];
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 65536};
   for (int i = 0; i < 256; i++) {
      ai.memoryTypeIndex = i & 1;
      CHECK(AllocateMemory(dev, &ai, NULL, &m[i]) == VK_SUCCESS, "alloc %d", i);
   }
   for (int i = 0; i < 256; i++)
      FreeMemory(dev, m[i], NULL);
   VkMemoryAllocateInfo huge = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = memprops.memoryHeaps[0].size * 2, .memoryTypeIndex = 0};
   VkDeviceMemory h;
   CHECK(AllocateMemory(dev, &huge, NULL, &h) == VK_ERROR_OUT_OF_DEVICE_MEMORY, "huge alloc");
   printf("{\"test\":\"many_allocs\",\"ok\":true}\n");
   return 1;
}

/* One run per handle type: DMA_BUF and OPAQUE_FD must both export a fresh, valid descriptor that can be imported
 * back into a second allocation and sees the same bytes (gamescope's zink asks for OPAQUE_FD). */
static int
test_dmabuf_one(int type, VkExternalMemoryHandleTypeFlagBits ht, const char *name)
{
   VkExportMemoryAllocateInfo ex = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .handleTypes = ht};
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ex,
                              .allocationSize = 1 << 20, .memoryTypeIndex = type};
   VkDeviceMemory a, b;
   CHECK(AllocateMemory(dev, &ai, NULL, &a) == VK_SUCCESS, "alloc exportable (%s)", name);
   VkMemoryGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = a, .handleType = ht};
   int fd = -1;
   CHECK(GetMemoryFdKHR(dev, &gi, &fd) == VK_SUCCESS && fd >= 0, "get fd (%s)", name);
   struct stat st;
   CHECK(fstat(fd, &st) == 0, "exported fd is not a valid descriptor (%s)", name);
   VkImportMemoryFdInfoKHR im = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                 .handleType = ht, .fd = fd};
   VkMemoryAllocateInfo bi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &im,
                              .allocationSize = 1 << 20, .memoryTypeIndex = type};
   CHECK(AllocateMemory(dev, &bi, NULL, &b) == VK_SUCCESS, "import");
   uint32_t *pa, *pb;
   CHECK(MapMemory(dev, a, 0, VK_WHOLE_SIZE, 0, (void **)&pa) == VK_SUCCESS, "map a");
   CHECK(MapMemory(dev, b, 0, VK_WHOLE_SIZE, 0, (void **)&pb) == VK_SUCCESS, "map b");
   VkMappedMemoryRange ra = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = a, .size = VK_WHOLE_SIZE};
   VkMappedMemoryRange rb = ra;
   rb.memory = b;
   for (int i = 0; i < (1 << 18); i++)
      pa[i] = 0xC0DE0000u + i;
   FlushMappedMemoryRanges(dev, 1, &ra);
   InvalidateMappedMemoryRanges(dev, 1, &rb);
   for (int i = 0; i < (1 << 18); i++)
      CHECK(pb[i] == 0xC0DE0000u + i, "shared contents differ at %d", i);
   for (int i = 0; i < (1 << 18); i++)
      pb[i] = ~pa[i];
   FlushMappedMemoryRanges(dev, 1, &rb);
   InvalidateMappedMemoryRanges(dev, 1, &ra);
   for (int i = 0; i < (1 << 18); i++)
      CHECK(pa[i] == ~(0xC0DE0000u + i), "reverse differ at %d", i);
   FreeMemory(dev, b, NULL);
   FreeMemory(dev, a, NULL);
   /* On success the import took ownership of fd (the driver closes it with the BO): closing it here would
    * close a descriptor the driver still owns. It is only closed on the failure paths above. */
   printf("{\"test\":\"dmabuf\",\"type\":%d,\"handle\":\"%s\",\"ok\":true}\n", type, name);
   return 1;
}

static int
test_dmabuf(int type)
{
   return test_dmabuf_one(type, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, "dma_buf") &
          test_dmabuf_one(type, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, "opaque_fd");
}

/* ------------------------------------------------------------------ sync */
static int
test_submits(int n)
{
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence f;
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "create fence");
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
   double mn = 1e18, mx = 0, sum = 0;
   int fds0 = count_fds();
   for (int i = 0; i < n; i++) {
      double t0 = now_us();
      VkResult r = QueueSubmit(queue, 1, &si, f);
      CHECK(r == VK_SUCCESS, "submit %d = %d", i, r);
      r = WaitForFences(dev, 1, &f, VK_TRUE, 5000000000ull);
      CHECK(r == VK_SUCCESS, "wait %d = %d (lost fence)", i, r);
      CHECK(GetFenceStatus(dev, f) == VK_SUCCESS, "status %d", i);
      CHECK(ResetFences(dev, 1, &f) == VK_SUCCESS, "reset");
      double dt = now_us() - t0;
      mn = dt < mn ? dt : mn;
      mx = dt > mx ? dt : mx;
      sum += dt;
   }
   CHECK(QueueWaitIdle(queue) == VK_SUCCESS, "idle");
   int fds1 = count_fds();
   CHECK(fds1 <= fds0 + 2, "fd leak %d -> %d", fds0, fds1);
   DestroyFence(dev, f, NULL);
   printf("{\"test\":\"submits\",\"n\":%d,\"lost_fences\":0,\"min_us\":%.1f,\"mean_us\":%.1f,\"max_us\":%.1f,"
          "\"fds\":[%d,%d],\"ok\":true}\n", n, mn, sum / n, mx, fds0, fds1);
   return 1;
}

static int
test_idle(void)
{
   /* many submits without waiting (more than the 64-entry ring), then idle */
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
   for (int i = 0; i < g_burst; i++)
      CHECK(QueueSubmit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS, "submit %d", i);
   CHECK(QueueWaitIdle(queue) == VK_SUCCESS, "queue idle");
   CHECK(DeviceWaitIdle(dev) == VK_SUCCESS, "device idle");
   printf("{\"test\":\"idle\",\"n\":%d,\"ok\":true}\n", g_burst);
   return 1;
}

/* Device teardown with a delayed fence still pending (MALI_FAKE_SLOW_US=100000): the fake backend's timer must be stopped and joined in
 * DestroyDevice. main() then dlclose()s the ICD and waits past the delay: a stray backend thread would wake in unmapped code (SEGV). */
static int
test_teardown(void)
{
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence f;
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
   for (int i = 0; i < 4; i++)
      CHECK(QueueSubmit(queue, 1, &si, i == 3 ? f : VK_NULL_HANDLE) == VK_SUCCESS, "submit %d", i);
   DestroyFence(dev, f, NULL); /* not waited: the signal is still in flight */
   printf("{\"test\":\"teardown\",\"ok\":true}\n");
   return 1;
}

/* Poll latency (MALI_FAKE_SLOW_US=300000): a thread sits in a blocking wait on a pending fence (the fake backend, like kbase,
 * serialises waiters in 20 ms slices); vkGetFenceStatus and a timeout-0 vkWaitForFences on that pending fence must answer
 * NOT_READY/TIMEOUT at once instead of queueing behind the slice, and the later signal must still be seen. */
static void *
poll0_waiter(void *a)
{
   WaitForFences(dev, 1, (VkFence *)a, VK_TRUE, 5000000000ull);
   return NULL;
}

static int
test_poll0(void)
{
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence f;
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
   CHECK(QueueSubmit(queue, 1, &si, f) == VK_SUCCESS, "submit");
   pthread_t th;
   pthread_create(&th, NULL, poll0_waiter, &f);
   usleep(5000); /* the waiter is inside its first slice */
   double worst = 0;
   int pending = 0;
   for (int i = 0; i < 8; i++) {
      double t = now_us();
      VkResult a = GetFenceStatus(dev, f), b = WaitForFences(dev, 1, &f, VK_TRUE, 0);
      t = now_us() - t;
      if (a == VK_NOT_READY && b == VK_TIMEOUT)
         pending++;
      if (t > worst)
         worst = t;
      usleep(2000);
   }
   CHECK(pending >= 4, "fence was pending in only %d of 8 polls", pending);
   CHECK(worst < 8000, "timeout-0 poll took %.0f us behind a blocking waiter (limit 8000)", worst);
   pthread_join(th, NULL);
   CHECK(GetFenceStatus(dev, f) == VK_SUCCESS, "signalled fence seen by a poll");
   CHECK(WaitForFences(dev, 1, &f, VK_TRUE, 0) == VK_SUCCESS, "signalled fence, timeout 0");
   DestroyFence(dev, f, NULL);
   printf("{\"test\":\"poll0\",\"pending\":%d,\"worst_us\":%.0f,\"ok\":true}\n", pending, worst);
   return 1;
}

static int
test_binary(void)
{
   VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkSemaphore s1, s2;
   VkFence f;
   CHECK(CreateSemaphore(dev, &sci, NULL, &s1) == VK_SUCCESS, "sem");
   CHECK(CreateSemaphore(dev, &sci, NULL, &s2) == VK_SUCCESS, "sem");
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   for (int i = 0; i < 200; i++) {
      VkSubmitInfo a = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .signalSemaphoreCount = 1, .pSignalSemaphores = &s1};
      VkSubmitInfo b = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &s1,
                        .pWaitDstStageMask = &st, .signalSemaphoreCount = 1, .pSignalSemaphores = &s2};
      VkSubmitInfo c = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &s2,
                        .pWaitDstStageMask = &st};
      CHECK(QueueSubmit(queue, 1, &a, VK_NULL_HANDLE) == VK_SUCCESS, "a");
      CHECK(QueueSubmit(queue, 1, &b, VK_NULL_HANDLE) == VK_SUCCESS, "b");
      CHECK(QueueSubmit(queue, 1, &c, f) == VK_SUCCESS, "c");
      CHECK(WaitForFences(dev, 1, &f, VK_TRUE, 5000000000ull) == VK_SUCCESS, "wait %d", i);
      ResetFences(dev, 1, &f);
   }
   CHECK(QueueWaitIdle(queue) == VK_SUCCESS, "idle");
   DestroyFence(dev, f, NULL);
   DestroySemaphore(dev, s1, NULL);
   DestroySemaphore(dev, s2, NULL);
   printf("{\"test\":\"binary\",\"chains\":200,\"ok\":true}\n");
   return 1;
}

/* Present ordering. A = the frame (signals S and fence FA), P = the present (waits S, signals FP; wsi_common commits the
 * buffer right after P's vkQueueSubmit returns). MALI_FAKE_SLOW_US makes fences complete late. Mode "wait"
 * (MALI_PRESENT_FENCE=0, kernels without dma-buf sync_file import): the commit must not precede the frame's completion, so
 * FA is signalled when P returns. Mode "fence" (=1): P returns at once (pipelining) (the ordering of the fence that travels with the buffer is proven on hardware by --only gpufence). */
static int
test_order(int fence_mode)
{
   VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkSemaphore s;
   VkFence fa, fp;
   CHECK(CreateSemaphore(dev, &sci, NULL, &s) == VK_SUCCESS, "sem");
   CHECK(CreateFence(dev, &fi, NULL, &fa) == VK_SUCCESS, "fence");
   CHECK(CreateFence(dev, &fi, NULL, &fp) == VK_SUCCESS, "fence");
   VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   for (int i = 0; i < 5; i++) {
      VkSubmitInfo a = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .signalSemaphoreCount = 1, .pSignalSemaphores = &s};
      VkSubmitInfo p = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &s,
                        .pWaitDstStageMask = &st};
      CHECK(QueueSubmit(queue, 1, &a, fa) == VK_SUCCESS, "frame submit");
      CHECK(QueueSubmit(queue, 1, &p, fp) == VK_SUCCESS, "present submit");
      VkResult fa_done = GetFenceStatus(dev, fa);   /* the moment wsi_common would commit */
      if (fence_mode) {
         CHECK(fa_done == VK_NOT_READY, "iter %d: present returned only after the frame finished (no pipelining)", i);
         CHECK(WaitForFences(dev, 1, &fp, VK_TRUE, 5000000000ull) == VK_SUCCESS, "fp wait");
         /* "FP never completes before FA" is hardware ordering (the fake completes each submission on its own timer): see --only gpufence */
      } else {
         CHECK(fa_done == VK_SUCCESS, "iter %d: present returned before the frame finished (commit would show a half-drawn buffer)", i);
      }
      CHECK(WaitForFences(dev, 1, &fp, VK_TRUE, 5000000000ull) == VK_SUCCESS, "fp wait");
      ResetFences(dev, 1, &fa);
      ResetFences(dev, 1, &fp);
   }
   CHECK(QueueWaitIdle(queue) == VK_SUCCESS, "idle");
   DestroyFence(dev, fa, NULL);
   DestroyFence(dev, fp, NULL);
   DestroySemaphore(dev, s, NULL);
   printf("{\"test\":\"order\",\"fence_mode\":%d,\"ok\":true}\n", fence_mode);
   return 1;
}

/* Hardware ordering probe (real GPU, no fake): a heavy frame A (buffer copies, ~ms) signals S; the present P waits S and signals an
 * exportable semaphore whose sync_file is what wsi_common imports into the dma-buf for the compositor. The sync_file must not
 * signal before A's fence does: that is "the compositor cannot show a half-drawn frame". With MALI_PRESENT_FENCE=1 P returns without
 * a CPU wait, so the sync_file (not the CPU) is the only thing ordering the commit. Prints how often P returned while A still ran. */
static int
test_gpufence(void)
{
   VkExportSemaphoreCreateInfo ex = {.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
                                     .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
   VkSemaphoreCreateInfo sx = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ex};
   VkSemaphoreCreateInfo sp = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkSemaphore s, s2;
   VkFence fa;
   CHECK(CreateSemaphore(dev, &sp, NULL, &s) == VK_SUCCESS, "sem");
   CHECK(CreateSemaphore(dev, &sx, NULL, &s2) == VK_SUCCESS, "exportable sem");
   CHECK(CreateFence(dev, &fi, NULL, &fa) == VK_SUCCESS, "fence");
   const VkDeviceSize sz = 64u << 20;
   VkBuffer b[2];
   VkDeviceMemory m[2];
   for (int i = 0; i < 2; i++) {
      VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sz,
                               .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
      CHECK(CreateBuffer(dev, &bi, NULL, &b[i]) == VK_SUCCESS, "buffer");
      VkMemoryRequirements mr;
      GetBufferMemoryRequirements(dev, b[i], &mr);
      uint32_t t = 0;
      while (!(mr.memoryTypeBits & (1u << t))) t++;
      VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t};
      CHECK(AllocateMemory(dev, &ai, NULL, &m[i]) == VK_SUCCESS, "alloc");
      CHECK(BindBufferMemory(dev, b[i], m[i], 0) == VK_SUCCESS, "bind");
   }
   VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   VkCommandPool pool;
   CHECK(CreateCommandPool(dev, &pi, NULL, &pool) == VK_SUCCESS, "pool");
   VkCommandBufferAllocateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                     .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   VkCommandBuffer cb;
   CHECK(AllocateCommandBuffers(dev, &ci, &cb) == VK_SUCCESS, "cb");
   VkCommandBufferBeginInfo bgi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   CHECK(BeginCommandBuffer(cb, &bgi) == VK_SUCCESS, "begin");
   VkBufferCopy rg = {.size = sz};
   for (int k = 0; k < 4; k++) {
      CmdFillBuffer(cb, b[0], 0, sz, 0x01010101u * (k + 1));
      CmdCopyBuffer(cb, b[0], b[1], 1, &rg);
      CmdCopyBuffer(cb, b[1], b[0], 1, &rg);
   }
   CHECK(EndCommandBuffer(cb) == VK_SUCCESS, "end");
   VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   int n = 40, early_return = 0, viol = 0;
   double sum_gpu = 0;
   for (int i = 0; i < n; i++) {
      VkSubmitInfo a = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb,
                        .signalSemaphoreCount = 1, .pSignalSemaphores = &s};
      VkSubmitInfo p = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &s,
                        .pWaitDstStageMask = &st, .signalSemaphoreCount = 1, .pSignalSemaphores = &s2};
      double t0 = now_us();
      CHECK(QueueSubmit(queue, 1, &a, fa) == VK_SUCCESS, "frame submit");
      CHECK(QueueSubmit(queue, 1, &p, VK_NULL_HANDLE) == VK_SUCCESS, "present submit");
      int ret_early = GetFenceStatus(dev, fa) == VK_NOT_READY;
      early_return += ret_early;
      VkSemaphoreGetFdInfoKHR gs = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, .semaphore = s2,
                                    .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
      int fd = -2;
      CHECK(GetSemaphoreFdKHR(dev, &gs, &fd) == VK_SUCCESS, "present semaphore fd");
      if (fd >= 0) {
         struct pollfd pf = {fd, POLLIN, 0};
         CHECK(poll(&pf, 1, 10000) == 1, "present sync_file never signalled");
         VkResult fs = GetFenceStatus(dev, fa);   /* the sync_file is signalled: the frame must be finished */
         if (fs != VK_SUCCESS) viol++;
         CHECK(fs == VK_SUCCESS, "iter %d: present sync_file signalled while the frame was still running", i);
         close(fd);
      }
      sum_gpu += now_us() - t0;
      CHECK(WaitForFences(dev, 1, &fa, VK_TRUE, 10000000000ull) == VK_SUCCESS, "frame wait");
      ResetFences(dev, 1, &fa);
   }
   CHECK(QueueWaitIdle(queue) == VK_SUCCESS, "idle");
   printf("{\"test\":\"gpufence\",\"iters\":%d,\"present_returned_before_frame_done\":%d,\"violations\":%d,\"ms_per_frame\":%.2f,\"ok\":%s}\n",
          n, early_return, viol, sum_gpu / n / 1000, viol ? "false" : "true");
   return !viol;
}

struct pp {
   VkSemaphore t;
   int n;
   int err;
};

static void *
host_side(void *arg)
{
   struct pp *p = arg;
   for (int i = 0; i < p->n; i++) {
      uint64_t v = 2 * i + 1;
      VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                                .pSemaphores = &p->t, .pValues = &v};
      if (WaitSemaphoresKHR(dev, &wi, 5000000000ull) != VK_SUCCESS) { p->err = 1; return NULL; }
      VkSemaphoreSignalInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = p->t,
                                  .value = 2 * i + 2};
      if (SignalSemaphoreKHR(dev, &si) != VK_SUCCESS) { p->err = 2; return NULL; }
   }
   return NULL;
}

static int
test_timeline(int n)
{
   VkSemaphoreTypeCreateInfo ti = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                   .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
   VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti};
   struct pp p = {.n = n};
   CHECK(CreateSemaphore(dev, &sci, NULL, &p.t) == VK_SUCCESS, "timeline");
   pthread_t th;
   pthread_create(&th, NULL, host_side, &p);
   VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   double t0 = now_us();
   for (int i = 0; i < n; i++) {
      uint64_t wv = 2 * i, sv = 2 * i + 1;
      VkTimelineSemaphoreSubmitInfo tsi = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                           .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &wv,
                                           .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &sv};
      VkSubmitInfo s = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tsi, .waitSemaphoreCount = 1,
                        .pWaitSemaphores = &p.t, .pWaitDstStageMask = &st, .signalSemaphoreCount = 1,
                        .pSignalSemaphores = &p.t};
      VkResult r = QueueSubmit(queue, 1, &s, VK_NULL_HANDLE);
      if (r != VK_SUCCESS) {
         pthread_cancel(th);
         pthread_join(th, NULL);
      }
      CHECK(r == VK_SUCCESS, "submit %d = %d", i, r);
   }
   pthread_join(th, NULL);
   CHECK(p.err == 0, "host thread error %d", p.err);
   uint64_t v = 2 * n, got = 0;
   VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                             .pSemaphores = &p.t, .pValues = &v};
   CHECK(WaitSemaphoresKHR(dev, &wi, 5000000000ull) == VK_SUCCESS, "final wait");
   CHECK(GetSemaphoreCounterValueKHR(dev, p.t, &got) == VK_SUCCESS && got == v, "counter %llu", (unsigned long long)got);
   double dt = now_us() - t0;
   CHECK(QueueWaitIdle(queue) == VK_SUCCESS, "idle");
   DestroySemaphore(dev, p.t, NULL);
   printf("{\"test\":\"timeline\",\"pingpongs\":%d,\"us_per\":%.1f,\"ok\":true}\n", n, dt / n);
   return 1;
}

static int
test_syncfile(void)
{
   VkExportSemaphoreCreateInfo ex = {.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
                                     .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
   VkSemaphoreCreateInfo sx = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ex};
   VkSemaphoreCreateInfo sp = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   VkExportFenceCreateInfo fx = {.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
                                 .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
   VkFenceCreateInfo fie = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = &fx};
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkSemaphore a, b;
   VkFence f, fe;
   CHECK(CreateSemaphore(dev, &sx, NULL, &a) == VK_SUCCESS, "exportable sem");
   CHECK(CreateSemaphore(dev, &sp, NULL, &b) == VK_SUCCESS, "plain sem");
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   CHECK(CreateFence(dev, &fie, NULL, &fe) == VK_SUCCESS, "exportable fence");

   VkSubmitInfo s1 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .signalSemaphoreCount = 1, .pSignalSemaphores = &a};
   CHECK(QueueSubmit(queue, 1, &s1, fe) == VK_SUCCESS, "submit");
   CHECK(WaitForFences(dev, 1, &fe, VK_TRUE, 5000000000ull) == VK_SUCCESS, "wait");

   /* fence -> sync_file */
   VkFenceGetFdInfoKHR gf = {.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, .fence = fe,
                             .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
   int ffd = -2;
   CHECK(GetFenceFdKHR(dev, &gf, &ffd) == VK_SUCCESS, "fence fd");
   if (ffd >= 0) {
      struct pollfd pf = {ffd, POLLIN, 0};
      CHECK(poll(&pf, 1, 5000) == 1, "fence sync_file not signalled");
      close(ffd);
   }

   /* semaphore -> sync_file -> other semaphore -> wait in a submit */
   VkSemaphoreGetFdInfoKHR gs = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, .semaphore = a,
                                 .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
   int sfd = -2;
   CHECK(GetSemaphoreFdKHR(dev, &gs, &sfd) == VK_SUCCESS, "semaphore fd");
   VkImportSemaphoreFdInfoKHR is = {.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR, .semaphore = b,
                                    .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
                                    .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, .fd = sfd};
   CHECK(ImportSemaphoreFdKHR(dev, &is) == VK_SUCCESS, "import fd");
   VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   VkSubmitInfo s2 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &b,
                      .pWaitDstStageMask = &st};
   CHECK(QueueSubmit(queue, 1, &s2, f) == VK_SUCCESS, "submit wait imported");
   CHECK(WaitForFences(dev, 1, &f, VK_TRUE, 5000000000ull) == VK_SUCCESS, "wait imported");

   /* an already-signalled import (fd = -1) */
   VkImportSemaphoreFdInfoKHR is2 = is;
   is2.fd = -1;
   CHECK(ImportSemaphoreFdKHR(dev, &is2) == VK_SUCCESS, "import -1");
   ResetFences(dev, 1, &f);
   CHECK(QueueSubmit(queue, 1, &s2, f) == VK_SUCCESS, "submit wait -1");
   CHECK(WaitForFences(dev, 1, &f, VK_TRUE, 5000000000ull) == VK_SUCCESS, "wait -1");
   CHECK(QueueWaitIdle(queue) == VK_SUCCESS, "idle");

   DestroyFence(dev, f, NULL);
   DestroyFence(dev, fe, NULL);
   DestroySemaphore(dev, a, NULL);
   DestroySemaphore(dev, b, NULL);
   printf("{\"test\":\"syncfile\",\"ok\":true}\n");
   return 1;
}

/* ---- WSI audit regressions ------------------------------------------------------------------------ */
static PFN_vkBindImageMemory2KHR BindImageMemory2KHR_;   /* only loaded for --only wsiimg (needs VK_KHR_bind_memory2) */

/* memory fd validation: bad/non-exportable inputs are refused with the spec's error */
static int
test_wsifd(void)
{
   VkMemoryFdPropertiesKHR props = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
   CHECK(GetMemoryFdPropertiesKHR(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, -1, &props) ==
            VK_ERROR_INVALID_EXTERNAL_HANDLE, "properties of fd -1");
   int fds[2];
   CHECK(pipe(fds) == 0, "pipe");
   close(fds[1]);
   close(fds[0]);
   CHECK(GetMemoryFdPropertiesKHR(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fds[0], &props) ==
            VK_ERROR_INVALID_EXTERNAL_HANDLE, "properties of a closed fd");

   /* memory allocated WITHOUT export must not give out a dma-buf */
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 65536, .memoryTypeIndex = 0};
   VkDeviceMemory m;
   CHECK(AllocateMemory(dev, &ai, NULL, &m) == VK_SUCCESS, "alloc plain");
   VkMemoryGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = m,
                              .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
   int fd = -1;
   CHECK(GetMemoryFdKHR(dev, &gi, &fd) == VK_ERROR_INVALID_EXTERNAL_HANDLE, "export of non-exportable memory");
   FreeMemory(dev, m, NULL);

   /* import larger than the dma-buf behind it */
   VkExportMemoryAllocateInfo ex = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
   ai.pNext = &ex;
   CHECK(AllocateMemory(dev, &ai, NULL, &m) == VK_SUCCESS, "alloc exportable");
   gi.memory = m;
   CHECK(GetMemoryFdKHR(dev, &gi, &fd) == VK_SUCCESS && fd >= 0, "export");
   CHECK(GetMemoryFdPropertiesKHR(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &props) == VK_SUCCESS,
         "properties of a real dma-buf");
   VkImportMemoryFdInfoKHR im = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                 .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = fd};
   VkMemoryAllocateInfo bi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &im,
                              .allocationSize = 4 * 65536, .memoryTypeIndex = 0};
   VkDeviceMemory big;
   CHECK(AllocateMemory(dev, &bi, NULL, &big) == VK_ERROR_INVALID_EXTERNAL_HANDLE, "oversized import");
   close(fd);   /* a refused import leaves the fd with the caller */
   FreeMemory(dev, m, NULL);
   printf("{\"test\":\"wsifd\",\"ok\":true}\n");
   return 1;
}

struct exp_arg {
   VkSemaphore sem;
   int fd;
   VkResult r;
   double done_us;
};

static void *
export_thread(void *p)
{
   struct exp_arg *a = p;
   VkSemaphoreGetFdInfoKHR gs = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, .semaphore = a->sem,
                                 .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
   a->fd = -2;
   a->r = GetSemaphoreFdKHR(dev, &gs, &a->fd);
   a->done_us = now_us();
   return NULL;
}

/* a sync_file exported while the signalling submission is still queued (it waits for a timeline value) must
 * not come back "already signalled": the export has to wait for the submission and carry its fence */
static int
test_syncexport(void)
{
   VkSemaphoreTypeCreateInfo ti = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                   .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
   VkSemaphoreCreateInfo tci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti};
   VkExportSemaphoreCreateInfo ex = {.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
                                     .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
   VkSemaphoreCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ex};
   VkSemaphore t, b;
   CHECK(CreateSemaphore(dev, &tci, NULL, &t) == VK_SUCCESS, "timeline");
   CHECK(CreateSemaphore(dev, &bci, NULL, &b) == VK_SUCCESS, "binary");
   uint64_t wv = 1, sv = 0;
   VkTimelineSemaphoreSubmitInfo tsi = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                        .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &wv,
                                        .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &sv};
   VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   VkSubmitInfo s = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tsi, .waitSemaphoreCount = 1,
                     .pWaitSemaphores = &t, .pWaitDstStageMask = &st, .signalSemaphoreCount = 1,
                     .pSignalSemaphores = &b};
   CHECK(QueueSubmit(queue, 1, &s, VK_NULL_HANDLE) == VK_SUCCESS, "submit waiting on the timeline");
   struct exp_arg a = {.sem = b};
   pthread_t th;
   pthread_create(&th, NULL, export_thread, &a);
   struct timespec nap = {0, 300 * 1000 * 1000};
   nanosleep(&nap, NULL);
   double t_signal = now_us();
   VkSemaphoreSignalInfoKHR si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = t, .value = 1};
   CHECK(SignalSemaphoreKHR(dev, &si) == VK_SUCCESS, "signal timeline");
   pthread_join(th, NULL);
   CHECK(a.r == VK_SUCCESS, "export = %d", a.r);
   CHECK(a.done_us >= t_signal, "export returned %.0f us BEFORE the signalling submission could run (pending reported as signalled)",
         t_signal - a.done_us);
   if (a.fd >= 0)
      close(a.fd);
   CHECK(QueueWaitIdle(queue) == VK_SUCCESS, "idle");
   DestroySemaphore(dev, b, NULL);
   DestroySemaphore(dev, t, NULL);
   printf("{\"test\":\"syncexport\",\"ok\":true}\n");
   return 1;
}

/* DRM format modifier validation at CreateImage and unsupported swapchain-aliased binds being reported */
static int
test_wsiimg(void)
{
   const uint64_t linear = 0, other = 0x0100000000000001ull;
   VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                           .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {64, 64, 1}, .mipLevels = 1, .arrayLayers = 1,
                           .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                           .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   VkImageDrmFormatModifierListCreateInfoEXT ml = {.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
                                                   .drmFormatModifierCount = 1, .pDrmFormatModifiers = &linear};
   VkImage img = VK_NULL_HANDLE, bad = VK_NULL_HANDLE;
   ci.pNext = &ml;
   CHECK(CreateImage(dev, &ci, NULL, &img) == VK_SUCCESS, "list {LINEAR}");
   ml.pDrmFormatModifiers = &other;
   CHECK(CreateImage(dev, &ci, NULL, &bad) == VK_ERROR_FORMAT_NOT_SUPPORTED, "list without LINEAR must be refused");
   ci.pNext = NULL;
   CHECK(CreateImage(dev, &ci, NULL, &bad) == VK_ERROR_FORMAT_NOT_SUPPORTED, "modifier tiling without any modifier info");

   VkImageSubresource sr = {.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT};
   VkSubresourceLayout lay;
   GetImageSubresourceLayout(dev, img, &sr, &lay);
   CHECK(lay.rowPitch >= 64 * 4, "row pitch %llu", (unsigned long long)lay.rowPitch);
   VkSubresourceLayout pl = {.offset = 0, .rowPitch = lay.rowPitch};
   VkImageDrmFormatModifierExplicitCreateInfoEXT me = {.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
                                                       .drmFormatModifier = linear, .drmFormatModifierPlaneCount = 1,
                                                       .pPlaneLayouts = &pl};
   ci.pNext = &me;
   VkImage ex = VK_NULL_HANDLE;
   CHECK(CreateImage(dev, &ci, NULL, &ex) == VK_SUCCESS, "explicit layout with the image's own pitch");
   pl.rowPitch = 0;
   CHECK(CreateImage(dev, &ci, NULL, &bad) == VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT, "explicit zero row pitch");
   /* A larger row pitch is a legal description of a linear plane (the dma-buf exporter picks it: X11 DRI3
    * pixmap_from_buffer, GBM, zink's winsys import); refusing it made every such import fail. 64 B aligned, so
    * adopted; one byte more is not, so still refused. */
   pl.rowPitch = lay.rowPitch + 64;
   VkImage wide = VK_NULL_HANDLE;
   CHECK(CreateImage(dev, &ci, NULL, &wide) == VK_SUCCESS, "larger 64 B-aligned row pitch is a legal linear layout");
   if (wide != VK_NULL_HANDLE) {
      VkSubresourceLayout wl;
      GetImageSubresourceLayout(dev, wide, &sr, &wl);
      CHECK(wl.rowPitch == pl.rowPitch, "adopted row pitch %llu != asked %llu", (unsigned long long)wl.rowPitch,
            (unsigned long long)pl.rowPitch);
      DestroyImage(dev, wide, NULL);
   }
   pl.rowPitch = lay.rowPitch + 1;
   CHECK(CreateImage(dev, &ci, NULL, &bad) == VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT, "misaligned row pitch");
   pl.rowPitch = lay.rowPitch - 64;
   CHECK(CreateImage(dev, &ci, NULL, &bad) == VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT, "row pitch below an unpadded row");
   pl.rowPitch = lay.rowPitch;
   /* The sizes a swapchain actually asks for: 32x32 (Steam), 800x600 (glxgears), 33x17, 1x1, each on the layout the
    * dma-buf importer would hand us (its own pitch rounded up to 64 B). All must be creatable. */
   const VkExtent2D small[] = {{32, 32}, {800, 600}, {33, 17}, {1, 1}};
   ml.pDrmFormatModifiers = &linear;
   for (unsigned i = 0; i < sizeof small / sizeof small[0]; i++) {
      VkImageCreateInfo sci = ci;
      sci.extent = (VkExtent3D){small[i].width, small[i].height, 1};
      sci.pNext = &ml;
      VkImage base = VK_NULL_HANDLE;
      CHECK(CreateImage(dev, &sci, NULL, &base) == VK_SUCCESS, "%ux%u list {LINEAR}", small[i].width, small[i].height);
      if (base == VK_NULL_HANDLE)
         continue;
      VkSubresourceLayout bl;
      GetImageSubresourceLayout(dev, base, &sr, &bl);
      VkSubresourceLayout spl = {.offset = 0, .rowPitch = (bl.rowPitch + 63) & ~63ull};
      VkImageDrmFormatModifierExplicitCreateInfoEXT sme = {.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
                                                           .drmFormatModifier = linear, .drmFormatModifierPlaneCount = 1,
                                                           .pPlaneLayouts = &spl};
      sci.pNext = &sme;
      VkImage dimg = VK_NULL_HANDLE;
      CHECK(CreateImage(dev, &sci, NULL, &dimg) == VK_SUCCESS, "%ux%u explicit layout with the importer's pitch %llu",
            small[i].width, small[i].height, (unsigned long long)spl.rowPitch);
      if (dimg != VK_NULL_HANDLE)
         DestroyImage(dev, dimg, NULL);
      DestroyImage(dev, base, NULL);
   }
   /* An importer pitch that is one tile bigger than minimal is what a DRI3 32x32 surface gets: 128 B padded to 256. */
   ci.extent = (VkExtent3D){32, 32, 1};
   pl.rowPitch = 256;
   ci.pNext = &me;
   VkImage p32 = VK_NULL_HANDLE;
   CHECK(CreateImage(dev, &ci, NULL, &p32) == VK_SUCCESS, "32x32 explicit linear layout, 256 B row pitch");
   if (p32 != VK_NULL_HANDLE) {
      VkMemoryRequirements r32;
      GetImageMemoryRequirements(dev, p32, &r32);
      CHECK(r32.size >= 256ull * 32, "32x32 image memory %llu does not cover 256 B x 32 rows",
            (unsigned long long)r32.size);
      DestroyImage(dev, p32, NULL);
   }
   ci.extent = (VkExtent3D){64, 64, 1};
   pl.rowPitch = lay.rowPitch;
   pl.offset = 64;
   CHECK(CreateImage(dev, &ci, NULL, &bad) == VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT, "plane offset != 0");
   pl.offset = 0;
   me.drmFormatModifier = other;
   CHECK(CreateImage(dev, &ci, NULL, &bad) == VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT, "explicit foreign modifier");

   /* BindImageMemory2: a real bind works; a swapchain-aliased one (no memory) is reported, not silently dropped */
   VkMemoryRequirements mr;
   GetImageMemoryRequirements(dev, img, &mr);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                              .memoryTypeIndex = 0};
   VkDeviceMemory mem;
   CHECK(AllocateMemory(dev, &ai, NULL, &mem) == VK_SUCCESS, "alloc image memory");
   VkBindImageMemoryInfo bi = {.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO, .image = img, .memory = mem};
   CHECK(BindImageMemory2KHR_(dev, 1, &bi) == VK_SUCCESS, "bind");
   VkResult status = VK_SUCCESS;
   VkBindMemoryStatus bs = {.sType = VK_STRUCTURE_TYPE_BIND_MEMORY_STATUS, .pResult = &status};
   VkBindImageMemorySwapchainInfoKHR sw = {.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_SWAPCHAIN_INFO_KHR, .pNext = &bs};
   VkBindImageMemoryInfo al = {.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO, .pNext = &sw, .image = ex,
                               .memory = VK_NULL_HANDLE};
   CHECK(BindImageMemory2KHR_(dev, 1, &al) == VK_ERROR_UNKNOWN, "swapchain-aliased bind reported success with the image unbound");
   CHECK(status == VK_ERROR_UNKNOWN, "VkBindMemoryStatus not set for the unbound image");
   FreeMemory(dev, mem, NULL);
   DestroyImage(dev, ex, NULL);
   DestroyImage(dev, img, NULL);
   printf("{\"test\":\"wsiimg\",\"ok\":true}\n");
   return 1;
}

/* a failed submission must still signal what it was to signal (here: a timeline value), so that waiters see
 * DEVICE_LOST rather than sitting out their timeout */
static int
test_failsignal(void)
{
   VkSemaphoreTypeCreateInfo ti = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                   .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
   VkSemaphoreCreateInfo tci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti};
   VkSemaphore t;
   CHECK(CreateSemaphore(dev, &tci, NULL, &t) == VK_SUCCESS, "timeline");
   const char *e = getenv("MALI_FAULT_INJECT");
   int n = e ? atoi(e) : 0;
   CHECK(n > 0, "MALI_FAULT_INJECT not set");
   uint64_t last = 0;
   VkResult failed = VK_SUCCESS;
   for (int i = 0; i < n + 20; i++) {
      uint64_t sv = i + 1;
      VkTimelineSemaphoreSubmitInfo tsi = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                           .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &sv};
      VkSubmitInfo s = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tsi, .signalSemaphoreCount = 1,
                        .pSignalSemaphores = &t};
      failed = QueueSubmit(queue, 1, &s, VK_NULL_HANDLE);
      if (failed != VK_SUCCESS)
         break;
      last = sv;
   }
   CHECK(failed == VK_ERROR_DEVICE_LOST, "fault injection must fail a submission with DEVICE_LOST, got %d after %llu submissions",
         failed, (unsigned long long)last);
   last++; /* the value the failed submission was to signal */
   VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &t,
                             .pValues = &last};
   double t0 = now_us();
   VkResult r = WaitSemaphoresKHR(dev, &wi, 8000000000ull);
   CHECK(r == VK_SUCCESS || r == VK_ERROR_DEVICE_LOST, "wait on the last (failed) signal = %d after %.0f ms (TIMEOUT = never signalled)",
         r, (now_us() - t0) / 1e3);
   DestroySemaphore(dev, t, NULL);
   printf("{\"test\":\"failsignal\",\"inject\":%d,\"wait\":%d,\"ok\":true}\n", n, r);
   return 1;
}

static int
test_fault(void)
{
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence f;
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   const char *e = getenv("MALI_FAULT_INJECT");
   int n = e ? atoi(e) : 0;
   CHECK(n > 0, "MALI_FAULT_INJECT not set");
   VkResult got = VK_SUCCESS;
   int at = -1;
   for (int i = 0; i < n + 20 && got == VK_SUCCESS; i++) {
      got = QueueSubmit(queue, 1, &si, f);
      if (got == VK_SUCCESS)
         got = WaitForFences(dev, 1, &f, VK_TRUE, 10000000000ull);
      if (got == VK_SUCCESS)
         ResetFences(dev, 1, &f);
      at = i + 1;
   }
   CHECK(got == VK_ERROR_DEVICE_LOST, "expected DEVICE_LOST, got %d", got);
   CHECK(at >= n, "lost before the injected fault (submission %d < %d)", at, n);
   VkResult r1 = QueueSubmit(queue, 1, &si, f), r2 = QueueWaitIdle(queue), r3 = DeviceWaitIdle(dev);
   CHECK(r1 == VK_ERROR_DEVICE_LOST && r2 == VK_ERROR_DEVICE_LOST && r3 == VK_ERROR_DEVICE_LOST,
         "after loss: submit %d idle %d devidle %d", r1, r2, r3);
   DestroyFence(dev, f, NULL);
   printf("{\"test\":\"fault\",\"inject\":%d,\"lost_at\":%d,\"after_loss\":\"DEVICE_LOST\",\"ok\":true}\n", n, at);
   return 1;
}

/* A submission without signals must not run submit_signal (the fake backend counts only those towards MALI_FAULT_INJECT=n):
 * many signal-less submits survive, and the n-th FENCED one is the lost one. */
static int
test_nofence(void)
{
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence f;
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
   CHECK(CreateFence(dev, &fi, NULL, &f) == VK_SUCCESS, "fence");
   const char *e = getenv("MALI_FAULT_INJECT");
   int n = e ? atoi(e) : 0;
   CHECK(n > 1, "MALI_FAULT_INJECT (>1) not set");
   for (int i = 0; i < 4 * n; i++)
      CHECK(QueueSubmit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS, "signal-less submit %d consumed a fence slot", i);
   for (int i = 0; i < n - 1; i++) {
      CHECK(QueueSubmit(queue, 1, &si, f) == VK_SUCCESS, "fenced submit %d", i);
      CHECK(WaitForFences(dev, 1, &f, VK_TRUE, 10000000000ull) == VK_SUCCESS, "wait %d", i);
      ResetFences(dev, 1, &f);
   }
   VkResult r = QueueSubmit(queue, 1, &si, f);
   CHECK(r == VK_ERROR_DEVICE_LOST, "n-th fenced submit: expected DEVICE_LOST, got %d", r);
   DestroyFence(dev, f, NULL);
   printf("{\"test\":\"nofence\",\"inject\":%d,\"ok\":true}\n", n);
   return 1;
}

int
main(int argc, char **argv)
{
   const char *icd = NULL;
   int submits = 10000;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--direct") && i + 1 < argc) icd = argv[++i];
      else if (!strcmp(argv[i], "--submits") && i + 1 < argc) submits = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--only") && i + 1 < argc) g_only = argv[++i];
      else if (!strcmp(argv[i], "--burst") && i + 1 < argc) g_burst = atoi(argv[++i]);
      else { fprintf(stderr, "usage: t_queue --direct icd.so [--submits N] [--only case]\n"); return 2; }
   }
   if (!icd) { fprintf(stderr, "--direct required\n"); return 2; }

   int fds0 = count_fds();
   void *lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("{\"fail\":\"dlopen: %s\"}\n", dlerror()); return 1; }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   VkResult (*negotiate)(uint32_t *) = (VkResult (*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   uint32_t iv = 5;
   if (!gipa || !negotiate || negotiate(&iv)) { printf("{\"fail\":\"icd entry\"}\n"); return 1; }

   PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = (g_only && !strcmp(g_only, "wsiimg")) ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   const char *iext[] = {"VK_KHR_get_physical_device_properties2", "VK_KHR_external_memory_capabilities",
                         "VK_KHR_external_semaphore_capabilities", "VK_KHR_external_fence_capabilities"};
   ici.enabledExtensionCount = 4;
   ici.ppEnabledExtensionNames = iext;
   VkInstance inst;
   if (CreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("{\"fail\":\"CreateInstance\"}\n"); return 1; }
   PFN_vkEnumeratePhysicalDevices EPD = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceMemoryProperties GMP = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
   PFN_vkGetPhysicalDeviceProperties GPP = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
   PFN_vkCreateDevice CreateDevice = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
   PFN_vkGetDeviceProcAddr GDPA = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
   PFN_vkDestroyInstance DestroyInstance = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
   VkPhysicalDevice pd;
   uint32_t cnt = 1;
   VkResult r = EPD(inst, &cnt, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || cnt < 1) { printf("{\"fail\":\"no device\"}\n"); return 1; }
   VkPhysicalDeviceProperties pp;
   GPP(pd, &pp);
   GMP(pd, &memprops);
   printf("{\"device\":\"%s\"}\n", pp.deviceName);

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
   const char *dext[] = {"VK_KHR_timeline_semaphore", "VK_KHR_external_memory", "VK_KHR_external_memory_fd",
                         "VK_EXT_external_memory_dma_buf", "VK_KHR_external_semaphore",
                         "VK_KHR_external_semaphore_fd", "VK_KHR_external_fence", "VK_KHR_external_fence_fd",
                          /* only enabled for --only wsiimg */
                          "VK_EXT_image_drm_format_modifier", "VK_KHR_sampler_ycbcr_conversion", "VK_KHR_maintenance1"};
   int wsiimg = g_only && !strcmp(g_only, "wsiimg");
   VkPhysicalDeviceTimelineSemaphoreFeatures tf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, .timelineSemaphore = VK_TRUE};
   VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &tf, .queueCreateInfoCount = 1,
                            .pQueueCreateInfos = &qi, .enabledExtensionCount = wsiimg ? 11 : 8, .ppEnabledExtensionNames = dext};
   if (CreateDevice(pd, &di, NULL, &dev) != VK_SUCCESS) { printf("{\"fail\":\"CreateDevice\"}\n"); return 1; }
#define LOAD(n) n = (PFN_vk##n)GDPA(dev, "vk" #n); if (!n) { printf("{\"fail\":\"missing vk" #n "\"}\n"); return 1; }
   DEV_FNS(LOAD)
   if (wsiimg) {
      BindImageMemory2KHR_ = (PFN_vkBindImageMemory2KHR)GDPA(dev, "vkBindImageMemory2");
      if (!BindImageMemory2KHR_) BindImageMemory2KHR_ = (PFN_vkBindImageMemory2KHR)GDPA(dev, "vkBindImageMemory2KHR");
      if (!BindImageMemory2KHR_) { printf("{\"fail\":\"missing vkBindImageMemory2\"}\n"); return 1; }
   }
   GetDeviceQueue(dev, 0, 0, &queue);
   /* vkGetDeviceQueue returns a loader-dispatchable handle; in direct mode the ICD's own. */

   int ok = 1;
   if (CASE_ON("mem")) {
      ok &= test_mem(0, 1 << 20);
      ok &= test_mem(1, 1 << 20);
      ok &= test_mem(1, 4096);
      ok &= test_mem(1, 1 << 20); /* recycled, previously dirtied cached pages */
      ok &= test_many_allocs();
   }
   if (ok && CASE_ON("dmabuf")) { ok &= test_dmabuf(0); ok &= test_dmabuf(1); }
   if (ok && CASE_ON("idle")) ok &= test_idle();
   if (ok && CASE_ON("binary")) ok &= test_binary();
   if (ok && CASE_ON("timeline")) ok &= test_timeline(500);
   if (ok && CASE_ON("syncfile")) ok &= test_syncfile();
   if (ok && g_only && !strcmp(g_only, "poll0")) ok &= test_poll0();
   if (ok && g_only && !strcmp(g_only, "gpufence")) ok &= test_gpufence();
   if (ok && g_only && !strcmp(g_only, "order")) ok &= test_order(getenv("MALI_PRESENT_FENCE") && atoi(getenv("MALI_PRESENT_FENCE")));
   if (ok && CASE_ON("submits")) ok &= test_submits(submits);
   if (ok && g_only && !strcmp(g_only, "fault")) ok &= test_fault();
   if (ok && g_only && !strcmp(g_only, "wsifd")) ok &= test_wsifd();
   if (ok && g_only && !strcmp(g_only, "syncexport")) ok &= test_syncexport();
   if (ok && g_only && !strcmp(g_only, "wsiimg")) ok &= test_wsiimg();
   if (ok && g_only && !strcmp(g_only, "failsignal")) ok &= test_failsignal();
   if (ok && g_only && !strcmp(g_only, "nofence")) ok &= test_nofence();

   int td = g_only && !strcmp(g_only, "teardown");
   if (ok && td) ok &= test_teardown();
   if (!td) DeviceWaitIdle(dev);
   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);
   DestroyDevice(dev, NULL);
   clock_gettime(CLOCK_MONOTONIC, &t1);
   DestroyInstance(inst, NULL);
   dlclose(lib);
   const char *sl = getenv("MALI_FAKE_SLOW_US");
   unsigned slow_us = sl ? (unsigned)strtoul(sl, NULL, 10) : 0;
   if (td) usleep(slow_us + 100000); /* longer than the delay: any thread left behind runs now */
   double dms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
   if (td && dms > slow_us / 2000.0) { /* destroy must cut the pending delay short, not sit it out */
      printf("{\"fail\":\"DestroyDevice took %.0f ms with a %u us fence delay\"}\n", dms, slow_us);
      g_fail = 1;
   }
   int fds1 = count_fds();
   if (td && fds1 != fds0) { printf("{\"fail\":\"teardown fds %d -> %d\"}\n", fds0, fds1); g_fail = 1; }
   printf("{\"fds\":[%d,%d],\"pass\":%s}\n", fds0, fds1, (ok && !g_fail) ? "true" : "false");
   return (ok && !g_fail) ? 0 : 1;
}
