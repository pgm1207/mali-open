/* SPDX-License-Identifier: MIT */
/* M8, Mesa-free core: a recorded frame (one render pass instance: attachments + draws) -> hardware descriptors + CSF command stream.
 * Plain C: no Vulkan or Mesa headers. All descriptors/CS words come from the libkbase builders that were proven on the phones
 * (kb_draw_*, kb_v13_*, kb_fb_pack, kb_cs_*, kb13_cs_*); this file only sequences them, the way tests/rung2.h (v11) and
 * tests/rung2_v13.h (v13) do. Arch (11 or 13) is a RUNTIME switch (struct mali_env.arch), not a per-arch compile of this file.
 *
 * Seams (everything the core needs from the outside is in struct mali_hw):
 *   alloc      zeroed GPU-visible descriptor memory; MUST be a rolling arena (kb_arena_begin), see libkbase/INTERMITTENT.md
 *   job_begin  called once per frame before any alloc (kb_arena_begin)
 *   submit     hands the finished CS to the queue
 * Limits (documented in R3-PLAN.md "M8"): one colour attachment RGBA8 + optional D32F/D24X8/D16, opaque blend, triangle lists,
 * non-indexed draws, one vertex-attribute path (RGBA32F-style 1D attributes), colour loadOp LOAD unsupported. */
#ifndef MALI_CS_EMIT_H
#define MALI_CS_EMIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mali_compile.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MALI_MAX_VB 8
#define MALI_MAX_ATTR 16
#define MALI_CMD_SETS 8
#define MALI_MAX_DYN 16        /* dynamic UBO/SSBO descriptors per draw (graphics) */
#define MALI_PUSH_MAX 256      /* == MALI_MAX_PUSH_BYTES (VkPhysicalDeviceLimits::maxPushConstantsSize) */

/* Per-queue resources the CS refers to (created by the backend once per device). */
struct mali_env {
   unsigned arch;                 /* 11 or 13 */
   uint32_t hmask;                /* tiler hierarchy mask: v11 0xfe, v13 0 (libkbase tests) */
   unsigned tile_log2;            /* v13 only: FBD / tiler tile size log2 (libkbase default 12) */
   unsigned slot;                 /* scoreboard slot of the vertex/fragment jobs (3) */
   uint32_t tf_or;                /* v13: ORed into the tiler flags (0x40000, V13-NEXT2.md); 0 on v11 */
   uint64_t heap_ctx_va;          /* tiler heap context (kb_tiler_heap.ctx_va) */
   uint64_t heap_first_chunk_va;
   uint32_t heap_chunk_size;
   bool heap_desc_ready;          /* heap descriptor already written by the queue (never rewrite it per frame) */
   void *geo_cpu;                 /* geometry BO: 4 KiB heap descriptor page, then a 64 KiB geometry buffer */
   uint64_t geo_va;
   uint64_t sync_va;              /* CQS word the frame increments (SYNC_ADD64) when it retires */
   int *light_pending;            /* per-device flag: a light ops frame was built, the next submit_signal of this device must not be skipped */
   unsigned core_id_range, tls_threads;   /* compute TLS sizing (BC6H/BC7 spill); 0 = TLS shaders unsupported */
};

struct mali_bo;   /* M15: bo_wrote only stores the pointer */
struct mali_frame;

struct mali_hw {
   void *user;
   void *(*alloc)(void *user, size_t size, size_t align, uint64_t *va);
   void *(*alloc_nz)(void *user, size_t size, size_t align, uint64_t *va);   /* optional: like alloc, memory not cleared */
   void (*job_begin)(void *user);
   int (*submit)(void *user, const uint64_t *cs, unsigned ninstr);
   void *(*tls)(void *user, size_t size, uint64_t *va);   /* persistent 4 KiB-aligned thread-local storage (grows; never shrinks) */
   int (*submit_multi)(void *user, const uint64_t *cs, unsigned ninstr, unsigned nframes);   /* nframes concatenated frame streams, one kick */
   void (*drain)(void *user);   /* wait until every submitted frame has retired */
   void (*bo_wrote)(void *user, struct mali_bo *bo);   /* M15: per-BO GPU-write mark (NULL bo = sweep everything next sync) */
   /* Desktop software rasteriser (mali_soft.c, MALI_SOFT=1): when `soft` is set, a frame is rendered on the CPU from its
    * record instead of being encoded into a command stream, and `va_cpu` resolves the record's GPU VAs to CPU memory. */
   bool (*soft)(void *user, const struct mali_frame *f);
   void *(*va_cpu)(void *user, uint64_t va, size_t size);
   struct mali_env env;
};

