/* SPDX-License-Identifier: MIT */
/* M5: format tables (see mali_format.h). Built from pan_format tables; policy (what we advertise) is below. */
#include "mali_format.h"

#include "mali_bc.h"
#include "mali_rtfmt_tab.h"
#include "mali_msaa.h"
#include "mali_private.h"
#include "pan_format.h"
#include "util/format/u_format.h"
#include "vk_format.h"
#include "vk_util.h"

#include <string.h>

/* Only the v10 (G610), v11 (G615) and v13 (G925) tables are linked; pan_format_table() would pull in every arch. */
static const struct pan_format *
fmt_table(unsigned arch)
{
   return arch >= 13 ? pan_pipe_format_v13 : arch == 10 ? pan_pipe_format_v10 : pan_pipe_format_v11;
}

static const struct pan_blendable_format *
blend_table(unsigned arch)
{
   return arch >= 13 ? pan_blendable_formats_v13 : arch == 10 ? pan_blendable_formats_v10 : pan_blendable_formats_v11;
}

/* VkFormat range we look at: the core 1.0 formats. ASTC (157..184) is skipped: it needs the ASTC plane descriptor (gap, R3-PLAN M5). */
#define FMT_FIRST 1
#define FMT_LAST  156

static bool
fmt_skipped(VkFormat f, enum pipe_format p)
{
   if (p == PIPE_FORMAT_NONE) return true;
   const struct util_format_description *d = util_format_description(p);
   if (d->layout == UTIL_FORMAT_LAYOUT_ASTC || d->layout == UTIL_FORMAT_LAYOUT_SUBSAMPLED || d->layout == UTIL_FORMAT_LAYOUT_PLANAR2 ||
       d->layout == UTIL_FORMAT_LAYOUT_PLANAR3 || d->layout == UTIL_FORMAT_LAYOUT_FXT1)
      return true;
   if (p == PIPE_FORMAT_ETC1_RGB8) return true;
   return false;
}

/* Generic Plane clump format (genxml "Clump Format"): raw by block size, or the special ones. */
static uint8_t
clump_format(enum pipe_format p)
{
   switch (p) {
   case PIPE_FORMAT_X32_S8X24_UINT: return 18;
   case PIPE_FORMAT_X24S8_UINT: return 20;
   case PIPE_FORMAT_S8X24_UINT: return 21;
   case PIPE_FORMAT_S8_UINT: return 22;
   case PIPE_FORMAT_L4A4_UNORM: return 24;
   case PIPE_FORMAT_L8A8_UNORM: case PIPE_FORMAT_L8A8_UINT: case PIPE_FORMAT_L8A8_SINT: return 25;
   case PIPE_FORMAT_A8_UNORM: return 26;
   case PIPE_FORMAT_ETC2_RGB8: case PIPE_FORMAT_ETC2_SRGB8: return 32;
   case PIPE_FORMAT_ETC2_R11_UNORM: return 33;
   case PIPE_FORMAT_ETC2_R11_SNORM: return 34;
   case PIPE_FORMAT_ETC2_RG11_UNORM: return 35;
   case PIPE_FORMAT_ETC2_RG11_SNORM: return 36;
   case PIPE_FORMAT_ETC2_RGBA8: case PIPE_FORMAT_ETC2_SRGBA8: return 37;
   case PIPE_FORMAT_ETC2_RGB8A1: case PIPE_FORMAT_ETC2_SRGB8A1: return 38;
   case PIPE_FORMAT_DXT1_RGB: case PIPE_FORMAT_DXT1_RGBA: case PIPE_FORMAT_DXT1_SRGB: case PIPE_FORMAT_DXT1_SRGBA: return 48;
   case PIPE_FORMAT_DXT3_RGBA: case PIPE_FORMAT_DXT3_SRGBA: return 49;
   case PIPE_FORMAT_DXT5_RGBA: case PIPE_FORMAT_DXT5_SRGBA: return 50;
   case PIPE_FORMAT_RGTC1_UNORM: return 51;
   case PIPE_FORMAT_RGTC1_SNORM: return 52;
   case PIPE_FORMAT_RGTC2_UNORM: return 53;
   case PIPE_FORMAT_RGTC2_SNORM: return 54;
   case PIPE_FORMAT_BPTC_RGB_UFLOAT: return 55;
   case PIPE_FORMAT_BPTC_RGB_FLOAT: return 56;
   case PIPE_FORMAT_BPTC_RGBA_UNORM: case PIPE_FORMAT_BPTC_SRGBA: return 57;
   default: break;
   }
   switch (util_format_get_blocksize(p)) {
   case 1: return 0;
   case 2: return 1;
   case 3: return 8;
   case 4: return 2;
   case 6: return 9;
   case 8: return 3;
   case 12: return 10;
   default: return 4; /* 16 */
   }
}

