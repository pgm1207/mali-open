/* SPDX-License-Identifier: MIT */
/* M12/M13 meta operation planner + software reference of the shaders. See mali_meta.h. */
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mali_meta.h"
#include "mali_pipeline_layout_flat.h"
#include "mali_meta_spv.h"
#include "mali_preload_spv.h"
#include "mali_bc.h"

int mali_meta_ref_oob;

/* ---------------------------------------------------------------- list */
void
mali_meta_list_free(struct mali_meta_list *l)
{
   for (unsigned i = 0; i < l->n; i++)
      free(l->j[i].inline_data);
   free(l->j);
   memset(l, 0, sizeof *l);
}

static struct mali_meta_job *
list_add(struct mali_meta_list *l, enum mali_meta_shader sh)
{
   if (l->n == l->cap) {
      unsigned nc = l->cap ? l->cap * 2 : 8;
      struct mali_meta_job *nj = realloc(l->j, nc * sizeof *nj);
      if (!nj)
         return NULL;
      l->j = nj;
      l->cap = nc;
   }
   struct mali_meta_job *j = &l->j[l->n++];
   memset(j, 0, sizeof *j);
   j->sh = sh;
   return j;
}

/* Binding of a surface whose touched bytes end at `end` (from va). The 32-bit shader offsets are relative to a 4 KiB-aligned rebase. */
static int
bind_surf(const struct mali_meta_surf *s, uint64_t end, uint64_t *off, struct mali_meta_bind *b)
{
   uint64_t rebase = s->off & ~0xfffull;
   if (end <= rebase)
      return -EINVAL;
   uint64_t sz = (end - rebase + 3) & ~3ull;
   if (sz > 0xffffffffull || s->off - rebase > 0xffffffffull)
      return -ERANGE;
   b->va = s->va + rebase;
   b->size = (uint32_t)sz;
   *off = s->off - rebase;
   return 0;
}

static void
set_groups(struct mali_meta_job *j, uint64_t total)
{
   uint64_t g = (total + 63) / 64;
   if (!g)
      g = 1;
   uint64_t gx = g > 32768 ? 32768 : g;
   j->groups[0] = (uint32_t)gx;
   j->groups[1] = (uint32_t)((g + gx - 1) / gx);
   j->groups[2] = 1;
}

/* ---------------------------------------------------------------- copy / const */
static int
plan_rect(struct mali_meta_list *l, const struct mali_meta_surf *dst, const struct mali_meta_surf *src, uint32_t bpt, uint32_t ex,
          uint32_t ey, uint32_t ez, const uint32_t *cst)
{
   if (!ex || !ey || !ez || !bpt || (cst && bpt > 16))
      return -EINVAL;
   uint64_t doff, soff = 0;
   struct mali_meta_bind db, sb = {0};
   uint64_t dend = dst->off + (uint64_t)(ez - 1) * dst->slice + (uint64_t)(ey - 1) * dst->row + (uint64_t)ex * bpt;
   int r = bind_surf(dst, dend, &doff, &db);
   if (r)
      return r;
   bool word = !(doff & 3) && !(bpt & 3) && (ey == 1 || !(dst->row & 3)) && (ez == 1 || !(dst->slice & 3));
   if (!cst) {
      uint64_t send = src->off + (uint64_t)(ez - 1) * src->slice + (uint64_t)(ey - 1) * src->row + (uint64_t)ex * bpt;
      r = bind_surf(src, send, &soff, &sb);
      if (r)
         return r;
      word = word && !(soff & 3) && (ey == 1 || !(src->row & 3)) && (ez == 1 || !(src->slice & 3));
   } else {
      sb = db;   /* unused second binding of a constant fill: any valid memory */
   }
   struct mali_meta_job *j = list_add(l, MALI_META_COPY);
   if (!j)
      return -ENOMEM;
   uint32_t *p = j->push;
   p[0] = (uint32_t)soff;
   p[1] = cst ? 0 : src->row;
   p[2] = cst ? 0 : src->slice;
   p[3] = (uint32_t)doff;
   p[4] = dst->row;
   p[5] = dst->slice;
   p[6] = ex;
   p[7] = ey;
   p[8] = ez;
   p[9] = bpt;
   p[10] = (word ? 1u : 0u) | (cst ? 2u : 0u);
   if (cst)
      memcpy(&p[12], cst, 16);
   j->push_bytes = 64;
   j->bind[0] = sb;
   j->bind[1] = db;
   j->nbind = 2;
   set_groups(j, (uint64_t)ex * ey * ez);
   return 0;
}

int
mali_meta_plan_copy(struct mali_meta_list *l, const struct mali_meta_surf *dst, const struct mali_meta_surf *src, uint32_t bpt, uint32_t ex,
                    uint32_t ey, uint32_t ez)
{
   return plan_rect(l, dst, src, bpt, ex, ey, ez, NULL);
}

int
mali_meta_plan_buffer_copy(struct mali_meta_list *l, const struct mali_meta_surf *dst, const struct mali_meta_surf *src, uint64_t size)
{
   if (!size)
      return 0;
   if (size > 0xffffffffull)
      return -ERANGE;
   struct mali_meta_surf d = *dst, s = *src;
   d.row = d.slice = s.row = s.slice = 0;
   if (!(d.off & 3) && !(s.off & 3) && !(size & 3))
      return plan_rect(l, &d, &s, 4, (uint32_t)(size / 4), 1, 1, NULL);
   return plan_rect(l, &d, &s, 1, (uint32_t)size, 1, 1, NULL);
}

