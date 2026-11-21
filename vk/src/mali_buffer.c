/* SPDX-License-Identifier: MIT */
/* M5: VkBuffer, VkBufferView and the resolver table the descriptor code uses (mali_res_ops). */
#include "mali_image.h"

#include <errno.h>
#include <string.h>

#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_util.h"

static inline uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

/* Texel buffer descriptor: Buffer, type Structure; stride = texel size, size = range rounded down to whole texels, conversion = memory
 * format word (not interpreted by hardware, read by the shader's texel fetch code). Same bits on v11 and v13. */
void
mali_pack_texel_buffer(struct mali_hw_desc *d, const struct mali_fmt *f, uint64_t va, uint64_t range)
{
   const uint64_t stride = f->bs;
   const uint64_t size = (range / stride) * stride;
   memset(d, 0, sizeof *d);
   d->w[0] = 9u | (3u << 4);
   d->w[1] = (uint32_t)size;
   d->w[2] = (uint32_t)va;
   d->w[3] = (uint32_t)(va >> 32);
   d->w[4] = (uint32_t)stride;
   d->w[5] = (uint32_t)(size >> 32);
   d->w[7] = f->hw;   /* Internal Conversion: memory format (22 bits), raw = 0 */
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_CreateBuffer(VkDevice _dev, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *alloc, VkBuffer *out)
{
   VK_FROM_HANDLE(mali_device, dev, _dev);
   if (ci->flags & (VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT | VK_BUFFER_CREATE_SPARSE_ALIASED_BIT))
      return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);
   struct mali_buffer *b = vk_buffer_create(&dev->vk, ci, alloc, sizeof *b);
   if (!b) return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   *out = mali_buffer_to_handle(b);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
mali_DestroyBuffer(VkDevice _dev, VkBuffer h, const VkAllocationCallbacks *alloc)
{
   VK_FROM_HANDLE(mali_device, dev, _dev);
   VK_FROM_HANDLE(mali_buffer, b, h);
   if (!b) return;
   vk_buffer_destroy(&dev->vk, alloc, &b->vk);
}

VKAPI_ATTR void VKAPI_CALL
mali_GetDeviceBufferMemoryRequirements(VkDevice _dev, const VkDeviceBufferMemoryRequirements *info, VkMemoryRequirements2 *r)
{
   VK_FROM_HANDLE(mali_device, dev, _dev);
   const struct mali_physical_device *pd = mali_device_pdev(dev);
   r->memoryRequirements.size = align_up(info->pCreateInfo->size, MALI_BUF_ALIGN);
   r->memoryRequirements.alignment = MALI_BUF_ALIGN;
   r->memoryRequirements.memoryTypeBits = (1u << pd->memory.memoryTypeCount) - 1;
   vk_foreach_struct(st, e, r->pNext) {
      if (st == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
         VkMemoryDedicatedRequirements *dr = e;
         dr->prefersDedicatedAllocation = false;
         dr->requiresDedicatedAllocation = false;
      }
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_BindBufferMemory2(VkDevice _dev, uint32_t n, const VkBindBufferMemoryInfo *b)
{
   VK_FROM_HANDLE(mali_device, dev, _dev);
   for (uint32_t i = 0; i < n; i++) {
      VK_FROM_HANDLE(mali_buffer, buf, b[i].buffer);
      VK_FROM_HANDLE(mali_memory, mem, b[i].memory);
      buf->mem = mem;
      buf->mem_off = b[i].memoryOffset;
      /* vk.usage is the EFFECTIVE usage: vk_buffer_init reads VkBufferUsageFlags2CreateInfoKHR from the chain
       * (vk_buffer_usage_flags), so a Vulkan-1.4 buffer created with ::usage = 0 and the chain is covered here. */
      if ((buf->vk.usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) && mali_device_pdev(dev)->backend->bo_host_touch)
         mali_device_pdev(dev)->backend->bo_host_touch(NULL, &mem->bo);
      buf->vk.device_address = mem->bo.gpu_va + b[i].memoryOffset;
   }
   return VK_SUCCESS;
}

/* ---- buffer views ---- */

static int
bview_refresh(struct mali_buffer_view *v)
{
   if (v->emitted && v->emitted_for_va == v->vk.buffer->device_address) return 0;
   mali_pack_texel_buffer(&v->desc, &v->fmt, v->vk.buffer->device_address + v->vk.offset, v->vk.range);
   v->emitted = true;
   v->emitted_for_va = v->vk.buffer->device_address;
   return 0;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_CreateBufferView(VkDevice _dev, const VkBufferViewCreateInfo *ci, const VkAllocationCallbacks *alloc, VkBufferView *out)
{
   VK_FROM_HANDLE(mali_device, dev, _dev);
   struct mali_fmt f;
   if (!mali_format_get(mali_device_pdev(dev)->gpu.arch_major, ci->format, &f) || !f.buffer) return vk_error(dev, VK_ERROR_FORMAT_NOT_SUPPORTED);
   struct mali_buffer_view *v = vk_buffer_view_create(&dev->vk, ci, alloc, sizeof *v);
   if (!v) return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   v->fmt = f;
   bview_refresh(v);
   *out = mali_buffer_view_to_handle(v);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
mali_DestroyBufferView(VkDevice _dev, VkBufferView h, const VkAllocationCallbacks *alloc)
{
   VK_FROM_HANDLE(mali_device, dev, _dev);
   VK_FROM_HANDLE(mali_buffer_view, v, h);
   if (!v) return;
   vk_buffer_view_destroy(&dev->vk, alloc, &v->vk);
}

int
mali_buffer_view_desc(VkBufferView h, struct mali_hw_desc *out)
{
   VK_FROM_HANDLE(mali_buffer_view, v, h);
   if (!v) return -EINVAL;
   bview_refresh(v);
   *out = v->desc;
   return 0;
}

/* ---- resolvers for mali_descriptor.c ---- */

static int
res_image_view(VkImageView v, VkDescriptorType t, struct mali_hw_desc *o) { return mali_image_view_desc(v, t, o); }

static int
res_sampler(VkSampler h, struct mali_hw_desc *o)
{
   VK_FROM_HANDLE(mali_sampler, s, h);
   if (!s) return -EINVAL;
   *o = s->hw;
   return 0;
}

static int
res_sampler_ycbcr(VkSampler h, struct vk_ycbcr_conversion_state *o)
{
   VK_FROM_HANDLE(mali_sampler, s, h);
   if (!s || !s->vk.ycbcr_conversion) return 0;
   *o = s->vk.ycbcr_conversion->state;
   return 1;
}

static int
res_buffer(VkBuffer h, uint64_t *va, uint64_t *size)
{
   VK_FROM_HANDLE(mali_buffer, b, h);
   if (!b) return -EINVAL;
   *va = b->vk.device_address;
   *size = b->vk.size;
   return 0;
}

const struct mali_desc_res_ops mali_res_ops = { res_image_view, res_sampler, res_buffer, mali_buffer_view_desc, res_sampler_ycbcr, mali_image_view_planes };
