/* SPDX-License-Identifier: MIT */
/* Software rasteriser over struct mali_frame: see mali_soft.h and docs/host-harness.md. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "mali_soft.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan_core.h>

/* One frame (and one registry change) at a time: g_st and g_prog are process-wide, and a pipeline destroy can run on
 * another thread than the frame that is using its program. */
static atomic_flag g_busy = ATOMIC_FLAG_INIT;

/* ---- program registry (keyed by the compiled shader pointers the frame record carries) ---- */
#define MAX_PROGS 32
static struct {
   const struct mali_compiled *vs, *fs;
   struct mali_soft_program p;
} g_prog[MAX_PROGS];
static unsigned g_nprog;
static struct mali_soft_stats g_st;

void
mali_soft_bind_program(const struct mali_compiled *vs, const struct mali_compiled *fs,
                       const struct mali_soft_program *p)
{
   while (atomic_flag_test_and_set_explicit(&g_busy, memory_order_acquire))
      ;
   for (unsigned i = 0; i < g_nprog; i++)
      if (g_prog[i].vs == vs && g_prog[i].fs == fs) {
         g_prog[i].p = *p;
         atomic_flag_clear_explicit(&g_busy, memory_order_release);
         return;
      }
   if (g_nprog == MAX_PROGS) {
      g_st.unsupported++;
      snprintf(g_st.why, sizeof g_st.why, "program registry full");
   } else {
      g_prog[g_nprog].vs = vs;
      g_prog[g_nprog].fs = fs;
      g_prog[g_nprog].p = *p;
      g_nprog++;
   }
   atomic_flag_clear_explicit(&g_busy, memory_order_release);
}

void
mali_soft_clear_programs(void)
{
   while (atomic_flag_test_and_set_explicit(&g_busy, memory_order_acquire))
      ;
   g_nprog = 0;
   atomic_flag_clear_explicit(&g_busy, memory_order_release);
}

void
mali_soft_retire(const struct mali_compiled *vs, const struct mali_compiled *fs)
{
   /* The key is the address of the pipeline's own compiled-shader members, so a freed pipeline's storage can be reused
    * by a new one and a stale entry would then answer for a program that was never registered. Drop the entry here,
    * before the pipeline's memory goes back to the allocator. */
   while (atomic_flag_test_and_set_explicit(&g_busy, memory_order_acquire))
      ;
   for (unsigned i = 0; i < g_nprog; i++)
      if (g_prog[i].vs == vs && g_prog[i].fs == fs) {
         g_prog[i] = g_prog[--g_nprog];
         break;
      }
   atomic_flag_clear_explicit(&g_busy, memory_order_release);
}

static const struct mali_soft_program *
find_prog(const struct mali_gfx_state *g)
{
   for (unsigned i = 0; i < g_nprog; i++)
      if (g_prog[i].vs == g->vs && g_prog[i].fs == g->fs)
         return &g_prog[i].p;
   return NULL;
}

const struct mali_soft_stats *
mali_soft_last(void)
{
   return &g_st;
}

static bool
unsupported(const char *why)
{
   g_st.unsupported++;
   snprintf(g_st.why, sizeof g_st.why, "%s", why);
   return false;
}

/* ---- surfaces: one attachment as linear memory, addressed exactly as the frame record says ---- */
struct surf {
   uint8_t *p;
   uint32_t stride;
   unsigned w, h;
   int fmt;          /* VkFormat of the attachment */
};

static unsigned
px_size(int fmt)
{
   switch (fmt) {
   case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
   case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:   /* same bytes as R8G8B8A8 (R in byte 0) */
   case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return 4;
   case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
   case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
   case VK_FORMAT_R32_SFLOAT: return 4;
   default: return 0;
   }
}

/* sRGB attachments store an encoded value: the fragment value is linear, the byte is not. Blend in linear space, so
 * decode on the way in and encode on the way out (alpha stays linear in both directions). */