int
mali_meta_plan_const(struct mali_meta_list *l, const struct mali_meta_surf *dst, uint32_t bpt, uint32_t ex, uint32_t ey, uint32_t ez,
                     const uint32_t cst[4])
{
   return plan_rect(l, dst, NULL, bpt, ex, ey, ez, cst);
}

int
mali_meta_plan_fill(struct mali_meta_list *l, const struct mali_meta_surf *dst, uint64_t size, uint32_t value)
{
   if (!size)
      return 0;
   if ((dst->off & 3) || (size & 3) || size > 0xffffffffull)
      return -EINVAL;
   struct mali_meta_surf d = *dst;
   d.row = d.slice = 0;
   uint32_t c[4] = { value, value, value, value };
   return plan_rect(l, &d, NULL, 4, (uint32_t)(size / 4), 1, 1, c);
}

int
mali_meta_plan_update(struct mali_meta_list *l, const struct mali_meta_surf *dst, uint64_t size, const void *data)
{
   if (!size)
      return 0;
   if ((dst->off & 3) || (size & 3) || size > 65536)
      return -EINVAL;
   struct mali_meta_surf s = { 0 }, d = *dst;
   d.row = d.slice = 0;
   int r = plan_rect(l, &d, &s, 4, (uint32_t)(size / 4), 1, 1, NULL);   /* src bind patched below */
   if (r)
      return r;
   struct mali_meta_job *j = &l->j[l->n - 1];
   j->inline_data = malloc(size);
   if (!j->inline_data) {
      l->n--;
      return -ENOMEM;
   }
   memcpy(j->inline_data, data, size);
   j->inline_len = (uint32_t)size;
   j->push[0] = 0;
   j->bind[0].va = 0;
   j->bind[0].size = (uint32_t)size;
   return 0;
}

int
mali_meta_list_add_bc(struct mali_meta_list *l, const struct mali_bc_job *bj)
{
   struct mali_meta_job *j = list_add(l, MALI_META_BC_FIRST + (int)bj->sh);
   if (!j)
      return -ENOMEM;
   memcpy(j->push, bj->push, sizeof bj->push);
   j->push_bytes = bj->push_bytes;
   for (unsigned i = 0; i < 2; i++)
      j->bind[i] = (struct mali_meta_bind){ bj->bind[i].va, bj->bind[i].size };
   j->nbind = bj->nbind;
   memcpy(j->groups, bj->groups, sizeof j->groups);
   return 0;
}

/* ---------------------------------------------------------------- blit */
int
mali_meta_plan_blit(struct mali_meta_list *l, const struct mali_meta_blit *b)
{
   if (b->ncomp < 1 || b->ncomp > 4 || b->clamp[2] <= b->clamp[0] || b->clamp[3] <= b->clamp[1] || !b->src_w || !b->src_h)
      return -EINVAL;
   if (b->mode != 2 && (b->s[0] == b->s[2] || b->s[1] == b->s[3] || b->d[0] == b->d[2] || b->d[1] == b->d[3]))
      return -EINVAL;
   if (b->mode == 2 && (b->nsamples < 1 || b->nsamples > 16))
      return -EINVAL;
   uint32_t bpp = (b->f32 ? 4 : 1) * b->ncomp, sbpp = b->generic ? b->sbs : bpp, dbpp = b->generic ? b->dbs : bpp;
   if (b->generic && (!b->sbs || !b->dbs || b->cls > 2 || (b->cls && b->mode != 0)))
      return -EINVAL;
   uint64_t soff, doff;
   struct mali_meta_bind sb, db;
   uint32_t ns = b->mode == 2 ? b->nsamples : b->mode == 1 && b->zw != 0.0f ? 2 : 1;
   uint64_t send = b->src.off + (uint64_t)(ns - 1) * b->sample_stride + (uint64_t)(b->src_h - 1) * b->src.row + (uint64_t)b->src_w * sbpp;
   uint64_t dend = b->dst.off + (uint64_t)(b->clamp[3] - 1) * b->dst.row + (uint64_t)b->clamp[2] * dbpp;
   int r = bind_surf(&b->src, send, &soff, &sb);
   if (r)
      return r;
   r = bind_surf(&b->dst, dend, &doff, &db);
   if (r)
      return r;
   struct mali_meta_job *j = list_add(l, MALI_META_BLIT);
   if (!j)
      return -ENOMEM;
   uint32_t *p = j->push;
   p[0] = (uint32_t)soff;
   p[1] = b->src.row;
   p[2] = b->sample_stride;
   p[3] = (uint32_t)doff;
   p[4] = b->dst.row;
   for (int i = 0; i < 4; i++) {
      p[6 + i] = (uint32_t)b->s[i];
      p[10 + i] = (uint32_t)b->d[i];
      p[14 + i] = (uint32_t)b->clamp[i];
   }
   p[18] = b->src_w;
   p[19] = b->src_h;
   p[20] = (b->ncomp - 1) | (b->f32 ? 4u : 0) | (b->swap_rb ? 8u : 0);
   if (b->generic) {
      p[20] = 16;
      p[5] = b->sbs | b->dbs << 8 | b->cls << 16 | (b->ssrgb ? 1u << 20 : 0) | (b->dsrgb ? 1u << 21 : 0);
      for (int i = 0; i < 4; i++) {
         p[24 + i] = b->sch[i];
         p[28 + i] = b->dch[i];
      }
   }
   p[21] = (uint32_t)b->mode;
   p[22] = b->nsamples;
   if (b->mode == 1 && b->zw != 0.0f)
      memcpy(&p[22], &b->zw, 4);
   p[23] = (uint32_t)(b->clamp[2] - b->clamp[0]);
   j->push_bytes = 128;
   j->bind[0] = sb;
   j->bind[1] = db;
   j->nbind = 2;
   set_groups(j, (uint64_t)p[23] * (uint32_t)(b->clamp[3] - b->clamp[1]));
   return 0;
}

