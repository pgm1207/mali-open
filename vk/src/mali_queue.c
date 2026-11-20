/* SPDX-License-Identifier: MIT */
/* Queue submit (M4): the command stream only carries a flush and the fence signal; real command
 * buffers arrive with M8. vk_queue runs in DEFERRED mode (emulated timeline semaphores), so this is
 * called from the runtime's submit thread, one submission at a time. */
#include "mali_queue.h"
#include "mali_fprof.h"
#include <time.h>
static double prof_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static double prof_acc[6]; static unsigned prof_n;

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kbase_cs.h"
#include "kbase_draw.h"
#include "mali_backend.h"
#include "mali_cmd.h"
#include "mali_event.h"
#include "mali_meta.h"
#include "mali_soft.h"
#include "vk_command_buffer.h"

/* ---- M9: frame resources ---- */
#define MALI_MAX_INFLIGHT 3u
#define MALI_MAX_OVF 64u
struct mali_hwctx {
   struct mali_device *dev;
   struct mali_bo arena, geo, sync, tls;
   bool tls_live;
   bool arena_live, geo_live, sync_live, heap_live;
   struct kb_arena ar;            /* only bo.{cpu,gpu_va,size} are used: a view of `arena` */
   struct mali_heap heap;
   uint64_t expected;             /* retire counter value after the last submitted frame */
   unsigned wait_ms;              /* MALI_FRAME_TIMEOUT_MS, default 10000 */
   struct mali_bo meta_sh[16];    /* M12: uploaded meta shader binaries, freed at device teardown */
   unsigned n_meta_sh;
   /* async submit: frames in flight (retire counter value + arena range), oldest first */
   struct { uint64_t expected; size_t start; } fl[MALI_MAX_INFLIGHT];
   unsigned nfl;
   size_t cur_start;
   struct mali_bo old_tls[8];     /* replaced TLS BOs: freed at teardown (in-flight / current frame may still use them) */
   unsigned n_old_tls;
   /* overflow descriptor memory: a frame that does not fit the rolling arena (even with every frame retired) takes extra BOs, freed once the
    * submission that used them has retired (tag = retire counter value, 0 = not yet submitted) */
   struct { struct mali_bo bo; uint64_t tag; } ovf[MALI_MAX_OVF];
   unsigned n_ovf;
   struct mali_bo ovf_pool[MALI_MAX_OVF];   /* retired overflow BOs kept for the next big frame (mapping and kbase alloc are slow) */
   unsigned n_pool;
   size_t pool_bytes;
   size_t ovf_off, ovf_last;
   uint32_t meta_sz[16];          /* byte size of each meta_sh[] upload (dedupe on re-bind) */
   int light_pending;             /* see mali_env.light_pending */
   struct mali_hw hw;             /* what mali_cmd/mali_meta singletons are bound to while this device is the current one */
   struct mali_meta_dev md;
   struct mali_fprof fp;          /* M15: MALI_FRAME_PROF; off (all no-ops) unless the env var asks for it */
   struct mali_hwctx *below;      /* previously current device (stack of live devices) */
};

/* mali_cmd.c / mali_meta.c keep process-global bindings to ONE device. dEQP (and apps) create short-lived extra devices
 * while the main one stays alive: destroying the extra one must re-bind the singletons to the one below it, not unbind them
 * (that made vkCreateEvent/meta allocations of the surviving device fail with VK_ERROR_OUT_OF_DEVICE_MEMORY). */
static pthread_mutex_t g_bind_lock = PTHREAD_MUTEX_INITIALIZER;
static struct mali_hwctx *g_cur;

static void
bind_hw(struct mali_hwctx *h)
{
   if (h) {
      mali_cmd_set_hw(&h->hw);
      mali_cmd_set_res(&mali_cmd_res);
      mali_meta_set_dev(&h->md);
      /* Not on the fake backend: the goldens dump its whole BO table, and the compile BOs a background prewarm allocates would land in it
         at a thread-timing-dependent index. The fake device has no first-use hitch to hide. */
      if (mali_device_pdev(h->dev)->backend != &mali_backend_fake)
         mali_meta_prewarm();
   } else {
      mali_cmd_set_hw(NULL);
      mali_cmd_set_res(NULL);
      mali_meta_set_dev(NULL);
   }
}