static float
srgb_to_lin(float c)
{
   return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float
lin_to_srgb(float c)
{
   if (!(c > 0.0f)) return 0.0f;
   if (c >= 1.0f) return 1.0f;
   return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static void
px_read(const struct surf *s, unsigned x, unsigned y, float c[4])
{
   const uint8_t *q = s->p + (size_t)y * s->stride + (size_t)x * px_size(s->fmt);
   memset(c, 0, 4 * sizeof(float));
   c[3] = 1.0f;
   switch (s->fmt) {
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
      for (unsigned i = 0; i < 4; i++) c[i] = q[i] / 255.0f;
      break;
   case VK_FORMAT_R8G8B8A8_SRGB:
   case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
      for (unsigned i = 0; i < 3; i++) c[i] = srgb_to_lin(q[i] / 255.0f);
      c[3] = q[3] / 255.0f;
      break;
   case VK_FORMAT_B8G8R8A8_UNORM:
      c[0] = q[2] / 255.0f; c[1] = q[1] / 255.0f; c[2] = q[0] / 255.0f; c[3] = q[3] / 255.0f;
      break;
   case VK_FORMAT_B8G8R8A8_SRGB:
      c[0] = srgb_to_lin(q[2] / 255.0f); c[1] = srgb_to_lin(q[1] / 255.0f);
      c[2] = srgb_to_lin(q[0] / 255.0f); c[3] = q[3] / 255.0f;
      break;
   case VK_FORMAT_R16G16B16A16_SFLOAT: {
      const uint16_t *h = (const uint16_t *)q;
      for (unsigned i = 0; i < 4; i++) {   /* _Float16 is not portable in C11 here: expand by hand */
         uint32_t sg = (h[i] >> 15) & 1, ex = (h[i] >> 10) & 0x1f, mn = h[i] & 0x3ff;
         float v;
         if (ex == 0) v = ldexpf((float)mn, -24);
         else if (ex == 31) v = mn ? NAN : INFINITY;
         else v = ldexpf((float)(mn | 0x400), (int)ex - 25);
         c[i] = sg ? -v : v;
      }
      break;
   }
   case VK_FORMAT_R32G32B32A32_SFLOAT: memcpy(c, q, 16); break;
   case VK_FORMAT_R32_SFLOAT: memcpy(c, q, 4); c[1] = c[2] = 0.0f; c[3] = 1.0f; break;
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
      uint32_t w;
      memcpy(&w, q, 4);
      c[0] = (w & 0x3ff) / 1023.0f; c[1] = ((w >> 10) & 0x3ff) / 1023.0f;
      c[2] = ((w >> 20) & 0x3ff) / 1023.0f; c[3] = ((w >> 30) & 3) / 3.0f;
      break;
   }
   default: break;
   }
}

static inline uint8_t
u8(float v)
{
   if (!(v > 0.0f)) return 0;
   if (v >= 1.0f) return 255;
   return (uint8_t)lrintf(v * 255.0f);
}

static uint16_t
f2h(float f)
{
   union { float f; uint32_t u; } v = { f };
   uint32_t s = (v.u >> 16) & 0x8000, e = (v.u >> 23) & 0xff, m = v.u & 0x7fffff;
   if (e == 0xff) return (uint16_t)(s | 0x7c00 | (m ? 0x200 : 0));
   int x = (int)e - 127 + 15;
   if (x >= 31) return (uint16_t)(s | 0x7c00);
   if (x <= 0) {
      if (x < -10) return (uint16_t)s;
      m |= 0x800000;
      return (uint16_t)(s | (m >> (14 - x)));
   }
   return (uint16_t)(s | (uint32_t)(x << 10) | (m >> 13));
}

static void
px_write(const struct surf *s, unsigned x, unsigned y, const float c[4])
{
   uint8_t *q = s->p + (size_t)y * s->stride + (size_t)x * px_size(s->fmt);
   switch (s->fmt) {
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
      for (unsigned i = 0; i < 4; i++) q[i] = u8(c[i]);
      break;
   case VK_FORMAT_R8G8B8A8_SRGB:
   case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
      for (unsigned i = 0; i < 3; i++) q[i] = u8(lin_to_srgb(c[i]));
      q[3] = u8(c[3]);
      break;
   case VK_FORMAT_B8G8R8A8_UNORM:
      q[0] = u8(c[2]); q[1] = u8(c[1]); q[2] = u8(c[0]); q[3] = u8(c[3]);
      break;
   case VK_FORMAT_B8G8R8A8_SRGB:
      q[0] = u8(lin_to_srgb(c[2])); q[1] = u8(lin_to_srgb(c[1])); q[2] = u8(lin_to_srgb(c[0]));
      q[3] = u8(c[3]);
      break;
   case VK_FORMAT_R16G16B16A16_SFLOAT: {
      uint16_t h[4];
      for (unsigned i = 0; i < 4; i++) h[i] = f2h(c[i]);
      memcpy(q, h, sizeof h);
      break;
   }
   case VK_FORMAT_R32G32B32A32_SFLOAT: memcpy(q, c, 16); break;
   case VK_FORMAT_R32_SFLOAT: memcpy(q, c, 4); break;
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
      uint32_t w = ((uint32_t)lrintf(fminf(fmaxf(c[0], 0), 1) * 1023.0f)) |
                   ((uint32_t)lrintf(fminf(fmaxf(c[1], 0), 1) * 1023.0f) << 10) |
                   ((uint32_t)lrintf(fminf(fmaxf(c[2], 0), 1) * 1023.0f) << 20) |
                   ((uint32_t)lrintf(fminf(fmaxf(c[3], 0), 1) * 3.0f) << 30);
      memcpy(q, &w, 4);
      break;
   }
   default: break;
   }
}