/* ---------------------------------------------------------------- query copy */
int
mali_meta_plan_qcopy(struct mali_meta_list *l, const struct mali_meta_surf *pool, const struct mali_meta_surf *dst, uint32_t first,
                     uint32_t count, uint64_t dst_off, uint64_t dst_stride, uint32_t flags)
{
   if (!count)
      return 0;
   bool is64 = flags & 1;
   if ((dst_off & (is64 ? 7 : 3)) || (dst_stride & (is64 ? 7 : 3)) || (count > 1 && !dst_stride))
      return -EINVAL;
   uint32_t elem = (is64 ? 8 : 4) + ((flags & 4) ? (is64 ? 8 : 4) : 0);
   struct mali_meta_surf d = *dst;
   d.off += dst_off;
   uint64_t pend = pool->off + (uint64_t)(first + count) * 16, doff, poff;
   uint64_t dend = d.off + (uint64_t)(count - 1) * dst_stride + elem;
   struct mali_meta_bind pb, db;
   int r = bind_surf(pool, pend, &poff, &pb);
   if (r)
      return r;
   r = bind_surf(&d, dend, &doff, &db);
   if (r)
      return r;
   struct mali_meta_job *j = list_add(l, MALI_META_QCOPY);
   if (!j)
      return -ENOMEM;
   j->push[0] = first + (uint32_t)(poff / 16);
   j->push[1] = count;
   j->push[2] = (uint32_t)doff;
   j->push[3] = (uint32_t)dst_stride;
   j->push[4] = flags;
   j->push_bytes = 20;
   j->bind[0] = pb;
   j->bind[1] = db;
   j->nbind = 2;
   set_groups(j, count);
   j->groups[1] = 1;
   if (j->groups[0] > 65535 && 0)
      return -EINVAL;
   return 0;
}

/* ---------------------------------------------------------------- device seam, shader cache, materialise */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct mali_meta_dev g_dev;
static bool g_dev_set;
static unsigned g_gen;   /* bumped by mali_meta_set_dev: a compile that finished for an older device is dropped */
static pthread_t g_pw;
static bool g_pw_run;
static struct {
   bool ok;
   struct mali_compiled c;
   uint64_t va;
} g_sh[MALI_META_NSHADERS];

void
mali_meta_set_dev(const struct mali_meta_dev *dev)
{
   pthread_mutex_lock(&g_lock);
   bool join = g_pw_run;
   g_pw_run = false;
   pthread_t pw = g_pw;
   pthread_mutex_unlock(&g_lock);
   if (join)
      pthread_join(pw, NULL);   /* the worker sees the new generation and drops its result; never held under g_lock */
   pthread_mutex_lock(&g_lock);
   /* Bump the generation in the same critical section that installs the device. Bumping it in the earlier
    * section left a window where g_gen was new but g_dev still described the old device: a compile there
    * captured the new generation with the old arch, and the install did not change g_gen again, so its
    * publication check passed and an old-arch binary was uploaded to the new device. */
   g_gen++;
   for (int i = 0; i < MALI_META_NSHADERS; i++)
      if (g_sh[i].ok) {
         mali_compiled_free(&g_sh[i].c);
         g_sh[i].ok = false;
      }
   g_dev_set = dev != NULL;
   if (dev)
      g_dev = *dev;
   pthread_mutex_unlock(&g_lock);
}

void *
mali_meta_dev_alloc(size_t size, size_t align, uint64_t *va)
{
   pthread_mutex_lock(&g_lock);
   void *p = g_dev_set && g_dev.alloc ? g_dev.alloc(g_dev.user, size, align, va) : NULL;
   pthread_mutex_unlock(&g_lock);
   return p;
}

void
mali_meta_dev_free(void *cpu, uint64_t va, size_t size)
{
   pthread_mutex_lock(&g_lock);
   if (g_dev_set && g_dev.free)
      g_dev.free(g_dev.user, cpu, va, size);
   pthread_mutex_unlock(&g_lock);
}

void
mali_meta_mem_release(struct mali_meta_mem *m)
{
   pthread_mutex_lock(&g_lock);
   for (unsigned i = 0; i < m->n; i++)
      if (g_dev_set && g_dev.free)
         g_dev.free(g_dev.user, m->a[i].cpu, m->a[i].va, m->a[i].size);
   pthread_mutex_unlock(&g_lock);
   free(m->a);
   memset(m, 0, sizeof *m);
}

