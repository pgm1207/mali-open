/* SPDX-License-Identifier: MIT */
/* M8: frame -> descriptors + CSF command stream, for v11 (RUN_IDVS) and v13 (RUN_IDVS2). Sequence and register maps are the
 * hardware-proven ones of libkbase tests/rung2.h / rung2_v13.h, generalised to per-draw state; every descriptor is built by a
 * libkbase packer. Rules kept from libkbase/INTERMITTENT.md and V13-NEXT2.md:
 *   - the caller's alloc is a rolling arena; job_begin (kb_arena_begin) runs once per frame
 *   - after the head FLUSH_CACHE2 there is always a WAIT on slot 0 before the first RUN_*
 *   - kb_wmb() before the submit hook
 *   - v13: FS FAU words 4,5 = blend descriptor words 2,3; FAU blocks sized by R2_FAU_BYTES (VS and FS blocks never overlap);
 *     tiler flags | env.tf_or (0x40000); heap context in r88; RUN_IDVS2(0,0); VS FAU count from the compiled shader
 *   - FAU pointers are written as two MOVE32 (a MOVE48 would drop the entry count in bits 63:56)
 * A shadow-register cache emits MOVEs only for changed registers (the CS ring is only 4 KiB). It is valid because each frame's CS
 * starts empty and every register is (re)written by the first draw. */
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include "mali_pipeline_layout_flat.h"

#include "kbase.h"
#include "kbase_cs.h"
#include "kbase_cs_v13.h"
#include "kbase_draw.h"
#include "kbase_draw_v13.h"
#include "kbase_fb.h"
#include "kbase_samples.h"
#include "mali_cs_emit.h"
#include "mali_event.h"

/* FAU block size in bytes: the qword count given to the hardware, rounded up to 64 B (V13-NEXT2.md). */
#define FAU_BYTES(words32) (((((size_t)(words32) + 1) / 2) * 8 + 63) & ~(size_t)63)

struct cs {
   uint64_t *v;
   unsigned n, cap;
   int err;
   uint32_t sh[128];
   uint8_t sv[16];   /* bitmap of known shadow registers */
};

static void
E(struct cs *c, uint64_t i)
{
   if (c->n == c->cap) {
      unsigned nc = c->cap ? c->cap * 2 : 256;
      uint64_t *nv = realloc(c->v, nc * sizeof *nv);
      if (!nv) {
         c->err = -ENOMEM;
         return;
      }
      c->v = nv;
      c->cap = nc;
   }
   c->v[c->n++] = i;
}

static bool
known(const struct cs *c, unsigned r, uint32_t v)
{
   return (c->sv[r >> 3] >> (r & 7) & 1) && c->sh[r] == v;
}
static void
set_known(struct cs *c, unsigned r, uint32_t v)
{
   c->sv[r >> 3] |= 1u << (r & 7);
   c->sh[r] = v;
}

static void
M32(struct cs *c, unsigned r, uint32_t v)
{
   if (known(c, r, v))
      return;
   E(c, kb_cs_move32(r, v));
   set_known(c, r, v);
}

static void
M48(struct cs *c, unsigned r, uint64_t v)
{
   uint32_t lo = (uint32_t)v, hi = (uint32_t)(v >> 32) & 0xffff;
   if (known(c, r, lo) && known(c, r + 1, hi))
      return;
   E(c, kb_cs_move48(r, v));
   set_known(c, r, lo);
   set_known(c, r + 1, hi);
}

static void
M64(struct cs *c, unsigned r, uint64_t v)
{
   M32(c, r, (uint32_t)v);
   M32(c, r + 1, (uint32_t)(v >> 32));
}

/* Registers of the two arches (kbase_cs.h KB_IDVS_*, kbase_cs_v13.h KB13_IDVS_*). */
struct regs {
   unsigned flush_id, tctx_copy, heap, sync_addr, sync_val;
};
static const struct regs R11 = { 72, 74, 64, 68, 70 };
static const struct regs R13 = { 80, 82, 88, 84, 86 };

/* ---- R4: fixed-function blend equation (port of panfrost lib/pan_blend.c to_mali_function for Vulkan enums) ---- */
struct bfn { unsigned a, na, b, nb, c, ic; };
struct bfac { unsigned c; bool inv; };

static bool
bfac_of(unsigned vk, struct bfac *o)
{
   switch (vk) {
   case 0: *o = (struct bfac){1, false}; return true;   /* ZERO */
   case 1: *o = (struct bfac){1, true}; return true;    /* ONE */
   case 2: case 3: *o = (struct bfac){2, vk == 3}; return true;      /* SRC_COLOR */
   case 4: case 5: *o = (struct bfac){3, vk == 5}; return true;      /* DST_COLOR */
   case 6: case 7: *o = (struct bfac){5, vk == 7}; return true;      /* SRC_ALPHA */
   case 8: case 9: *o = (struct bfac){6, vk == 9}; return true;      /* DST_ALPHA */
   case 10: case 11: case 12: case 13: *o = (struct bfac){7, vk & 1}; return true;   /* CONSTANT_COLOR / CONSTANT_ALPHA */
   default: return false;
   }
}

static bool
bfn_of(unsigned op, unsigned sf, unsigned df, struct bfn *f)
{
   struct bfac s, d;
   if (op > 2 || !bfac_of(sf, &s) || !bfac_of(df, &d))
      return false;
   memset(f, 0, sizeof *f);
   bool same_base = sf >= 2 && sf <= 13 && df >= 2 && df <= 13 && ((sf - 2) >> 1) == ((df - 2) >> 1);
   if (sf == 0) {
      f->a = 1; f->b = 3; f->nb = op == 1; f->ic = d.inv; f->c = d.c;
   } else if (sf == 1) {
      f->a = 2; f->b = 3;
      if (op == 1) f->nb = 1; else if (op == 2) f->na = 1;
      f->ic = d.inv; f->c = d.c;
   } else if (df == 0) {
      f->a = 1; f->b = 2; f->nb = op == 2; f->ic = s.inv; f->c = s.c;
   } else if (df == 1) {
      f->a = 3; f->b = 2;
      if (op == 1) f->na = 1; else if (op == 2) f->nb = 1;
      f->ic = s.inv; f->c = s.c;
   } else if (sf == df) {
      f->a = 1; f->ic = s.inv; f->c = s.c;
      if (op == 0) f->b = 1;
      else { f->b = 0; f->nb = op == 2; }
   } else if (op == 0 && ((sf == 4 && df == 2) || (sf == 2 && df == 4))) {
      f->a = 1; f->b = 3; f->c = 4;   /* src*dst + dst*src = dst * (2 src) */
   } else if (same_base && s.inv != d.inv) {
      f->a = 3; f->ic = s.inv; f->c = s.c;
      if (op == 0) f->b = 0;
      else if (op == 2) { f->b = 1; f->nb = 1; }
      else { f->b = 1; f->na = 1; }
   } else {
      return false;
   }
   return true;
}

static unsigned
bconst_bits(unsigned vk)
{
   return vk >= 10 && vk <= 11 ? 7u : vk >= 12 && vk <= 13 ? 8u : 0u;
}

int
mali_blend_equation(const struct mali_fixed *fx, uint32_t *eq, unsigned *cm)
{
   struct bfn c, a;
   if (!fx->blend_en) {
      *eq = 0x122u | (0x122u << 12);   /* replace: src */
      *cm = 0;
      return 0;
   }
   if (!bfn_of(fx->op_c, fx->src_c, fx->dst_c, &c) || !bfn_of(fx->op_a, fx->src_a, fx->dst_a, &a))
      return -1;
   uint32_t ec = c.a | (c.na << 3) | (c.b << 4) | (c.nb << 7) | (c.c << 8) | (c.ic << 11);
   uint32_t ea = a.a | (a.na << 3) | (a.b << 4) | (a.nb << 7) | (a.c << 8) | (a.ic << 11);
   *eq = ec | (ea << 12);
   *cm = bconst_bits(fx->src_c) | bconst_bits(fx->dst_c) | bconst_bits(fx->src_a) | bconst_bits(fx->dst_a);
   return 0;
}

/* Applies the colour attachment 0 fixed-function blend state to a blend descriptor made by kb_draw_blend_opaque_*(). Returns -1 when
 * the state cannot be expressed (inconsistent constants, unsupported equation). */
static int
apply_blend(uint32_t *bl, const struct mali_fixed *fx)
{
   uint32_t eq;
   unsigned cm;
   if (!fx->set || (!fx->blend_en && fx->wmask == 0xf))
      return 0;
   if (mali_blend_equation(fx, &eq, &cm))
      return -1;
   float cv = 0.0f;
   bool first = true;
   for (unsigned i = 0; i < 4; i++) {
      if (!(cm & (1u << i)))
         continue;
      if (first) { cv = fx->bconst[i]; first = false; }
      else if (fx->bconst[i] != cv)
         return -1;   /* the descriptor holds one constant */
   }
   if (cv < 0.0f) cv = 0.0f;
   if (cv > 1.0f) cv = 1.0f;
   bl[0] = (bl[0] & 0xffffu) | 1u | ((uint32_t)(cv * 65280.0f + 0.5f) << 16);
   bl[1] = eq | ((uint32_t)(fx->wmask & 0xf) << 28);
   bl[2] = (bl[2] & ~3u) | 2u;
   return 0;
}

struct ddesc {   /* per-draw GPU blocks */
   uint64_t spd_p, spd_v, spd_f, vst, fst, bl, zsd, vfau, ffau;
   uint8_t *vsset;           /* the driver set (VS and FS share it and one SRT: the FS reads only the all-zero dummy sampler), reused by every draw with the same static state */
   uint64_t vsset_va;
   uint32_t bl23[2];         /* words 2..3 of the blend descriptor (CPU copy: the arena is write-combined, never read back) */
   bool static_same;         /* this draw's static state (pipeline, FX, bound sets, firstInstance) equals the previous draw's (s1) */
   bool dyn_same;            /* ... and so do its dynamic offsets (s2) */
};

static void *
A(const struct mali_hw *hw, size_t n, size_t al, uint64_t *va)
{
   return hw->alloc(hw->user, n, al, va);
}

static void *
Anz(const struct mali_hw *hw, size_t n, size_t al, uint64_t *va)
{
   return hw->alloc_nz ? hw->alloc_nz(hw->user, n, al, va) : hw->alloc(hw->user, n, al, va);
}