/* ---- depth/stencil attachment ---- */
static float
zs_read(const struct mali_att *a, uint8_t *p, unsigned x, unsigned y)
{
   uint8_t *q = p + (size_t)y * a->row_stride + (size_t)x * (a->zs_format == 3 ? 2 : 4);
   float d = 1.0f;
   if (a->zs_format == 1) {
      memcpy(&d, q, 4);
   } else if (a->zs_format == 2) {
      uint32_t w; memcpy(&w, q, 4); d = (float)(w & 0xffffff) / 16777215.0f;
   } else if (a->zs_format == 3) {
      uint16_t w; memcpy(&w, q, 2); d = (float)w / 65535.0f;
   }
   return d;
}

static void
zs_write(const struct mali_att *a, uint8_t *p, unsigned x, unsigned y, float d)
{
   uint8_t *q = p + (size_t)y * a->row_stride + (size_t)x * (a->zs_format == 3 ? 2 : 4);
   if (a->zs_format == 1) {
      memcpy(q, &d, 4);
   } else if (a->zs_format == 2) {
      uint32_t w; memcpy(&w, q, 4); w = (w & 0xff000000u) | (uint32_t)lrintf(fminf(fmaxf(d, 0), 1) * 16777215.0f);
      memcpy(q, &w, 4);
   } else if (a->zs_format == 3) {
      uint16_t w = (uint16_t)lrintf(fminf(fmaxf(d, 0), 1) * 65535.0f);
      memcpy(q, &w, 2);
   }
}

/* ---- vertex attribute fetch: raw bytes -> float[4], by the VkFormat the pipeline declared ---- */
static bool
attr_fetch(int fmt, const uint8_t *src, float out[4])
{
   out[0] = out[1] = out[2] = 0.0f;
   out[3] = 1.0f;
   switch (fmt) {
   case VK_FORMAT_R32_SFLOAT: memcpy(out, src, 4); return true;
   case VK_FORMAT_R32G32_SFLOAT: memcpy(out, src, 8); return true;
   case VK_FORMAT_R32G32B32_SFLOAT: memcpy(out, src, 12); return true;
   case VK_FORMAT_R32G32B32A32_SFLOAT: memcpy(out, src, 16); return true;
   case VK_FORMAT_R8G8B8A8_UNORM: for (unsigned i = 0; i < 4; i++) out[i] = src[i] / 255.0f; return true;
   case VK_FORMAT_R8G8_UNORM: for (unsigned i = 0; i < 2; i++) out[i] = src[i] / 255.0f; return true;
   case VK_FORMAT_R16G16_SFLOAT: {
      struct surf s = {.p = (uint8_t *)src, .stride = 4, .w = 1, .h = 1, .fmt = VK_FORMAT_R16G16B16A16_SFLOAT};
      float c[4];   /* reuse the half decoder: a 2-component read of the 4-component layout is wrong, do it directly */
      (void)s; (void)c;
      for (unsigned i = 0; i < 2; i++) {
         uint16_t h; memcpy(&h, src + 2 * i, 2);
         uint32_t sg = (h >> 15) & 1, ex = (h >> 10) & 0x1f, mn = h & 0x3ff;
         float v = ex == 0 ? ldexpf((float)mn, -24) : (ex == 31 ? INFINITY : ldexpf((float)(mn | 0x400), (int)ex - 25));
         out[i] = sg ? -v : v;
      }
      return true;
   }
   default: return false;
   }
}

static bool
depth_test(VkCompareOp op, float a, float b)
{
   switch (op) {
   case VK_COMPARE_OP_NEVER: return false;
   case VK_COMPARE_OP_LESS: return a < b;
   case VK_COMPARE_OP_EQUAL: return a == b;
   case VK_COMPARE_OP_LESS_OR_EQUAL: return a <= b;
   case VK_COMPARE_OP_GREATER: return a > b;
   case VK_COMPARE_OP_NOT_EQUAL: return a != b;
   case VK_COMPARE_OP_GREATER_OR_EQUAL: return a >= b;
   case VK_COMPARE_OP_ALWAYS: return true;
   default: return true;
   }
}