static void *
mem_alloc(struct mali_meta_mem *m, size_t size, size_t align, uint64_t *va)
{
   if (m->n == m->cap) {
      unsigned nc = m->cap ? m->cap * 2 : 8;
      void *na = realloc(m->a, nc * sizeof *m->a);
      if (!na)
         return NULL;
      m->a = na;
      m->cap = nc;
   }
   void *p = g_dev.alloc(g_dev.user, size, align, va);
   if (!p)
      return NULL;
   m->a[m->n].cpu = p;
   m->a[m->n].va = *va;
   m->a[m->n].size = size;
   m->n++;
   return p;
}

static const struct { const uint32_t *spv; size_t words; unsigned nbind, push; } g_src[MALI_META_NSHADERS] = {
   [MALI_META_COPY] = { meta_copy_spv, sizeof meta_copy_spv / 4, 2, 64 },
   [MALI_META_BLIT] = { meta_blit_spv, sizeof meta_blit_spv / 4, 2, 128 },
   [MALI_META_QCOPY] = { meta_qcopy_spv, sizeof meta_qcopy_spv / 4, 2, 20 },
};

/* Descriptor-set layout the meta shaders are compiled against: set 0 = two storage buffers. */
static void
meta_layout(struct mali_compile_layout *lay, struct mali_lbinding b[2], unsigned push)
{
   memset(lay, 0, sizeof *lay);
   for (unsigned i = 0; i < 2; i++)
      b[i] = (struct mali_lbinding){ MALI_DT_STORAGE_BUFFER, 1, i, 1, 0, MALI_NO_IMM_SAMPLER, 0, 0, 0 };
   lay->nsets = 1;
   lay->set[0] = (struct mali_lset){ 2, 2, 0, b, NULL };
   lay->push_size = push;
}

/* Compiles outside g_lock (a first-use compile is tens to hundreds of ms and g_lock also guards every meta/query/event allocation),
 * publishes under it. Two racing first uses may both compile; the loser's result is freed. */
static int
shader_get(enum mali_meta_shader sh, const struct mali_compiled **c, uint64_t *va)
{
   /* A generation change does not mean "no device": set(A) after set(B) is legal while A is still recording. Retry a
    * small number of times against the device that is installed now; only a cleared device is -ENODEV. */
   for (unsigned attempt = 0;; attempt++) {
   pthread_mutex_lock(&g_lock);
   if (!g_dev_set) {
      pthread_mutex_unlock(&g_lock);
      return -ENODEV;
   }
   if (g_sh[sh].ok) {
      *c = &g_sh[sh].c;
      *va = g_sh[sh].va;
      pthread_mutex_unlock(&g_lock);
      return 0;
   }
   const unsigned gen = g_gen, arch = g_dev.arch;
   pthread_mutex_unlock(&g_lock);

   struct mali_compile_layout lay;
   struct mali_lbinding b[2];
   char err[256] = "";
   struct mali_compiled comp;
   const uint32_t *spv = g_src[sh].spv;
   size_t words = g_src[sh].words, push = g_src[sh].push;
   if (sh >= MALI_META_BC_FIRST) {
      spv = mali_bc_spirv((enum mali_bc_shader)(sh - MALI_META_BC_FIRST), &words);
      push = MALI_BC_PUSH_WORDS * 4;
      if (!spv)
         return -EINVAL;
   }
   meta_layout(&lay, b, push);
   struct mali_compile_opts o = { 0 };
   o.arch = arch;
   o.stage = MALI_STAGE_COMPUTE;
   o.spirv = spv;
   o.spirv_words = words;
   o.layout = &lay;
   o.fau_reserved = (push + 3) / 4;   /* compute: the push block is copied to FAU word 0 on, immediates go after it */
   if (mali_compile(&o, &comp, err, sizeof err) < 0) {
      fprintf(stderr, "mali_meta: compile of shader %d failed: %s\n", sh, err);
      return -EINVAL;
   }

   pthread_mutex_lock(&g_lock);
   int r = 0;
   if (!g_dev_set || g_gen != gen) {
      mali_compiled_free(&comp);
      if (g_dev_set && attempt < 3) {   /* device swapped underneath: recompile for the installed one */
         pthread_mutex_unlock(&g_lock);
         continue;
      }
      r = -ENODEV;
   } else if (g_sh[sh].ok) {
      mali_compiled_free(&comp);
   } else if (g_dev.upload_shader(g_dev.user, comp.bin, comp.bin_size, &g_sh[sh].va) < 0) {
      mali_compiled_free(&comp);
      r = -ENOMEM;
   } else {
      g_sh[sh].c = comp;
      g_sh[sh].ok = true;
   }
   if (!r) {
      *c = &g_sh[sh].c;
      *va = g_sh[sh].va;
   }
   pthread_mutex_unlock(&g_lock);
   return r;
   }
}

int
mali_meta_shader_get(enum mali_meta_shader sh, const struct mali_compiled **c, uint64_t *va)
{
   return shader_get(sh, c, va);
}

/* Compile the built-in meta shaders (copy, blit, qcopy) on a background thread so the first blit/copy of a frame does not hitch.
 * Opt-in (the queue calls it after mali_meta_set_dev); MALI_META_PREWARM=0 disables. mali_meta_set_dev joins it. */
static void *
prewarm_main(void *unused)
{
   const struct mali_compiled *c;
   uint64_t va;
   for (int i = 0; i <= MALI_META_QCOPY; i++)
      shader_get(i, &c, &va);
   return NULL;
}