static void
fill_plane(unsigned arch, struct mali_fmt *o, unsigned i, enum pipe_format p, VkImageAspectFlagBits aspect)
{
   o->plane[i].pipe = p;
   o->plane[i].clump = clump_format(p);
   o->plane[i].bs = util_format_get_blocksize(p);
   o->plane[i].bw = util_format_get_blockwidth(p);
   o->plane[i].bh = util_format_get_blockheight(p);
   o->plane[i].hw = fmt_table(arch)[p].hw;
   o->plane[i].aspect = aspect;
}

static bool is_pure_int(enum pipe_format p) { return util_format_is_pure_integer(p); }

/* NV12 = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM: two memory planes (R8 luma, R8G8 interleaved Cb/Cr at half width and height). The hardware
 * only ever sees the two planes as ordinary textures; the YCbCr model/range conversion and the chroma siting are NIR (nir_vk_lower_ycbcr_tex,
 * mali_compile.c), the same split PanVK uses on the cores without the YUV texture descriptor. Not part of mali_format_count/nth (core 1.0 range). */
static bool
format_get_ycbcr(unsigned arch, struct mali_fmt *o)
{
   const struct pan_format *pt = fmt_table(arch);
   if (!(pt[PIPE_FORMAT_R8_UNORM].bind & PAN_BIND_SAMPLER_VIEW) || !(pt[PIPE_FORMAT_R8G8_UNORM].bind & PAN_BIND_SAMPLER_VIEW)) return false;
   o->vk = MALI_YCBCR_FORMAT;
   o->pipe = vk_format_to_pipe_format(MALI_YCBCR_FORMAT);
   o->ycbcr = true;
   o->bs = 1; o->bw = 1; o->bh = 1;
   o->nplanes = 2;
   fill_plane(arch, o, 0, PIPE_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_PLANE_0_BIT);
   fill_plane(arch, o, 1, PIPE_FORMAT_R8G8_UNORM, VK_IMAGE_ASPECT_PLANE_1_BIT);
   o->plane[1].ssx = o->plane[1].ssy = 1;
   o->bind = PAN_BIND_SAMPLER_VIEW;
   o->linear = o->optimal = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                            VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT | VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT |
                            VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT;
   return true;
}

