/* SPDX-License-Identifier: MIT */
/* M12, Vulkan layer of the meta operations: vkCmdCopyBuffer2 / CopyImage2 / BlitImage2 / CopyBufferToImage2 / CopyImageToBuffer2 /
 * FillBuffer / UpdateBuffer / ResolveImage2 and the compute clears. Each region is planned by mali_meta.c into compute jobs which are
 * pushed as compute frames (mali_cmd_push_frame, wiring item, R3-PLAN.md M12), executed in order with everything else.
 *
 * Only linear images are handled (the default, R3-PLAN.md M5): tiled -> VK_ERROR_FEATURE_NOT_PRESENT on the command buffer (sticky,
 * like every unsupported command of mali_cmd.c). */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "mali_cmd.h"
#include "mali_image.h"
#include "mali_meta.h"
#include "mali_meta_vk.h"
#include "mali_bc.h"

#include "util/format/u_format.h"
#include "vk_format.h"

/* Exported from mali_cmd.c by the wiring (alias of push_frame). Weak: the file links without it and the commands then fail cleanly. */
__attribute__((weak)) void mali_cmd_push_frame(struct mali_cmd_buffer *cmd, const struct mali_frame *f);

#define CMD(h) struct mali_cmd_buffer *cmd = mali_cmd_buffer_from_vk(vk_command_buffer_from_handle(h))

/* M15: copy destinations are GPU-written; the fence wait for this submission only CSYNCs marked BOs (vk/MEMORY.md). */
static void
mark_dst_buf(struct mali_cmd_buffer *cmd, struct vk_buffer *b)
{
   mali_cmd_mark_buffer_written(cmd, vk_buffer_to_handle(b));
}

static void
mark_dst_img(struct mali_cmd_buffer *cmd, struct mali_image *img)
{
   mali_cmd_mark_image_written(cmd, mali_image_to_handle(img));
}

/* ---- per command buffer persistent memory (descriptor sets, staged update data) ---- */
static pthread_mutex_t g_tab_lock = PTHREAD_MUTEX_INITIALIZER;
static struct tab_ent { const void *cmd; struct mali_meta_mem mem; } *g_tab;
static unsigned g_ntab, g_captab;

static struct mali_meta_mem *
mem_of(const void *cmd)
{
   struct mali_meta_mem *r = NULL;
   pthread_mutex_lock(&g_tab_lock);
   for (unsigned i = 0; i < g_ntab; i++)
      if (g_tab[i].cmd == cmd)
         r = &g_tab[i].mem;
   if (!r) {
      if (g_ntab == g_captab) {
         unsigned nc = g_captab ? g_captab * 2 : 8;
         struct tab_ent *n = realloc(g_tab, nc * sizeof *n);
         if (!n) {
            pthread_mutex_unlock(&g_tab_lock);
            return NULL;
         }
         g_tab = n;
         g_captab = nc;
      }
      g_tab[g_ntab].cmd = cmd;
      memset(&g_tab[g_ntab].mem, 0, sizeof g_tab[g_ntab].mem);
      r = &g_tab[g_ntab++].mem;
   }
   pthread_mutex_unlock(&g_tab_lock);
   return r;
}

/* R4: loadOp LOAD (see mali_meta_preload). */
int
mali_meta_vk_preload(struct mali_cmd_buffer *cmd, enum mali_preload_kind kind, const struct mali_preload_src *src, struct mali_draw *d)
{
   struct mali_meta_mem *m = mem_of(cmd);
   return m ? mali_meta_preload(m, kind, src, d) : -ENOMEM;
}

/* Wiring: call from the command buffer reset and destroy paths (after the work was waited for). */
void
mali_meta_cmd_forget(const void *cmd)
{
   pthread_mutex_lock(&g_tab_lock);
   for (unsigned i = 0; i < g_ntab; i++)
      if (g_tab[i].cmd == cmd) {
         mali_meta_mem_release(&g_tab[i].mem);
         g_tab[i] = g_tab[--g_ntab];
         break;
      }
   if (!g_ntab) {   /* last command buffer gone: drop the table too (it was the 192-byte LeakSanitizer finding in t_vk_draw) */
      free(g_tab);
      g_tab = NULL;
      g_captab = 0;
   }
   pthread_mutex_unlock(&g_tab_lock);
}

static void
fail(struct mali_cmd_buffer *cmd, VkResult r)
{
   vk_command_buffer_set_error(&cmd->vk, r);
}

/* Plans -> frames. Consumes (frees) the list. */
/* One line, never silently: the failing command's name and the reason. The VkResult alone (FEATURE_NOT_PRESENT at
 * vkEndCommandBuffer, from mali_cmd.c's sticky error) is what apps report as a black screen (HL2 + wined3d), and it
 * does not say whether the cause is a tiled image, an in-pass record, a plan limit or an out-of-memory. */