static float
bfactor(VkBlendFactor f, const float s[4], const float d[4], const float bc[4], unsigned i, bool alpha)
{
   unsigned ch = alpha ? 3 : i;
   switch (f) {
   case VK_BLEND_FACTOR_ZERO: return 0.0f;
   case VK_BLEND_FACTOR_ONE: return 1.0f;
   case VK_BLEND_FACTOR_SRC_COLOR: return s[ch];
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return 1.0f - s[ch];
   case VK_BLEND_FACTOR_DST_COLOR: return d[ch];
   case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return 1.0f - d[ch];
   case VK_BLEND_FACTOR_SRC_ALPHA: return s[3];
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return 1.0f - s[3];
   case VK_BLEND_FACTOR_DST_ALPHA: return d[3];
   case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return 1.0f - d[3];
   case VK_BLEND_FACTOR_CONSTANT_COLOR: return bc[ch];
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR: return 1.0f - bc[ch];
   case VK_BLEND_FACTOR_CONSTANT_ALPHA: return bc[3];
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA: return 1.0f - bc[3];
   case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE: return alpha ? 1.0f : fminf(s[3], 1.0f - d[3]);
   default: return 0.0f;   /* dual-source and the odd factors: not in the CPU model */
   }
}

static float
bop(VkBlendOp op, float src, float dst)
{
   switch (op) {
   case VK_BLEND_OP_ADD: return src + dst;
   case VK_BLEND_OP_SUBTRACT: return src - dst;
   case VK_BLEND_OP_REVERSE_SUBTRACT: return dst - src;
   case VK_BLEND_OP_MIN: return fminf(src, dst);
   case VK_BLEND_OP_MAX: return fmaxf(src, dst);
   default: return src;
   }
}

/* An edge of the triangle as the weight functions traverse it (v_a -> v_b), already multiplied by the sign that makes
 * the interior weights positive. In framebuffer coordinates (y grows down) a top edge is horizontal with the interior
 * below it, and a left edge runs downwards: those own the samples that lie exactly on them. The other edges must not,
 * or the neighbour on the shared edge covers the same sample too. */
static bool
edge_is_top_left(float ex, float ey)
{
   return ey > 0.0f || (ey == 0.0f && ex > 0.0f);
}

/* ---- one triangle: viewport, cull, scissor, depth, fragment, blend, store ---- */
struct rctx {
   const struct mali_frame *f;
   const struct mali_draw *d;
   const struct mali_soft_program *p;
   struct surf col[MALI_SOFT_MRT];
   unsigned ncol;
   uint8_t *scratch[MALI_SOFT_MRT];   /* storeOp DONT_CARE targets render here (a copy of memory), freed by mali_soft_frame */
   uint8_t *zs;
   bool sc_set;
   int ax0, ay0, ax1, ay1;
};

