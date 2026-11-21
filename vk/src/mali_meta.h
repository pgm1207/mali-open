/* SPDX-License-Identifier: MIT */
/* M12/M13, Mesa-free core of the meta operations (copies, fills, clears, blits, MSAA resolve, query result copies).
 *
 * Every operation is planned as one or more compute jobs (struct mali_meta_job: shader id, push constants, <= 2 buffer bindings,
 * workgroup counts). Images are linear by default (R3-PLAN.md M5), so an image is "a buffer with a row/slice pitch" for the shaders
 * and no storage-image descriptor is needed. A job is materialised into a struct mali_dispatch (mali_cs_emit.h): descriptor set in
 * persistent GPU memory, shader binary uploaded once (seam mali_meta_set_dev). The command buffer then pushes it as a compute frame;
 * frames complete in order (mali_cmd_submit waits for each), so no barrier is needed between meta jobs.
 *
 * Plain C: no Vulkan/Mesa header. mali_meta_ref_run() is a bit-exact (copy) / formula-equal (blit) software reference of the shaders
 * that treats GPU VAs as host pointers; the tests use it. Shaders: shader/meta_{copy,blit,qcopy}.comp -> mali_meta_spv.h. */
#ifndef MALI_META_H
#define MALI_META_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mali_cs_emit.h"