static void
emit(struct mali_cmd_buffer *cmd, struct mali_meta_list *l, int plan_err, const char *op)
{
   if (plan_err) {
      fprintf(stderr, "mali_vk: %s: plan error %d -> VK_ERROR_FEATURE_NOT_PRESENT\n", op, plan_err);
      if (getenv("MALI_META_DBG")) fprintf(stderr, "mali_meta_vk: plan error %d\n", plan_err);
      fail(cmd, plan_err == -ENOMEM ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_FEATURE_NOT_PRESENT);
      mali_meta_list_free(l);
      return;
   }
   if (!mali_cmd_push_frame) {
      fprintf(stderr, "mali_vk: %s: no meta frame push hook -> VK_ERROR_FEATURE_NOT_PRESENT\n", op);
      fail(cmd, VK_ERROR_FEATURE_NOT_PRESENT);
      mali_meta_list_free(l);
      return;
   }
   /* Recorded inside a render pass: the frame is queued and pushed right after the pass' own frame
    * (mali_cmd_push_frame_deferred, mali_cmd.c), which is what "in submission order with the pass" means on one
    * in-order CS ring. Legal Vulkan, no longer an error. */
   if (cmd->in_render && getenv("MALI_META_DBG"))
      fprintf(stderr, "mali_meta_vk: %s deferred to the end of the render pass\n", op);
   struct mali_meta_mem *mem = mem_of(cmd);
   if (getenv("MALI_META_DBG")) fprintf(stderr, "mali_meta_vk: emit %u jobs\n", l->n);
   for (unsigned i = 0; i < l->n; i++) {
      struct mali_frame f;
      memset(&f, 0, sizeof f);
      f.compute = true;
      int r = mem ? mali_meta_materialize(&l->j[i], mem, &f.disp) : -ENOMEM;
      if (r) {
         fprintf(stderr, "mali_vk: %s: materialize failed (%d) -> %s\n", op, r,
                 r == -ENOMEM ? "VK_ERROR_OUT_OF_DEVICE_MEMORY" : "VK_ERROR_FEATURE_NOT_PRESENT");
         fail(cmd, r == -ENOMEM ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_FEATURE_NOT_PRESENT);
         break;
      }
      mali_cmd_push_frame_deferred(cmd, &f);
   }
   mali_meta_list_free(l);
}

/* Exported for mali_query.c: plan result -> compute frames of the command buffer (consumes the list). */
void
mali_meta_vk_emit(struct mali_cmd_buffer *cmd, struct mali_meta_list *l, int plan_err)
{
   emit(cmd, l, plan_err, "query");
}

/* ---- surfaces ---- */
static void
buf_surf(struct mali_meta_surf *s, struct vk_buffer *b, uint64_t off)
{
   *s = (struct mali_meta_surf){ vk_buffer_address(b, 0), b->size, off, 0, 0 };
}

/* The image subresource at texel offset `o`; false for tiled or unbound images. */
static bool
img_surf(struct mali_meta_surf *s, const struct mali_image *img, VkImageSubresourceLayers sub, VkOffset3D o, const struct mali_plane_layout **pl)
{
   if (img->lay.d.tiled || !img->va) {
      if (getenv("MALI_META_DBG")) fprintf(stderr, "mali_meta_vk: img_surf rejects tiled=%d va=%d fmt=%d\n", (int)img->lay.d.tiled, img->va != 0, (int)img->vk.format);
      return false;
   }
   const struct mali_plane_layout *p = mali_image_plane_for_aspect(&img->lay, sub.aspectMask);
   if (!p)
      return false;
   bool is3d = img->lay.d.type == VK_IMAGE_TYPE_3D;
   const struct mali_slice *sl = &p->slice[sub.mipLevel];
   uint64_t off = mali_image_subresource_offset(p, sub.mipLevel, is3d ? 0 : sub.baseArrayLayer);
   off += (uint64_t)(o.x / p->bw) * p->bs + (uint64_t)(o.y / p->bh) * sl->row_stride;
   if (is3d)
      off += (uint64_t)o.z * sl->surface_stride;
   *s = (struct mali_meta_surf){ img->va, img->lay.size, off, sl->row_stride, (uint32_t)(is3d ? sl->surface_stride : p->array_stride) };
   if (pl)
      *pl = p;
   return true;
}

static uint32_t
lvl(uint32_t v, uint32_t l)
{
   v >>= l;
   return v ? v : 1;
}

static uint32_t
zcount(const struct mali_image *img, VkImageSubresourceLayers sub, VkExtent3D e)
{
   return img->lay.d.type == VK_IMAGE_TYPE_3D ? e.depth : sub.layerCount;
}