void
mali_meta_prewarm(void)
{
   const char *e = getenv("MALI_META_PREWARM");
   if (e && e[0] == '0')
      return;
   pthread_mutex_lock(&g_lock);
   if (g_dev_set && !g_pw_run && pthread_create(&g_pw, NULL, prewarm_main, NULL) == 0)
      g_pw_run = true;
   pthread_mutex_unlock(&g_lock);
}

/* Hardware Buffer descriptor (same bytes as mali_pack_buffer(): w0 = type 9 | 1<<4, w1 = size, w2/w3 = va; tests compare them). */
static void
pack_buffer(uint32_t w[8], uint64_t va, uint32_t size)
{
   memset(w, 0, 32);
   w[0] = 9u | (1u << 4);
   w[1] = size;
   w[2] = (uint32_t)va;
   w[3] = (uint32_t)(va >> 32);
}

int
mali_meta_materialize(const struct mali_meta_job *j, struct mali_meta_mem *mem, struct mali_dispatch *out)
{
   const struct mali_compiled *c;
   uint64_t cs_va;
   struct mali_meta_bind bind[2] = { j->bind[0], j->bind[1] };
   int r;

   /* shader_get releases g_lock before it returns, so mali_meta_set_dev can free the cached shader and install another
    * device in between. Revalidate under g_lock before allocating anything: a shader/VA from an older device must not
    * be paired with descriptor memory from the one installed now (a foreign VA, and a freed mali_compiled). Retry, and
    * take the VA from the live cache entry rather than from the copy shader_get returned. */
   for (unsigned attempt = 0;; attempt++) {
   r = shader_get(j->sh, &c, &cs_va);
   if (r)
      return r;
   pthread_mutex_lock(&g_lock);
   if (!g_sh[j->sh].ok || c != &g_sh[j->sh].c) {
      pthread_mutex_unlock(&g_lock);
      if (attempt < 3)
         continue;
      return -ENODEV;   /* the device keeps changing under us: no stable shader to record with */
   }
   cs_va = g_sh[j->sh].va;
   if (j->inline_data) {
      uint64_t va;
      void *p = mem_alloc(mem, (j->inline_len + 63) & ~63u, 64, &va);
      if (!p) {
         pthread_mutex_unlock(&g_lock);
         return -ENOMEM;
      }
      memcpy(p, j->inline_data, j->inline_len);
      bind[0].va = va;
   }
   uint64_t set_va;
   uint32_t *set = mem_alloc(mem, 64, 64, &set_va);
   pthread_mutex_unlock(&g_lock);
   if (!set)
      return -ENOMEM;
   for (unsigned i = 0; i < j->nbind; i++)
      pack_buffer(set + i * 8, bind[i].va, bind[i].size);
   memset(out, 0, sizeof *out);
   out->cs = c;
   out->cs_va = cs_va;
   memcpy(out->push, j->push, j->push_bytes);
   out->nsets = 1;
   out->set_va[0] = set_va;
   out->set_bytes[0] = j->nbind * 32;
   memcpy(out->groups, j->groups, sizeof out->groups);
   return 0;
   }
}

/* R4: loadOp LOAD. A graphics "preload" draw (full-viewport triangle) re-emits the old contents of the (linear) attachment(s), read as storage
 * buffers, into the tile before the application's draws: colour through a colour output, depth/stencil through gl_FragDepth /
 * gl_FragStencilRefARB (force-late ZS, see mali_cs_emit.c). One compiled FS per kind, compiled once per process (arch of the installed device). */
static struct { bool ok; struct mali_compiled vs, fs; uint64_t vs_va, fs_va; } g_pre[MALI_PRELOAD_KINDS + 3];   /* + colour with an unsigned / signed integer colour output (arg bits 16..17 = 1 / 2), + the multisampled colour variant */
static struct { bool ok; struct mali_compiled vs; uint64_t vs_va; } g_pre_vs;