/* One vertex attribute: 1D attribute fetch from vertex binding `binding`. pixfmt = Mali Pixel Format word (KB_PIXFMT_*). */
struct mali_attr { uint32_t pixfmt, binding, offset, stride; bool instance; /* per-instance (divisor 1, or 0 with stride 0) */ bool zero_div; /* instance divisor 0: stride 0 whatever the binding says */ uint32_t divisor; /* instance rate: hardware divisor (>= 1) */ int vk_format; /* VkFormat, for the software rasteriser's vertex fetch */ };

/* Pipeline state the draw needs (filled by the pipeline resolver seam in mali_cmd.c, M6 owns the real objects). */
struct mali_gfx_state {
   const struct mali_compiled *vs, *fs;   /* compiled with arch == env.arch */
   unsigned samples;                      /* rasterizationSamples: 1 or 4 */
   bool dyn_stride;                       /* VERTEX_INPUT_BINDING_STRIDE is dynamic: attribute strides come from the bind call (draw.vb_stride) */
   bool discard;                          /* no fragment stage / rasterizerDiscardEnable: draws are skipped */
   bool per_sample;                       /* sampleRateShading: the fragment shader runs per sample (DCD0 evaluate-per-sample) */
   bool logic_inert;                      /* static logicOp enabled but it cannot change the output (no colour output 0 / attachment 0 unwritable): no blend shader needed */
   uint64_t blend_va;                     /* dualSrcBlend: blend shader VA (0 = fixed-function blend); colour attachment 0 */
   uint64_t vs_va, fs_va;                 /* GPU VA of the binaries; VS secondary shader at vs_va + vs->vs_secondary_offset (v11) */
   unsigned nattr;
   struct mali_attr attr[MALI_MAX_ATTR];
   bool depth_test, depth_write;
   unsigned depth_func;                   /* VkCompareOp == kb_func order (NEVER..ALWAYS) */
   unsigned prim;                         /* Mali draw mode: 1 points, 2 lines, 4 line strip, 8 triangles (default 0 = 8), 10 strip, 12 fan */
   bool prim_restart;
   unsigned poly_mode;                    /* fillModeNonSolid: 0 fill, 1 line, 2 point. LINE: prim is 2 and the draw is rewritten to an indexed line list at frame build (mali_poly_lines); POINT: prim is 1 and the draw is likewise rewritten to an indexed point list */
   unsigned poly_prim;                    /* LINE/POINT: the triangle topology (8 list, 10 strip, 12 fan) the pipeline was created with */
   unsigned push_base[2];                 /* byte offset of the push block in the VS / FS FAU (defaults 32 / 0) */
};

/* R4: fixed-function state of one draw (rasteriser, depth bias, colour attachment 0 blend). All zero = "not set": defaults apply
 * (no culling, CCW front, depth clip on, no bias, line width 1, opaque, write mask 0xf). Values are Vulkan enums. */
/* one stencil face: VkCompareOp / VkStencilOp values, masks, reference */
struct mali_st_face { uint8_t cmp, fail, dfail, pass, wmask, vmask, ref; };

struct mali_fixed {
   bool set;
   uint8_t cull;                          /* 1 = cull front faces, 2 = cull back faces */
   bool front_ccw;
   bool depth_clamp, depth_clip;
   bool bias_en;
   float bias_const, bias_slope, bias_clamp;
   float line_width;
   bool blend_en;
   uint8_t wmask;                         /* VK_COLOR_COMPONENT_* (R=1 G=2 B=4 A=8) */
   uint8_t src_c, dst_c, src_a, dst_a;    /* VkBlendFactor */
   uint8_t op_c, op_a;                    /* VkBlendOp (ADD 0, SUBTRACT 1, REVERSE_SUBTRACT 2) */
   float bconst[4];
   /* stencil (Vulkan values; applied only when the frame has a stencil attachment) */
   bool st_test;
   struct mali_st_face st_f, st_b;
};