static void
raster_tri(struct rctx *c, const float clip[3][4], const float vary[3][MALI_SOFT_VARY][4])
{
   const struct mali_draw *d = c->d;
   const struct mali_fixed *fx = &d->fx;
   const bool set = fx->set;
   const unsigned cull = set ? fx->cull : 0;
   const bool front_ccw = set ? fx->front_ccw : true;
   const bool depth_clip = set ? fx->depth_clip : true;
   const uint8_t wmask = set ? fx->wmask : 0xfu;   /* a configured 0 is a depth-only draw, not "unset" */

   if (clip[0][3] <= 1e-6f || clip[1][3] <= 1e-6f || clip[2][3] <= 1e-6f)
      return;   /* no near-plane clipping: triangles that reach behind the eye are dropped (documented) */
   float sx[3], sy[3], sz[3], iw[3];
   for (unsigned i = 0; i < 3; i++) {
      const float nx = clip[i][0] / clip[i][3], ny = clip[i][1] / clip[i][3];
      sx[i] = d->vp[0] + (nx + 1.0f) * d->vp[2] * 0.5f;
      sy[i] = d->vp[1] + (ny + 1.0f) * d->vp[3] * 0.5f;
      sz[i] = clip[i][2] / clip[i][3];
      iw[i] = 1.0f / clip[i][3];
   }
   const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
   if (fabsf(area) < 1e-9f)
      return;
   /* signed area in framebuffer coordinates (y down): the positive winding is counter-clockwise in NDC */
   const bool front = (area > 0.0f) == front_ccw;
   if (getenv("MALI_SOFT_DBG"))
      fprintf(stderr, "soft: tri area %.1f front %d cull %u\n", area, front, cull);
   if ((cull & VK_CULL_MODE_FRONT_BIT) && front)
      return;
   if ((cull & VK_CULL_MODE_BACK_BIT) && !front)
      return;
   /* framebuffer depths, for the bias slope: Vulkan defines that slope in framebuffer coordinates */
   const float zf[3] = {d->vp[4] + sz[0] * (d->vp[5] - d->vp[4]),
                        d->vp[4] + sz[1] * (d->vp[5] - d->vp[4]),
                        d->vp[4] + sz[2] * (d->vp[5] - d->vp[4])};
   const float s = area > 0 ? 1.0f : -1.0f;   /* make the edge functions positive inside */
   const float iarea = 1.0f / (area * s);

   int x0 = (int)floorf(fminf(sx[0], fminf(sx[1], sx[2])));
   int x1 = (int)ceilf(fmaxf(sx[0], fmaxf(sx[1], sx[2])));
   int y0 = (int)floorf(fminf(sy[0], fminf(sy[1], sy[2])));
   int y1 = (int)ceilf(fmaxf(sy[0], fmaxf(sy[1], sy[2])));
   x0 = x0 > c->ax0 ? x0 : c->ax0;
   y0 = y0 > c->ay0 ? y0 : c->ay0;
   x1 = x1 < c->ax1 ? x1 : c->ax1;
   y1 = y1 < c->ay1 ? y1 : c->ay1;
   if (c->sc_set) {   /* a set scissor, empty or not: an empty rectangle covers no samples */
      x0 = x0 > d->sc[0] ? x0 : d->sc[0];
      y0 = y0 > d->sc[1] ? y0 : d->sc[1];
      x1 = x1 < d->sc[0] + d->sc[2] ? x1 : d->sc[0] + d->sc[2];
      y1 = y1 < d->sc[1] + d->sc[3] ? y1 : d->sc[1] + d->sc[3];
   }
   x0 = x0 > 0 ? x0 : 0;
   y0 = y0 > 0 ? y0 : 0;
   if (x1 <= x0 || y1 <= y0)
      return;   /* nothing survives the clip: decide in signed arithmetic, never cast a negative to unsigned */
   const unsigned xe = x1 < (int)c->f->width ? (unsigned)x1 : c->f->width;
   const unsigned ye = y1 < (int)c->f->height ? (unsigned)y1 : c->f->height;

   const float r = c->f->zs.zs_format == 1 ? ldexpf(1.0f, -23) : c->f->zs.zs_format == 2 ? 1.0f / 16777215.0f : 1.0f / 65535.0f;
   const bool dep_en = d->gfx.depth_test;
   const unsigned dep_func = d->gfx.depth_func;
   const bool dep_write = d->gfx.depth_write;

   for (unsigned y = (unsigned)y0; y < ye; y++) {
      for (unsigned x = (unsigned)x0; x < xe; x++) {
         const float px = (float)x + 0.5f, py = (float)y + 0.5f;
         float w0 = ((sx[2] - sx[1]) * (py - sy[1]) - (sy[2] - sy[1]) * (px - sx[1])) * s;
         float w1 = ((sx[0] - sx[2]) * (py - sy[2]) - (sy[0] - sy[2]) * (px - sx[2])) * s;
         float w2 = ((sx[1] - sx[0]) * (py - sy[0]) - (sy[1] - sy[0]) * (px - sx[0])) * s;
         w0 *= iarea; w1 *= iarea; w2 *= iarea;
         if (w0 < 0 || w1 < 0 || w2 < 0)
            continue;
         /* Vulkan's fill rule: a sample exactly on an edge is covered only when that edge is a top or left edge, so
          * two triangles sharing a diagonal cover the seam exactly once instead of both (with blending that is a
          * line along every shared edge; with the depth test on the second write usually hid it). */
         if ((w0 == 0 && !edge_is_top_left((sx[2] - sx[1]) * s, (sy[2] - sy[1]) * s)) ||
             (w1 == 0 && !edge_is_top_left((sx[0] - sx[2]) * s, (sy[0] - sy[2]) * s)) ||
             (w2 == 0 && !edge_is_top_left((sx[1] - sx[0]) * s, (sy[1] - sy[0]) * s)))
            continue;
         const float zn = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];   /* NDC depth, clipped against [0, 1] */
         if (depth_clip && (zn < 0.0f || zn > 1.0f))
            continue;
         /* the framebuffer depth the test and the store see is the viewport transform of the NDC depth */
         float z = d->vp[4] + zn * (d->vp[5] - d->vp[4]);
         if (set && fx->bias_en) {
            float b = fx->bias_const * r;
            float dzdx = fabsf((sx[0] - sx[1]) * zf[2] + (sx[1] - sx[2]) * zf[0] + (sx[2] - sx[0]) * zf[1]) * iarea;
            b += fx->bias_slope * dzdx;
            if (fx->bias_clamp != 0.0f)
               b = fminf(fmaxf(b, -fabsf(fx->bias_clamp)), fabsf(fx->bias_clamp));
            z += b;
         }
         if (dep_en && c->zs && !depth_test((VkCompareOp)dep_func, z, zs_read(&c->f->zs, c->zs, x, y)))
            continue;
         /* perspective-correct interpolation: sum(w * v / w) / sum(w / w) */
         const float den = w0 * iw[0] + w1 * iw[1] + w2 * iw[2];
         float vin[MALI_SOFT_VARY][4];
         for (unsigned v = 0; v < MALI_SOFT_VARY; v++)
            for (unsigned k = 0; k < 4; k++)
               vin[v][k] = (w0 * vary[0][v][k] * iw[0] + w1 * vary[1][v][k] * iw[1] + w2 * vary[2][v][k] * iw[2]) / den;
         float frag[MALI_SOFT_MRT][4];
         for (unsigned r = 0; r < MALI_SOFT_MRT; r++) { frag[r][0] = frag[r][1] = frag[r][2] = 0; frag[r][3] = 1; }
         if (c->p->fs_mrt)
            c->p->fs_mrt(c->p->user, (const float (*)[4])vin, d->push[1], x, y, frag);
         else
            c->p->fs(c->p->user, (const float (*)[4])vin, d->push[1], x, y, frag[0]);
         g_st.frags++;
         /* the blend state of attachment 0 applies to every attachment (Vulkan 1.0: no independentBlend) */
         for (unsigned r = 0; r < c->ncol; r++) {
            float dst[4];
            px_read(&c->col[r], x, y, dst);
            float out[4];
            for (unsigned k = 0; k < 4; k++) {
               if (!(wmask & (1u << k))) { out[k] = dst[k]; continue; }
               if (set && fx->blend_en && k < 3)
                  out[k] = bop((VkBlendOp)fx->op_c, frag[r][k] * bfactor((VkBlendFactor)fx->src_c, frag[r], dst, fx->bconst, k, false),
                                            dst[k] * bfactor((VkBlendFactor)fx->dst_c, frag[r], dst, fx->bconst, k, false));
               else if (set && fx->blend_en)
                  out[k] = bop((VkBlendOp)fx->op_a, frag[r][k] * bfactor((VkBlendFactor)fx->src_a, frag[r], dst, fx->bconst, k, true),
                                            dst[k] * bfactor((VkBlendFactor)fx->dst_a, frag[r], dst, fx->bconst, k, true));
               else
                  out[k] = frag[r][k];
            }
            px_write(&c->col[r], x, y, out);
         }
         if (dep_en && dep_write && c->zs)
            zs_write(&c->f->zs, c->zs, x, y, z);
      }
   }
}