static int
preload_compile(unsigned kind)   /* kind < MALI_PRELOAD_KINDS: enum mali_preload_kind; MALI_PRELOAD_KINDS + 0/1: integer-output colour variants (U / S) */
{
   char err[256] = "";
   struct mali_lbinding b[2] = { { MALI_DT_STORAGE_BUFFER, 1, 0, 1, 0, MALI_NO_IMM_SAMPLER, 0, 0, 0 }, { MALI_DT_STORAGE_BUFFER, 1, 1, 1, 0, MALI_NO_IMM_SAMPLER, 0, 0, 0 } };
   struct mali_compile_layout lay;
   memset(&lay, 0, sizeof lay);
   lay.nsets = 1;
   unsigned nb = kind == MALI_PRELOAD_ZS ? 2 : 1;
   lay.set[0] = (struct mali_lset){ nb, nb, 0, b, NULL };
   lay.push_size = 16;
   struct mali_compile_opts o = { 0 };
   o.arch = g_dev.arch;
   if (!g_pre_vs.ok) {
      o.stage = MALI_STAGE_VERTEX;
      o.spirv = preload_vert_spv;
      o.spirv_words = sizeof preload_vert_spv / 4;
      o.strip_psiz = 1;
      if (mali_compile(&o, &g_pre_vs.vs, err, sizeof err) < 0) {
         fprintf(stderr, "mali_meta: preload VS compile failed: %s\n", err);
         return -EINVAL;
      }
      if (g_dev.upload_shader(g_dev.user, g_pre_vs.vs.bin, g_pre_vs.vs.bin_size, &g_pre_vs.vs_va) < 0)
         return -ENOMEM;
      g_pre_vs.ok = true;
   }
   const uint32_t *spv = kind == MALI_PRELOAD_KINDS ? preload_uint_frag_spv : kind == MALI_PRELOAD_KINDS + 1 ? preload_sint_frag_spv : kind == MALI_PRELOAD_KINDS + 2 ? preload_msaa_frag_spv :
                         kind == MALI_PRELOAD_COLOUR ? preload_frag_spv : kind == MALI_PRELOAD_Z ? preload_z_frag_spv :
                         kind == MALI_PRELOAD_S ? preload_s_frag_spv : preload_zs_frag_spv;
   size_t words = kind == MALI_PRELOAD_KINDS ? sizeof preload_uint_frag_spv : kind == MALI_PRELOAD_KINDS + 1 ? sizeof preload_sint_frag_spv : kind == MALI_PRELOAD_KINDS + 2 ? sizeof preload_msaa_frag_spv :
                  kind == MALI_PRELOAD_COLOUR ? sizeof preload_frag_spv : kind == MALI_PRELOAD_Z ? sizeof preload_z_frag_spv :
                  kind == MALI_PRELOAD_S ? sizeof preload_s_frag_spv : sizeof preload_zs_frag_spv;
   memset(&o, 0, sizeof o);
   o.arch = g_dev.arch;
   o.stage = MALI_STAGE_FRAGMENT;
   o.spirv = spv;
   o.spirv_words = words / 4;
   o.layout = &lay;
   o.link_vs = &g_pre_vs.vs.layout;
   o.sample_shading = kind == MALI_PRELOAD_KINDS + 2;   /* multisampled preload: the FS runs once per sample (gl_SampleID) */
   if (mali_compile(&o, &g_pre[kind].fs, err, sizeof err) < 0) {
      fprintf(stderr, "mali_meta: preload FS (kind %u) compile failed: %s\n", kind, err);
      return -EINVAL;
   }
   if (g_dev.upload_shader(g_dev.user, g_pre[kind].fs.bin, g_pre[kind].fs.bin_size, &g_pre[kind].fs_va) < 0)
      return -ENOMEM;
   g_pre[kind].ok = true;
   return 0;
}

/* va0/bytes0/row0: colour, depth, or stencil plane; va1/bytes1/row1: the stencil plane of MALI_PRELOAD_ZS. push words: row bytes (0), row
 * bytes (1) or kind flags, per-kind extra (see shaders). */
int
mali_meta_preload(struct mali_meta_mem *mem, enum mali_preload_kind kind, const struct mali_preload_src *src, struct mali_draw *d)
{
   pthread_mutex_lock(&g_lock);
   if (!g_dev_set) {
      pthread_mutex_unlock(&g_lock);
      return -ENODEV;
   }
   const bool msaa = kind == MALI_PRELOAD_COLOUR && src->samples > 1;
   unsigned slot = msaa ? MALI_PRELOAD_KINDS + 2 :
                   kind == MALI_PRELOAD_COLOUR && ((src->arg >> 16) & 3) ? MALI_PRELOAD_KINDS - 1 + ((src->arg >> 16) & 3) : (unsigned)kind;
   if (!g_pre[slot].ok) {
      int r = preload_compile(slot);
      if (r) {
         pthread_mutex_unlock(&g_lock);
         return r;
      }
   }
   uint64_t set_va, clr_va = 0;
   uint32_t *set = mem_alloc(mem, 64, 64, &set_va);
   uint32_t *clr = kind == MALI_PRELOAD_COLOUR && (src->arg & 255u) == 19u ? mem_alloc(mem, 16, 16, &clr_va) : NULL;
   pthread_mutex_unlock(&g_lock);
   if (!set || (kind == MALI_PRELOAD_COLOUR && (src->arg & 255u) == 19u && !clr))
      return -ENOMEM;
   if (clr) {
      memcpy(clr, src->clear, 16);
      pack_buffer(set, clr_va, 16);
   } else
      pack_buffer(set, src->va[0], src->bytes[0]);
   if (kind == MALI_PRELOAD_ZS)
      pack_buffer(set + 8, src->va[1], src->bytes[1]);
   memset(d, 0, sizeof *d);
   d->gfx.vs = &g_pre_vs.vs;
   d->gfx.fs = &g_pre[slot].fs;
   d->gfx.vs_va = g_pre_vs.vs_va;
   d->gfx.fs_va = g_pre[slot].fs_va;
   d->gfx.push_base[0] = 40;
   d->gfx.push_base[1] = g_dev.arch == 13 ? 24 : 0;
   d->gfx.depth_func = 7;   /* ALWAYS */
   uint32_t pc[4] = { src->row[0], src->row[1], src->arg, src->bpp };
   if (kind == MALI_PRELOAD_COLOUR) {
      pc[0] = src->row[0];
      pc[1] = src->arg;
      pc[2] = src->bpp;
      pc[3] = msaa ? src->sample_stride : 0;   /* pad: bytes per sample plane for the per-sample variant */
      if (msaa) {
         d->gfx.samples = src->samples;   /* the draw runs in the 4x pass... */
         d->gfx.per_sample = true;        /* ...with the FS evaluated per sample (DCD0 evaluate-per-sample) */
      }
   } else if (kind == MALI_PRELOAD_S) {
      pc[1] = src->row[0];   /* stencil-only variant reads its row from the srow word */
   }
   memcpy(d->push[1], pc, sizeof pc);
   d->nsets = 1;
   d->set_va[0] = set_va;
   d->set_bytes[0] = (kind == MALI_PRELOAD_ZS ? 2 : 1) * 32;
   d->count = 3;
   d->instance_count = 1;
   if (kind != MALI_PRELOAD_COLOUR) {
      /* depth/stencil only: colour write mask 0; Z through the depth test (ALWAYS + write), S through a REPLACE-always stencil state */
      d->fx.set = true;
      d->fx.front_ccw = true;
      d->fx.depth_clip = false;   /* panvk: depth_cull_enable = false for the ZS preload */
      d->fx.wmask = 0;
      d->fx.src_c = d->fx.src_a = 1;
      d->gfx.depth_test = (kind == MALI_PRELOAD_Z || kind == MALI_PRELOAD_ZS);
      d->gfx.depth_write = d->gfx.depth_test;
      if (kind == MALI_PRELOAD_S || kind == MALI_PRELOAD_ZS) {
         d->fx.st_test = true;
         d->fx.st_f = d->fx.st_b = (struct mali_st_face){ 7 /* ALWAYS */, 2, 2, 2 /* REPLACE */, 0xff, 0xff, 0 };
      }
   }
   return 0;
}