static bool
check_dirty(void)
{
   static int c = -1;
   if (c < 0)
      c = getenv("MALI_DIRTY_CHECK") != NULL;
   return c;
}

/* Draws that differ only in counts / dynamic offsets / push constants / viewport share the pipeline-derived blocks (shader program
 * descriptors, blend, depth-stencil): `a` is the previous draw of the frame. Everything these blocks are built from is compared. */
static bool
same_static(const struct mali_draw *a, const struct mali_draw *b)
{
   if (!a || memcmp(&a->gfx, &b->gfx, sizeof a->gfx) || memcmp(&a->fx, &b->fx, sizeof a->fx))
      return false;
   if (a->nsets != b->nsets || a->ndyn != b->ndyn || a->first_instance != b->first_instance)
      return false;
   if (memcmp(a->vb_va, b->vb_va, sizeof a->vb_va) || memcmp(a->vb_size, b->vb_size, sizeof a->vb_size) ||
       (a->gfx.dyn_stride && memcmp(a->vb_stride, b->vb_stride, sizeof a->vb_stride)))   /* a stride change between draws rebuilds the shared driver set */
      return false;
   return !memcmp(a->set_va, b->set_va, b->nsets * sizeof b->set_va[0]) && !memcmp(a->set_bytes, b->set_bytes, b->nsets * sizeof b->set_bytes[0]);
}

static int
build_draw_descs(const struct mali_hw *hw, const struct mali_frame *f, const struct mali_draw *d, struct ddesc *o,
                 const struct mali_draw *pd, const struct ddesc *po)
{
   const struct mali_env *env = &hw->env;
   const struct mali_gfx_state *g = &d->gfx;
   const struct mali_compiled *vs = g->vs, *fs = g->fs;
   bool v13 = env->arch == 13;
   uint64_t va_vfau, va_ffau;
   uint32_t *dyn = NULL;
   uint64_t va_dyn = 0;
   uint32_t *spd_p, *spd_v = NULL, *spd_f, *vst, *bl, *zsd, *vfau, *ffau;
   uint8_t *vsset;

   if (!vs || !fs || !vs->bin_size || !fs->bin_size)
      return -EINVAL;
   if (d->indexed && d->index_size != 1 && d->index_size != 2 && d->index_size != 4)
      return -EINVAL;

   unsigned nvb = 0;
   for (unsigned i = 0; i < g->nattr; i++)
      if (g->attr[i].binding >= nvb)
         nvb = g->attr[i].binding + 1;
   if (nvb > MALI_MAX_VB)
      return -EINVAL;
   unsigned vs_entries = KB_VS_ATTRS + 1 + (nvb ? nvb : 1);
   const unsigned nres = d->ndyn ? MALI_DYN_TABLE + 1 : 1 + d->nsets;   /* SRT entries: driver set, user sets, (dynamic table at MALI_DYN_TABLE) */
   const unsigned res_alloc = (((nres + 3) & ~3u) * KB_RES_SIZE + 63u) & ~63u;   /* 64-aligned: the dynamic table follows it in the same block */
   const unsigned dyn_alloc = d->ndyn ? ((d->ndyn * KB_DESC_SET_STRIDE) + 63u) & ~63u : 0;

   /* s1/s2/s3 from the record-time dirty mask (no state compares). first_instance is a per-draw parameter. */
   const bool s1 = po && pd && !(d->dirty & (MALI_D_PIPE | MALI_D_FX | MALI_D_BIND)) && pd->first_instance == d->first_instance;
   /* s1: static state incl. first_instance, vertex buffers and bound sets: shader programs, blend, depth-stencil and the driver set are reused.
    * s2: also the same dynamic offsets: the SRT and dynamic table are reused too. */
   const bool s2 = s1 && !(d->dirty & MALI_D_DYNBUF);
   o->static_same = s1;   /* emit_draw_regs skips the registers derived from the static state when the previous draw set them */
   o->dyn_same = s2;
   const bool s3 = s1 && pd->first_vertex == d->first_vertex && pd->draw_id == d->draw_id && !(d->dirty & (MALI_D_VP | MALI_D_PUSH));   /* FAUs reusable */
   if (check_dirty()) {   /* trap: whatever the mask lets us reuse must be byte-equal to what the compares would have reused */
      const bool o1 = po && same_static(pd, d);
      const bool o2 = o1 && !memcmp(pd->dyn_desc, d->dyn_desc, d->ndyn * sizeof d->dyn_desc[0]);
      const bool o3 = o1 && pd->first_vertex == d->first_vertex && pd->draw_id == d->draw_id && !memcmp(pd->vp, d->vp, sizeof d->vp) &&
                      !memcmp(pd->push, d->push, sizeof d->push);
      if ((s1 && !o1) || (s2 && !o2) || (s3 && !o3)) {
         fprintf(stderr, "MALI_DIRTY_CHECK: mask 0x%x reuses what changed (s1 %d/%d s2 %d/%d s3 %d/%d)\n", d->dirty, s1, o1, s2, o2, s3, o3);
         abort();
      }
   }
   const unsigned vs_used = KB_VS_ATTRS + 1 + (nvb ? nvb : 1);   /* entries the vertex shader can read below the dummy sampler */

   if (s1) {
      o->spd_p = po->spd_p; o->spd_v = po->spd_v; o->spd_f = po->spd_f;
      o->bl = po->bl; o->zsd = po->zsd; o->bl23[0] = po->bl23[0]; o->bl23[1] = po->bl23[1];
      spd_p = spd_v = spd_f = bl = zsd = NULL;
   } else {
      spd_p = A(hw, KB_SPD_SIZE, 32, &o->spd_p);
      if (!v13)
         spd_v = A(hw, KB_SPD_SIZE, 32, &o->spd_v);
      spd_f = A(hw, KB_SPD_SIZE, 32, &o->spd_f);
      bl = A(hw, KB_BLEND_SIZE, 64, &o->bl);
      zsd = A(hw, KB_ZSD_SIZE, 32, &o->zsd);
   }
   if (s1) {
      o->vsset = po->vsset;
      o->vsset_va = po->vsset_va;
      vsset = NULL;
   } else {
      /* the driver set is sparse: only the attribute / vertex-buffer entries and the dummy sampler (zero) are read */
      vsset = Anz(hw, vs_entries * KB_DESC_SET_STRIDE, 64, &o->vsset_va);
      o->vsset = vsset;
      if (!vsset)
         return -ENOMEM;
   }
   if (s2) {
      o->vst = po->vst;
      o->fst = po->fst;
      vst = NULL;
   } else {
      /* The resource table and the dynamic table are one block: one bump instead of two, and the 16-byte SRT entry that points at the
       * table sits next to the table itself. Not zeroed (Anz): every entry the hardware reads is written below (entry 0, one per bound
       * user set, and MALI_DYN_TABLE), and the gaps are never read (kb_draw_srt_ptr's count only bounds the reads). The arena is
       * uncached on the device, where a zeroing memset here costs more per draw than everything this function does afterwards. */
      vst = Anz(hw, res_alloc + dyn_alloc, 64, &o->vst);
      if (!vst)
         return -ENOMEM;
      o->fst = o->vst;
      if (dyn_alloc) {
         dyn = (uint32_t *)((uint8_t *)vst + res_alloc);
         va_dyn = o->vst + res_alloc;
      }
   }
   if (s3) {
      o->vfau = po->vfau; o->ffau = po->ffau;
      vfau = ffau = NULL;
      va_vfau = va_ffau = 0;
   } else {
      vfau = A(hw, FAU_BYTES(vs->fau_count), 64, &va_vfau);
      ffau = A(hw, FAU_BYTES(fs->fau_count), 64, &va_ffau);
   }
   if ((!s1 && (!spd_p || (!v13 && !spd_v) || !spd_f || !bl || !zsd)) || (!s3 && (!vfau || !ffau)))
      return -ENOMEM;
   if (!s3) {
      o->vfau = kb_draw_fau_ptr(va_vfau, vs->fau_count);
      o->ffau = kb_draw_fau_ptr(va_ffau, fs->fau_count);
   }
   if (s2 && s3)
      return 0;
   (void)vs_used;

   /* shader programs */
   if (s1) {
      /* shared with the previous draw */
   } else if (v13) {
      kb_v13_spd(spd_p, 3, g->vs_va, vs->work_reg_count, vs->preload, 0);
      kb_v13_spd(spd_f, 2, g->fs_va, fs->work_reg_count, fs->preload, 0);
   } else {
      kb_draw_spd(spd_p, 3, g->vs_va, vs->work_reg_count, vs->preload);
      kb_draw_spd_vs_varying(spd_v, g->vs_va + vs->vs_secondary_offset, vs->vs_secondary_work_reg_count,
                             vs->vs_secondary_preload);
      kb_draw_spd(spd_f, 2, g->fs_va, fs->work_reg_count, fs->preload);
   }

   if (!s1) {
      /* driver set: attributes 0..15 (unused ones filled), dummy sampler at entry 16 (all zero), buffer descriptors from entry 17 (vertex binding b = 17 + b) */
      memset(vsset, 0, vs_used * KB_DESC_SET_STRIDE);
      for (unsigned i = 0; i < KB_VS_ATTRS; i++) {
         uint32_t *e = (uint32_t *)(vsset + i * KB_DESC_SET_STRIDE);
         if (i < g->nattr)
         {
            kb_draw_attr_1d(e, g->attr[i].pixfmt, 0, KB_VS_ATTRS + 1 + g->attr[i].binding, g->attr[i].offset,
                            g->dyn_stride && !g->attr[i].zero_div ? d->vb_stride[g->attr[i].binding] : g->attr[i].stride);
            if (g->attr[i].instance) {
               /* Attribute Frequency = Instance, offset enable off, POT/NPOT divisor (libkbase kb_draw_attr_instance, divisor 1 = plain instance rate) */
               kb_draw_attr_instance((uint32_t *)e, g->attr[i].divisor);
               /* no first_instance shift: the hardware adds the draw's INSTANCE_OFFSET itself (hardware: with the shift, firstInstance k drew instance 2k on v10 and v13) */
            }
         }
         else
            kb_draw_attr_unused(e);
      }
      for (unsigned b = 0; b < nvb; b++)
         kb_draw_buffer((uint32_t *)(vsset + (KB_VS_ATTRS + 1 + b) * KB_DESC_SET_STRIDE), d->vb_va[b], d->vb_size[b]);
      memset(vsset + MALI_DUMMY_SAMPLER_IDX * KB_DESC_SET_STRIDE, 0, KB_DESC_SET_STRIDE);   /* dummy sampler: must be all zero */
   }
   if (!s2) {
      /* resource table: entry 0 = driver set, 1.. = bound user sets, MALI_DYN_TABLE = the dynamic buffers (a table of its own); the entries between are never read */
      kb_draw_resource(vst, o->vsset_va, vs_entries * KB_DESC_SET_STRIDE, 1);
      for (unsigned s = 0; s < d->nsets; s++)
         kb_draw_resource(vst + (1 + s) * (KB_RES_SIZE / 4), d->set_va[s], d->set_bytes[s], 1);
      if (d->ndyn) {
         memcpy(dyn, d->dyn_desc, d->ndyn * KB_DESC_SET_STRIDE);
         kb_draw_resource(vst + MALI_DYN_TABLE * (KB_RES_SIZE / 4), va_dyn, d->ndyn * KB_DESC_SET_STRIDE, 1);
      }
   }
   if (s1)
      goto fau;
   uint32_t blt[4] = {0, 0, 0, 0};   /* built on the stack, stored once (the arena is write-combined: no read-modify-write there) */
   uint32_t *bl_arena = bl;
   bl = blt;
   if (f->colour.rt) {
      kb_draw_blend_opaque_fmt(bl, f->colour.rt->blend_pixfmt, f->colour.rt->ncomp);
      if (f->colour.rt->int_bits)
         bl[0] &= ~(1u << 11);   /* no rounding for integer targets */
   } else
      kb_draw_blend_opaque_rgba8(bl);
   if (g->blend_va && d->fx.blend_en) {
      /* blend shader (mode 0): word0 = load dest | enable | round-to-FB-precision (no sRGB RT formats are exposed), word3 = shader PC
       * (16-aligned, same 4G region as the FS) */
      bl[0] = 1u | (1u << 9) | (1u << 11);
      bl[1] = 0;
      bl[2] = 0;
      bl[3] = (uint32_t)g->blend_va;
   } else if (apply_blend(bl, &d->fx))
      return -EINVAL;
   memcpy(bl_arena, blt, sizeof blt);
   o->bl23[0] = blt[2];
   o->bl23[1] = blt[3];

   /* depth/stencil */
   if (g->depth_test && f->has_zs) {
      if (v13)
         kb_v13_zsd_depth(zsd, (enum kb_func)g->depth_func, g->depth_write);
      else
         kb_draw_zsd_depth(zsd, (enum kb_func)g->depth_func, g->depth_write);
   } else if (v13) {
      kb_v13_zsd_depth(zsd, KB_FUNC_ALWAYS, 0);
   } else {
      kb_draw_zsd_off(zsd);
   }

   /* Z/S written by the fragment shader (genxml Depth/stencil: Depth source 4:25 = Shader, Stencil from shader 0:30) */
   if (g->depth_test && f->has_zs && g->fs && g->fs->fs_writes_depth)
      zsd[4] = (zsd[4] & ~(3u << 25)) | (3u << 25);
   if (f->has_st && d->fx.set && d->fx.st_test && g->fs && g->fs->fs_writes_stencil)
      zsd[0] |= 1u << 30;

   /* stencil test (genxml Depth/stencil words 0..2, identical on v11 and v13; panvk build_zsd) */
   if (f->has_st && d->fx.set && d->fx.st_test) {
      static const uint8_t sop[8] = { 0 /* KEEP */, 2 /* ZERO */, 1 /* REPLACE */, 6 /* INCR_SAT */, 7 /* DECR_SAT */, 3 /* INVERT */, 4 /* INCR_WRAP */, 5 /* DECR_WRAP */ };
      const struct mali_st_face *fc = &d->fx.st_f, *bc = &d->fx.st_b;
      zsd[0] |= (1u << 31) | ((uint32_t)(fc->cmp & 7) << 4) | ((uint32_t)sop[fc->fail & 7] << 7) | ((uint32_t)sop[fc->dfail & 7] << 10) |
                ((uint32_t)sop[fc->pass & 7] << 13) | ((uint32_t)(bc->cmp & 7) << 16) | ((uint32_t)sop[bc->fail & 7] << 19) |
                ((uint32_t)sop[bc->dfail & 7] << 22) | ((uint32_t)sop[bc->pass & 7] << 25);
      zsd[1] = (uint32_t)fc->wmask | ((uint32_t)bc->wmask << 8) | ((uint32_t)fc->vmask << 16) | ((uint32_t)bc->vmask << 24);
      zsd[2] = (uint32_t)fc->ref | ((uint32_t)bc->ref << 8);
   }

   if (d->fx.set) {
      /* rasteriser depth state: clip (depth cull), clamp to the viewport depth range, bias */
      zsd[4] &= ~((1u << 22) | (3u << 23) | (1u << 28));
      zsd[4] |= (d->fx.depth_clip ? 1u << 22 : 0u) | ((d->fx.depth_clamp ? 2u : 1u) << 23) | (d->fx.bias_en ? 1u << 28 : 0u);
      memcpy(&zsd[5], &d->fx.bias_const, 4);
      memcpy(&zsd[6], &d->fx.bias_slope, 4);
      memcpy(&zsd[7], &d->fx.bias_clamp, 4);
   }

fau:;
   if (s3)
      return 0;   /* FAUs are independent of the dynamic offsets: shared with the previous draw even when its driver sets are not */
   /* FAU: VS = immediates + viewport sysvals (words 0..5) + push block; FS = immediates + push block */
   const float *vp = d->vp;
   kb_draw_fau_imm(vfau, (const uint32_t(*)[2])vs->imm, vs->nimm);
   kb_draw_fau_viewport(vfau, vp[2], vp[3], vp[5] - vp[4], vp[4]);
   if (vp[0] != 0.0f || vp[1] != 0.0f) {
      float ox = vp[0] + vp[2] * 0.5f, oy = vp[1] + vp[3] * 0.5f;
      memcpy(&vfau[3], &ox, 4);
      memcpy(&vfau[4], &oy, 4);
   }
   if (vs->fau_count >= 9) {   /* shaderDrawParameters sysvals (compile.c lower_sysvals): BaseVertex, BaseInstance, DrawIndex */
      vfau[6] = d->first_vertex;
      vfau[7] = d->first_instance;
      vfau[8] = d->draw_id;
   }
   kb_draw_fau_imm(ffau, (const uint32_t(*)[2])fs->imm, fs->nimm);
   for (int st = 0; st < 2; st++) {
      const struct mali_compiled *c = st ? fs : vs;
      uint32_t *fau = st ? ffau : vfau;
      size_t room = FAU_BYTES(c->fau_count);
      unsigned n = c->push_const_bytes > MALI_PUSH_MAX ? MALI_PUSH_MAX : c->push_const_bytes;
      if (g->push_base[st] + n > room)
         n = room > g->push_base[st] ? (unsigned)(room - g->push_base[st]) : 0;
      memcpy((uint8_t *)fau + g->push_base[st], d->push[st], n);
   }
   if (v13) {
      ffau[4] = o->bl23[0];
      ffau[5] = o->bl23[1];
   }
   if (g->blend_va && d->fx.blend_en)
      memcpy(ffau, d->fx.bconst, 16);   /* blend shader constants: FAU words 0..3 */
   return 0;
}