static bool soft_frame(const struct mali_hw *hw, const struct mali_frame *f, struct rctx *cp);

bool
mali_soft_frame(const struct mali_hw *hw, const struct mali_frame *f)
{
   /* g_st and the program registry are process-wide, so two device contexts must not render at the same time: one
    * frame would clear the other's counters. Serialise whole frames (the harness renders a frame at a time anyway). */
   while (atomic_flag_test_and_set_explicit(&g_busy, memory_order_acquire))
      ;
   struct rctx c;
   memset(&c, 0, sizeof c);
   const bool ok = soft_frame(hw, f, &c);
   for (unsigned i = 0; i < MALI_SOFT_MRT; i++)
      free(c.scratch[i]);
   atomic_flag_clear_explicit(&g_busy, memory_order_release);
   return ok;
}

static bool
soft_frame(const struct mali_hw *hw, const struct mali_frame *f, struct rctx *cp)
{
   memset(&g_st, 0, sizeof g_st);
   g_st.frames = 1;
   if (!hw || !hw->va_cpu)
      return unsupported("no va_cpu resolver (the fake backend is required)");
   if (f->compute)
      return unsupported("compute frames");
   if (f->samples > 1)
      return unsupported("multisampled frames");
   if (f->has_st)
      return unsupported("stencil attachment");
   if (!f->has_colour && !f->has_zs)
      return unsupported("frame without attachments");

   cp->f = f;
   /* the render area clips every write, clears included: a CLEAR only touches f->area */
   const int32_t *ar = f->area;
   cp->ax0 = ar[2] ? ar[0] : 0;
   cp->ay0 = ar[2] ? ar[1] : 0;
   cp->ax1 = ar[2] ? ar[0] + (int)ar[2] : (int)f->width;
   cp->ay1 = ar[2] ? ar[1] + (int)ar[3] : (int)f->height;
   if (cp->ax0 < 0) cp->ax0 = 0;
   if (cp->ay0 < 0) cp->ay0 = 0;
   if (cp->ax1 > (int)f->width) cp->ax1 = (int)f->width;
   if (cp->ay1 > (int)f->height) cp->ay1 = (int)f->height;
   if (f->has_colour) {
      const unsigned n = f->ncolour ? f->ncolour : 1;
      if (n > MALI_SOFT_MRT)
         return unsupported("more colour attachments than the CPU model has");
      for (unsigned r = 0; r < n; r++) {
         const struct mali_att *a = r ? &f->colour_x[r - 1] : &f->colour;
         const int fmt = a->rt ? a->rt->vk_format : VK_FORMAT_R8G8B8A8_UNORM;
         const unsigned bpp = px_size(fmt);
         if (!bpp)
            return unsupported("colour format not in the CPU model");
         if (a->row_stride < f->width * bpp)
            return unsupported("colour row stride smaller than one row");
         const size_t bytes = (size_t)a->row_stride * f->height;
         uint8_t *mem = hw->va_cpu(hw->user, a->va, bytes);
         if (!mem)
            return unsupported("colour attachment address is not mapped");
         if (a->no_store) {   /* storeOp DONT_CARE: work on a copy so memory keeps what it had */
            cp->scratch[r] = malloc(bytes);
            if (!cp->scratch[r])
               return unsupported("out of memory for a DONT_CARE colour target");
            memcpy(cp->scratch[r], mem, bytes);
            mem = cp->scratch[r];
         }
         cp->col[r] = (struct surf){.p = mem, .stride = a->row_stride, .w = f->width, .h = f->height, .fmt = fmt};
         /* loadOp CLEAR clears; DONT_CARE is the driver's clear-to-0 (mali_render.c); LOAD keeps what is in memory. */
         if (a->clear || a->dont_care) {
            const float z[4] = {0, 0, 0, 0};
            for (int y = cp->ay0; y < cp->ay1; y++)
               for (int x = cp->ax0; x < cp->ax1; x++)
                  px_write(&cp->col[r], (unsigned)x, (unsigned)y, a->clear ? a->clear_color : z);
         }
      }
      cp->ncol = n;
   }
   if (f->has_zs) {
      if (f->zs.zs_format < 1 || f->zs.zs_format > 3)
         return unsupported("depth format not in the CPU model");
      cp->zs = hw->va_cpu(hw->user, f->zs.va, (size_t)f->zs.row_stride * f->height);
      if (!cp->zs)
         return unsupported("depth attachment address is not mapped");
      if (f->zs.clear)
         for (int y = cp->ay0; y < cp->ay1; y++)
            for (int x = cp->ax0; x < cp->ax1; x++)
               zs_write(&f->zs, cp->zs, (unsigned)x, (unsigned)y, f->zs.clear_depth);
   }
   struct mali_draw x;
   memset(&x, 0, sizeof x);
   for (unsigned di = 0; di < f->ndraws; di++) {
      const struct mali_rec *r = &f->draws[di];
      /* An indirect draw is expanded (mali_cmd.c) before it reaches here and the origin is lost with it, so reject it
       * on the record: a three-vertex indirect draw would otherwise pass as a direct one and "succeed". */
      if (r->ind)
         return unsupported("indirect draw");
      mali_draw_expand(&x, r);
      g_st.draws++;
      const struct mali_soft_program *p = find_prog(&x.gfx);
      if (!p || !p->vs || !p->fs)
         return unsupported("no CPU program registered for this pipeline (mali_soft_bind_program)");
      cp->d = &x;
      cp->p = p;
      cp->sc_set = x.sc_set;
      if (cp->ncol > 1 && !p->fs_mrt)
         return unsupported("MRT frame: the program has no fs_mrt");
      if (x.gfx.discard || x.count == 0 || x.instance_count == 0)
         continue;
      if (x.gfx.prim != 8 && x.gfx.prim != 10 && x.gfx.prim != 12)
         return unsupported("non-triangle topology");
      /* A restart index is a primitive boundary, not a vertex: fetching it as one reads outside the vertex data and
       * joins the strips across the restart. Reject before the fetch until the assembly below can split on it. */
      if (x.gfx.prim_restart && x.indexed)
         return unsupported("primitive restart in an indexed draw");

      const uint8_t *vb[MALI_MAX_VB];
      for (unsigned b = 0; b < MALI_MAX_VB; b++)
         vb[b] = x.vb_va[b] ? hw->va_cpu(hw->user, x.vb_va[b], x.vb_size[b]) : NULL;
      const uint8_t *ib = NULL;
      if (x.indexed) {
         ib = hw->va_cpu(hw->user, x.index_va, x.index_bytes);
         if (!ib)
            return unsupported("index buffer address is not mapped");
      }
      const unsigned nv = x.count;
      if (nv > (1u << 20))
         return unsupported("draw with more than 1M vertices");
      float (*vclip)[4] = malloc((size_t)nv * sizeof *vclip);
      float (*vvary)[MALI_SOFT_VARY][4] = malloc((size_t)nv * sizeof *vvary);
      if (!vclip || !vvary) {
         free(vclip); free(vvary);
         return unsupported("out of memory for the vertex arrays");
      }
      bool bad_fmt = false;
      for (unsigned inst = 0; inst < x.instance_count && !bad_fmt; inst++) {
         for (unsigned vi = 0; vi < nv; vi++) {
            unsigned idx;
            if (x.indexed) {
               if (x.index_size == 2) { uint16_t v; memcpy(&v, ib + (size_t)(x.first_index + vi) * 2, 2); idx = v; }
               else if (x.index_size == 4) { uint32_t v; memcpy(&v, ib + (size_t)(x.first_index + vi) * 4, 4); idx = v; }
               else idx = ib[x.first_index + vi];
               idx += (uint32_t)x.first_vertex;
            } else {
               idx = x.first_vertex + vi;
            }
            float attr[MALI_MAX_ATTR][4];
            for (unsigned a = 0; a < x.gfx.nattr; a++) {
               const struct mali_attr *at = &x.gfx.attr[a];
               unsigned bidx = idx;
               if (at->instance)
                  bidx = x.first_instance + inst / (at->divisor ? at->divisor : 1);
               uint32_t stride = at->stride;
               if (x.gfx.dyn_stride && at->binding < MALI_MAX_VB)
                  stride = x.vb_stride[at->binding];
               if (at->zero_div)
                  stride = 0;
               const uint8_t *src = vb[at->binding];
               if (!src) {
                  memset(attr[a], 0, sizeof attr[a]);
                  attr[a][3] = 1.0f;
                  continue;
               }
               if (!attr_fetch(at->vk_format, src + at->offset + (size_t)bidx * stride, attr[a])) {
                  bad_fmt = true;
                  break;
               }
            }
            if (bad_fmt)
               break;
            p->vs(p->user, (const float (*)[4])attr, x.push[0], x.first_vertex + vi,
                  x.first_instance + inst, vclip[vi], vvary[vi]);
         }
         if (bad_fmt)
            break;
         /* emit triangles for the three topologies */
         unsigned step = x.gfx.prim == 8 ? 3 : 1;
         for (unsigned i = 0; i + 2 < nv; i += step) {
            unsigned a = i, b = i + 1, cc = i + 2;
            if (x.gfx.prim == 10 && (i & 1)) { b = i + 2; cc = i + 1; }   /* strip: alternate the winding */
            if (x.gfx.prim == 12) { a = 0; b = i + 1; cc = i + 2; }      /* fan: i=0 gives (0,1,2), i=1 gives (0,2,3) */
            const float c3[3][4] = {{vclip[a][0], vclip[a][1], vclip[a][2], vclip[a][3]},
                                    {vclip[b][0], vclip[b][1], vclip[b][2], vclip[b][3]},
                                    {vclip[cc][0], vclip[cc][1], vclip[cc][2], vclip[cc][3]}};
            float v3[3][MALI_SOFT_VARY][4];
            memcpy(v3[0], vvary[a], sizeof v3[0]);
            memcpy(v3[1], vvary[b], sizeof v3[1]);
            memcpy(v3[2], vvary[cc], sizeof v3[2]);
            g_st.tris++;
            raster_tri(cp, c3, (const float (*)[MALI_SOFT_VARY][4])v3);
         }
      }
      free(vclip);
      free(vvary);
      if (bad_fmt)
         return unsupported("vertex attribute format not in the CPU model");
   }
   return true;
}