/* ---------------------------------------------------------------- software reference */
struct rb { uint8_t *p; uint32_t size; };

static uint32_t
rd32(const struct rb *b, uint32_t byteoff)
{
   if ((byteoff & 3) || (uint64_t)byteoff + 4 > b->size) {
      mali_meta_ref_oob++;
      return 0;
   }
   uint32_t v;
   memcpy(&v, b->p + byteoff, 4);
   return v;
}

static void
wr32(const struct rb *b, uint32_t byteoff, uint32_t v)
{
   if ((byteoff & 3) || (uint64_t)byteoff + 4 > b->size) {
      mali_meta_ref_oob++;
      return;
   }
   memcpy(b->p + byteoff, &v, 4);
}

static uint32_t
rd8(const struct rb *b, uint32_t o)
{
   return (rd32(b, o & ~3u) >> ((o & 3) * 8)) & 0xff;
}

static void
wr8(const struct rb *b, uint32_t o, uint32_t v)
{
   uint32_t sh = (o & 3) * 8, w = rd32(b, o & ~3u);
   wr32(b, o & ~3u, (w & ~(0xffu << sh)) | (v << sh));
}

static void
ref_copy(const struct mali_meta_job *j, const struct rb *src, const struct rb *dst)
{
   const uint32_t *p = j->push;
   uint32_t ex = p[6], ey = p[7], ez = p[8], bpt = p[9];
   uint64_t total = (uint64_t)ex * ey * ez;
   uint64_t inv = (uint64_t)j->groups[0] * j->groups[1] * 64;   /* invocations launched: the shader needs i < total only */
   for (uint64_t i = 0; i < inv && i < total; i++) {
      uint32_t x = (uint32_t)(i % ex), r = (uint32_t)(i / ex), y = r % ey, z = r / ey;
      uint32_t so = p[0] + z * p[2] + y * p[1] + x * bpt, dof = p[3] + z * p[5] + y * p[4] + x * bpt;
      if (p[10] & 1) {
         for (uint32_t w = 0; w < bpt / 4; w++)
            wr32(dst, dof + w * 4, (p[10] & 2) ? p[12 + (w & 3)] : rd32(src, so + w * 4));
      } else {
         for (uint32_t b = 0; b < bpt; b++)
            wr8(dst, dof + b, (p[10] & 2) ? (p[12 + ((b >> 2) & 3)] >> ((b & 3) * 8)) & 0xff : rd8(src, so + b));
      }
   }
   if (total > inv)
      mali_meta_ref_oob++;   /* grid too small */
}

static void
fetch(const struct mali_meta_job *j, const struct rb *src, int x, int y, uint32_t s, float v[4])
{
   const uint32_t *p = j->push;
   int sw = (int)p[18], sh = (int)p[19];
   uint32_t fmt = p[20], n = (fmt & 3) + 1;
   bool isf = fmt & 4;
   x = x < 0 ? 0 : x > sw - 1 ? sw - 1 : x;
   y = y < 0 ? 0 : y > sh - 1 ? sh - 1 : y;
   uint32_t o = p[0] + s * p[2] + (uint32_t)y * p[1] + (uint32_t)x * (isf ? 4 * n : n);
   v[0] = v[1] = v[2] = 0;
   v[3] = 1;
   for (uint32_t c = 0; c < n; c++) {
      if (isf) {
         uint32_t w = rd32(src, o + c * 4);
         memcpy(&v[c], &w, 4);
      } else {
         v[c] = (float)rd8(src, o + c) / 255.0f;
      }
   }
   if (fmt & 8) {
      float t = v[0];
      v[0] = v[2];
      v[2] = t;
   }
}

