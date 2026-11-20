/* SPDX-License-Identifier: MIT */
/* M5: image layout, plane/texture packers, VkImage and VkImageView. See mali_image.h. */
#include "mali_image.h"
#include "mali_msaa.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "util/macros.h"
#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_util.h"

static inline uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
static inline uint32_t minify(uint32_t v, uint32_t l) { v >>= l; return v ? v : 1; }
static inline uint32_t div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

/* ---- layout ---- */

bool
mali_image_wants_tiled(VkImageTiling tiling, VkImageType type, uint32_t samples)
{
   const char *e = getenv("MALI_IMAGE_TILED");
   return e && e[0] == '1' && tiling == VK_IMAGE_TILING_OPTIMAL && type != VK_IMAGE_TYPE_1D && samples == 1;
}

int
mali_image_layout_init(unsigned arch, const struct mali_image_desc *d, struct mali_image_layout *out)
{
   memset(out, 0, sizeof *out);
   struct mali_fmt f;
   if (!mali_format_get(arch, d->format, &f)) return -EINVAL;
   if (!d->extent.width || !d->extent.height || !d->extent.depth || !d->levels || !d->layers) return -EINVAL;
   if (d->samples != 1 && d->samples != 4) return -EINVAL;
   if (f.ycbcr && (d->type != VK_IMAGE_TYPE_2D || d->samples != 1 || d->levels != 1 || d->layers != 1 || (d->extent.width | d->extent.height) & 1))
      return -EINVAL;   /* NV12: 2D, one level and layer, even extent (4:2:0) */
   if (d->type == VK_IMAGE_TYPE_1D && (d->extent.height != 1 || d->extent.depth != 1)) return -EINVAL;
   if (d->type == VK_IMAGE_TYPE_2D && d->extent.depth != 1) return -EINVAL;
   if (d->type == VK_IMAGE_TYPE_3D && d->layers != 1) return -EINVAL;
   if (d->samples > 1 && (d->type != VK_IMAGE_TYPE_2D || d->levels != 1)) return -EINVAL;
   const uint32_t maxd = d->type == VK_IMAGE_TYPE_1D ? MALI_MAX_DIM_1D : d->type == VK_IMAGE_TYPE_2D ? MALI_MAX_DIM_2D : MALI_MAX_DIM_3D;
   if (d->extent.width > maxd || d->extent.height > maxd || d->extent.depth > maxd) return -EINVAL;
   if (d->layers > MALI_MAX_LAYERS || d->levels > MALI_MAX_MIPS) return -EINVAL;
   uint32_t m = MAX3(d->extent.width, d->extent.height, d->extent.depth), full = 1;
   while (m >>= 1) full++;
   if (d->levels > full) return -EINVAL;

   out->d = *d;
   out->nplanes = f.nplanes;
   out->alignment = MALI_IMAGE_ALIGN;
   uint64_t base = 0;
   for (unsigned pi = 0; pi < f.nplanes; pi++) {
      struct mali_plane_layout *p = &out->plane[pi];
      p->aspect = f.plane[pi].aspect;
      p->pipe = f.plane[pi].pipe;
      p->clump = f.plane[pi].clump;
      p->bs = f.plane[pi].bs;
      p->bw = f.plane[pi].bw;
      p->bh = f.plane[pi].bh;
      p->hw = f.plane[pi].hw;
      p->ssx = f.plane[pi].ssx;
      p->ssy = f.plane[pi].ssy;
      p->offset = align_up(base, MALI_IMAGE_ALIGN);
      uint64_t off = 0;
      for (unsigned l = 0; l < d->levels; l++) {
         struct mali_slice *s = &p->slice[l];
         const uint32_t w = div_up(div_up(minify(d->extent.width, l), 1u << p->ssx), p->bw), h = div_up(div_up(minify(d->extent.height, l), 1u << p->ssy), p->bh);
         const uint32_t z = d->type == VK_IMAGE_TYPE_3D ? minify(d->extent.depth, l) : 1;
         if (d->tiled) {
            const uint64_t tile_b = (uint64_t)MALI_TILE_EL * MALI_TILE_EL * p->bs;
            s->row_stride = (uint32_t)align_up(tile_b * div_up(w, MALI_TILE_EL), 64);
            s->surface_stride = align_up((uint64_t)s->row_stride * div_up(h, MALI_TILE_EL), 64);
         } else {
            s->row_stride = (uint32_t)align_up((uint64_t)w * p->bs, 64);
            s->surface_stride = align_up((uint64_t)s->row_stride * h, 64);
         }
         s->size = s->surface_stride * z * d->samples;
         s->offset = off;
         off += s->size;
      }
      p->array_stride = align_up(off, 64);
      p->size = p->array_stride * d->layers;
      base = p->offset + p->size;
   }
   out->size = base;
   return 0;
}