/* Triangle assembly for fillModeNonSolid. v = the n vertex numbers of the draw; prim = triangle topology (8 list, 10 strip, 12 fan); `restart`
 * = the primitive-restart index when restart applies (strip/fan, indexed), else NULL. Every ASSEMBLED triangle yields its 3 edges (mode 1:
 * a-b, b-c, c-a) or its 3 vertices (mode 2), so edges/vertices shared by neighbouring strip/fan triangles are emitted once per triangle (additive
 * blend / stencil increment see every polygon). Incomplete primitives (list tail, strip/fan segments under 3 vertices) and restart markers
 * produce nothing. `out` must hold 6 * n entries. Returns the number of entries written. */
size_t
mali_poly_assemble(unsigned mode, unsigned prim, const uint32_t *restart, const uint32_t *v, uint32_t n, uint32_t *out)
{
   size_t ni = 0;
#define TRI(a, b, c) do { if (mode == 1) { out[ni++] = (a); out[ni++] = (b); out[ni++] = (b); out[ni++] = (c); out[ni++] = (c); out[ni++] = (a); } \
                         else { out[ni++] = (a); out[ni++] = (b); out[ni++] = (c); } } while (0)
   if (prim == 8) {
      for (uint32_t t = 0; t + 2 < n; t += 3)
         TRI(v[t], v[t + 1], v[t + 2]);
   } else {
      uint32_t s = 0;
      while (s < n) {
         uint32_t e = s;
         while (e < n && !(restart && v[e] == *restart))
            e++;
         const uint32_t *w = v + s;
         for (uint32_t t = 0; t + 2 < e - s; t++) {
            if (prim == 10) TRI(w[t], w[t + 1], w[t + 2]);
            else TRI(w[0], w[t + 1], w[t + 2]);
         }
         s = e + 1;
      }
   }
#undef TRI
   return ni;
}

/* fillModeNonSolid, polygonMode LINE / POINT: Mali has no polygon mode, so the triangle draw becomes an indexed LINE LIST of its edges (LINE) or
 * POINT LIST of its vertices (POINT), per assembled triangle (mali_poly_assemble). Indices (u32, vertex numbers as the original draw would fetch
 * them, so vertexOffset/firstVertex stay in the draw) are generated here from the CPU view of the bound index buffer (the same submit-time read
 * the indirect draws use). Limits: cullMode and gl_FrontFacing are not honoured; Flat varyings come from each generated point's / line's own
 * provoking vertex, NOT the source triangle's (the spec wants the triangle's value for all of them). Index generation cannot express that:
 * a point has one vertex, and an edge a-b cannot take the value of a third vertex c; a fix needs a VS variant that re-fetches the triangle's
 * provoking vertex outputs. Visible only when Flat/integer varyings differ across a triangle in LINE/POINT mode. Returns 0, -ENOMEM, or -EINVAL (indexed without CPU view). */