/* M15: per-BO write tracking for the fence-wait CSYNC (mali_backend.h bo_wrote, vk/MEMORY.md). */
static void
hw_bo_wrote(void *user, struct mali_bo *bo)
{
   struct mali_hwctx *h = user;
   const struct mali_backend_ops *be = mali_device_pdev(h->dev)->backend;
   if (be->bo_wrote)
      be->bo_wrote(h->dev->kdev, bo);
}

/* ---- M12/M13 device seam of mali_meta.c: long-lived zeroed memory (one uncached BO each, 64 B header keeps the struct mali_bo) and
 * executable shader memory ---- */
#define META_HDR 64u
struct meta_hdr {
   struct mali_bo bo;
};
_Static_assert(sizeof(struct meta_hdr) <= META_HDR, "meta header");

/* Device-scoped: the BO is allocated and freed through the device the caller names, never through the process-global binding
 * (g_cur) and never through a pointer stored in the BO (which could dangle once another device is destroyed). */
void *
mali_hw_meta_alloc(struct mali_device *dev, size_t size, size_t align, uint64_t *va)
{
   struct meta_hdr hdr;
   if (!dev || mali_device_pdev(dev)->backend->bo_alloc(dev->kdev, size + META_HDR, MALI_BO_COH, &hdr.bo) != 0)
      return NULL;
   struct mali_bo bo = hdr.bo;
   memcpy(bo.cpu, &hdr, sizeof hdr);
   *va = bo.gpu_va + META_HDR;
   return (char *)bo.cpu + META_HDR;
}

void
mali_hw_meta_free(struct mali_device *dev, void *cpu)
{
   struct meta_hdr hdr;
   if (!cpu)
      return;
   memcpy(&hdr, (char *)cpu - META_HDR, sizeof hdr);
   mali_device_pdev(dev)->backend->bo_free(dev->kdev, &hdr.bo);
}

static void *
meta_alloc(void *u, size_t size, size_t align, uint64_t *va)
{
   struct mali_hwctx *h = u;
   return mali_hw_meta_alloc(h->dev, size, align, va);
}

static void
meta_free(void *u, void *cpu, uint64_t va, size_t size)
{
   struct mali_hwctx *h = u;
   mali_hw_meta_free(h->dev, cpu);
}

static int
meta_upload_shader(void *u, const uint8_t *bin, uint32_t size, uint64_t *va)
{
   struct mali_hwctx *h = u;
   if (h->n_meta_sh >= 16)
      return -ENOMEM;
   for (unsigned i = 0; i < h->n_meta_sh; i++)   /* re-bind after another device was destroyed: reuse the identical upload */
      if (h->meta_sz[i] == size && !memcmp(h->meta_sh[i].cpu, bin, size)) {
         *va = h->meta_sh[i].gpu_va;
         return 0;
      }
   struct mali_bo *bo = &h->meta_sh[h->n_meta_sh];
   h->meta_sz[h->n_meta_sh] = size;
   if (mali_device_pdev(h->dev)->backend->bo_alloc(h->dev->kdev, size, MALI_BO_EXEC, bo))
      return -ENOMEM;
   h->n_meta_sh++;
   memcpy(bo->cpu, bin, size);
   *va = bo->gpu_va;
   return 0;
}

#define ARENA_BYTES (32u << 20)   /* per-frame descriptor arena: ~4.5 KB per draw (two 2 KiB driver sets); 1 MiB capped a frame at ~230 draws */
#define HEAP_CHUNK (2u << 20)

static int hw_wait(struct mali_hwctx *h, uint64_t target);
static void hw_retire(struct mali_hwctx *h, unsigned max_left);