/* ---- BC decode (R4-M7): after a write to the compressed region, decode the touched blocks into the shadow ---- */
static int
bc_decode(struct mali_meta_list *l, const struct mali_image *img, VkImageSubresourceLayers sub, VkOffset3D o, VkExtent3D e)
{
   struct mali_bc_list bl = { 0 };
   const bool is3d = img->lay.d.type == VK_IMAGE_TYPE_3D;
   int r;
   if (!img->va)
      return -EINVAL;
   /* texel extent rounded to whole blocks, clamped to the level (the planner accepts "multiple of 4 or reaching the level edge") */
   VkExtent3D lv = { lvl(img->lay.d.extent.width, sub.mipLevel), lvl(img->lay.d.extent.height, sub.mipLevel), lvl(img->lay.d.extent.depth, sub.mipLevel) };
   VkOffset3D o4 = { o.x & ~3, o.y & ~3, o.z };
   VkExtent3D e4 = e;
   e4.width = ((uint32_t)(o.x - o4.x) + e.width + 3) & ~3u;
   e4.height = ((uint32_t)(o.y - o4.y) + e.height + 3) & ~3u;
   if ((uint32_t)o4.x + e4.width > lv.width) e4.width = lv.width - (uint32_t)o4.x;
   if ((uint32_t)o4.y + e4.height > lv.height) e4.height = lv.height - (uint32_t)o4.y;
   if (is3d) {
      r = mali_bc_plan_decode(&bl, &img->bc_lay, img->va, sub.mipLevel, 0, 1, o4, e4);
   } else {
      o4.z = 0;
      e4.depth = 1;
      r = mali_bc_plan_decode(&bl, &img->bc_lay, img->va, sub.mipLevel, sub.baseArrayLayer, sub.layerCount, o4, e4);
   }
   for (unsigned i = 0; !r && i < bl.n; i++)
      r = mali_meta_list_add_bc(l, &bl.j[i]);
   mali_bc_list_free(&bl);
   return r;
}