static int
poly_lines(const struct mali_hw *hw, const struct mali_draw *d, struct mali_draw *o)
{
   const unsigned prim = d->gfx.poly_prim;
   const unsigned mode = d->gfx.poly_mode;
   const uint32_t n = d->count;
   uint32_t restart = d->index_size == 4 ? 0xffffffffu : d->index_size == 2 ? 0xffffu : 0xffu;
   if (d->indexed && (!d->index_cpu || (uint64_t)(d->first_index + n) * d->index_size > d->index_bytes))
      return -EINVAL;
   /* vertex numbers of the draw */
   uint32_t *v = malloc((size_t)(n ? n : 1) * sizeof *v);
   if (!v)
      return -ENOMEM;
   for (uint32_t i = 0; i < n; i++) {
      uint32_t k = d->first_index + i;
      v[i] = !d->indexed ? i : d->index_size == 4 ? ((const uint32_t *)(const void *)d->index_cpu)[k] :
             d->index_size == 2 ? ((const uint16_t *)(const void *)d->index_cpu)[k] : d->index_cpu[k];
   }
   /* worst case 6 indices per triangle */
   size_t cap = (size_t)(n ? n : 1) * 6;
   uint32_t *out = malloc(cap * sizeof *out);
   if (!out) {
      free(v);
      return -ENOMEM;
   }
   size_t ni = mali_poly_assemble(mode, prim, d->gfx.prim_restart && d->indexed ? &restart : NULL, v, n, out);
   free(v);
   *o = *d;
   o->gfx.prim = mode == 2 && ni ? 1 : 2;   /* nothing assembled in POINT mode: the zero-length line below, a point would cover a pixel */
   o->gfx.prim_restart = false;
   o->indexed = true;
   o->index_size = 4;
   o->first_index = 0;
   o->count = (uint32_t)ni;
   o->index_bytes = (uint32_t)(ni * 4);
   if (!ni) {
      out[0] = out[1] = 0;   /* nothing to draw: one degenerate (zero-length) line keeps the draw well-formed */
      ni = 2;
      o->count = 2;
      o->index_bytes = 8;
   }
   uint32_t *dst = A(hw, ni * 4, 64, &o->index_va);
   if (!dst) {
      free(out);
      return -ENOMEM;
   }
   memcpy(dst, out, ni * 4);
   free(out);
   return 0;
}

/* v11 IDVS SR (genxml v11.xml "IDVS SR"): not in libkbase's header. */
#define KB_IDVS_INDEX_BUFFER_SIZE 39
#define KB_IDVS_INDEX_BUFFER 54

/* PRIMITIVE_FLAGS bits 10:8 Index type (genxml v11 and v13: 0 none, 1 u8, 2 u16, 3 u32). */
static uint32_t
idx_type_bits(const struct mali_draw *d)
{
   if (!d->indexed)
      return 0;
   return (d->index_size == 1 ? 1u : d->index_size == 2 ? 2u : 3u) << 8;
}

/* Tiler flags: draw mode (bits 3:0), point size array FP16 (v11, bits 12:11), extended FIFO (bits 23:22 = 2) when the VS writes the
 * point size of a point list, primitive restart (19). `base` = the triangle flags. */
static uint32_t
prim_flag_bits(const struct mali_gfx_state *g, uint32_t base, bool v13)
{
   unsigned m = g->prim ? g->prim : 8;
   uint32_t f = (base & ~0xfu) | m;
   if (m == 1 && g->vs->vs_writes_point_size) {
      if (!v13)
         f |= 2u << 11;
      f = (f & ~(3u << 22)) | (2u << 22);
   }
   if (g->prim_restart && (m == 4 || m == 10 || m == 12))
      f |= 1u << 19;
   return f;
}

static uint32_t
prim_size_bits(const struct mali_gfx_state *g, const struct mali_fixed *fx)
{
   unsigned m = g->prim ? g->prim : 8;
   if (m != 1 && m != 2 && m != 4)
      return 0;
   float w = fx && fx->set && fx->line_width > 0.0f ? fx->line_width : 1.0f;   /* default point size / line width */
   if (g->poly_mode == 2)
      w = 1.0f;   /* polygonMode POINT: the VS point size is not read (stripped), size 1 */
   uint32_t b;
   memcpy(&b, &w, 4);
   return b;
}

/* The per-draw register file (everything but the FAU pointers, which come after the heap prologue like in the references). */
/* The per-draw parameters emit_draw_regs writes (everything the state groups do not carry). Kept for the previous draw so
 * the whole per-draw register packing can be skipped when a record renews none of the state groups and every parameter is
 * the same, which is the common case (same pipeline and viewport, only the dynamic offsets move). */
struct dwp {
   uint64_t index_va, occ_va;
   uint32_t count, instance_count, first_index, first_vertex, first_instance, occ_mode;
   uint32_t index_bytes, index_size, indexed;
};

static void
dwp_of(struct dwp *p, const struct mali_draw *d)
{
   p->index_va = d->index_va; p->occ_va = d->occ_va;
   p->count = d->count; p->instance_count = d->instance_count; p->first_index = d->first_index;
   p->first_vertex = d->first_vertex; p->first_instance = d->first_instance; p->occ_mode = d->occ_mode;
   p->index_bytes = d->index_bytes; p->index_size = d->index_size; p->indexed = d->indexed ? 1u : 0u;
}

/* True when every register emit_draw_regs writes for `b` is the value the previous draw wrote, except the two SRT pointers
 * (they follow the per-draw dynamic table). The record must renew no state group; the index-buffer fields are per-record
 * (vkCmdBindIndexBuffer sets no dirty bit), so they are part of the key. */
static bool
same_draw_words(const struct dwp *a, const struct mali_draw *b)
{
   if (!a || (b->dirty & (MALI_D_PIPE | MALI_D_FX | MALI_D_VP | MALI_D_SC | MALI_D_BIND)))
      return false;
   return a->count == b->count && a->instance_count == b->instance_count && a->first_index == b->first_index &&
          a->first_vertex == b->first_vertex && a->first_instance == b->first_instance && a->occ_mode == b->occ_mode &&
          a->occ_va == b->occ_va && a->indexed == (b->indexed ? 1u : 0u) && a->index_va == b->index_va &&
          a->index_bytes == b->index_bytes && a->index_size == b->index_size;
}