static void *
hw_alloc_ovf(struct mali_hwctx *h, size_t n, size_t al, uint64_t *va, bool zero)
{
   const struct mali_backend_ops *be = mali_device_pdev(h->dev)->backend;
   struct mali_bo *cur = h->n_ovf && !h->ovf[h->n_ovf - 1].tag ? &h->ovf[h->n_ovf - 1].bo : NULL;
   size_t o = cur ? (h->ovf_off + al - 1) & ~(al - 1) : 0;
   if (!cur || o + n > cur->size) {
      if (h->n_ovf == MALI_MAX_OVF) {   /* all slots taken by submitted ones: wait for them, then reuse */
         hw_retire(h, 0);
         if (h->n_ovf == MALI_MAX_OVF)
            return NULL;
      }
      size_t sz = h->ovf_last ? h->ovf_last * 2 : (16u << 20);
      if (sz < n + al)
         sz = (n + al + (1u << 20) - 1) & ~((size_t)(1u << 20) - 1);
      if (sz > (512u << 20))
         sz = n + al > (512u << 20) ? n + al : (512u << 20);
      unsigned best = h->n_pool;
      for (unsigned i = 0; i < h->n_pool; i++)
         if (h->ovf_pool[i].size >= sz && h->ovf_pool[i].size <= 2 * sz && (best == h->n_pool || h->ovf_pool[i].size < h->ovf_pool[best].size))
            best = i;
      if (best < h->n_pool) {
         h->ovf[h->n_ovf].bo = h->ovf_pool[best];
         h->pool_bytes -= h->ovf_pool[best].size;
         h->ovf_pool[best] = h->ovf_pool[--h->n_pool];
         sz = h->ovf[h->n_ovf].bo.size;
      } else if (be->bo_alloc(h->dev->kdev, sz, MALI_BO_HOT, &h->ovf[h->n_ovf].bo))
         return NULL;
      h->ovf[h->n_ovf].tag = 0;
      cur = &h->ovf[h->n_ovf++].bo;
      h->ovf_last = sz;
      o = 0;
   }
   h->ovf_off = o + n;
   if (zero)
      memset((char *)cur->cpu + o, 0, n);
   if (va)
      *va = cur->gpu_va + o;
   return (char *)cur->cpu + o;
}

static uint64_t g_alloc_bytes;   /* test statistic: bytes asked of hw_alloc (MALI_ALLOC_STATS=<file> writes the running total at each submit) */

static void *
hw_alloc_x(void *u, size_t n, size_t al, uint64_t *va, bool zero)
{
   struct mali_hwctx *h = u;
   g_alloc_bytes += n;
   if (h->n_ovf && !h->ovf[h->n_ovf - 1].tag)   /* this submission already spilled: stay in order */
      return hw_alloc_ovf(h, n, al, va, zero);
   void *p = kb_arena_alloc_nz(&h->ar, n, al, va);
   if (!p && h->nfl) {
      /* arena full of in-flight frames: retire them all; the limit is then this submission's own start */
      hw_retire(h, 0);
      h->ar.start = h->cur_start;
      if (h->ar.off >= h->cur_start) {   /* failed at the end of the BO: wrap to 0, bounded by this submission's own start */
         h->ar.off = 0;
         h->ar.lapped = 1;
      }
      p = kb_arena_alloc_nz(&h->ar, n, al, va);
   }
   if (!p)
      return hw_alloc_ovf(h, n, al, va, zero);
   if (zero)
      memset(p, 0, n);
   return p;
}

static void *
hw_alloc(void *u, size_t n, size_t al, uint64_t *va)
{
   return hw_alloc_x(u, n, al, va, true);
}

static void *
hw_alloc_nz(void *u, size_t n, size_t al, uint64_t *va)
{
   return hw_alloc_x(u, n, al, va, false);
}

static int
hw_wait(struct mali_hwctx *h, uint64_t target)
{
   const struct mali_backend_ops *be = mali_device_pdev(h->dev)->backend;
   int r = be->wait_counter(h->dev->kdev, (volatile uint64_t *)h->sync.cpu, target, h->wait_ms);
   if (r == 0 && h->fp.on)
      mali_fprof_retired(&h->fp, h->dev, target);   /* M15: prompt retire observation (the CPU is blocked here anyway) */
   return r;
}

static void
hw_ovf_reap(struct mali_hwctx *h, uint64_t counter)
{
   const struct mali_backend_ops *be = mali_device_pdev(h->dev)->backend;
   unsigned w = 0;
   for (unsigned i = 0; i < h->n_ovf; i++) {
      if (h->ovf[i].tag && h->ovf[i].tag <= counter) {
         if (h->n_pool < MALI_MAX_OVF && h->pool_bytes + h->ovf[i].bo.size <= (256u << 20)) {
            h->ovf_pool[h->n_pool++] = h->ovf[i].bo;
            h->pool_bytes += h->ovf[i].bo.size;
         } else
            be->bo_free(h->dev->kdev, &h->ovf[i].bo);
      } else
         h->ovf[w++] = h->ovf[i];
   }
   h->n_ovf = w;
}