#ifdef __cplusplus
extern "C" {
#endif

/* BC decode shaders (mali_bc.h, 5 of them) follow the three built-in ones: id = MALI_META_BC_FIRST + enum mali_bc_shader. */
enum mali_meta_shader { MALI_META_COPY = 0, MALI_META_BLIT = 1, MALI_META_QCOPY = 2, MALI_META_BC_FIRST = 3, MALI_META_NSHADERS = 8 };

#define MALI_META_PUSH_WORDS 32

struct mali_meta_bind { uint64_t va; uint32_t size; };

struct mali_meta_job {
   enum mali_meta_shader sh;
   uint32_t push[MALI_META_PUSH_WORDS];
   unsigned push_bytes;
   struct mali_meta_bind bind[2];   /* 0 = source (set 0 binding 0), 1 = destination (binding 1); bind[0].va == 0 with inline_data: staged */
   unsigned nbind;
   uint32_t groups[3];
   uint8_t *inline_data;            /* vkCmdUpdateBuffer payload (owned by the list), copied into GPU memory at materialise time */
   uint32_t inline_len;
};

struct mali_meta_list { struct mali_meta_job *j; unsigned n, cap; };
void mali_meta_list_free(struct mali_meta_list *l);

/* A linear surface: byte `off` from `va` is the first texel, rows `row` bytes apart, slices (z or array layer) `slice` bytes apart.
 * size = bytes bound at va (bounds for the descriptor). */
struct mali_meta_surf { uint64_t va, size, off; uint32_t row, slice; };

/* All planners: 0 or -ENOMEM / -ERANGE (span over 4 GiB) / -EINVAL. */
/* Rectangle ex x ey x ez of `bpt`-byte elements, src -> dst. Any alignment (byte path when not 4-aligned). */
int mali_meta_plan_copy(struct mali_meta_list *l, const struct mali_meta_surf *dst, const struct mali_meta_surf *src, uint32_t bpt,
                        uint32_t ex, uint32_t ey, uint32_t ez);
/* vkCmdCopyBuffer region (flat, any alignment). */
int mali_meta_plan_buffer_copy(struct mali_meta_list *l, const struct mali_meta_surf *dst, const struct mali_meta_surf *src, uint64_t size);
/* Constant fill: bpt <= 16 bytes of `cst` (4 words) replicated over the rectangle. */
int mali_meta_plan_const(struct mali_meta_list *l, const struct mali_meta_surf *dst, uint32_t bpt, uint32_t ex, uint32_t ey, uint32_t ez,
                         const uint32_t cst[4]);
/* vkCmdFillBuffer: off and size multiples of 4. */
int mali_meta_plan_fill(struct mali_meta_list *l, const struct mali_meta_surf *dst, uint64_t size, uint32_t value);
/* vkCmdUpdateBuffer: size <= 65536, off multiple of 4. data is copied. */
int mali_meta_plan_update(struct mali_meta_list *l, const struct mali_meta_surf *dst, uint64_t size, const void *data);

/* Blit / resolve. Pixels are 8-bit UNORM or 32-bit float x ncomp (1..4); swap_rb when exactly one side is BGRA-ordered. Linear filter only for
 * the same formats the shader handles. src/dst rectangles may be reversed (mirror); written pixels = clamp rectangle (dst space, exclusive). */
struct mali_meta_blit {
   struct mali_meta_surf src, dst;
   uint32_t src_w, src_h;           /* source subresource size (clamp) */
   int32_t s[4], d[4];              /* x0 y0 x1 y1 */
   int32_t clamp[4];                /* written rectangle x0 y0 x1 y1 (exclusive) */
   unsigned ncomp;                  /* 1..4 */
   bool f32, swap_rb;
   int mode;                        /* 0 nearest, 1 linear, 2 resolve */
   uint32_t nsamples, sample_stride;
   uint32_t zslices;                /* with zw: 1 */
   float zw;                        /* mode 1 (3D): weight of the second source slice at sample_stride (trilinear z); 0 = bilinear */
   /* generic path (any plain colour format): per logical channel R,G,B,A  off | size<<8 | type<<16, type 0 absent 1 unorm 2 snorm 3 uint 4 sint 5 float;
    * sbs/dbs bytes per pixel; cls 0 float-ish (unorm/snorm/float), 1 uint, 2 sint (nearest only); sRGB decode/encode of RGB. */
   bool generic, ssrgb, dsrgb;
   uint32_t sch[4], dch[4], sbs, dbs, cls;
};
int mali_meta_plan_blit(struct mali_meta_list *l, const struct mali_meta_blit *b);

/* vkCmdCopyQueryPoolResults over the 16-byte pool entries (see mali_event.h). */
int mali_meta_plan_qcopy(struct mali_meta_list *l, const struct mali_meta_surf *pool, const struct mali_meta_surf *dst, uint32_t first,
                         uint32_t count, uint64_t dst_off, uint64_t dst_stride, uint32_t flags);

/* ---- device seam (installed once at device creation, see R3-PLAN.md M12 wiring) ---- */
struct mali_meta_dev {
   void *user;
   unsigned arch;                                                       /* 11 or 13 */
   void *(*alloc)(void *user, size_t size, size_t align, uint64_t *va); /* zeroed GPU-visible memory that lives until free */
   void (*free)(void *user, void *cpu, uint64_t va, size_t size);
   int (*upload_shader)(void *user, const uint8_t *bin, uint32_t size, uint64_t *va);   /* executable memory, never freed */
};
void mali_meta_prewarm(void);   /* async-compile the built-in meta shaders for the installed device (no-op without one) */
void mali_meta_set_dev(const struct mali_meta_dev *dev);   /* copied; NULL clears (and drops the compiled shader cache) */

/* Long-lived zeroed GPU memory from the installed device seam (query pools, events). NULL when no device is set. */
void *mali_meta_dev_alloc(size_t size, size_t align, uint64_t *va);
void mali_meta_dev_free(void *cpu, uint64_t va, size_t size);

/* Persistent allocations of one command buffer (descriptor sets, staged update data). Release on reset/destroy of the command buffer. */
struct mali_meta_mem { struct { void *cpu; uint64_t va; size_t size; } *a; unsigned n, cap; };
void mali_meta_mem_release(struct mali_meta_mem *m);

/* Compiles (once per arch/shader) and uploads the shader of job j. */
struct mali_bc_job;
/* Appends a BC decode job (compute, 2 SSBO bindings, 48 B push) to the list. 0 or -ENOMEM. */
int mali_meta_list_add_bc(struct mali_meta_list *l, const struct mali_bc_job *bj);
int mali_meta_shader_get(enum mali_meta_shader sh, const struct mali_compiled **c, uint64_t *va);
/* Builds the dispatch (descriptor set bytes in persistent memory, push constants, groups). Does not touch the frame list. */
int mali_meta_materialize(const struct mali_meta_job *j, struct mali_meta_mem *mem, struct mali_dispatch *out);

/* R4: fills a draw (full-viewport triangle) that re-emits the old contents of a linear attachment. kind: colour (any render-target format:
 * arg = format kind | 0x100 sRGB | (colour output type << 16: 0 float, 1 uint, 2 sint), bpp = bytes per texel, see preload.frag), depth (arg 0 = D32F, 1 = D16), stencil, or depth+stencil
 * (va[0] depth, va[1] stencil). vp/sc are left to the caller. 0 or -errno. */
enum mali_preload_kind { MALI_PRELOAD_COLOUR, MALI_PRELOAD_Z, MALI_PRELOAD_S, MALI_PRELOAD_ZS, MALI_PRELOAD_KINDS };
struct mali_preload_src { uint64_t va[2]; uint32_t bytes[2], row[2], arg, bpp; uint32_t clear[4];
                          uint32_t samples, sample_stride; };   /* samples > 1: per-sample preload (gl_SampleID), sample_stride = bytes per sample plane */   /* arg 19 (MALI_PRELOAD_COLOUR): constant colour quad of the 4 clear words (CmdClearAttachments), va[0] unused */
int mali_meta_preload(struct mali_meta_mem *mem, enum mali_preload_kind kind, const struct mali_preload_src *src, struct mali_draw *d);

/* Software reference of the shaders: executes the job with GPU VAs interpreted as host pointers. */
void mali_meta_ref_run(const struct mali_meta_job *j);

#ifdef __cplusplus
}
#endif
#endif