static void
emit_draw_regs(struct cs *c, const struct mali_env *env, const struct mali_frame *f, const struct mali_draw *d,
               const struct ddesc *o, uint64_t va_tsd, uint64_t va_tctx, bool same_words)
{
   const struct mali_gfx_state *g = &d->gfx;
   bool v13 = env->arch == 13;
   if (same_words) {
      /* nothing but the SRT pointers can have changed (see same_draw_words) */
      if (!o->dyn_same) {
         if (v13) {
            M48(c, KB13_IDVS_VERTEX_SRT, kb_draw_srt_ptr(o->vst, d->ndyn ? MALI_DYN_TABLE + 1 : 1 + d->nsets));
            M48(c, KB13_IDVS_FRAGMENT_SRT, kb_draw_srt_ptr(o->fst, d->ndyn ? MALI_DYN_TABLE + 1 : 1 + d->nsets));
         } else {
            M48(c, KB_IDVS_VERTEX_SRT, kb_draw_srt_ptr(o->vst, d->ndyn ? MALI_DYN_TABLE + 1 : 1 + d->nsets));
            M48(c, KB_IDVS_FRAGMENT_SRT, kb_draw_srt_ptr(o->fst, d->ndyn ? MALI_DYN_TABLE + 1 : 1 + d->nsets));
         }
      }
      return;
   }
   unsigned pk = ((g->depth_test && f->has_zs) || (f->has_st && d->fx.set && d->fx.st_test)) ? 0 : 2;
   struct kb_dcd_opts dop = { pk, 0, 1, 1, 1 };
   /* a Z/S-only fragment shader (gl_FragDepth / stencil export, no colour output): no render target is written (panvk rt_written = 0: DCD render-target and write masks, blend count) */
   const bool nort = !f->has_colour || (g->fs && (g->fs->fs_writes_depth || g->fs->fs_writes_stencil) && !g->fs->fs_writes_colour);   /* no colour attachment: the colour output has no render target (panvk rt_written = 0) */
   if (g->fs && (g->fs->fs_writes_depth || g->fs->fs_writes_stencil)) {
      dop.pixel_kill = 3;   /* the fragment shader produces Z/S: the test and the update both run late (panvk: force late for shader-written depth) */
      dop.zs_update = 3;
      dop.allow_fpk = 0;
   }
   /* stencil-only pass (no colour attachment): the stencil update is the draw's only visible effect; forward pixel kill would drop the fragment on a depth fail before the stencil dfail op runs */
   if (!f->has_colour && f->has_st && d->fx.set && d->fx.st_test)
      dop.allow_fpk = 0;
   uint32_t dcd_extra = 0, pf_clear = 0;
   if (d->fx.set) {
      dop.front_ccw = d->fx.front_ccw;
      if (d->fx.blend_en || d->fx.wmask != 0xf)
         dop.allow_fpk = 0;   /* the draw reads the destination: no forward pixel kill (panvk any_dest_read) */
      if (g->prim == 0 || g->prim == 8 || g->prim == 10 || g->prim == 12)
         dcd_extra = (uint32_t)(d->fx.cull & 3u) << 17;
      if (!d->fx.depth_clip)
         pf_clear = (1u << 16) | (1u << 17);
   }
   if (g->samples == 4) {
      dcd_extra |= 1u << 19;                                  /* Multisample Enable */
      if (g->per_sample || g->blend_va)
         dcd_extra |= 1u << 11;                               /* Evaluate per-sample (sampleRateShading; a blend shader reads per sample) */
   }
   unsigned vx = d->vp[0] < 0 ? 0 : (unsigned)d->vp[0], vy = d->vp[1] < 0 ? 0 : (unsigned)d->vp[1];
   unsigned vmx = vx + (d->vp[2] > 1 ? (unsigned)d->vp[2] : 1) - 1, vmy = vy + (d->vp[3] > 1 ? (unsigned)d->vp[3] : 1) - 1;
   int32_t sx = d->sc[0] < 0 ? 0 : d->sc[0], sy = d->sc[1] < 0 ? 0 : d->sc[1];
   uint64_t scis = kb_draw_scissor((unsigned)sx, (unsigned)sy, (unsigned)(sx + (d->sc[2] > 1 ? d->sc[2] : 1) - 1),
                                   (unsigned)(sy + (d->sc[3] > 1 ? d->sc[3] : 1) - 1));
   uint32_t minz, maxz;
   memcpy(&minz, &d->vp[4], 4);
   memcpy(&maxz, &d->vp[5], 4);

   /* The registers derived from the static state (shader programs, blend, depth-stencil, the frame's tiler/FBD pointers) are re-emitted
    * only when this draw's record says the previous draw of the frame did not already write the identical values: MALI_D_PIPE/FX/BIND
    * clean and the same firstInstance (`static_same`, the same predicate build_draw_descs reuses the blocks with). The hardware keeps
    * the last write, and the `known` shadow would have suppressed an identical value anyway, so the command stream is unchanged.
    * The SRT pointers follow the per-draw dynamic table, so they belong to the `dyn_same` group instead. */
   if (!v13) {
      if (!o->dyn_same) {
         M48(c, KB_IDVS_VERTEX_SRT, kb_draw_srt_ptr(o->vst, d->ndyn ? MALI_DYN_TABLE + 1 : 1 + d->nsets));
         M48(c, KB_IDVS_FRAGMENT_SRT, kb_draw_srt_ptr(o->fst, d->ndyn ? MALI_DYN_TABLE + 1 : 1 + d->nsets));
      }
      M32(c, KB_IDVS_INDEX_COUNT, d->count);
      M32(c, KB_IDVS_INSTANCE_COUNT, d->instance_count);
      M32(c, KB_IDVS_INDEX_OFFSET, d->first_index);
      M32(c, KB_IDVS_INDEX_BUFFER_SIZE, d->indexed ? d->index_bytes : 0);
      M48(c, KB_IDVS_INDEX_BUFFER, d->indexed ? d->index_va : 0);
      M32(c, KB_IDVS_VERTEX_OFFSET, d->first_vertex);   /* UNVERIFIED for firstVertex != 0 (no hardware evidence) */
      M32(c, KB_IDVS_INSTANCE_OFFSET, d->first_instance);
      M32(c, KB_IDVS_SCISSOR, (uint32_t)scis);
      M32(c, KB_IDVS_SCISSOR + 1, (uint32_t)(scis >> 32));
      M32(c, KB_IDVS_LOW_DEPTH_CLAMP, minz);
      M32(c, KB_IDVS_HIGH_DEPTH_CLAMP, maxz);
      M48(c, KB_IDVS_OQ, d->occ_mode ? d->occ_va : 0);
      M32(c, KB_IDVS_TILER_FLAGS, (prim_flag_bits(g, kb_draw_prim_flags_triangles(g->vs->vs_secondary_enable), false) & ~pf_clear) | idx_type_bits(d));
      M32(c, KB_IDVS_DCD0, kb_draw_dcd0(&dop) | dcd_extra | ((d->occ_mode & 3u) << 14));
      if (!o->static_same) {
         M48(c, KB_IDVS_VERTEX_POS_SPD, o->spd_p);
         M48(c, KB_IDVS_VERTEX_VARY_SPD, o->spd_v);
         M48(c, KB_IDVS_FRAGMENT_SPD, o->spd_f);
         M48(c, KB_IDVS_TSD, va_tsd);
         M32(c, KB_IDVS_GLOBAL_ATTR_OFFSET, 0);
         M32(c, KB_IDVS_DCD2, kb_draw_dcd2(0, nort ? 0 : 1, 1, 1));
         M48(c, KB_IDVS_TILER_CTX, va_tctx);
         M32(c, KB_IDVS_VARY_SIZE, g->vs->generic_size_B);
         M48(c, KB_IDVS_BLEND_DESC, nort ? 0 : (o->bl | 1));
         M48(c, KB_IDVS_ZSD, o->zsd);
         M32(c, KB_IDVS_DCD1, kb_draw_dcd1(0xffff, nort ? 0 : 1));
         M32(c, KB_IDVS_PRIMITIVE_SIZE, g->vs->vs_writes_point_size && g->prim == 1 ? 0 : prim_size_bits(g, &d->fx));
         M32(c, KB_IDVS_TILER_FLAGS2, 0);
      }
   } else {
      uint64_t vh = kb_v13_viewport_hi(vx, vy, vmx, vmy), vl = kb_v13_viewport_lo(d->vp[4], d->vp[5]);
      if (!o->dyn_same) {
         M48(c, KB13_IDVS_VERTEX_SRT, kb_draw_srt_ptr(o->vst, d->ndyn ? MALI_DYN_TABLE + 1 : 1 + d->nsets));
         M48(c, KB13_IDVS_FRAGMENT_SRT, kb_draw_srt_ptr(o->fst, d->ndyn ? MALI_DYN_TABLE + 1 : 1 + d->nsets));
      }
      M32(c, KB13_IDVS_INDEX_COUNT, d->count);
      M32(c, KB13_IDVS_INSTANCE_COUNT, d->instance_count);
      M32(c, KB13_IDVS_INDEX_OFFSET, d->first_index);
      M32(c, KB13_IDVS_VERTEX_OFFSET, d->first_vertex);   /* UNVERIFIED for firstVertex != 0 */
      M32(c, KB13_IDVS_INSTANCE_OFFSET, d->first_instance);
      M32(c, KB13_IDVS_SCISSOR, (uint32_t)scis);
      M32(c, KB13_IDVS_SCISSOR + 1, (uint32_t)(scis >> 32));
      M32(c, KB13_IDVS_VIEWPORT_HIGH, (uint32_t)vh);
      M32(c, KB13_IDVS_VIEWPORT_HIGH + 1, (uint32_t)(vh >> 32));
      M32(c, KB13_IDVS_VIEWPORT_LOW, (uint32_t)vl);
      M32(c, KB13_IDVS_VIEWPORT_LOW + 1, (uint32_t)(vl >> 32));
      M64(c, KB13_IDVS_INDEX_BUFFER, d->indexed ? d->index_va : 0);
      M32(c, KB13_IDVS_INDEX_BUFFER_SIZE, d->indexed ? d->index_bytes : 0);
      M32(c, KB13_IDVS_TILER_FLAGS, (prim_flag_bits(g, kb_v13_prim_flags_triangles(), true) & ~pf_clear) | env->tf_or | idx_type_bits(d));
      M32(c, KB13_IDVS_DCD0, kb_draw_dcd0(&dop) | dcd_extra | ((d->occ_mode & 3u) << 14));
      M64(c, KB13_IDVS_OQ, d->occ_mode ? d->occ_va : 0);
      if (!o->static_same) {
         M48(c, KB13_IDVS_VERTEX_SPD, o->spd_p);
         M48(c, KB13_IDVS_FRAGMENT_SPD, o->spd_f);
         M48(c, KB13_IDVS_VERTEX_TSD, va_tsd);
         M48(c, KB13_IDVS_FRAGMENT_TSD, va_tsd);
         M32(c, KB13_IDVS_GLOBAL_ATTR_OFFSET, 0);
         M48(c, KB13_IDVS_TILER_CTX, va_tctx);
         M32(c, KB13_IDVS_VARY_SIZE, g->vs->generic_size_B);
         M48(c, KB13_IDVS_ZSD, o->zsd);
         M48(c, KB13_IDVS_BLEND_DESC, nort ? 0 : (o->bl | 1));
         M32(c, KB13_IDVS_DCD1, kb_draw_dcd1(0xffff, nort ? 0 : 1));
         M32(c, KB13_IDVS_DCD2, kb_v13_dcd2(0, nort ? 0 : 1, 1, 1, 0, 0));
         M32(c, KB13_IDVS_LINE_WIDTH, prim_size_bits(g, &d->fx));
         M32(c, KB13_IDVS_TILER_FLAGS2, 0);
      }
   }
}

static void
emit_fragment(struct cs *c, const struct mali_env *env, const struct mali_frame *f, const struct regs *R, uint64_t va_fbd,
              uint64_t va_tctx, bool geometry)
{
   unsigned slot = env->slot;
   int x0 = f->area[0] < 0 ? 0 : f->area[0], y0 = f->area[1] < 0 ? 0 : f->area[1];
   unsigned x1 = x0 + (f->area[2] > 1 ? f->area[2] : 1) - 1, y1 = y0 + (f->area[3] > 1 ? f->area[3] : 1) - 1;

   E(c, kb_cs_req_res(KB_CS_RES_FRAGMENT));
   E(c, kb_cs_set_state_imm32(KB_CS_SB_SEL_ENDPOINT, slot));
   M48(c, 40, kb_fb_pointer_ex(va_fbd, 1, f->has_zs || f->has_st));
   M32(c, 42, kb_fb_bbox_min(x0, y0));
   M32(c, 43, kb_fb_bbox_max(x1, y1));
   if (geometry)
      M48(c, R->tctx_copy, va_tctx);
   M48(c, 44, 0);   /* FRAGMENT TEM_POINTER / ROW_STRIDE alias the IDVS depth clamps / viewport regs: zero */
   M32(c, 46, 0);
   E(c, kb_cs_run_fragment(0));
   E(c, kb_cs_wait(1u << slot));
   if (geometry) {
      E(c, kb_cs_load_multiple(10, R->tctx_copy, 0xf, 40));
      E(c, kb_cs_wait(1u << KB_CS_SLOT_LS));
      E(c, kb_cs_finish_fragment_heap(true, 10, 12));
   }
   E(c, kb_cs_req_res(0));
}

/* ---- M11 compute: RUN_COMPUTE (genxml v11/v13 "COMPUTE SR": SRT r0, FAU r8, SPD r16, TSD r24, GLOBAL_ATTR_OFFSET r32, WG_SIZE r33,
 * JOB_OFFSET r34..36, JOB_SIZE r37..39 (workgroup counts)). Task axis X, increment 1 (always valid; panvk picks a better split). ---- */