bool
mali_format_get(unsigned arch, VkFormat f, struct mali_fmt *o)
{
   memset(o, 0, sizeof *o);
   if (f == MALI_YCBCR_FORMAT) return arch >= 10 && arch <= 14 && format_get_ycbcr(arch, o);
   if (arch < 10 || arch > 14 || f < FMT_FIRST || f > FMT_LAST) return false;
   enum pipe_format p = vk_format_to_pipe_format(f);
   if (fmt_skipped(f, p)) return false;

   const struct pan_format *pt = fmt_table(arch);
   const struct pan_blendable_format *bt = blend_table(arch);
   const struct util_format_description *d = util_format_description(p);
   const bool ds = util_format_is_depth_or_stencil(p);

   o->vk = f;
   o->pipe = p;
   o->compressed = util_format_is_compressed(p);
   o->bs = util_format_get_blocksize(p);
   o->bw = util_format_get_blockwidth(p);
   o->bh = util_format_get_blockheight(p);

   if (ds) {
      o->has_depth = util_format_has_depth(d);
      o->has_stencil = util_format_has_stencil(d);
      unsigned n = 0;
      if (o->has_depth) {
         enum pipe_format dp = util_format_get_depth_only(p);
         if (!(pt[dp].bind & PAN_BIND_DEPTH_STENCIL)) return false;
         fill_plane(arch, o, n++, dp, VK_IMAGE_ASPECT_DEPTH_BIT);
         switch (dp) {
         case PIPE_FORMAT_Z16_UNORM: o->zs_write = 1; o->zs_internal = 0; break;
         case PIPE_FORMAT_Z24X8_UNORM: o->zs_write = 4; o->zs_internal = 1; break;
         case PIPE_FORMAT_Z32_FLOAT: o->zs_write = 14; o->zs_internal = 2; break;
         default: return false;
         }
      }
      if (o->has_stencil) {
         if (!(pt[PIPE_FORMAT_S8_UINT].bind & PAN_BIND_DEPTH_STENCIL)) return false;
         fill_plane(arch, o, n++, PIPE_FORMAT_S8_UINT, VK_IMAGE_ASPECT_STENCIL_BIT);
      }
      o->nplanes = n;
      o->hw = o->plane[0].hw;
      o->bind = pt[p].bind ? pt[p].bind : PAN_BIND_DEPTH_STENCIL;
      /* D16S8 has no Mali plane pair (Z16 + S8 is fine as two planes, but VK D16_UNORM_S8_UINT maps to NONE in Mesa and is optional): not advertised. */
      VkFormatFeatureFlags ff = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
      if (o->has_depth && !o->has_stencil) ff |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
      if (o->has_depth && o->has_stencil) ff |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
      o->optimal = ff;
      o->linear = 0;   /* depth/stencil: optimal tiling only */
      o->buffer = 0;
      return true;
   }

   const struct pan_format *e = &pt[p];
   if (!e->bind) return false;
   o->nplanes = 1;
   o->hw = e->hw;
   o->bind = e->bind;
   fill_plane(arch, o, 0, p, VK_IMAGE_ASPECT_COLOR_BIT);

   /* Render target info from the blendable table (writeback + internal + dithered tile-buffer pixel format). */
   if (e->bind & PAN_BIND_RENDER_TARGET) {
      o->rt = true;
      bool rt_ok;
      mali_rtfmt_find(f, &rt_ok);   /* COLOR_ATTACHMENT_BLEND only where the render path can really draw to the format (no R8G8B8 yet) */
      if (bt[p].internal && rt_ok) {
         o->blendable = true;
         o->rt_internal = bt[p].internal;
         o->rt_writeback = bt[p].writeback;
         o->rt_tb = bt[p].bifrost[1];
      }
   }

   const bool pure = is_pure_int(p);
   VkFormatFeatureFlags img = 0, buf = 0;
   if (e->bind & PAN_BIND_SAMPLER_VIEW) {
      img |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
      if (!pure) img |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
      /* 8-bit-per-channel-or-less texture-only formats with no RT: copy-in goes through a raw-store path (M8). */
      img |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
   }
   if (o->rt && !o->compressed) {
      img |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
             VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
      if (o->blendable && !pure) img |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
   }
   if (e->bind & PAN_BIND_STORAGE_IMAGE) {
      img |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
      if (p == PIPE_FORMAT_R32_UINT || p == PIPE_FORMAT_R32_SINT) img |= VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT;
   }
   if (e->bind & PAN_BIND_VERTEX_BUFFER) buf |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
   if (e->bind & PAN_BIND_TEXEL_BUFFER) {
      buf |= VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
      if (e->bind & PAN_BIND_STORAGE_IMAGE) {
         buf |= VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
         if (p == PIPE_FORMAT_R32_UINT || p == PIPE_FORMAT_R32_SINT) buf |= VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT;
      }
   }
   o->optimal = img;
   o->buffer = buf;
   /* Linear tiling: single-level 2D, no compressed formats. Our layout is linear for both tilings by default, so same bits minus compressed. */
   o->linear = o->compressed ? 0 : (img & ~(VkFormatFeatureFlags)0);
   if (mali_bc_is_bc(f)) {
      /* R4-M7: BC is emulated (decode to a shadow image, mali_bc.h), whatever the sampler tables say */
      o->optimal = mali_bc_optimal_features(f);
      o->linear = 0;
      o->buffer = 0;
      o->rt = false;
   }
   if (!(o->optimal | o->buffer)) return false;
   return true;
}