/* Drop retired frames from the in-flight list; block on the oldest while the list is full. */
static void
hw_retire(struct mali_hwctx *h, unsigned max_left)
{
   for (;;) {
      uint64_t c = *(volatile uint64_t *)h->sync.cpu;
      hw_ovf_reap(h, c);
      unsigned k = 0;
      while (k < h->nfl && h->fl[k].expected <= c)
         k++;
      if (k && h->fp.on)
         mali_fprof_retired(&h->fp, h->dev, c);   /* M15: the retire this pass can see (one GPU-clock sample) */
      if (k) {
         memmove(h->fl, h->fl + k, (h->nfl - k) * sizeof h->fl[0]);
         h->nfl -= k;
      }
      if (h->nfl <= max_left)
         return;
      if (hw_wait(h, h->fl[0].expected))
         return;
   }
}

static void
hw_drain(void *u)
{
   struct mali_hwctx *h = u;
   if (h->nfl)
      hw_wait(h, h->fl[h->nfl - 1].expected);
   hw_retire(h, 0);
}

static void
hw_job_begin(void *u)
{
   struct mali_hwctx *h = u;
   hw_retire(h, MALI_MAX_INFLIGHT - 1);
   kb_arena_begin(&h->ar);
   h->cur_start = h->ar.start;
   if (h->nfl && !getenv("MALI_SYNC_SUBMIT")) {
      /* the new frame must stay clear of the oldest in-flight frame's arena range */
      size_t so = h->fl[0].start;
      if (so > h->ar.start)
         h->ar.lapped = 1;   /* already wrapped: limit is the oldest frame */
      h->ar.start = so;
   }
}

static void *
hw_tls(void *u, size_t size, uint64_t *va)
{
   struct mali_hwctx *h = u;
   const struct mali_backend_ops *be = mali_device_pdev(h->dev)->backend;
   if (!h->tls_live || h->tls.size < size) {
      struct mali_bo nb;
      if (be->bo_alloc(h->dev->kdev, size, 0, &nb))
         return NULL;
      if (h->tls_live) {
         if (h->n_old_tls < 8)
            h->old_tls[h->n_old_tls++] = h->tls;   /* may still be in use by in-flight frames or earlier draws of this one */
         else {
            hw_retire(h, 0);
            be->bo_free(h->dev->kdev, &h->tls);
         }
      }
      h->tls = nb;
      h->tls_live = true;
   }
   *va = h->tls.gpu_va;
   return h->tls.cpu;
}

/* MALI_SOFT=1: the desktop software rasteriser (mali_soft.c) renders each frame from its record instead of encoding a
 * command stream. A frame it cannot do fails the submit (-ENOTSUP in mali_frame_build), never silently draws nothing. */
const struct mali_hw *
mali_soft_device_hw(VkDevice dev_handle)
{
   VK_FROM_HANDLE(mali_device, dev, dev_handle);
   struct mali_hwctx *h = dev ? dev->hw : NULL;
   return h ? &h->hw : NULL;
}

static bool
hw_soft(void *u, const struct mali_frame *f)
{
   struct mali_hwctx *h = u;
   return mali_soft_frame(&h->hw, f);
}

static void *
hw_va_cpu(void *u, uint64_t va, size_t size)
{
   struct mali_hwctx *h = u;
   const struct mali_backend_ops *be = mali_device_pdev(h->dev)->backend;
   return be->va_cpu ? be->va_cpu(h->dev->kdev, va, size) : NULL;
}