#define CR_SRT 0
#define CR_FAU 8
#define CR_SPD 16
#define CR_TSD 24
#define CR_GAO 32
#define CR_WG_SIZE 33
#define CR_JOB_OFFSET 34
#define CR_JOB_SIZE 37
#define CR_TMP 60
#define WLS_CORE_ID_RANGE 16u
#define WLS_MAX_TOTAL (384u * 1024u)

static uint64_t
run_compute_instr(unsigned task_incr, unsigned axis)
{
   return ((uint64_t)4 << 56) | (task_incr & 0x3fff) | ((uint64_t)(axis & 3) << 14);
}

static int
build_compute(const struct mali_hw *hw, const struct mali_frame *f, uint64_t **out, unsigned *nout)
{
   const struct mali_env *env = &hw->env;
   const struct mali_dispatch *d = &f->disp;
   const struct mali_compiled *cs = d->cs;
   bool v13 = env->arch == 13;
   const struct regs *R = v13 ? &R13 : &R11;
   struct cs c;
   uint64_t va_spd, va_set, va_srt, va_fau, va_tsd, va_wls = 0;
   uint32_t *spd, *tsd, *fau;
   uint8_t *set;

   *out = NULL;
   *nout = 0;
   if (!cs || !cs->bin_size || !d->cs_va)
      return -EINVAL;
   for (int i = 0; i < 3; i++)
      if (!cs->local_size[i])
         return -EINVAL;
   unsigned nres = d->ndyn ? MALI_DYN_TABLE + 1 : 1 + d->nsets, res_alloc = ((nres + 3) & ~3u) * KB_RES_SIZE;   /* dynamic table at MALI_DYN_TABLE as in the draw path */
   uint64_t va_dyn = 0;
   uint32_t *dyn = NULL;

   spd = A(hw, KB_SPD_SIZE, 32, &va_spd);
   set = A(hw, (MALI_DUMMY_SAMPLER_IDX + 1) * KB_DESC_SET_STRIDE, 64, &va_set);   /* entry MALI_DUMMY_SAMPLER_IDX (all zero) serves texelFetch */
   uint32_t *srt = A(hw, res_alloc, 64, &va_srt);
   fau = A(hw, FAU_BYTES(cs->fau_count), 64, &va_fau);
   tsd = A(hw, KB_LS_SIZE, 64, &va_tsd);
   if (d->ndyn)
      dyn = A(hw, ((d->ndyn * KB_DESC_SET_STRIDE) + 63) & ~63u, 64, &va_dyn);
   if (!spd || !set || !srt || !fau || !tsd || (d->ndyn && !dyn))
      return -ENOMEM;

   if (v13)
      kb_v13_spd(spd, 1, d->cs_va, cs->work_reg_count, cs->preload, 0);
   else {
      kb_draw_spd(spd, 1, d->cs_va, cs->work_reg_count, cs->preload);
      spd[0] &= ~(3u << 8);   /* bits 9:8 are fragment/vertex only */
   }

   if (cs->wls_size) {
      unsigned adj = 1;
      while (adj < (cs->wls_size > 128 ? cs->wls_size : 128))
         adj <<= 1;
      unsigned inst = 16, log2i = 4;
      while (inst > 1 && (uint64_t)adj * inst * WLS_CORE_ID_RANGE > WLS_MAX_TOTAL) {
         inst >>= 1;
         log2i--;
      }
      size_t total = (size_t)adj * inst * WLS_CORE_ID_RANGE;
      if (total > WLS_MAX_TOTAL)
         return -ENOMEM;
      void *w = A(hw, total, 4096, &va_wls);
      if (!w)
         return -ENOMEM;
      unsigned lg = 0;
      while ((1u << lg) < adj)
         lg++;
      memset(tsd, 0, KB_LS_SIZE);
      tsd[1] = log2i | ((lg + 1) << 8);
      tsd[4] = (uint32_t)va_wls;
      tsd[5] = (uint32_t)(va_wls >> 32);
   } else {
      kb_draw_local_storage_none(tsd);
   }

   if (cs->tls_size) {
      /* TLS spill (panvk GENX(pan_emit_tls), PAN_ARCH >= 9: packed addressing, base >> 8). Size as pan_get_total_stack_size(). */
      unsigned per = cs->tls_size < 16 ? 16 : (cs->tls_size + 15) & ~15u, pot = 1, shift = 0;
      while (pot < per)
         pot <<= 1;
      while ((16u << shift) < cs->tls_size)
         shift++;
      if (!hw->tls || !env->tls_threads || !env->core_id_range)
         return -ENOTSUP;
      uint64_t total = (uint64_t)pot * env->tls_threads * env->core_id_range, va_tls = 0;
      void *tp = hw->tls(hw->user, (size_t)((total + 4095) & ~4095ull), &va_tls);
      if (!tp || (va_tls & 4095))
         return -ENOMEM;
      tsd[0] = shift;
      tsd[2] = (uint32_t)(va_tls >> 8);
      tsd[3] = (uint32_t)(va_tls >> 40) | (1u << 28);
   }

   kb_draw_resource(srt, va_set, (MALI_DUMMY_SAMPLER_IDX + 1) * KB_DESC_SET_STRIDE, 1);
   for (unsigned s = 0; s < d->nsets; s++)
      kb_draw_resource(srt + (1 + s) * (KB_RES_SIZE / 4), d->set_va[s], d->set_bytes[s], 1);
   if (d->ndyn) {
      memcpy(dyn, d->dyn_desc, d->ndyn * KB_DESC_SET_STRIDE);
      kb_draw_resource(srt + MALI_DYN_TABLE * (KB_RES_SIZE / 4), va_dyn, d->ndyn * KB_DESC_SET_STRIDE, 1);
   }
   kb_draw_fau_imm(fau, (const uint32_t(*)[2])cs->imm, cs->nimm);
   {
      size_t room = FAU_BYTES(cs->fau_count);
      unsigned n = cs->push_const_bytes > MALI_PUSH_MAX ? MALI_PUSH_MAX : cs->push_const_bytes;
      if (n > room)
         n = (unsigned)room;
      memcpy((uint8_t *)fau, d->push, n);
   }

   memset(&c, 0, sizeof c);
   E(&c, kb_cs_nop());
   if (getenv("MALI_ZR")) for (unsigned r = 0; r < (unsigned)atoi(getenv("MALI_ZR")); r++) if (r != R->flush_id) M32(&c, r, 0);
   {
      const char *e = getenv("MALI_CHF");   /* compute head flush A/B (hex l2/lsc/other nibbles, default 0x332); 0 = none */
      int hf = e ? (int)strtol(e, NULL, 16) : 0x332;
      if (hf) {
         M32(&c, R->flush_id, 0);
         E(&c, kb_cs_flush_cache2((hf >> 8) & 3, (hf >> 4) & 3, hf & 3, R->flush_id));
      }
      E(&c, kb_cs_wait(1u << KB_CS_SLOT_LS));
   }
   M48(&c, CR_SRT, kb_draw_srt_ptr(va_srt, nres));
   M64(&c, CR_FAU, kb_draw_fau_ptr(va_fau, cs->fau_count));
   M48(&c, CR_SPD, va_spd);
   M48(&c, CR_TSD, va_tsd);
   M32(&c, CR_GAO, 0);
   M32(&c, CR_WG_SIZE, (cs->local_size[0] - 1) | ((cs->local_size[1] - 1) << 10) | ((cs->local_size[2] - 1) << 20) | (cs->allow_merge ? 1u << 31 : 0));
   M32(&c, CR_JOB_OFFSET, 0);
   M32(&c, CR_JOB_OFFSET + 1, 0);
   M32(&c, CR_JOB_OFFSET + 2, 0);
   if (d->indirect_va) {
      M48(&c, CR_TMP, d->indirect_va);
      E(&c, kb_cs_load_multiple(CR_JOB_SIZE, CR_TMP, 0x7, 0));
      E(&c, kb_cs_wait(1u << KB_CS_SLOT_LS));
   } else {
      M32(&c, CR_JOB_SIZE, d->groups[0]);
      M32(&c, CR_JOB_SIZE + 1, d->groups[1]);
      M32(&c, CR_JOB_SIZE + 2, d->groups[2]);
   }
   {
      const char *cvs = getenv("MALI_CV");
      unsigned cv = cvs ? (unsigned)strtoul(cvs, NULL, 0) : 0;
      if (cv & 1) E(&c, kb_cs_wait(0xffff));
      if (cv & 64) { M48(&c, R->heap, 0); E(&c, kb_cs_heap_set(R->heap)); }
      if (cv & 128) { E(&c, kb_cs_req_res(KB_CS_RES_IDVS | KB_CS_RES_TILER)); E(&c, kb_cs_finish_tiling()); E(&c, kb_cs_wait(0xffff)); E(&c, kb_cs_req_res(0)); }
      if (cv & 2) E(&c, kb_cs_set_state_imm32(KB_CS_SB_SEL_OTHER, 0));
      if (cv & 4) E(&c, kb_cs_req_res(0));
      if (cv & 8) E(&c, kb_cs_set_state_imm32(KB_CS_SB_SEL_DEFERRED, 0));
      if (getenv("MALI_CV_OTHER")) E(&c, kb_cs_set_state_imm32(KB_CS_SB_SEL_OTHER, (unsigned)atoi(getenv("MALI_CV_OTHER"))));
   }
   E(&c, kb_cs_set_state_imm32(KB_CS_SB_SEL_ENDPOINT, env->slot));
   E(&c, kb_cs_req_res(KB_CS_RES_COMPUTE));
   {
      /* task axis/increment as panvk calculate_task_axis_and_increment: spread workgroups over cores, ~T threads per task */
      unsigned inc = 1, axis = 0;
      const char *te = getenv("MALI_TASK_THREADS");
      unsigned T = te ? (unsigned)atoi(te) : (env->tls_threads ? env->tls_threads / 4 : 0);
      if (!d->indirect_va && T) {
         unsigned wgc[3] = { d->groups[0], d->groups[1], d->groups[2] };
         unsigned tpw = cs->local_size[0] * cs->local_size[1] * cs->local_size[2];
         unsigned total = wgc[0] * wgc[1] * wgc[2], cores = env->core_id_range ? env->core_id_range : 1;
         if (total && tpw) {
            unsigned per_core = (total + cores - 1) / cores, w = T / tpw;
            w = w < 1 ? 1 : w > per_core ? per_core : w;
            inc = w;
            for (unsigned i = 0; i < 2; i++) {
               if (inc <= wgc[i])
                  break;
               axis++;
               inc /= wgc[i];
            }
            if (!inc) inc = 1;
         }
      }
      E(&c, run_compute_instr(inc, axis));
   }
   E(&c, kb_cs_wait(1u << env->slot));
   E(&c, kb_cs_req_res(0));
   {
      const char *cvs = getenv("MALI_CV");
      unsigned cv = cvs ? (unsigned)strtoul(cvs, NULL, 0) : 0;
      M32(&c, R->flush_id, 0);
      E(&c, kb_cs_flush_cache2((cv & 16) ? 3 : 1, (cv & 16) ? 3 : 1, (cv & 16) ? 2 : 0, R->flush_id));
      E(&c, kb_cs_wait(1u << KB_CS_SLOT_LS));
      if (cv & 32) {
         M32(&c, R->flush_id, 0);
         E(&c, kb_cs_flush_cache2(3, 3, 2, R->flush_id));
         E(&c, kb_cs_wait(0xffff));
      }
   }
   M48(&c, R->sync_addr, env->sync_va);
   M48(&c, R->sync_val, 1);
   E(&c, kb_cs_sync_add64(R->sync_addr, R->sync_val, KB_CS_SCOPE_SYSTEM));
   if (c.err) {
      free(c.v);
      return c.err;
   }
   *out = c.v;
   *nout = c.n;
   return 0;
}