/* Mali fixed-function blend equation (word 1 bits 0..23) for the factors/ops in f; also reports the constant channel mask used
 * (bit 0..2 colour, 3 alpha). 0 = ok, -1 = not expressible in fixed function (min/max, SRC_ALPHA_SATURATE, dual source, odd pairs). */
int mali_blend_equation(const struct mali_fixed *f, uint32_t *eq, unsigned *const_mask);

/* Record-time dirty mask of a draw: the state groups that changed since the previous draw of the frame (a frame's first draw: all).
 * build_draw_descs reuses the previous draw's blocks by testing these bits only (no compares). */
#define MALI_D_PIPE   0x01u   /* bound pipeline / shaders (gfx state) */
#define MALI_D_FX     0x02u   /* dynamic state other than viewport / scissor (fixed-function block, depth-stencil, blend) */
#define MALI_D_VP     0x04u   /* viewport (feeds the FAUs) */
#define MALI_D_BIND   0x08u   /* vertex buffers, descriptor sets, their count, the dynamic-descriptor count */
#define MALI_D_DYNBUF 0x10u   /* dynamic buffer descriptor values (dynamic offsets) */
#define MALI_D_PUSH   0x20u   /* push constants */
#define MALI_D_SC     0x40u   /* scissor (a per-draw register: only the pipe block is renewed, the descriptor tables are unaffected) */
#define MALI_D_ALL    0x7fu

struct mali_draw {
   uint32_t dirty;                        /* MALI_D_* since the previous draw */
   struct mali_fixed fx;
   struct mali_gfx_state gfx;
   uint64_t vb_va[MALI_MAX_VB];           /* bound vertex buffers (offset applied) */
   uint32_t vb_size[MALI_MAX_VB];
   uint32_t vb_stride[MALI_MAX_VB];       /* dynamic binding strides (gfx.dyn_stride) */
   uint8_t push[2][MALI_PUSH_MAX];        /* [0] vertex stage view, [1] fragment stage view of the push constants */
   float vp[6];                           /* x, y, w, h, minDepth, maxDepth */
   int32_t sc[4];                         /* scissor x, y, w, h */
   bool sc_set;                           /* a scissor was set: an empty one clips everything, unset means render area */
   unsigned nsets;                        /* bound descriptor sets, appear as SRT entries 1.. (entry 0 = driver set) */
   uint64_t set_va[MALI_CMD_SETS];
   uint32_t set_bytes[MALI_CMD_SETS];
   unsigned ndyn;                         /* dynamic buffer descriptors (offset applied), written to driver-set entries MALI_DYN_BUF_START + i of VS and FS */
   uint32_t dyn_desc[MALI_MAX_DYN][8];
   bool indexed;                          /* vkCmdDrawIndexed: index_va/index_size/first_index/vertex_offset used (count = indexCount) */
   uint64_t index_va;                     /* bound index buffer VA with offset applied */
   const uint8_t *index_cpu;              /* CPU view of index_va (NULL without a mapping); read at frame build for polygonMode LINE */
   uint32_t index_bytes;                  /* bytes from index_va to the buffer end */
   unsigned index_size;                   /* 1, 2 or 4 */
   uint32_t first_index;
   uint32_t count, instance_count, first_vertex, first_instance, draw_id;
   uint64_t occ_va;                       /* M13: occlusion counter address (8-aligned, 0 = none); sets OQ register + DCD flags_0 mode */
   const uint8_t *ind;                    /* indirect draw: CPU view of the VkDraw*IndirectCommand array (expanded at submit, mali_cmd.c) */
   const uint32_t *ind_count;             /* ...Count variants: CPU view of the count buffer value (NULL = ind_n) */
   uint32_t ind_n, ind_stride;
   unsigned occ_mode;                     /* M13: 0 disabled, 1 predicate (boolean), 3 counter (genxml "Occlusion Mode") */
};

/* What a draw records (struct mali_rec): a small header plus pointers to immutable state blocks in the frame pool. A block is renewed only when
 * its dirty bits are set, otherwise the pointer is the previous draw's. mali_draw_expand rebuilds the full struct mali_draw at submit. */