static int
hw_submit_multi(void *u, const uint64_t *cs, unsigned n, unsigned nframes)
{
   struct mali_hwctx *h = u;
   const struct mali_backend_ops *be = mali_device_pdev(h->dev)->backend;
   if (getenv("MALI_CSDUMP") && n == 27) { for (unsigned i = 0; i < n; i++) fprintf(stderr, "CS %02u %016llx\n", i, (unsigned long long)cs[i]); }
   {
      const char *sf = getenv("MALI_ALLOC_STATS");
      FILE *fp = sf ? fopen(sf, "w") : NULL;
      if (fp) {
         fprintf(fp, "%llu\n", (unsigned long long)g_alloc_bytes);
         fclose(fp);
      }
   }
   /* M15 (MALI_FRAME_PROF): `pending` is what the GPU had *not* retired at the kick, read at the kick (lag-free).
    * submit_us below covers only the backend kick; the command stream was built before this call. */
   if (h->fp.on) {
      uint64_t done = *(volatile uint64_t *)h->sync.cpu;
      mali_fprof_kick(&h->fp, h->dev, h->expected + nframes, h->expected > done ? (unsigned)(h->expected - done) : 0);
   }
   double q0 = prof_now();
   int r = be->submit_cs(h->dev->kdev, cs, n);
   if (h->fp.on && r == 0)
      mali_fprof_kicked(&h->fp, prof_now() - q0);
   if (getenv("MALI_PROF")) {
      static double acc; static unsigned cnt;
      acc += prof_now() - q0;
      if (++cnt % 200 == 0) fprintf(stderr, "PROF/hw_submit submit_cs avg %.3f ms (n=%u instr)\n", acc / cnt, n);
   }
   if (getenv("MALI_PIPE_DBG")) fprintf(stderr, "mali_vk: submit_cs n=%u arena off=%zx start=%zx rc=%d\n", n, h->ar.off, h->ar.start, r);
   if (r)
      return r;
   /* The arena is rolling and must not hand out the VAs of an in-flight frame (INTERMITTENT.md): every frame retires
    * (CS SYNC_ADD64 on the event word) before the next one is built. */
   h->expected += nframes;
   for (unsigned i = 0; i < h->n_ovf; i++)
      if (!h->ovf[i].tag)
         h->ovf[i].tag = h->expected;
   h->ovf_last = 0;   /* the next frame's overflow sequence starts small again (and finds its BOs in the pool) */
   if (!getenv("MALI_SYNC_SUBMIT")) {
      h->fl[h->nfl].expected = h->expected;
      h->fl[h->nfl].start = h->cur_start;
      h->nfl++;
      return 0;
   }
   double w0 = prof_now();
   int wr = hw_wait(h, h->expected);
   if (getenv("MALI_PROF")) { static double acc[2]; static unsigned cnt; acc[0] += prof_now() - w0; if (++cnt % 100 == 0) fprintf(stderr, "PROF/syncwait frame GPU latency avg %.3f ms (n=%u instr)\n", acc[0] / cnt, n); }
   if (getenv("MALI_PIPE_DBG")) fprintf(stderr, "mali_vk:   wait rc=%d counter=%llu expected=%llu\n", wr, (unsigned long long)*(volatile uint64_t *)h->sync.cpu, (unsigned long long)h->expected);
   return wr;
}

static int
hw_submit(void *u, const uint64_t *cs, unsigned n)
{
   return hw_submit_multi(u, cs, n, 1);
}

void
mali_hw_finish(struct mali_device *dev)
{
   struct mali_hwctx *h = dev->hw;
   const struct mali_backend_ops *be = mali_device_pdev(dev)->backend;

   if (!h)
      return;
   if (h->expected)
      hw_wait(h, h->expected);
   for (unsigned i = 0; i < h->n_old_tls; i++)
      be->bo_free(dev->kdev, &h->old_tls[i]);
   for (unsigned i = 0; i < h->n_ovf; i++)
      be->bo_free(dev->kdev, &h->ovf[i].bo);
   h->n_ovf = 0;
   for (unsigned i = 0; i < h->n_pool; i++)
      be->bo_free(dev->kdev, &h->ovf_pool[i]);
   h->n_pool = 0;
   pthread_mutex_lock(&g_bind_lock);
   if (g_cur == h) {
      g_cur = h->below;
      bind_hw(g_cur);
   } else {
      for (struct mali_hwctx *c = g_cur; c; c = c->below)
         if (c->below == h) {
            c->below = h->below;
            break;
         }
   }
   pthread_mutex_unlock(&g_bind_lock);
   for (unsigned i = 0; i < h->n_meta_sh; i++)
      be->bo_free(dev->kdev, &h->meta_sh[i]);
   if (h->heap_live)
      be->heap_destroy(dev->kdev, &h->heap);
   mali_fprof_finish(&h->fp);
   if (h->tls_live)
      be->bo_free(dev->kdev, &h->tls);
   if (h->arena_live)
      be->bo_free(dev->kdev, &h->arena);
   if (h->geo_live)
      be->bo_free(dev->kdev, &h->geo);
   if (h->sync_live)
      be->bo_free(dev->kdev, &h->sync);
   free(h);
   dev->hw = NULL;
}