static void
ref_blit(const struct mali_meta_job *j, const struct rb *src, const struct rb *dst)
{
   const uint32_t *p = j->push;
   int sx0 = (int)p[6], sy0 = (int)p[7], sx1 = (int)p[8], sy1 = (int)p[9];
   int dx0 = (int)p[10], dy0 = (int)p[11], dx1 = (int)p[12], dy1 = (int)p[13];
   int cx0 = (int)p[14], cy0 = (int)p[15], cy1 = (int)p[17];
   uint32_t fmt = p[20], mode = p[21], ns = p[22], lw = p[23], n = (fmt & 3) + 1;
   bool isf = fmt & 4;
   uint64_t total = (uint64_t)lw * (uint32_t)(cy1 - cy0);
   if (total > (uint64_t)j->groups[0] * j->groups[1] * 64)
      mali_meta_ref_oob++;
   for (uint64_t i = 0; i < total; i++) {
      int ox = cx0 + (int)(i % lw), oy = cy0 + (int)(i / lw);
      float v[4], a[4], b[4], c[4], d[4];
      if (mode == 2) {
         v[0] = v[1] = v[2] = v[3] = 0;
         for (uint32_t s = 0; s < ns; s++) {
            fetch(j, src, ox - dx0 + sx0, oy - dy0 + sy0, s, a);
            for (int k = 0; k < 4; k++)
               v[k] += a[k];
         }
         for (int k = 0; k < 4; k++)
            v[k] /= (float)ns;
      } else {
         float u = ((float)ox + 0.5f - (float)dx0) / (float)(dx1 - dx0);
         float w = ((float)oy + 0.5f - (float)dy0) / (float)(dy1 - dy0);
         float fx = (float)sx0 + u * (float)(sx1 - sx0), fy = (float)sy0 + w * (float)(sy1 - sy0);
         if (mode == 0) {
            fetch(j, src, (int)floorf(fx), (int)floorf(fy), 0, v);
         } else {
            fx -= 0.5f;
            fy -= 0.5f;
            int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
            float ax = fx - (float)x0, ay = fy - (float)y0;
            fetch(j, src, x0, y0, 0, a);
            fetch(j, src, x0 + 1, y0, 0, b);
            fetch(j, src, x0, y0 + 1, 0, c);
            fetch(j, src, x0 + 1, y0 + 1, 0, d);
            float wz, a1[4], b1[4], c1[4], d1[4];
            memcpy(&wz, &ns, 4);   /* trilinear: the second slice (push[2] bytes on) weighted wz */
            if (wz != 0) {
               fetch(j, src, x0, y0, 1, a1);
               fetch(j, src, x0 + 1, y0, 1, b1);
               fetch(j, src, x0, y0 + 1, 1, c1);
               fetch(j, src, x0 + 1, y0 + 1, 1, d1);
            }
            for (int k = 0; k < 4; k++) {
               float t0 = a[k] * (1 - ax) + b[k] * ax, t1 = c[k] * (1 - ax) + d[k] * ax;
               v[k] = t0 * (1 - ay) + t1 * ay;
               if (wz != 0) {
                  t0 = a1[k] * (1 - ax) + b1[k] * ax;
                  t1 = c1[k] * (1 - ax) + d1[k] * ax;
                  v[k] = v[k] * (1 - wz) + (t0 * (1 - ay) + t1 * ay) * wz;
               }
            }
         }
      }
      uint32_t bpp = isf ? 4 * n : n;
      uint32_t o = p[3] + (uint32_t)oy * p[4] + (uint32_t)ox * bpp;
      for (uint32_t k = 0; k < n; k++) {
         if (isf) {
            uint32_t w;
            memcpy(&w, &v[k], 4);
            wr32(dst, o + k * 4, w);
         } else {
            float f = v[k] < 0 ? 0 : v[k] > 1 ? 1 : v[k];
            wr8(dst, o + k, (uint32_t)(f * 255.0f + 0.5f));
         }
      }
   }
}

static void
ref_qcopy(const struct mali_meta_job *j, const struct rb *pool, const struct rb *dst)
{
   const uint32_t *p = j->push;
   uint32_t flags = p[4];
   bool is64 = flags & 1;
   if (p[1] > (uint64_t)j->groups[0] * 64)
      mali_meta_ref_oob++;
   for (uint32_t i = 0; i < p[1]; i++) {
      uint32_t base = (p[0] + i) * 16;
      uint32_t avail = rd32(pool, base), lo = rd32(pool, base + 8), hi = rd32(pool, base + 12);
      uint32_t o = p[2] + i * p[3];
      if (avail || (flags & 8)) {
         if (is64) {
            wr32(dst, o, lo);
            wr32(dst, o + 4, hi);
            o += 8;
         } else {
            wr32(dst, o, hi ? 0xffffffffu : lo);
            o += 4;
         }
      } else {
         o += is64 ? 8 : 4;
      }
      if (flags & 4) {
         wr32(dst, o, avail ? 1 : 0);
         if (is64)
            wr32(dst, o + 4, 0);
      }
   }
}

void
mali_meta_ref_run(const struct mali_meta_job *j)
{
   struct rb s = { (uint8_t *)(uintptr_t)j->bind[0].va, j->bind[0].size };
   struct rb d = { (uint8_t *)(uintptr_t)j->bind[1].va, j->bind[1].size };
   if (j->inline_data) {
      s.p = j->inline_data;
      s.size = (j->inline_len + 3) & ~3u;
   }
   switch (j->sh) {
   case MALI_META_COPY: ref_copy(j, &s, &d); break;
   case MALI_META_BLIT: ref_blit(j, &s, &d); break;
   case MALI_META_QCOPY: ref_qcopy(j, &s, &d); break;
   default: break;
   }
}