unsigned
mali_format_count(unsigned arch)
{
   struct mali_fmt t;
   unsigned n = 0;
   for (unsigned f = FMT_FIRST; f <= FMT_LAST; f++)
      if (mali_format_get(arch, (VkFormat)f, &t)) n++;
   return n;
}

bool
mali_format_nth(unsigned arch, unsigned i, struct mali_fmt *out)
{
   unsigned n = 0;
   for (unsigned f = FMT_FIRST; f <= FMT_LAST; f++)
      if (mali_format_get(arch, (VkFormat)f, out) && n++ == i) return true;
   return false;
}

uint32_t
mali_format_attr_word0(const struct mali_fmt *f)
{
   if (!(f->bind & PAN_BIND_VERTEX_BUFFER)) return 0;
   return 5u | (1u << 4) | (1u << 8) | (f->hw << 10);   /* Attribute, 1D, offset enable (libkbase kb_draw_attr_1d) */
}

/* ---- image format properties ---- */

static unsigned
log2_floor(unsigned v) { unsigned n = 0; while (v >>= 1) n++; return n; }

VkResult
mali_image_format_properties(unsigned arch, VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage,
                             VkImageCreateFlags flags, VkImageFormatProperties *out)
{
   memset(out, 0, sizeof *out);
   struct mali_fmt f;
   if (!mali_format_get(arch, format, &f)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if (tiling != VK_IMAGE_TILING_LINEAR && tiling != VK_IMAGE_TILING_OPTIMAL) return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if (mali_bc_is_bc(format)) {
      VkResult br = mali_bc_image_supported(format, type, tiling, usage, flags, 1);
      if (br != VK_SUCCESS) return br;
   }
   if (f.ycbcr && (type != VK_IMAGE_TYPE_2D || (flags & (VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT | VK_IMAGE_CREATE_DISJOINT_BIT |
                                                         VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT)) ||
                   (usage & ~(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;   /* NV12: 2D, one level/layer, sampled + transfer, not disjoint */
   if (flags & (VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT | VK_IMAGE_CREATE_SPARSE_ALIASED_BIT |
                VK_IMAGE_CREATE_PROTECTED_BIT))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;

   const VkFormatFeatureFlags feat = tiling == VK_IMAGE_TILING_LINEAR ? f.linear : f.optimal;
   if (!feat) return VK_ERROR_FORMAT_NOT_SUPPORTED;
   const bool ds = f.has_depth || f.has_stencil;

   if ((usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && !(feat & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if ((usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) && !(feat & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) && !(feat & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) && !(feat & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && !(feat & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) && !(feat & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if ((usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) &&
       !(feat & (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if (usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) {
      if (!(usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)))
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
   }
   if (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) {
      if (type != VK_IMAGE_TYPE_2D || tiling == VK_IMAGE_TILING_LINEAR) return VK_ERROR_FORMAT_NOT_SUPPORTED;
   }
   if ((flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT) && (type != VK_IMAGE_TYPE_3D || tiling == VK_IMAGE_TILING_LINEAR))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if ((flags & VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT) && !f.compressed) return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if (ds && type != VK_IMAGE_TYPE_2D && type != VK_IMAGE_TYPE_1D) return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if (f.compressed && type == VK_IMAGE_TYPE_1D) return VK_ERROR_FORMAT_NOT_SUPPORTED;

   unsigned maxdim;
   switch (type) {
   case VK_IMAGE_TYPE_1D: maxdim = MALI_MAX_DIM_1D; out->maxExtent = (VkExtent3D){ maxdim, 1, 1 }; break;
   case VK_IMAGE_TYPE_2D: maxdim = MALI_MAX_DIM_2D; out->maxExtent = (VkExtent3D){ maxdim, maxdim, 1 }; break;
   case VK_IMAGE_TYPE_3D:
      if (ds) return VK_ERROR_FORMAT_NOT_SUPPORTED;
      maxdim = MALI_MAX_DIM_3D; out->maxExtent = (VkExtent3D){ maxdim, maxdim, maxdim }; break;
   default: return VK_ERROR_FORMAT_NOT_SUPPORTED;
   }
   out->maxMipLevels = log2_floor(maxdim) + 1;
   out->maxArrayLayers = type == VK_IMAGE_TYPE_3D ? 1 : MALI_MAX_LAYERS;
   out->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
   out->maxResourceSize = 1ull << 31;

   if (f.ycbcr) {
      out->maxMipLevels = 1;
      out->maxArrayLayers = 1;
   } else if (tiling == VK_IMAGE_TILING_LINEAR) {
      /* Spec allows restricting linear images to one 2D level/layer, single sampled. */
      if (type != VK_IMAGE_TYPE_2D) {
         memset(out, 0, sizeof *out);   /* on VK_ERROR_FORMAT_NOT_SUPPORTED every field must be 0 (dEQP info.image_format_properties) */
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
      }
      out->maxMipLevels = 1;
      out->maxArrayLayers = 1;
   } else if (type == VK_IMAGE_TYPE_2D && !(flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) && !f.compressed &&
              !(usage & VK_IMAGE_USAGE_STORAGE_BIT) && (feat & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))) {
      if (mali_experimental_msaa())
         out->sampleCounts |= VK_SAMPLE_COUNT_4_BIT;   /* Mali: 1 and 4 (16x is not wired); gated like 4x pipelines */
   }
   return VK_SUCCESS;
}

/* ---- entrypoints ---- */

static unsigned
pdev_arch(VkPhysicalDevice h)
{
   VK_FROM_HANDLE(mali_physical_device, pdev, h);
   return pdev->gpu.arch_major;
}

VKAPI_ATTR void VKAPI_CALL
mali_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice pd, VkFormat format, VkFormatProperties2 *p)
{
   struct mali_fmt f;
   const bool ok = mali_format_get(pdev_arch(pd), format, &f);
   p->formatProperties = (VkFormatProperties){ ok ? f.linear : 0, ok ? f.optimal : 0, ok ? f.buffer : 0 };
   VkFormatProperties3 *p3 = vk_find_struct(p->pNext, FORMAT_PROPERTIES_3);
   if (p3) {
      p3->linearTilingFeatures = ok ? f.linear : 0;
      p3->optimalTilingFeatures = ok ? f.optimal : 0;
      p3->bufferFeatures = ok ? f.buffer : 0;
      /* 1.3 *_WITHOUT_FORMAT bits: the storage path is format-agnostic for all _I formats. */
      const uint64_t nf = 0x100000000ull;   /* STORAGE_WRITE_WITHOUT_FORMAT only: shaderStorageImageReadWithoutFormat is not advertised (mali_physical_device.c) */
      if (ok && (f.optimal & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) p3->optimalTilingFeatures |= nf;
      if (ok && (f.linear & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) p3->linearTilingFeatures |= nf;
      if (ok && (f.buffer & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT)) p3->bufferFeatures |= nf;
      /* 1.3: sampled depth formats report SAMPLED_IMAGE_DEPTH_COMPARISON (the sampler has compareOp, mali_sampler.c) */
      if (ok && f.has_depth && (f.optimal & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) p3->optimalTilingFeatures |= 0x200000000ull;
   }
   /* M14 image DRM format modifiers: one, DRM_FORMAT_MOD_LINEAR. A linear-only driver must report that modifier
    * (with a plane count) for every format it supports, not an empty list: wsi_common / gamescope's WSI layer and
    * zink read this to decide how to import and export images, and a zero count for a supported format is what
    * shows up as "returned zero modifiers for DRM format 0x3231564e" (NV12) before the X11/GLX swapchain fails.
    * NV12/P010 linear are two-plane layouts, everything else is one plane. */
   const bool mod_ok = ok && (f.linear & f.optimal) != 0;
   const uint32_t mod_planes = f.ycbcr ? 2 : 1;
   const uint64_t mf = mod_ok ? (uint64_t)(f.linear & f.optimal) : 0;
   vk_foreach_struct(st, e, p->pNext) {
      if (st == VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT) {
         VkDrmFormatModifierPropertiesListEXT *l = e;
         /* M14: LINEAR only (our "optimal" layout is linear unless MALI_IMAGE_TILED=1, which never applies to modifier images).
          * Multi-planar formats report one modifier plane per memory plane; the image layout puts them in one allocation at
          * plane[].offset (mali_image_layout_init) and GetImageSubresourceLayout exposes each (MEMORY_PLANE_n). NV12 reports
          * 2 planes: gamescope asks for its DRM modifiers and treats a zero count as "no NV12 scanout". */
         const VkFormatFeatureFlags mf = ok ? (f.linear & f.optimal) : 0;
         const uint32_t mplanes = f.ycbcr ? f.nplanes : 1u;
         if (!l->pDrmFormatModifierProperties) {
            l->drmFormatModifierCount = mf ? 1 : 0;
         } else if (l->drmFormatModifierCount) {
            if (mf) {
               l->pDrmFormatModifierProperties[0] = (VkDrmFormatModifierPropertiesEXT){
                  .drmFormatModifier = mali_wsi_modifier, .drmFormatModifierPlaneCount = mplanes, .drmFormatModifierTilingFeatures = mf};
               l->drmFormatModifierCount = 1;
            } else l->drmFormatModifierCount = 0;
         }
      }
      if (st == VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT) {
         VkDrmFormatModifierPropertiesList2EXT *l2 = e;
         if (!l2->pDrmFormatModifierProperties) {
            l2->drmFormatModifierCount = mf ? 1 : 0;
         } else if (l2->drmFormatModifierCount) {
            if (mf) {
               l2->pDrmFormatModifierProperties[0] = (VkDrmFormatModifierProperties2EXT){
                  .drmFormatModifier = mali_wsi_modifier, .drmFormatModifierPlaneCount = mod_planes,
                  .drmFormatModifierTilingFeatures = mf};
               l2->drmFormatModifierCount = 1;
            } else l2->drmFormatModifierCount = 0;
         }
      }
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *p)
{
   memset(&p->imageFormatProperties, 0, sizeof p->imageFormatProperties);
   const VkPhysicalDeviceExternalImageFormatInfo *ext = vk_find_struct_const(info->pNext, PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
   if (ext && ext->handleType && ext->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) return VK_ERROR_FORMAT_NOT_SUPPORTED;
   VkImageTiling tiling = info->tiling;
   if (tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
      const VkPhysicalDeviceImageDrmFormatModifierInfoEXT *mi =
         vk_find_struct_const(info->pNext, PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT);
      if (!mi || mi->drmFormatModifier != mali_wsi_modifier) return VK_ERROR_FORMAT_NOT_SUPPORTED;
      tiling = VK_IMAGE_TILING_LINEAR;
   }
   VkResult r = mali_image_format_properties(pdev_arch(pd), info->format, info->type, tiling, info->usage, info->flags,
                                             &p->imageFormatProperties);
   if (r != VK_SUCCESS) return r;
   /* gamescope sizes its descriptor pool from this: NV12 through a conversion is a 3-slot element (luma view, chroma view, sampler) where
    * a plain combined image sampler is 1 (the pool counts a combined image sampler as 2 slots and apps multiply by this number). */
   VkSamplerYcbcrConversionImageFormatProperties *yp = vk_find_struct(p->pNext, SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES);
   if (yp) yp->combinedImageSamplerDescriptorCount = info->format == MALI_YCBCR_FORMAT ? MALI_YCBCR_SLOTS : 1;
   VkExternalImageFormatProperties *eo = vk_find_struct(p->pNext, EXTERNAL_IMAGE_FORMAT_PROPERTIES);
   if (ext && ext->handleType && info->format == MALI_YCBCR_FORMAT) return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if (eo) {
      eo->externalMemoryProperties = (VkExternalMemoryProperties){ 0 };
      if (ext && ext->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
         eo->externalMemoryProperties = (VkExternalMemoryProperties){
            VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
   }
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
mali_GetPhysicalDeviceSparseImageFormatProperties2(VkPhysicalDevice pd, const VkPhysicalDeviceSparseImageFormatInfo2 *info, uint32_t *count,
                                                   VkSparseImageFormatProperties2 *props)
{
   *count = 0;   /* no sparse (sparseBinding/Residency features are off) */
}