struct mali_bpipe { struct mali_fixed fx; struct mali_gfx_state gfx; float vp[6]; int32_t sc[4]; };           /* PIPE|FX|VP|SC */
struct mali_bbind { uint64_t vb_va[MALI_MAX_VB]; uint32_t vb_size[MALI_MAX_VB]; uint32_t vb_stride[MALI_MAX_VB]; unsigned nsets; uint64_t set_va[MALI_CMD_SETS];
                    uint32_t set_bytes[MALI_CMD_SETS]; unsigned ndyn; };                                         /* BIND */
struct mali_bpush { uint8_t push[2][MALI_PUSH_MAX]; };                                                           /* PUSH */
struct mali_bdyn { uint32_t n; uint32_t d[][8]; };                                                               /* BIND|DYNBUF: n entries */

struct mali_rec {
   uint32_t dirty;
   const struct mali_bpipe *pipe;
   const struct mali_bbind *bind;
   const struct mali_bpush *push;
   const struct mali_bdyn *dyn;
   bool indexed;
   uint64_t index_va;
   const uint8_t *index_cpu;
   uint32_t index_bytes;
   unsigned index_size;
   uint32_t first_index;
   uint32_t count, instance_count, first_vertex, first_instance, draw_id;
   uint64_t occ_va;
   const uint8_t *ind;
   const uint32_t *ind_count;
   uint32_t ind_n, ind_stride;
   unsigned occ_mode;
};

/* A compute dispatch (M11): one per frame record, executed with RUN_COMPUTE. */
struct mali_dispatch {
   const struct mali_compiled *cs;        /* compiled with stage COMPUTE */
   uint64_t cs_va;
   uint8_t push[MALI_PUSH_MAX];
   unsigned nsets;
   uint64_t set_va[MALI_CMD_SETS];
   uint32_t set_bytes[MALI_CMD_SETS];
   unsigned ndyn;                         /* dynamic buffer descriptors (offset applied), table MALI_DYN_TABLE like the draw path */
   uint32_t dyn_desc[MALI_MAX_DYN][8];
   uint32_t groups[3];                    /* workgroup counts (direct) */
   uint64_t indirect_va;                  /* != 0: 3 x u32 workgroup counts are loaded from here by the CS */
};

struct mali_op;

/* A blendable non-RGBA8 colour render target (mali_render.c table). NULL in mali_att = R8G8B8A8_UNORM. */
struct mali_rtfmt {
   int vk_format;
   uint8_t internal, wb, ncomp, alloc, is_float;   /* FBD Internal Format, writeback format, components, colour allocation (>>10), 16F/11F clear */
   uint16_t swizzle;                               /* RT swizzle (12 bits) */
   uint32_t blend_pixfmt;                          /* blend conversion format word */
   uint8_t int_bits, is_signed;                    /* integer colour formats (RAW tile buffer): bits per channel (0 = not integer), SINT */
};
const struct mali_rtfmt *mali_rtfmt_lookup(int vk_format, bool *supported);
void mali_rtfmt_clear_words(const struct mali_rtfmt *rt, const float c[4], uint32_t w[4]);

struct mali_att {
   const struct mali_rtfmt *rt;           /* colour only */
   uint64_t va;                           /* 64-byte aligned */
   uint32_t row_stride;                   /* bytes, multiple of 16 */
   int zs_format;                         /* depth only: 1 D32F, 2 D24X8, 3 D16 (enum kb_zs_format); colour is RGBA8 */
   bool clear;                            /* loadOp CLEAR (false = DONT_CARE, handled as clear to 0; LOAD is rejected by the caller) */
   bool store;                            /* storeOp STORE; colour: always stored (no FBD knob yet); depth: false = Z write enable off */
   bool dont_care;                        /* software rasteriser only: loadOp DONT_CARE (!clear && !dont_care = LOAD, memory kept); modelled as clear to 0 */
   bool no_store;                         /* software rasteriser only, colour: storeOp DONT_CARE, the pass's result never reaches memory */
   float clear_color[4];
   float clear_depth;
   uint8_t clear_stencil;                 /* stencil attachment (struct mali_frame.st): S Clear value */
   uint32_t surface_stride;               /* MSAA (frame.samples 4): bytes per sample plane of the multisampled image (samples consecutive) */
   uint64_t resolve_va;                   /* colour, MSAA: single-sample resolve target (hardware "Average" writeback); 0 = store the samples (Layered) */
   uint32_t resolve_row_stride;
};