const struct mali_plane_layout *
mali_image_plane_for_aspect(const struct mali_image_layout *l, VkImageAspectFlags aspect)
{
   for (unsigned i = 0; i < l->nplanes; i++)
      if (l->plane[i].aspect & aspect) return &l->plane[i];
   return NULL;
}

uint64_t
mali_image_subresource_offset(const struct mali_plane_layout *p, unsigned level, unsigned layer)
{
   return p->offset + p->slice[level].offset + (uint64_t)layer * p->array_stride;
}

/* ---- packers ---- */

void
mali_pack_plane(struct mali_hw_desc *d, unsigned ordering, unsigned clump, uint64_t va, uint64_t size, uint32_t row_stride,
                uint64_t slice_stride, unsigned w, unsigned h)
{
   memset(d, 0, sizeof *d);
   d->w[0] = 10u | (1u << 4) | (ordering << 8) | (clump << 24);   /* Plane, Generic */
   d->w[1] = (uint32_t)size;
   d->w[2] = (uint32_t)va;
   d->w[3] = ((uint32_t)(va >> 32) & 0xffffu) | ((uint32_t)(size >> 32) << 16);
   d->w[4] = row_stride | ((uint32_t)(slice_stride >> 32) << 27);
   d->w[6] = (uint32_t)slice_stride;
   d->w[7] = (w - 1) | ((h - 1) << 16);
}

static unsigned
swz_sel(VkComponentSwizzle s, unsigned identity)
{
   switch (s) {
   case VK_COMPONENT_SWIZZLE_R: return 0;
   case VK_COMPONENT_SWIZZLE_G: return 1;
   case VK_COMPONENT_SWIZZLE_B: return 2;
   case VK_COMPONENT_SWIZZLE_A: return 3;
   case VK_COMPONENT_SWIZZLE_ZERO: return 4;
   case VK_COMPONENT_SWIZZLE_ONE: return 5;
   default: return identity;
   }
}

static unsigned
view_dimension(VkImageViewType t)
{
   switch (t) {
   case VK_IMAGE_VIEW_TYPE_1D: case VK_IMAGE_VIEW_TYPE_1D_ARRAY: return 1;
   case VK_IMAGE_VIEW_TYPE_CUBE: case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return 0;
   case VK_IMAGE_VIEW_TYPE_3D: return 3;
   default: return 2;
   }
}

unsigned
mali_view_payload_entries(const struct mali_image_layout *l, const struct mali_view_desc *v)
{
   const bool vol = v->type == VK_IMAGE_VIEW_TYPE_3D;   /* whole volume in one entry per level */
   return v->levels * (vol ? 1u : v->layers);
}