VkResult
mali_hw_init(struct mali_device *dev)
{
   struct mali_physical_device *pdev = mali_device_pdev(dev);
   const struct mali_backend_ops *be = pdev->backend;
   struct mali_hwctx *h = calloc(1, sizeof *h);
   const unsigned arch = pdev->gpu.arch_major >= 12 ? 13 : pdev->gpu.arch_major == 10 ? 10 : 11;
   kb_v10 = arch == 10;   /* libkbase: SET_SB_ENTRY scoreboard select, ZSD bit 21 (see kbase_cs.h) */

   if (!h)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   h->dev = dev;
   dev->hw = h;
   mali_fprof_init(&h->fp, dev);
   const char *t = getenv("MALI_FRAME_TIMEOUT_MS");
   h->wait_ms = t ? (unsigned)strtoul(t, NULL, 0) : 10000;
   if (be->bo_alloc(dev->kdev, ARENA_BYTES, MALI_BO_HOT, &h->arena))
      goto fail;
   h->arena_live = true;
   if (be->bo_alloc(dev->kdev, 4096 + 65536, MALI_BO_COH, &h->geo)   /* tiler heap descriptor: the tiler updates it from several cores; coverage 0 at random without COHERENT_LOCAL (arch 11) */)
      goto fail;
   h->geo_live = true;
   if (be->bo_alloc(dev->kdev, 4096, MALI_BO_EVENT, &h->sync))
      goto fail;
   h->sync_live = true;
   if (be->heap_create(dev->kdev, getenv("MALI_HEAP_CHUNK") ? (uint32_t)atoi(getenv("MALI_HEAP_CHUNK")) : HEAP_CHUNK,
                       getenv("MALI_HEAP_INIT") ? (uint32_t)atoi(getenv("MALI_HEAP_INIT")) : 4, 64, &h->heap))
      goto fail;
   h->heap_live = true;
   h->ar.bo.cpu = h->arena.cpu;
   h->ar.bo.gpu_va = h->arena.gpu_va;
   h->ar.bo.size = h->arena.size;

   struct mali_hw hw = {.user = h, .alloc = hw_alloc, .alloc_nz = hw_alloc_nz, .job_begin = hw_job_begin, .submit = hw_submit, .submit_multi = hw_submit_multi, .tls = hw_tls, .drain = hw_drain, .bo_wrote = hw_bo_wrote};
   hw.env = (struct mali_env){.arch = arch,
                              .hmask = arch == 13 ? 0 : 0xfe,
                              .tile_log2 = 12,
                              .slot = 3,
                              .tf_or = arch == 13 ? 0x40000 : 0,
                              .heap_ctx_va = h->heap.ctx_va,
                              .heap_first_chunk_va = h->heap.first_chunk_va,
                              .heap_chunk_size = h->heap.chunk_size,
                              .geo_cpu = h->geo.cpu,
                              .geo_va = h->geo.gpu_va,
                              .sync_va = h->sync.gpu_va,
                              .light_pending = &h->light_pending,
                              .core_id_range = pdev->gpu.core_id_range,
                              .tls_threads = pdev->gpu.tls_threads};
   kb_draw_tiler_heap_desc(h->geo.cpu, h->heap.first_chunk_va, h->heap.chunk_size);
   hw.env.heap_desc_ready = true;
   if (getenv("MALI_SOFT") && getenv("MALI_SOFT")[0] && strcmp(getenv("MALI_SOFT"), "0") != 0 &&
       pdev->backend->va_cpu) {
      hw.soft = hw_soft;
      hw.va_cpu = hw_va_cpu;
   }
   if (getenv("MALI_PIPE_DBG")) fprintf(stderr, "mali_vk: va arena %llx+%zx geo %llx sync %llx heap ctx %llx first %llx\n", (unsigned long long)h->arena.gpu_va, (size_t)h->arena.size, (unsigned long long)h->geo.gpu_va, (unsigned long long)h->sync.gpu_va, (unsigned long long)h->heap.ctx_va, (unsigned long long)h->heap.first_chunk_va);
   h->hw = hw;
   h->md = (struct mali_meta_dev){.user = h, .arch = arch, .alloc = meta_alloc, .free = meta_free, .upload_shader = meta_upload_shader};
   pthread_mutex_lock(&g_bind_lock);
   h->below = g_cur;
   g_cur = h;
   bind_hw(h);
   pthread_mutex_unlock(&g_bind_lock);
   return VK_SUCCESS;
fail:
   mali_hw_finish(dev);
   return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

VkResult
mali_queue_device_init(struct mali_device *dev)
{
   pthread_condattr_t ca;

   pthread_mutex_init(&dev->sync_lock, NULL);
   pthread_condattr_init(&ca);
   pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
   pthread_cond_init(&dev->sync_cv, &ca);
   pthread_condattr_destroy(&ca);
   return VK_SUCCESS;
}

void
mali_queue_device_finish(struct mali_device *dev)
{
   if (dev->present_fence_fd >= 0)
      close(dev->present_fence_fd);
   dev->present_fence_fd = -1;
   pthread_cond_destroy(&dev->sync_cv);
   pthread_mutex_destroy(&dev->sync_lock);
}

int
mali_queue_last_fence_dup(struct mali_device *dev)
{
   if (!dev)
      return -1;
   int d = -1;
   pthread_mutex_lock(&dev->sync_lock);
   if (dev->present_fence_fd >= 0)
      d = dup(dev->present_fence_fd);
   pthread_mutex_unlock(&dev->sync_lock);
   return d;
}

/* May a present submission return before the frame it presents has finished on the GPU? Only when a fence travels with
 * the buffer: wsi_common imports the present's sync_file into the swapchain dma-buf (DMA_BUF_IOCTL_IMPORT_SYNC_FILE,
 * needs kernel >= 6.0: CMF 6.1 and poco-x8 6.6 have it, poco-x4's 5.10 returns ENOTTY) and the compositor waits on it.
 * kbase attaches no implicit fence to its dma-bufs itself, so without the ioctl the CPU wait is the only thing
 * keeping the compositor from sampling a half-drawn buffer, and stays. wsi_common's own probe result (made at the
 * first swapchain creation) is the decision; MALI_PRESENT_FENCE=0/1 forces it (tests), MALI_QUEUE_CPU_WAIT=1 = 0. */
static bool
mali_present_fence_ok(struct mali_device *dev)
{
   static int forced = -2;
   if (forced == -2) {
      const char *e = getenv("MALI_PRESENT_FENCE");
      forced = getenv("MALI_QUEUE_CPU_WAIT") ? 0 : e ? atoi(e) : -1;
   }
   if (forced >= 0)
      return forced;
   const struct wsi_device *w = &mali_device_pdev(dev)->wsi;
   if (w->sw)
      return 0;   /* software present: the CPU copies the frame, so the queue must wait for rendering first */
   return __atomic_load_n(&w->cached_sync_file_import_export_result, __ATOMIC_RELAXED) == UINT32_MAX;   /* wsi stores result - 1; VK_SUCCESS -> UINT32_MAX */
}

static VkResult
mali_queue_submit(struct vk_queue *vq, struct vk_queue_submit *submit)
{
   struct mali_device *dev = container_of(vq->base.device, struct mali_device, vk);
   const struct mali_backend_ops *be = mali_device_pdev(dev)->backend;

   if (vk_queue_is_lost(vq))
      return VK_ERROR_DEVICE_LOST;
   if (vk_queue_submit_has_bind(submit))
      return vk_error(vq, VK_ERROR_FEATURE_NOT_PRESENT); /* sparse: not supported */

   double p0 = prof_now();
   /* Every wait is a CPU wait for now (this queue is the only producer, external sync_files are the
    * rare case); the GPU-side wait is M8 work. */
   for (uint32_t i = 0; i < submit->wait_count; i++) {
      const struct vk_sync_wait *w = &submit->waits[i];
      /* A semaphore signalled by an earlier submission of THIS queue needs no CPU wait: the queue is one in-order
       * CS ring, so everything after it already runs behind that work. Waiting here made vkQueuePresent block
       * until the GPU finished the frame it presents, which serialised CPU and GPU whatever the frames in flight.
       * Only valid while the present carries the frame's fence to the compositor (mali_present_fence_ok). */
      if (w->sync->type == &mali_sync_type && mali_present_fence_ok(dev)) {
         struct mali_sync *ws = container_of(w->sync, struct mali_sync, vk);
         pthread_mutex_lock(&dev->sync_lock);
         bool own = ws->owner == vq && ws->fd >= 0;
         pthread_mutex_unlock(&dev->sync_lock);
         if (own)
            continue;
      }
      VkResult r = vk_sync_wait(&dev->vk, w->sync, w->wait_value, VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
      if (r != VK_SUCCESS)
         return r;
   }

   double p1 = prof_now();
   if (submit->command_buffer_count && be->host_sync)
      be->host_sync(dev->kdev);
   /* Recorded frames: each is built, submitted and retired in order (mali_cmd_submit -> hw_submit). */
   for (uint32_t i = 0; i < submit->command_buffer_count; i++) {
      VkResult cr = mali_cmd_submit(submit->command_buffers[i]);
      if (cr == VK_ERROR_DEVICE_LOST && getenv("MALI_PIPE_DBG")) fprintf(stderr, "mali_vk: frame submit lost\n");
      if (cr == VK_ERROR_DEVICE_LOST)
         return vk_queue_set_lost(vq, "frame submit failed");
      if (cr != VK_SUCCESS)
         return vk_error(vq, cr);
   }

   double p2 = prof_now();
   int fd = -1;
   /* The fence is only consumed through submit->signals (VkFence, semaphores, QueueWaitIdle and present all signal), so a
    * submission without signals skips the 2 KCPU ioctls, the kick and the fd close. Exception: a light ops frame (event.c)
    * relies on submit_signal's head flush; MALI_FENCE_ALWAYS=1 keeps the old behaviour. */
   static int always = -1;
   if (always < 0)
      always = getenv("MALI_FENCE_ALWAYS") != NULL;
   bool light = mali_ops_take_light(&dev->hw->light_pending);
   int r = 0;
   if (submit->signal_count || light || always)
      r = be->submit_signal(dev->kdev, &fd);
   if (r != 0) {
      if (getenv("MALI_PIPE_DBG")) fprintf(stderr, "mali_vk: submit_signal failed: %d\n", r);
      return vk_queue_set_lost(vq, "queue submit failed: %d", r);
   }

   VkResult res = VK_SUCCESS;
   for (uint32_t i = 0; i < submit->signal_count && res == VK_SUCCESS; i++) {
      struct vk_sync *s = submit->signals[i].sync;
      res = vk_sync_import_sync_file(&dev->vk, s, fd);
      if (res == VK_SUCCESS && s->type == &mali_sync_type && fd >= 0) {
         pthread_mutex_lock(&dev->sync_lock);
         container_of(s, struct mali_sync, vk)->owner = vq;
         pthread_mutex_unlock(&dev->sync_lock);
      }
   }
   /* Keep a duplicate of this submission's sync_file for the CPU-side present paths (X11 shared memory): they have no
    * CPU-visible semaphore to wait, so they wait this fence before reading the image this submission wrote. */
   if (fd >= 0) {
      int d = dup(fd);
      if (d >= 0) {
         pthread_mutex_lock(&dev->sync_lock);
         if (dev->present_fence_fd >= 0)
            close(dev->present_fence_fd);
         dev->present_fence_fd = d;
         pthread_mutex_unlock(&dev->sync_lock);
      }
   }
   close(fd);
   /* M15: vk_common's present submit has no command buffers, the image acquire semaphores as waits and the render
    * semaphore as signal (an app's fence-only submit has no waits, so it is not counted). */
   if (dev->hw && dev->hw->fp.on && submit->command_buffer_count == 0 && submit->wait_count > 0 && submit->signal_count > 0)
      mali_fprof_present(&dev->hw->fp);
   if (getenv("MALI_PROF")) {
      double p3 = prof_now();
      prof_acc[0] += p1 - p0; prof_acc[1] += p2 - p1; prof_acc[2] += p3 - p2;
      if (++prof_n % 200 == 0) {
         fprintf(stderr, "PROF/submit(avg ms of %u): waits %.3f cmd_submit %.3f signal+import %.3f\n", prof_n, prof_acc[0] / prof_n, prof_acc[1] / prof_n, prof_acc[2] / prof_n);
      }
   }
   return res;
}

static VkResult
mali_device_check_status(struct vk_device *vk)
{
   struct mali_device *dev = container_of(vk, struct mali_device, vk);

   if (mali_device_pdev(dev)->backend->device_lost(dev->kdev))
      return VK_ERROR_DEVICE_LOST;
   return VK_SUCCESS;
}

void
mali_queue_setup(struct mali_device *dev)
{
   dev->present_fence_fd = -1;
   dev->queue.vk.driver_submit = mali_queue_submit;
   dev->vk.check_status = mali_device_check_status;
}