/* ---- copies ---- */
VKAPI_ATTR void VKAPI_CALL
mali_CmdCopyBuffer2(VkCommandBuffer h, const VkCopyBufferInfo2 *ci)
{
   CMD(h);
   struct vk_buffer *src = vk_buffer_from_handle(ci->srcBuffer), *dst = vk_buffer_from_handle(ci->dstBuffer);
   struct mali_meta_list l = { 0 };
   int r = 0;
   mark_dst_buf(cmd, dst);
   for (uint32_t i = 0; i < ci->regionCount && !r; i++) {
      const VkBufferCopy2 *g = &ci->pRegions[i];
      struct mali_meta_surf s, d;
      buf_surf(&s, src, g->srcOffset);
      buf_surf(&d, dst, g->dstOffset);
      r = mali_meta_plan_buffer_copy(&l, &d, &s, g->size);
   }
   emit(cmd, &l, r, "CmdCopyBuffer2");
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdCopyImage2(VkCommandBuffer h, const VkCopyImageInfo2 *ci)
{
   CMD(h);
   struct mali_image *si = mali_image_from_handle(ci->srcImage), *di = mali_image_from_handle(ci->dstImage);
   struct mali_meta_list l = { 0 };
   int r = 0;
   mark_dst_img(cmd, di);
   for (uint32_t i = 0; i < ci->regionCount && !r; i++) {
      const VkImageCopy2 *g = &ci->pRegions[i];
      /* D+S images are two planes: a region naming both aspects copies the depth plane, then the stencil plane */
      const VkImageAspectFlags am = g->srcSubresource.aspectMask;
      const bool both = (am & VK_IMAGE_ASPECT_DEPTH_BIT) && (am & VK_IMAGE_ASPECT_STENCIL_BIT);
      for (unsigned a = 0; a < (both ? 2u : 1u) && !r; a++) {
         VkImageSubresourceLayers ss = g->srcSubresource, ds = g->dstSubresource;
         if (both) {
            ss.aspectMask = ds.aspectMask = a ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
         }
         struct mali_meta_surf s, d;
         const struct mali_plane_layout *sp, *dp;
         if (!img_surf(&s, si, ss, g->srcOffset, &sp) || !img_surf(&d, di, ds, g->dstOffset, &dp) ||
             sp->bs != dp->bs) {
            r = -EINVAL;
            break;
         }
         /* The extent is in source texels; a compressed <-> uncompressed copy moves one block per texel (equal block size), so the
          * destination extent in its own texels is the block count times its block size. */
         uint32_t ez = zcount(si, ss, g->extent), ex = (g->extent.width + sp->bw - 1) / sp->bw, ey = (g->extent.height + sp->bh - 1) / sp->bh;
         r = mali_meta_plan_copy(&l, &d, &s, sp->bs, ex, ey, ez);
         if (!r && di->bc) {
            VkExtent3D de = g->extent;
            if (sp->bw != dp->bw || sp->bh != dp->bh) {
               de.width = ex * dp->bw;
               de.height = ey * dp->bh;
            }
            r = bc_decode(&l, di, ds, g->dstOffset, de);
         }
      }
   }
   emit(cmd, &l, r, "CmdCopyImage2");
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdCopyBufferToImage2(VkCommandBuffer h, const VkCopyBufferToImageInfo2 *ci)
{
   CMD(h);
   struct vk_buffer *buf = vk_buffer_from_handle(ci->srcBuffer);
   struct mali_image *img = mali_image_from_handle(ci->dstImage);
   struct mali_meta_list l = { 0 };
   int r = 0;
   mark_dst_img(cmd, img);
   for (uint32_t i = 0; i < ci->regionCount && !r; i++) {
      const VkBufferImageCopy2 *g = &ci->pRegions[i];
      struct mali_meta_surf s, d;
      const struct mali_plane_layout *p;
      if (!img_surf(&d, img, g->imageSubresource, g->imageOffset, &p)) {
         r = -EINVAL;
         break;
      }
      uint32_t ex = (g->imageExtent.width + p->bw - 1) / p->bw, ey = (g->imageExtent.height + p->bh - 1) / p->bh;
      uint32_t rl = g->bufferRowLength ? g->bufferRowLength : g->imageExtent.width;
      uint32_t ih = g->bufferImageHeight ? g->bufferImageHeight : g->imageExtent.height;
      buf_surf(&s, buf, g->bufferOffset);
      s.row = (rl + p->bw - 1) / p->bw * p->bs;
      s.slice = (ih + p->bh - 1) / p->bh * s.row;
      r = mali_meta_plan_copy(&l, &d, &s, p->bs, ex, ey, zcount(img, g->imageSubresource, g->imageExtent));
      if (!r && img->bc)
         r = bc_decode(&l, img, g->imageSubresource, g->imageOffset, g->imageExtent);
   }
   emit(cmd, &l, r, "CmdCopyBufferToImage2");
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdCopyImageToBuffer2(VkCommandBuffer h, const VkCopyImageToBufferInfo2 *ci)
{
   CMD(h);
   struct mali_image *img = mali_image_from_handle(ci->srcImage);
   struct vk_buffer *buf = vk_buffer_from_handle(ci->dstBuffer);
   struct mali_meta_list l = { 0 };
   int r = 0;
   mark_dst_buf(cmd, buf);
   for (uint32_t i = 0; i < ci->regionCount && !r; i++) {
      const VkBufferImageCopy2 *g = &ci->pRegions[i];
      struct mali_meta_surf s, d;
      const struct mali_plane_layout *p;
      if (!img_surf(&s, img, g->imageSubresource, g->imageOffset, &p)) {
         r = -EINVAL;
         break;
      }
      uint32_t ex = (g->imageExtent.width + p->bw - 1) / p->bw, ey = (g->imageExtent.height + p->bh - 1) / p->bh;
      uint32_t rl = g->bufferRowLength ? g->bufferRowLength : g->imageExtent.width;
      uint32_t ih = g->bufferImageHeight ? g->bufferImageHeight : g->imageExtent.height;
      buf_surf(&d, buf, g->bufferOffset);
      d.row = (rl + p->bw - 1) / p->bw * p->bs;
      d.slice = (ih + p->bh - 1) / p->bh * d.row;
      if (getenv("MALI_META_DBG")) fprintf(stderr, "mali_meta_vk: i2b src va %llx size %llu off %llu row %u slice %u | dst va %llx size %llu row %u | ex %u ey %u bs %u\n", (unsigned long long)s.va, (unsigned long long)s.size, (unsigned long long)s.off, s.row, s.slice, (unsigned long long)d.va, (unsigned long long)d.size, d.row, ex, ey, p->bs);
      r = mali_meta_plan_copy(&l, &d, &s, p->bs, ex, ey, zcount(img, g->imageSubresource, g->imageExtent));
   }
   emit(cmd, &l, r, "CmdCopyImageToBuffer2");
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdFillBuffer(VkCommandBuffer h, VkBuffer b, VkDeviceSize off, VkDeviceSize size, uint32_t data)
{
   CMD(h);
   struct vk_buffer *buf = vk_buffer_from_handle(b);
   struct mali_meta_list l = { 0 };
   struct mali_meta_surf d;
   mark_dst_buf(cmd, buf);   /* M15: the fill is a GPU write; an unmarked BO reads back stale after the fence wait */
   if (size == VK_WHOLE_SIZE)
      size = (buf->size - off) & ~3ull;
   buf_surf(&d, buf, off);
   int r = size ? mali_meta_plan_fill(&l, &d, size, data) : 0;
   emit(cmd, &l, r, "CmdFillBuffer");
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdUpdateBuffer(VkCommandBuffer h, VkBuffer b, VkDeviceSize off, VkDeviceSize size, const void *data)
{
   CMD(h);
   struct vk_buffer *buf = vk_buffer_from_handle(b);
   struct mali_meta_list l = { 0 };
   struct mali_meta_surf d;
   mark_dst_buf(cmd, buf);   /* M15: same as vkCmdFillBuffer: the update writes the BO on the GPU */
   buf_surf(&d, buf, off);
   int r = size ? mali_meta_plan_update(&l, &d, size, data) : 0;
   emit(cmd, &l, r, "CmdUpdateBuffer");
}

/* ---- blit / resolve ---- */
/* Pixel class the blit shader handles: 8-bit UNORM x n or 32-bit float x n (RGBA order or BGRA = swap_rb). */
static bool
blit_class(int pipe, unsigned *n, bool *f32, bool *bgra)
{
   const struct util_format_description *d = util_format_description(pipe);
   if (!d || d->layout != UTIL_FORMAT_LAYOUT_PLAIN || d->colorspace != UTIL_FORMAT_COLORSPACE_RGB || d->nr_channels < 1)
      return false;
   unsigned nc = d->nr_channels;
   bool u8 = true, fl = true;
   for (unsigned c = 0; c < nc; c++) {
      u8 &= d->channel[c].size == 8 && d->channel[c].type == UTIL_FORMAT_TYPE_UNSIGNED && d->channel[c].normalized;
      fl &= d->channel[c].size == 32 && d->channel[c].type == UTIL_FORMAT_TYPE_FLOAT;
   }
   if (!u8 && !fl)
      return false;
   *n = nc;
   *f32 = fl;
   *bgra = nc >= 3 && d->swizzle[0] == PIPE_SWIZZLE_Z && d->swizzle[2] == PIPE_SWIZZLE_X;
   return true;
}

/* Generic pixel class (any plain colour format up to 128 bit): channel descriptors of mali_meta_blit. False for packed floats (B10G11R11, E5B9G9R9),
 * depth/stencil, compressed and multi-planar formats. */
struct gdesc { uint32_t ch[4], bs, cls; bool srgb; };

static bool
blit_gclass(int pipe, struct gdesc *g)
{
   if (pipe == PIPE_FORMAT_R11G11B10_FLOAT || pipe == PIPE_FORMAT_R9G9B9E5_FLOAT) {
      memset(g, 0, sizeof *g);
      g->bs = 4;
      bool e5 = pipe == PIPE_FORMAT_R9G9B9E5_FLOAT;
      g->ch[0] = 0u | (e5 ? 9u : 11u) << 8 | (e5 ? 7u : 6u) << 16;
      g->ch[1] = (e5 ? 9u : 11u) | (e5 ? 9u : 11u) << 8 | (e5 ? 7u : 6u) << 16;
      g->ch[2] = (e5 ? 18u : 22u) | (e5 ? 9u : 10u) << 8 | (e5 ? 7u : 6u) << 16;
      return true;
   }
   const struct util_format_description *d = util_format_description(pipe);
   if (!d || d->layout != UTIL_FORMAT_LAYOUT_PLAIN || (d->colorspace != UTIL_FORMAT_COLORSPACE_RGB && d->colorspace != UTIL_FORMAT_COLORSPACE_SRGB) ||
       d->block.width != 1 || d->block.height != 1 || d->block.bits > 128 || d->block.bits % 8)
      return false;
   memset(g, 0, sizeof *g);
   g->bs = d->block.bits / 8;
   g->srgb = d->colorspace == UTIL_FORMAT_COLORSPACE_SRGB;
   bool ui = false, si = false;
   for (unsigned c = 0; c < 4; c++) {
      unsigned s = d->swizzle[c];
      if (s > PIPE_SWIZZLE_W)
         continue;
      const struct util_format_channel_description *ch = &d->channel[s];
      unsigned t;
      switch (ch->type) {
      case UTIL_FORMAT_TYPE_UNSIGNED: t = ch->normalized ? 1 : 3; ui |= t == 3; break;
      case UTIL_FORMAT_TYPE_SIGNED: t = ch->normalized ? 2 : 4; si |= t == 4; break;
      case UTIL_FORMAT_TYPE_FLOAT: if (ch->size != 16 && ch->size != 32) return false; t = 5; break;
      default: return false;
      }
      if (ch->size < 1 || ch->size > 32 || ch->shift > 127)
         return false;
      g->ch[c] = (uint32_t)ch->shift | ch->size << 8 | t << 16;
   }
   /* integer formats must be integer in every present channel */
   bool pure = d->channel[0].pure_integer;
   if (pure != (ui || si))
      return false;
   g->cls = si ? 2 : ui ? 1 : 0;
   return true;
}

static int
blit_one(struct mali_meta_list *l, const struct mali_image *si, const struct mali_image *di, const VkImageBlit2 *g, VkFilter filter, bool resolve_mode,
         const VkImageResolve2 *rg)
{
   VkImageSubresourceLayers ss = resolve_mode ? rg->srcSubresource : g->srcSubresource, ds = resolve_mode ? rg->dstSubresource : g->dstSubresource;
   VkOffset3D so[2], dof[2];
   if (resolve_mode) {
      so[0] = rg->srcOffset;
      so[1] = (VkOffset3D){ rg->srcOffset.x + (int)rg->extent.width, rg->srcOffset.y + (int)rg->extent.height, rg->srcOffset.z + (int)rg->extent.depth };
      dof[0] = rg->dstOffset;
      dof[1] = (VkOffset3D){ rg->dstOffset.x + (int)rg->extent.width, rg->dstOffset.y + (int)rg->extent.height, rg->dstOffset.z + (int)rg->extent.depth };
   } else {
      so[0] = g->srcOffsets[0]; so[1] = g->srcOffsets[1];
      dof[0] = g->dstOffsets[0]; dof[1] = g->dstOffsets[1];
   }
   struct mali_meta_surf s, d;
   const struct mali_plane_layout *sp, *dp;
   VkOffset3D zero = { 0, 0, 0 };
   if (di->bc)
      return -EINVAL;   /* blit to BC images is not valid */
   struct mali_image ssh;
   if (si->bc) {   /* blit from a BC image: read the decoded shadow (same allocation, the shadow layout and format) */
      ssh = *si;
      ssh.lay = si->sh_lay;
      ssh.lay.size = si->lay.size;
      si = &ssh;
   }
   if (!img_surf(&s, si, ss, zero, &sp) || !img_surf(&d, di, ds, zero, &dp))
      return -EINVAL;
   uint32_t sw = lvl(si->lay.d.extent.width, ss.mipLevel), sh = lvl(si->lay.d.extent.height, ss.mipLevel);
   uint32_t dw = lvl(di->lay.d.extent.width, ds.mipLevel), dh = lvl(di->lay.d.extent.height, ds.mipLevel);
   bool s3 = si->lay.d.type == VK_IMAGE_TYPE_3D, d3 = di->lay.d.type == VK_IMAGE_TYPE_3D;
   int sdz = so[1].z - so[0].z, ddz = dof[1].z - dof[0].z;
   uint32_t nz = s3 || d3 ? 1 : ss.layerCount;
   bool zmap = false;   /* 3D -> 3D: per-destination-slice source z mapping (also for a one-slice destination: it samples the centre of the source range) */
   bool zdir = (sdz > 0) == (ddz > 0);
   if (getenv("MALI_META_DBG") && (s3 || d3))
      fprintf(stderr, "mali_meta_vk: 3d blit s3=%d d3=%d sdz=%d ddz=%d layers %u/%u src %d..%d dst %d..%d\n", s3, d3, sdz, ddz, ss.layerCount, ds.layerCount, so[0].z, so[1].z, dof[0].z, dof[1].z);
   if (s3 && d3 && (abs(sdz) == abs(ddz) || !resolve_mode) && ss.layerCount == 1 && ds.layerCount == 1 && sdz && ddz)
      nz = (uint32_t)abs(ddz), zmap = true;   /* 3D -> 3D, 1:1 in z (possibly reversed) or nearest-scaled: one job per destination slice (z scaling: nearest, or trilinear = two source slices mixed) */
   else if ((s3 || d3) && ((s3 && abs(sdz) != 1) || (d3 && abs(ddz) != 1) || ss.layerCount != 1))
      return -EINVAL;   /* z scaling / 3D <-> array mixes */

   struct mali_meta_blit b;
   memset(&b, 0, sizeof b);
   b.src_w = sw;
   b.src_h = sh;
   b.s[0] = so[0].x; b.s[1] = so[0].y; b.s[2] = so[1].x; b.s[3] = so[1].y;
   b.d[0] = dof[0].x; b.d[1] = dof[0].y; b.d[2] = dof[1].x; b.d[3] = dof[1].y;
   int dx0 = b.d[0] < b.d[2] ? b.d[0] : b.d[2], dx1 = b.d[0] < b.d[2] ? b.d[2] : b.d[0];
   int dy0 = b.d[1] < b.d[3] ? b.d[1] : b.d[3], dy1 = b.d[1] < b.d[3] ? b.d[3] : b.d[1];
   b.clamp[0] = dx0 < 0 ? 0 : dx0; b.clamp[1] = dy0 < 0 ? 0 : dy0;
   b.clamp[2] = dx1 > (int)dw ? (int)dw : dx1; b.clamp[3] = dy1 > (int)dh ? (int)dh : dy1;
   if (b.clamp[2] <= b.clamp[0] || b.clamp[3] <= b.clamp[1])
      return 0;

   unsigned sn, dn;
   bool sf, df, sbgra, dbgra;
   bool same_pix = sp->pipe == dp->pipe;
   bool plain = b.s[0] == b.d[0] && b.s[1] == b.d[1] && b.s[2] == b.d[2] && b.s[3] == b.d[3];   /* 1:1, no mirror */
   bool cls = blit_class(sp->pipe, &sn, &sf, &sbgra) && blit_class(dp->pipe, &dn, &df, &dbgra) && sn == dn && sf == df;
   struct gdesc gs, gd;
   bool gen = !cls && blit_gclass(sp->pipe, &gs) && blit_gclass(dp->pipe, &gd) && gs.cls == gd.cls && !(gs.cls && resolve_mode);
   if (resolve_mode && ((!cls && !gen) || !same_pix || si->lay.d.samples < 2))
      return -EINVAL;
   if (!resolve_mode && !cls && !gen) {
      /* Unsupported pixel class (sRGB, 16 bit, integer, depth): only the identical-format 1:1 nearest case, as a rectangle copy. */
      if (!same_pix || !plain || s3 != d3 || ss.layerCount != ds.layerCount)
         return -EINVAL;
      if (s3 && sdz != ddz)
         return -EINVAL;
      VkOffset3D so0 = { so[0].x, so[0].y, sdz < 0 ? so[1].z : so[0].z }, do0 = { dof[0].x, dof[0].y, ddz < 0 ? dof[1].z : dof[0].z };
      if (!img_surf(&s, si, ss, so0, NULL) || !img_surf(&d, di, ds, do0, NULL))
         return -EINVAL;
      uint32_t w = (uint32_t)(b.s[2] > b.s[0] ? b.s[2] - b.s[0] : b.s[0] - b.s[2]), hh = (uint32_t)(b.s[3] > b.s[1] ? b.s[3] - b.s[1] : b.s[1] - b.s[3]);
      return mali_meta_plan_copy(l, &d, &s, sp->bs, w, hh, s3 ? (uint32_t)abs(sdz) : ss.layerCount);
   }
   if (gen) {
      b.generic = true;
      b.ncomp = 4;
      memcpy(b.sch, gs.ch, sizeof b.sch);
      memcpy(b.dch, gd.ch, sizeof b.dch);
      b.sbs = gs.bs;
      b.dbs = gd.bs;
      b.cls = gs.cls;
      b.ssrgb = gs.srgb;
      b.dsrgb = gd.srgb;
   } else {
      b.ncomp = sn;
      b.f32 = sf;
      b.swap_rb = sbgra != dbgra;
   }
   b.mode = resolve_mode ? 2 : (filter == VK_FILTER_LINEAR && !(gen && gs.cls) ? 1 : 0);
   if (resolve_mode) {
      b.nsamples = si->lay.d.samples;
      b.sample_stride = (uint32_t)si->lay.plane[0].slice[ss.mipLevel].surface_stride;
   }
   for (uint32_t z = 0; z < nz; z++) {
      VkOffset3D zs = { 0, 0, s3 ? (sdz > 0 ? so[0].z : so[0].z + sdz) : 0 }, zd = { 0, 0, d3 ? (ddz > 0 ? dof[0].z : dof[0].z + ddz) : 0 };
      if (zmap) {
         int au = zdir ? (int)z : (int)(nz - 1 - z);
         int sz = (int)(((2ll * au + 1) * abs(sdz)) / (2ll * (int)nz));
         if (b.mode == 1) {
            /* trilinear: source z of the destination slice centre, minus 0.5 = between slice z0 and z0 + 1 (clamped to the image like the x/y taps) */
            double t = (double)(sdz > 0 ? so[0].z : so[1].z) + ((double)au + 0.5) * abs(sdz) / (double)nz - 0.5;
            int z0 = (int)floor(t), zl = (int)lvl(si->lay.d.extent.depth, ss.mipLevel) - 1, z1 = z0 + 1;
            double fr = t - (double)z0;
            z0 = z0 < 0 ? 0 : z0 > zl ? zl : z0;
            z1 = z1 < 0 ? 0 : z1 > zl ? zl : z1;
            zs.z = z0;
            b.zw = z1 > z0 ? (float)fr : 0.0f;
            b.zslices = (uint32_t)(z1 - z0);
         } else {
            zs.z += sz < abs(sdz) ? sz : abs(sdz) - 1;
         }
         zd.z += (int)z;
      }
      VkImageSubresourceLayers s1 = ss, d1 = ds;
      s1.baseArrayLayer += z;
      d1.baseArrayLayer += z;
      if (!img_surf(&b.src, si, s1, zs, NULL) || !img_surf(&b.dst, di, d1, zd, NULL))
         return -EINVAL;
      b.src.size = si->lay.size;
      b.dst.size = di->lay.size;
      if (b.zw != 0.0f)
         b.sample_stride = b.src.slice * b.zslices;
      int r = mali_meta_plan_blit(l, &b);
      if (r)
         return r;
   }
   return 0;
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdBlitImage2(VkCommandBuffer h, const VkBlitImageInfo2 *ci)
{
   CMD(h);
   struct mali_image *si = mali_image_from_handle(ci->srcImage), *di = mali_image_from_handle(ci->dstImage);
   struct mali_meta_list l = { 0 };
   int r = 0;
   mark_dst_img(cmd, di);
   for (uint32_t i = 0; i < ci->regionCount && !r; i++)
      r = blit_one(&l, si, di, &ci->pRegions[i], ci->filter, false, NULL);
   emit(cmd, &l, r, "CmdBlitImage2");
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdResolveImage2(VkCommandBuffer h, const VkResolveImageInfo2 *ci)
{
   CMD(h);
   struct mali_image *si = mali_image_from_handle(ci->srcImage), *di = mali_image_from_handle(ci->dstImage);
   struct mali_meta_list l = { 0 };
   int r = 0;
   mark_dst_img(cmd, di);
   for (uint32_t i = 0; i < ci->regionCount && !r; i++)
      r = blit_one(&l, si, di, NULL, VK_FILTER_NEAREST, true, &ci->pRegions[i]);
   emit(cmd, &l, r, "CmdResolveImage2");
}

/* ---- clears (named mali_meta_*: mali_cmd.c owns mali_CmdClear*Image for the render-target fast path; wiring falls back to these) ---- */
static int
clear_range(struct mali_meta_list *l, const struct mali_image *img, VkImageAspectFlags aspect, const VkImageSubresourceRange *rg, const uint32_t cst[4])
{
   uint32_t nl = rg->levelCount == VK_REMAINING_MIP_LEVELS ? img->lay.d.levels - rg->baseMipLevel : rg->levelCount;
   uint32_t nlay = rg->layerCount == VK_REMAINING_ARRAY_LAYERS ? img->lay.d.layers - rg->baseArrayLayer : rg->layerCount;
   for (uint32_t m = 0; m < nl; m++) {
      VkImageSubresourceLayers sub = { aspect, rg->baseMipLevel + m, rg->baseArrayLayer, nlay };
      struct mali_meta_surf d;
      const struct mali_plane_layout *p;
      VkOffset3D z = { 0, 0, 0 };
      if (!img_surf(&d, img, sub, z, &p))
         return -EINVAL;
      VkExtent3D e = { lvl(img->lay.d.extent.width, sub.mipLevel), lvl(img->lay.d.extent.height, sub.mipLevel), lvl(img->lay.d.extent.depth, sub.mipLevel) };
      if (p->bw != 1 || p->bh != 1 || p->bs > 16)
         return -EINVAL;
      /* multisampled: sample k of each layer sits at k * surface_stride */
      for (uint32_t k = 0; k < img->lay.d.samples; k++) {
         struct mali_meta_surf dk = d;
         dk.off += (uint64_t)k * p->slice[sub.mipLevel].surface_stride;
         int r = mali_meta_plan_const(l, &dk, p->bs, e.width, e.height, zcount(img, sub, e), cst);
         if (r)
            return r;
      }
   }
   return 0;
}

VKAPI_ATTR void VKAPI_CALL
mali_meta_CmdClearColorImage(VkCommandBuffer h, VkImage image, VkImageLayout lo, const VkClearColorValue *c, uint32_t n, const VkImageSubresourceRange *r)
{
   CMD(h);
   struct mali_image *img = mali_image_from_handle(image);
   struct mali_meta_list l = { 0 };
   int err = 0;
   const struct mali_plane_layout *p = mali_image_plane_for_aspect(&img->lay, VK_IMAGE_ASPECT_COLOR_BIT);
   uint32_t cst[4] = { 0 };
   if (!p) {
      emit(cmd, &l, -EINVAL, "CmdClearColorImage");
      return;
   }
   util_format_pack_rgba(p->pipe, cst, c, 1);
   for (uint32_t i = 0; i < n && !err; i++)
      err = clear_range(&l, img, VK_IMAGE_ASPECT_COLOR_BIT, &r[i], cst);
   emit(cmd, &l, err, "CmdClearColorImage");
}

VKAPI_ATTR void VKAPI_CALL
mali_meta_CmdClearDepthStencilImage(VkCommandBuffer h, VkImage image, VkImageLayout lo, const VkClearDepthStencilValue *v, uint32_t n, const VkImageSubresourceRange *r)
{
   CMD(h);
   struct mali_image *img = mali_image_from_handle(image);
   struct mali_meta_list l = { 0 };
   int err = 0;
   for (uint32_t i = 0; i < n && !err; i++) {
      for (unsigned a = 0; a < 2 && !err; a++) {
         VkImageAspectFlags asp = a ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
         if (!(r[i].aspectMask & asp))
            continue;
         const struct mali_plane_layout *p = mali_image_plane_for_aspect(&img->lay, asp);
         uint32_t cst[4] = { 0 };
         if (!p) {
            err = -EINVAL;
            break;
         }
         if (a) {
            uint8_t s = (uint8_t)v->stencil;
            util_format_pack_s_8uint(p->pipe, cst, &s, 1);
         } else {
            util_format_pack_z_float(p->pipe, cst, &v->depth, 1);
         }
         err = clear_range(&l, img, asp, &r[i], cst);
      }
   }
   emit(cmd, &l, err, "CmdClearDepthStencilImage");
}