int
mali_view_emit(unsigned arch, const struct mali_image_layout *l, uint64_t image_va, const struct mali_view_desc *v, void *payload,
               uint64_t payload_va, struct mali_hw_desc *tex)
{
   struct mali_fmt vf;
   if (!mali_format_get(arch, v->format, &vf)) return -EINVAL;
   const struct mali_plane_layout *p = mali_image_plane_for_aspect(l, v->aspect ? v->aspect : VK_IMAGE_ASPECT_COLOR_BIT);
   if (!p) return -EINVAL;
   /* The view format's own plane (view format may differ from the image format within a compatibility class). */
   unsigned vpi = 0;
   for (unsigned i = 0; i < vf.nplanes; i++)
      if (vf.plane[i].aspect & p->aspect) vpi = i;
   const unsigned clump = vf.plane[vpi].clump;
   const uint32_t hw = vf.plane[vpi].hw;
   const struct mali_image_desc *d = &l->d;
   const bool vol = v->type == VK_IMAGE_VIEW_TYPE_3D;
   const bool slices = d->type == VK_IMAGE_TYPE_3D && !vol;   /* 2D (array) view of a 3D image: each layer = one z slice */
   const unsigned ordering = d->tiled ? 1 : 2;

   /* Payload order (Mesa pan_emit_iview_texture_payload, v7+): layer-major, then mip level. */
   uint8_t *out = payload;
   const unsigned nl = vol ? 1u : v->layers;
   for (unsigned ly = 0; ly < nl; ly++) {
      for (unsigned li = 0; li < v->levels; li++) {
         const unsigned lev = v->base_level + li;
         const struct mali_slice *s = &p->slice[lev];
         const unsigned w = div_up(minify(d->extent.width, lev), 1u << p->ssx), h = div_up(minify(d->extent.height, lev), 1u << p->ssy);
         uint64_t va = image_va + p->offset + s->offset, size = s->size, ss = 0;
         const unsigned layer = v->base_layer + ly;
         if (vol) {
            ss = s->surface_stride;
         } else if (slices) {
            va += (uint64_t)layer * s->surface_stride;
            size -= (uint64_t)layer * s->surface_stride;
         } else {
            va += (uint64_t)layer * p->array_stride;
            ss = d->samples > 1 ? s->surface_stride : 0;
         }
         struct mali_hw_desc pl;
         mali_pack_plane(&pl, ordering, clump, va, size, s->row_stride, ss, div_up(w, 1), div_up(h, 1));
         memcpy(out, &pl, 32);
         out += 32;
      }
   }

   const unsigned dim = view_dimension(v->type);
   memset(tex, 0, sizeof *tex);
   const unsigned levels = v->storage ? 1 : v->levels;
   unsigned array = v->layers;
   if (dim == 0) array = v->layers / 6 ? v->layers / 6 : 1;   /* cube: array size counts cubes (payload still holds 6 faces each) */
   if (dim == 3) array = 1;
   const unsigned w0 = div_up(minify(d->extent.width, v->base_level), 1u << p->ssx), h0 = div_up(minify(d->extent.height, v->base_level), 1u << p->ssy);
   const unsigned z0 = d->type == VK_IMAGE_TYPE_3D ? minify(d->extent.depth, v->base_level) : 1;
   unsigned swz = 0x688;   /* identity RGBA */
   if (!v->storage)
      swz = swz_sel(v->swizzle.r, 0) | (swz_sel(v->swizzle.g, 1) << 3) | (swz_sel(v->swizzle.b, 2) << 6) | (swz_sel(v->swizzle.a, 3) << 9);
   const bool interleave = d->tiled || vf.compressed;
   tex->w[0] = 2u | (dim << 4) | (hw << 10);
   tex->w[1] = (w0 - 1) | ((h0 - 1) << 16);
   tex->w[2] = swz | ((unsigned)interleave << 12) | ((levels - 1) << 16);
   {
      const unsigned minlod = v->storage ? 0 : (unsigned)(fmaxf(v->min_lod - (float)v->base_level, 0.0f) * 256.0f);   /* EXT_image_view_min_lod: minLod counts from image level 0, the descriptor from the view's first level */
      const unsigned maxlod = (levels - 1) * 256u;
      unsigned log2s = d->samples == 4 ? 2 : 0;
      tex->w[3] = (minlod & 0x1fff) | ((dim == 3 ? 0u : log2s) << 13) | ((maxlod & 0x1fff) << 16);
   }
   tex->w[4] = (uint32_t)payload_va;
   tex->w[5] = (uint32_t)(payload_va >> 32);
   tex->w[6] = array - 1;
   tex->w[7] = dim == 3 ? z0 - 1 : 0;
   return 0;
}

/* ---- seams ---- */

static const struct mali_desc_mem_ops *g_pmem;
void mali_image_set_mem_ops(const struct mali_desc_mem_ops *ops) { g_pmem = ops; }

static void *
pay_alloc(struct vk_device *dev, size_t size, uint64_t *va)
{
   if (g_pmem) return g_pmem->alloc(dev, size, va);
   void *p = NULL;
   if (posix_memalign(&p, 64, size ? size : 64)) return NULL;
   memset(p, 0, size);
   *va = (uint64_t)(uintptr_t)p;
   return p;
}