struct mali_frame {
   unsigned width, height;                /* framebuffer size (attachment extent) */
   int32_t area[4];                       /* render area x, y, w, h */
   bool has_colour, has_zs, has_st;       /* has_st: S8 plane attached (st.va/row_stride/clear/clear_stencil/store); independent of has_zs */
   bool provoking_last;                   /* VK_EXT_provoking_vertex LAST_VERTEX for this render pass: clears 'First provoking vertex' in the FBD (word 11 bit 14) and tiler ctx (word 2 bit 18) */
   unsigned samples;                      /* 0/1 = single sampled, 4 = 4x MSAA (MALI_EXPERIMENTAL_MSAA) */
   uint64_t resolve_src, resolve_dst;     /* MSAA storeOp STORE + resolve: images for the separate resolve pass (0 = none) */
   unsigned resolve_level, resolve_layer;         /* source: the attachment view's mip level / base array layer */
   unsigned resolve_dst_level, resolve_dst_layer; /* destination: the RESOLVE view's own mip level / base array layer */
   struct mali_att colour, zs, st;
   unsigned ncolour;                      /* software rasteriser only: colour attachments (0 = has_colour ? 1 : 0); RT0 is `colour`, RT1.. are colour_x[0..] */
   struct mali_att colour_x[3];           /* RT1..RT3: own va/row_stride/rt format and load/store flags. The hardware path rejects ncolour > 1 */
   bool compute;                          /* a single compute dispatch instead of a render pass: disp is used */
   struct mali_dispatch disp;
   struct mali_op *ops;                   /* M13: an ops frame (events/timestamps/availability, mali_event.h): malloc'd, owned by the frame */
   unsigned nops;
   struct mali_rec *draws;
   unsigned ndraws, cap;
   struct mali_pool *pool;                /* state blocks of the recorded draws (freed with the frame) */
};

struct mali_rec *mali_frame_draw_slot(struct mali_frame *f);   /* next draw slot (uninitialised), NULL on ENOMEM; commit with f->ndraws++ */
int mali_frame_add_draw(struct mali_frame *f, const struct mali_draw *d);   /* full draw (preload): new blocks, dirty ALL. 0 or -ENOMEM */
int mali_frame_add_rec(struct mali_frame *f, const struct mali_rec *r);     /* 0 or -ENOMEM */
unsigned mali_frame_pool_cached(void);                                      /* test hook: recycled pool chunks parked */
void *mali_frame_blk(struct mali_frame *f, size_t n);                       /* zeroed block from the frame pool, NULL on ENOMEM */
/* Updates the running full draw `x` (zero-initialised before the first rec of a frame) with the groups rec `r` renewed. */
void mali_draw_expand(struct mali_draw *x, const struct mali_rec *r);
void mali_frame_finish(struct mali_frame *f);                               /* frees the draw list */

/* Allocates + fills the descriptors via hw->alloc and returns the CS in a malloc'd array (caller frees). 0 or -errno:
 * -ENOMEM (arena full), -ENOTSUP (indexed draw, unsupported format), -EINVAL (no colour attachment, bad shader). */
int mali_frame_build(const struct mali_hw *hw, const struct mali_frame *f, uint64_t **cs, unsigned *ninstr);
/* job_begin + build + write barrier + hw->submit. */
int mali_frame_submit(const struct mali_hw *hw, const struct mali_frame *f);

/* polygonMode LINE (mode 1: 6 indices per triangle) / POINT (mode 2: 3) from the assembled triangles of a list (8) / strip (10) / fan (12); see mali_cs_emit.c. out: 6 * n entries. */
size_t mali_poly_assemble(unsigned mode, unsigned prim, const uint32_t *restart, const uint32_t *v, uint32_t n, uint32_t *out);

/* mali_render.c: framebuffer descriptor (v11 and v13) and per-frame blocks. out: 256 B (KB_FB_DESC_SIZE_ZS), 64-aligned. */
int mali_render_pack_fbd(const struct mali_env *env, const struct mali_frame *f, uint8_t *out, uint64_t tiler_ctx_va,
                         uint64_t sample_locations_va);
/* Converts a 0..1 float to an 8-bit UNORM clear channel (round to nearest, clamped). */
uint8_t mali_unorm8(float c);

#ifdef __cplusplus
}
#endif
#endif