int
mali_frame_build(const struct mali_hw *hw, const struct mali_frame *f, uint64_t **out, unsigned *nout)
{
   const struct mali_env *env = &hw->env;
   bool v13 = env->arch == 13;
   const struct regs *R = v13 ? &R13 : &R11;
   struct cs c;
   /* One draw at a time: build_draw_descs only compares against the previous draw's blocks and emit_draw_regs only needs
    * this draw's, so two slots are enough (a per-frame array of ndraws cost a calloc + free of ndraws blocks per frame). */
   struct ddesc dd, dd_prev;
   struct mali_draw mod;           /* polygonMode LINE/POINT draw rewritten to a list */
   int rc = 0;
   uint64_t va_smp = 0, va_tctx = 0, va_fbd = 0, va_tsd = 0;

   if (f->nops)
      return mali_ops_build(env, f->ops, f->nops, out, nout);
   if (hw->soft) {
      /* MALI_SOFT=1: render on the CPU instead of encoding a command stream. A frame the rasteriser cannot do is a hard
       * error (-ENOTSUP), never a silent no-op: the whole point of the harness is that nothing passes unnoticed.
       * This branch comes BEFORE the compute dispatch: with a software backend there is nothing that could execute a
       * dispatch, so a compute frame must not be built and accepted. Reserve the placeholder word first too, so an
       * allocation failure is reported before the rasteriser has written into the attachments. */
      uint64_t *keep = calloc(1, sizeof *keep);   /* callers free() it: a valid pointer with ninstr 0 */
      if (!keep)
         return -ENOMEM;
      if (!hw->soft(hw->user, f)) {
         free(keep);
         return -ENOTSUP;
      }
      *out = keep;
      *nout = 0;
      return 0;
   }
   if (f->compute)
      return build_compute(hw, f, out, nout);
   *out = NULL;
   *nout = 0;
   if (f->ncolour > 1)
      return -ENOTSUP;   /* MRT frames exist for the software rasteriser only (maxColorAttachments = 1) */
   if ((env->arch != 10 && env->arch != 11 && env->arch != 13) || !(f->has_colour || f->has_zs || f->has_st || f->width))
      return -EINVAL;
   memset(&c, 0, sizeof c);
   bool geometry = f->ndraws > 0;

   void *smp = A(hw, sizeof kb_sample_positions, 64, &va_smp);
   uint32_t *tctx = A(hw, KB_TILER_CTX_SIZE, 64, &va_tctx);
   uint8_t *fbd = A(hw, KB_FB_DESC_SIZE_ZS, 64, &va_fbd);
   uint32_t *tsd = A(hw, KB_LS_SIZE, 64, &va_tsd);
   if (!smp || !tctx || !fbd || !tsd)
      return -ENOMEM;
   kb_draw_local_storage_none(tsd);
   memcpy(smp, kb_sample_positions, sizeof kb_sample_positions);

   if (geometry) {
      /* The heap descriptor is written ONCE (queue creation). Rewriting it per frame resets its pointers to the first chunk
       * while firmware owns grown chunks: the second frame that needed heap growth (>~600 draws) TRANSLATION_FAULTed on v11. */
      if (!env->heap_desc_ready)
         kb_draw_tiler_heap_desc(env->geo_cpu, env->heap_first_chunk_va, env->heap_chunk_size);
      uint64_t geom = (env->geo_va + 4096) | ((getenv("MALI_GEOM_SIZE") ? (uint32_t)atoi(getenv("MALI_GEOM_SIZE")) : 65536u) >> 12);
      if (v13)
         kb_v13_tiler_ctx(tctx, f->width, f->height, env->hmask, env->tile_log2, env->geo_va, geom);
      else
         kb_draw_tiler_ctx(tctx, f->width, f->height, env->hmask, env->geo_va, geom);
      if (f->provoking_last)
         tctx[2] &= ~(1u << 18);   /* First provoking vertex off = last vertex (v11 word 2 bit 18, v13.xml:2636 same) */
      if (f->samples == 4)
         tctx[2] |= 2u << 13;   /* Sample Pattern (15:13) rotated 4x; v13 additionally has Effective Tile Size at 23:20 (tile_log2 - 2 below) */
      if (f->samples == 4 && v13)
         tctx[2] = (tctx[2] & ~(0xfu << 20)) | (((env->tile_log2 - 2) & 0xfu) << 20);
   }
   if ((rc = mali_render_pack_fbd(env, f, fbd, geometry ? va_tctx : 0, va_smp)))
      goto out;

   /* head: flush stale descriptor lines, WAIT for it (async), then registers */
   E(&c, kb_cs_nop());
   {
      static int hf = -1;
      if (hf < 0) { const char *e = getenv("MALI_HF"); hf = e ? (int)strtol(e, NULL, 16) : 0x332; }
      if (hf) {
         M32(&c, R->flush_id, 0);
         E(&c, kb_cs_flush_cache2((hf >> 8) & 3, (hf >> 4) & 3, hf & 3, R->flush_id));
      }
      E(&c, kb_cs_wait(1u << KB_CS_SLOT_LS));
   }
   if (geometry) {
      unsigned prev_prim = 0;
      struct mali_draw x, pv;   /* the running full draw (updated by the groups each record renewed) and what build_draw_descs needs of the previous one */
      memset(&x, 0, sizeof x);
      memset(&pv, 0, sizeof pv);
      memset(&dd_prev, 0, sizeof dd_prev);
      struct dwp dwp_prev, dwp_cur;
      bool have_dwp = false;
      for (unsigned i = 0; i < f->ndraws; i++) {
         mali_draw_expand(&x, &f->draws[i]);
         struct mali_draw *dr = &x;
         if (dr->gfx.poly_mode) {
            if ((rc = poly_lines(hw, dr, &mod)))
               goto out;
            dr = &mod;
         }
         /* poly_lines rewrites gfx.prim (an empty POINT draw even picks a different one than a non-empty one) behind the record-time dirty
          * mask's back: a change of the EFFECTIVE primitive, in either direction, must not reuse the previous draw's descriptors. */
         if (i && prev_prim != dr->gfx.prim)
            dr->dirty |= MALI_D_PIPE;
         prev_prim = dr->gfx.prim;
         /* zeroed like the array it replaces: build_draw_descs leaves the fields of a reused group to the previous draw, and
          * the blend words (bl23) are only written on the path that rebuilds them */
         memset(&dd, 0, sizeof dd);
         if ((rc = build_draw_descs(hw, f, dr, &dd, i ? &pv : NULL, i ? &dd_prev : NULL)))
            goto out;
         const bool same_words = same_draw_words(have_dwp ? &dwp_prev : NULL, dr);
         emit_draw_regs(&c, env, f, dr, &dd, va_tsd, va_tctx, same_words);
         dwp_of(&dwp_cur, dr);
         dwp_prev = dwp_cur;
         have_dwp = true;
         if (i == 0) {
            M48(&c, R->heap, env->heap_ctx_va);
            E(&c, kb_cs_heap_set(R->heap));
            E(&c, kb_cs_set_state_imm32(KB_CS_SB_SEL_ENDPOINT, env->slot));
            E(&c, kb_cs_set_state_imm32(KB_CS_SB_SEL_OTHER, KB_CS_SLOT_LS));
            E(&c, kb_cs_req_res(KB_CS_RES_IDVS | KB_CS_RES_TILER));
            E(&c, kb_cs_heap_operation(KB_CS_HEAP_VT_STARTED));
         }
         M64(&c, v13 ? KB13_IDVS_VERTEX_FAU : KB_IDVS_VERTEX_FAU, dd.vfau);
         M64(&c, v13 ? KB13_IDVS_FRAGMENT_FAU : KB_IDVS_FRAGMENT_FAU, dd.ffau);
         E(&c, v13 ? kb13_cs_run_idvs2(0, 0) : kb_cs_run_idvs());
         if (check_dirty())
            pv = *dr;
         else {
            pv.first_instance = dr->first_instance;
            pv.first_vertex = dr->first_vertex;
            pv.draw_id = dr->draw_id;
         }
         dd_prev = dd;
      }
      E(&c, kb_cs_finish_tiling());
      E(&c, kb_cs_wait(1u << env->slot));
      E(&c, kb_cs_heap_operation(KB_CS_HEAP_VT_COMPLETED));
   }
   emit_fragment(&c, env, f, R, va_fbd, va_tctx, geometry);
   if (geometry && !getenv("MALI_NOTF")) {
      M32(&c, R->flush_id, 0);
      E(&c, kb_cs_flush_cache2(1, 1, 0, R->flush_id));
      E(&c, kb_cs_wait(1u << KB_CS_SLOT_LS));
   }
   M48(&c, R->sync_addr, env->sync_va);
   M48(&c, R->sync_val, 1);
   E(&c, kb_cs_sync_add64(R->sync_addr, R->sync_val, KB_CS_SCOPE_SYSTEM));
   rc = c.err;
out:
   if (rc) {
      free(c.v);
      return rc;
   }
   *out = c.v;
   *nout = c.n;
   return 0;
}