static void
pay_free(struct vk_device *dev, void *cpu, size_t size)
{
   if (!cpu) return;
   if (g_pmem) g_pmem->free(dev, cpu, size);
   else free(cpu);
}

/* ---- VkImage ---- */

static unsigned
dev_arch(struct mali_device *dev) { return mali_device_pdev(dev)->gpu.arch_major; }

static void
desc_from_ci(const VkImageCreateInfo *ci, struct mali_image_desc *d)
{
   d->format = ci->format;
   d->type = ci->imageType;
   d->extent = ci->extent;
   d->levels = ci->mipLevels;
   d->layers = ci->arrayLayers;
   d->samples = ci->samples;
   d->tiled = mali_image_wants_tiled(ci->tiling, ci->imageType, ci->samples);
}

/* Layout of a VkImage: plain, or for a BC format the compressed region (planes overridden from the BC layout) + shadow layout. */
static int
image_layout(unsigned arch, const VkImageCreateInfo *ci, struct mali_image_layout *lay, struct mali_bc_layout *bc, struct mali_image_layout *sh,
             bool *is_bc)
{
   struct mali_image_desc d;
   desc_from_ci(ci, &d);
   *is_bc = mali_bc_is_bc(ci->format);
   /* ETC2 / EAC: the native layout stays (hardware samples it); a decoded shadow is appended after it as the blit source. Linear 2D / 3D only,
    * anything else keeps the plain layout (no shadow, blits from it are refused as before). */
   const bool etc2 = !*is_bc && mali_bc_is_etc2(ci->format) && !d.tiled && ci->samples == 1 &&
                     mali_bc_layout_init(ci->format, ci->imageType, ci->extent, ci->mipLevels, ci->arrayLayers, bc) == 0;
   if (*is_bc) {
      d.tiled = false;
      if (ci->samples != 1 || mali_bc_layout_init(ci->format, ci->imageType, ci->extent, ci->mipLevels, ci->arrayLayers, bc) != 0)
         return -EINVAL;
   }
   int r = mali_image_layout_init(arch, &d, lay);
   if (r || !(*is_bc || etc2))
      return r;
   struct mali_plane_layout *p = &lay->plane[0];
   if (etc2) {
      /* the compressed region is the native plane: take its offsets and strides into the BC layout, then place the shadow behind it */
      bc->comp.base = p->offset;
      bc->comp.array_stride = p->array_stride;
      bc->comp.size = p->size;
      for (unsigned l = 0; l < d.levels; l++) {
         bc->comp.lv[l].off = p->slice[l].offset;
         bc->comp.lv[l].row = p->slice[l].row_stride;
         bc->comp.lv[l].slice = p->slice[l].surface_stride;
         bc->comp.lv[l].size = p->slice[l].size;
      }
      bc->shadow.base = align_up(lay->size, MALI_BC_ALIGN);
      bc->size = bc->shadow.base + bc->shadow.size;
      *is_bc = true;
   } else {
      p->offset = bc->comp.base;
      p->array_stride = bc->comp.array_stride;
      p->size = bc->comp.size;
      for (unsigned l = 0; l < d.levels; l++) {
         p->slice[l] = (struct mali_slice){ .offset = bc->comp.lv[l].off, .row_stride = bc->comp.lv[l].row, .surface_stride = bc->comp.lv[l].slice,
                                            .size = bc->comp.lv[l].size };
      }
   }
   lay->size = bc->size;
   if (bc->alignment > lay->alignment)
      lay->alignment = bc->alignment;
   if (!sh)
      return 0;
   struct mali_image_desc sd = d;
   sd.format = bc->fmt->shadow;
   r = mali_image_layout_init(arch, &sd, sh);
   if (r)
      return r;
   struct mali_plane_layout *q = &sh->plane[0];
   q->offset = bc->shadow.base;
   q->array_stride = bc->shadow.array_stride;
   q->size = bc->shadow.size;
   for (unsigned l = 0; l < d.levels; l++)
      q->slice[l] = (struct mali_slice){ .offset = bc->shadow.lv[l].off, .row_stride = bc->shadow.lv[l].row,
                                         .surface_stride = bc->shadow.lv[l].slice, .size = bc->shadow.lv[l].size };
   return 0;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_CreateImage(VkDevice _dev, const VkImageCreateInfo *ci, const VkAllocationCallbacks *alloc, VkImage *out)
{
   VK_FROM_HANDLE(mali_device, dev, _dev);
   struct mali_image_layout lay;
   struct mali_bc_layout bc;
   bool is_bc;
   struct mali_image *img = vk_image_create(&dev->vk, ci, alloc, sizeof *img);
   if (!img) return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   if (image_layout(dev_arch(dev), ci, &lay, &bc, &img->sh_lay, &is_bc) != 0) {
      vk_image_destroy(&dev->vk, alloc, &img->vk);
      return vk_error(dev, VK_ERROR_FORMAT_NOT_SUPPORTED);
   }
   img->bc = is_bc;
   img->native_view = is_bc && mali_bc_is_etc2(ci->format);
   if (is_bc) img->bc_lay = bc;
   img->lay = lay;
   img->arch = dev_arch(dev);
   if (ci->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) img->vk.drm_format_mod = mali_wsi_modifier;   /* M14: linear only */
   *out = mali_image_to_handle(img);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
mali_DestroyImage(VkDevice _dev, VkImage h, const VkAllocationCallbacks *alloc)
{
   VK_FROM_HANDLE(mali_device, dev, _dev);
   VK_FROM_HANDLE(mali_image, img, h);
   if (!img) return;
   vk_image_destroy(&dev->vk, alloc, &img->vk);
}

static void
fill_reqs(struct mali_device *dev, uint64_t size, uint64_t align, VkMemoryRequirements2 *r)
{
   const struct mali_physical_device *pd = mali_device_pdev(dev);
   r->memoryRequirements.size = align_up(size, align);
   r->memoryRequirements.alignment = align;
   r->memoryRequirements.memoryTypeBits = (1u << pd->memory.memoryTypeCount) - 1;
   vk_foreach_struct(st, e, r->pNext) {
      if (st == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
         VkMemoryDedicatedRequirements *dr = e;
         dr->prefersDedicatedAllocation = false;
         dr->requiresDedicatedAllocation = false;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
mali_GetImageMemoryRequirements2(VkDevice _dev, const VkImageMemoryRequirementsInfo2 *info, VkMemoryRequirements2 *r)
{
   VK_FROM_HANDLE(mali_device, dev, _dev);
   VK_FROM_HANDLE(mali_image, img, info->image);
   fill_reqs(dev, img->lay.size, img->lay.alignment, r);
}

VKAPI_ATTR void VKAPI_CALL
mali_GetDeviceImageMemoryRequirements(VkDevice _dev, const VkDeviceImageMemoryRequirements *info, VkMemoryRequirements2 *r)
{
   VK_FROM_HANDLE(mali_device, dev, _dev);
   struct mali_image_layout lay;
   struct mali_bc_layout bc;
   bool is_bc;
   if (image_layout(dev_arch(dev), info->pCreateInfo, &lay, &bc, NULL, &is_bc) != 0) memset(&lay, 0, sizeof lay), lay.alignment = MALI_IMAGE_ALIGN;
   fill_reqs(dev, lay.size, lay.alignment, r);
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_BindImageMemory2(VkDevice _dev, uint32_t n, const VkBindImageMemoryInfo *b)
{
   for (uint32_t i = 0; i < n; i++) {
      VK_FROM_HANDLE(mali_image, img, b[i].image);
      VK_FROM_HANDLE(mali_memory, mem, b[i].memory);
      if (!mem) {
         /* Swapchain-aliased binds (VkBindImageMemorySwapchainInfoKHR, memory == VK_NULL_HANDLE) are not implemented: wsi_common has no lookup
            in this Mesa. Fail with VK_ERROR_UNKNOWN (FEATURE_NOT_PRESENT is not a permitted vkBindImageMemory2 result) instead of returning VK_SUCCESS for an image that stayed unbound. */
         if (vk_find_struct_const(b[i].pNext, BIND_IMAGE_MEMORY_SWAPCHAIN_INFO_KHR)) {
            /* VkBindMemoryStatus (VK_KHR_maintenance6): report each bind's own result, VK_SUCCESS for the ones before this one */
            for (uint32_t j = 0; j < n; j++) {
               const VkBindMemoryStatus *st = vk_find_struct_const(b[j].pNext, BIND_MEMORY_STATUS);
               if (st && st->pResult)
                  *st->pResult = j < i ? VK_SUCCESS : j == i ? VK_ERROR_UNKNOWN : VK_SUCCESS;
            }
            return VK_ERROR_UNKNOWN;
         }
         continue;
      }
      img->mem = mem;
      img->mem_off = b[i].memoryOffset;
      img->va = mem->bo.gpu_va + b[i].memoryOffset;
   }
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
mali_GetImageSparseMemoryRequirements2(VkDevice d, const VkImageSparseMemoryRequirementsInfo2 *i, uint32_t *n, VkSparseImageMemoryRequirements2 *r)
{
   *n = 0;
}

static void
subres_layout(const struct mali_image_layout *lay, const VkImageSubresource *s, VkSubresourceLayout *o)
{
   const struct mali_plane_layout *p = mali_image_plane_for_aspect(lay, s->aspectMask);
   if (!p && (s->aspectMask & VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT)) p = &lay->plane[0];   /* M14: modifier LINEAR has one memory plane */
   memset(o, 0, sizeof *o);
   if (!p || s->mipLevel >= lay->d.levels) return;
   const struct mali_slice *sl = &p->slice[s->mipLevel];
   o->offset = mali_image_subresource_offset(p, s->mipLevel, 0) + (uint64_t)s->arrayLayer * p->array_stride - 0;
   o->size = sl->size;
   o->rowPitch = sl->row_stride;
   o->arrayPitch = p->array_stride;
   o->depthPitch = sl->surface_stride;
}

VKAPI_ATTR void VKAPI_CALL
mali_GetImageSubresourceLayout2(VkDevice _dev, VkImage h, const VkImageSubresource2 *s, VkSubresourceLayout2 *o)
{
   VK_FROM_HANDLE(mali_image, img, h);
   subres_layout(&img->lay, &s->imageSubresource, &o->subresourceLayout);
}

VKAPI_ATTR void VKAPI_CALL
mali_GetDeviceImageSubresourceLayout(VkDevice _dev, const VkDeviceImageSubresourceInfo *info, VkSubresourceLayout2 *o)
{
   VK_FROM_HANDLE(mali_device, dev, _dev);
   struct mali_image_layout lay;
   struct mali_bc_layout bc;
   bool is_bc;
   if (image_layout(dev_arch(dev), info->pCreateInfo, &lay, &bc, NULL, &is_bc) != 0) { memset(&o->subresourceLayout, 0, sizeof o->subresourceLayout); return; }
   subres_layout(&lay, &info->pSubresource->imageSubresource, &o->subresourceLayout);
}

/* ---- VkImageView ---- */

static void
view_refresh(struct mali_image_view *v)
{
   struct mali_image *img = container_of(v->vk.image, struct mali_image, vk);
   if (v->emitted && v->emitted_for_va == img->va) return;
   const struct mali_image_layout *vl = img->bc && !img->native_view ? &img->sh_lay : &img->lay;   /* BC: views sample the decoded shadow (ETC2: native) */
   if (v->ycbcr) {   /* NV12: two ordinary textures, luma (R8) and chroma (R8G8); the conversion happens in the shader */
      struct mali_view_desc y = v->vd, c = v->vd;
      y.format = VK_FORMAT_R8_UNORM; y.aspect = VK_IMAGE_ASPECT_PLANE_0_BIT;
      c.format = VK_FORMAT_R8G8_UNORM; c.aspect = VK_IMAGE_ASPECT_PLANE_1_BIT;
      mali_view_emit(v->arch, vl, img->va, &y, v->payload, v->payload_va, &v->tex);
      mali_view_emit(v->arch, vl, img->va, &c, v->payload_c, v->payload_c_va, &v->tex_c);
      v->storage = v->tex;
      v->emitted = true;
      v->emitted_for_va = img->va;
      return;
   }
   mali_view_emit(v->arch, vl, img->va, &v->vd, v->payload, v->payload_va, &v->tex);
   struct mali_view_desc sd = v->vd;
   sd.storage = true;
   mali_view_emit(v->arch, vl, img->va, &sd, v->payload, v->payload_va, &v->storage);   /* same payload, identical content */
   v->emitted = true;
   v->emitted_for_va = img->va;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_CreateImageView(VkDevice _dev, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *alloc, VkImageView *out)
{
   VK_FROM_HANDLE(mali_device, dev, _dev);
   VK_FROM_HANDLE(mali_image, img, ci->image);
   struct mali_image_view *v = vk_image_view_create(&dev->vk, ci, alloc, sizeof *v);
   if (!v) return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   v->arch = img->arch;
   v->vd = (struct mali_view_desc){
      .format = v->vk.view_format ? v->vk.view_format : v->vk.format,
      .type = v->vk.view_type,
      .aspect = v->vk.aspects,
      .base_level = v->vk.base_mip_level,
      .levels = v->vk.level_count,
      .base_layer = v->vk.base_array_layer,
      .layers = v->vk.layer_count,
      .swizzle = v->vk.swizzle,
      .min_lod = v->vk.min_lod,
   };
   if (v->vk.aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
      /* Sampling picks one aspect (depth wins when both are set, as a sampled depth/stencil view must name exactly one). */
      v->vd.aspect = (v->vk.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT;
   }
   /* A 3D image seen as 2D / 2D array exposes its z slices as layers. */
   if (img->lay.d.type == VK_IMAGE_TYPE_3D && v->vd.type != VK_IMAGE_VIEW_TYPE_3D) {
      v->vd.layers = v->vk.layer_count;
   }
   if (img->bc && !img->native_view) {
      v->vd.format = img->bc_lay.fmt->shadow;
      v->vd.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
   }
   v->ycbcr = v->vk.format == MALI_YCBCR_FORMAT && (v->vk.aspects & VK_IMAGE_ASPECT_PLANE_0_BIT) && (v->vk.aspects & VK_IMAGE_ASPECT_PLANE_1_BIT);
   if (v->ycbcr) v->vd.aspect = VK_IMAGE_ASPECT_PLANE_0_BIT;   /* payload sizing only: both planes have the same level/layer counts */
   v->payload_entries = mali_view_payload_entries(&img->lay, &v->vd);
   const size_t sz = (size_t)v->payload_entries * 32;
   v->payload = pay_alloc(&dev->vk, sz, &v->payload_va);
   if (v->payload && v->ycbcr) {
      v->payload_c_entries = v->payload_entries;
      v->payload_c = pay_alloc(&dev->vk, sz, &v->payload_c_va);
      if (!v->payload_c) pay_free(&dev->vk, v->payload, sz), v->payload = NULL;
   }
   if (!v->payload) {
      vk_image_view_destroy(&dev->vk, alloc, &v->vk);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   view_refresh(v);
   *out = mali_image_view_to_handle(v);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
mali_DestroyImageView(VkDevice _dev, VkImageView h, const VkAllocationCallbacks *alloc)
{
   VK_FROM_HANDLE(mali_device, dev, _dev);
   VK_FROM_HANDLE(mali_image_view, v, h);
   if (!v) return;
   pay_free(&dev->vk, v->payload, (size_t)v->payload_entries * 32);
   pay_free(&dev->vk, v->payload_c, (size_t)v->payload_c_entries * 32);
   vk_image_view_destroy(&dev->vk, alloc, &v->vk);
}

int
mali_image_view_desc(VkImageView h, VkDescriptorType type, struct mali_hw_desc *out)
{
   VK_FROM_HANDLE(mali_image_view, v, h);
   if (!v) return -EINVAL;
   view_refresh(v);
   *out = type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? v->storage : v->tex;
   return 0;
}

int
mali_image_view_planes(VkImageView h, struct mali_hw_desc out[2])
{
   VK_FROM_HANDLE(mali_image_view, v, h);
   if (!v || !v->ycbcr) return 0;
   view_refresh(v);
   out[0] = v->tex;
   out[1] = v->tex_c;
   return 2;
}