int
mali_frame_submit(const struct mali_hw *hw, const struct mali_frame *f)
{
   uint64_t *cs;
   unsigned n;
   int rc;

   if (!hw || !hw->alloc || !hw->submit)
      return -ENODEV;
   struct timespec pt0, pt1, pt2;
   clock_gettime(CLOCK_MONOTONIC, &pt0);
   if (hw->job_begin)
      hw->job_begin(hw->user);
   clock_gettime(CLOCK_MONOTONIC, &pt1);
   if ((rc = mali_frame_build(hw, f, &cs, &n)))
      return rc;
   clock_gettime(CLOCK_MONOTONIC, &pt2);
   if (getenv("MALI_PROF")) {
      static double a, b; static unsigned c;
      a += (pt1.tv_sec - pt0.tv_sec) * 1e3 + (pt1.tv_nsec - pt0.tv_nsec) / 1e6;
      b += (pt2.tv_sec - pt1.tv_sec) * 1e3 + (pt2.tv_nsec - pt1.tv_nsec) / 1e6;
      if (++c % 200 == 0) fprintf(stderr, "PROF/frame job_begin %.3f build %.3f ms (ndraws %u nops %u)\n", a / c, b / c, f->ndraws, f->nops);
   }
   kb_wmb();
   rc = hw->submit(hw->user, cs, n);
   free(cs);
   return rc;
}

/* ---- frame pool: immutable state blocks of the recorded draws ---- */
struct mali_pool {
   struct mali_pool *next;
   size_t used, size;
   _Alignas(16) uint8_t mem[];
};

/* Standard 64 KiB pool chunks are recycled through a small process-wide free list instead of calloc/free per frame: a 3000-draw frame
 * needs ~16 chunks, and returning them to glibc every frame cost a brk trim + fresh zero pages (page faults) on the next one. Blocks
 * are memset individually in mali_frame_blk, so the chunks need no zeroing. At most MALI_POOL_KEEP chunks are kept (4 MiB). */
#define MALI_POOL_CHUNK (64u << 10)
#define MALI_POOL_KEEP 64
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static struct mali_pool *g_pool_free;
static unsigned g_pool_nfree;

static struct mali_pool *
pool_chunk_get(size_t sz)
{
   struct mali_pool *np = NULL;
   if (sz == MALI_POOL_CHUNK) {
      pthread_mutex_lock(&g_pool_lock);
      if ((np = g_pool_free)) {
         g_pool_free = np->next;
         g_pool_nfree--;
      }
      pthread_mutex_unlock(&g_pool_lock);
   }
   if (!np && !(np = malloc(sizeof *np + sz)))
      return NULL;
   np->used = 0;
   np->size = sz;
   return np;
}

static void
pool_chunk_put(struct mali_pool *p)
{
   if (p->size == MALI_POOL_CHUNK) {
      pthread_mutex_lock(&g_pool_lock);
      if (g_pool_nfree < MALI_POOL_KEEP) {
         p->next = g_pool_free;
         g_pool_free = p;
         g_pool_nfree++;
         pthread_mutex_unlock(&g_pool_lock);
         return;
      }
      pthread_mutex_unlock(&g_pool_lock);
   }
   free(p);
}

unsigned
mali_frame_pool_cached(void)   /* test hook: chunks parked in the free list */
{
   pthread_mutex_lock(&g_pool_lock);
   unsigned n = g_pool_nfree;
   pthread_mutex_unlock(&g_pool_lock);
   return n;
}

void *
mali_frame_blk(struct mali_frame *f, size_t n)
{
   n = (n + 15) & ~(size_t)15;
   struct mali_pool *p = f->pool;
   if (!p || p->used + n > p->size) {
      struct mali_pool *np = pool_chunk_get(n > MALI_POOL_CHUNK ? n : MALI_POOL_CHUNK);
      if (!np)
         return NULL;
      np->next = p;
      f->pool = p = np;
   }
   void *r = p->mem + p->used;
   p->used += n;
   memset(r, 0, n);   /* the pool is calloc'd per chunk, but blocks are small: keep padding deterministic */
   return r;
}

struct mali_rec *
mali_frame_draw_slot(struct mali_frame *f)
{
   if (f->ndraws == f->cap) {
      unsigned nc = f->cap ? f->cap * 2 : 8;
      struct mali_rec *nd = realloc(f->draws, nc * sizeof *nd);
      if (!nd)
         return NULL;
      f->draws = nd;
      f->cap = nc;
   }
   return &f->draws[f->ndraws];
}

int
mali_frame_add_rec(struct mali_frame *f, const struct mali_rec *r)
{
   struct mali_rec *s = mali_frame_draw_slot(f);
   if (!s)
      return -ENOMEM;
   *s = *r;
   f->ndraws++;
   mali_frame_note_rec(f, s);
   return 0;
}

/* Accumulates the per-frame submit flags (see struct mali_frame): the same test mali_cmd_submit used to run over every
 * record of the frame, done once per record while it is still cache-hot. */
void
mali_frame_note_rec(struct mali_frame *f, const struct mali_rec *r)
{
   if (r->dirty & (MALI_D_PIPE | MALI_D_FX | MALI_D_VP | MALI_D_SC))
      f->cur_pipe = r->pipe;
   if (r->ind)
      f->any_ind = true;
   if (f->cur_pipe && f->cur_pipe->gfx.poly_mode && r->indexed)
      f->poly_cpu = true;
}

/* A full draw (the preload draws): own blocks for every group. */
int
mali_frame_add_draw(struct mali_frame *f, const struct mali_draw *d)
{
   struct mali_bpipe *bp = mali_frame_blk(f, sizeof *bp);
   struct mali_bbind *bb = mali_frame_blk(f, sizeof *bb);
   struct mali_bpush *bu = mali_frame_blk(f, sizeof *bu);
   struct mali_bdyn *bd = mali_frame_blk(f, sizeof *bd + d->ndyn * sizeof bd->d[0]);
   if (!bp || !bb || !bu || !bd)
      return -ENOMEM;
   bp->fx = d->fx; bp->gfx = d->gfx;
   memcpy(bp->vp, d->vp, sizeof bp->vp); memcpy(bp->sc, d->sc, sizeof bp->sc);
   memcpy(bb->vb_va, d->vb_va, sizeof bb->vb_va); memcpy(bb->vb_size, d->vb_size, sizeof bb->vb_size); memcpy(bb->vb_stride, d->vb_stride, sizeof bb->vb_stride);
   bb->nsets = d->nsets; bb->ndyn = d->ndyn;
   memcpy(bb->set_va, d->set_va, sizeof bb->set_va); memcpy(bb->set_bytes, d->set_bytes, sizeof bb->set_bytes);
   memcpy(bu->push, d->push, sizeof bu->push);
   bd->n = d->ndyn;
   memcpy(bd->d, d->dyn_desc, d->ndyn * sizeof bd->d[0]);
   struct mali_rec r = {
      .dirty = MALI_D_ALL, .pipe = bp, .bind = bb, .push = bu, .dyn = bd,
      .indexed = d->indexed, .index_va = d->index_va, .index_cpu = d->index_cpu, .index_bytes = d->index_bytes, .index_size = d->index_size,
      .first_index = d->first_index, .count = d->count, .instance_count = d->instance_count, .first_vertex = d->first_vertex,
      .first_instance = d->first_instance, .draw_id = d->draw_id, .occ_va = d->occ_va, .ind = d->ind, .ind_count = d->ind_count,
      .ind_n = d->ind_n, .ind_stride = d->ind_stride, .occ_mode = d->occ_mode,
   };
   return mali_frame_add_rec(f, &r);
}

void
mali_draw_expand(struct mali_draw *x, const struct mali_rec *r)
{
   const uint32_t m = r->dirty;
   if (m & (MALI_D_PIPE | MALI_D_FX | MALI_D_VP | MALI_D_SC)) {
      x->fx = r->pipe->fx;
      x->gfx = r->pipe->gfx;
      memcpy(x->vp, r->pipe->vp, sizeof x->vp);
      memcpy(x->sc, r->pipe->sc, sizeof x->sc);
      if (m & MALI_D_SC)
         x->sc_set = true;   /* sticky like sc itself: a later draw that does not renew the scissor inherits it */
   }
   if (m & MALI_D_BIND) {
      const struct mali_bbind *b = r->bind;
      memcpy(x->vb_va, b->vb_va, sizeof x->vb_va);
      memcpy(x->vb_size, b->vb_size, sizeof x->vb_size);
      memcpy(x->vb_stride, b->vb_stride, sizeof x->vb_stride);
      x->nsets = b->nsets;
      memcpy(x->set_va, b->set_va, sizeof x->set_va);
      memcpy(x->set_bytes, b->set_bytes, sizeof x->set_bytes);
      x->ndyn = b->ndyn;
   }
   if (m & (MALI_D_BIND | MALI_D_DYNBUF))
      memcpy(x->dyn_desc, r->dyn->d, r->dyn->n * sizeof r->dyn->d[0]);
   if (m & MALI_D_PUSH)
      memcpy(x->push, r->push->push, sizeof x->push);
   x->dirty = m;
   x->indexed = r->indexed; x->index_va = r->index_va; x->index_cpu = r->index_cpu; x->index_bytes = r->index_bytes;
   x->index_size = r->index_size; x->first_index = r->first_index;
   x->count = r->count; x->instance_count = r->instance_count; x->first_vertex = r->first_vertex; x->first_instance = r->first_instance;
   x->draw_id = r->draw_id; x->occ_va = r->occ_va; x->ind = r->ind; x->ind_count = r->ind_count; x->ind_n = r->ind_n;
   x->ind_stride = r->ind_stride; x->occ_mode = r->occ_mode;
}

void
mali_frame_finish(struct mali_frame *f)
{
   free(f->draws);
   for (struct mali_pool *p = f->pool, *n; p; p = n) {
      n = p->next;
      pool_chunk_put(p);
   }
   f->pool = NULL;
   free(f->ops);
   f->ops = NULL;
   f->nops = 0;
   f->draws = NULL;
   f->ndraws = f->cap = 0;
}
