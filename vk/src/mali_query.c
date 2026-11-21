/* SPDX-License-Identifier: MIT */
/* M13, Vulkan layer of query pools (occlusion + timestamp) over vk_query_pool and mali_query_core.c.
 *   Create/Destroy/Reset/GetQueryPoolResults     host side, pool memory from the mali_meta device seam (zeroed, CPU-mapped, cached BO: the
 *                                                host must invalidate before reading, wiring item)
 *   CmdResetQueryPool / CmdCopyQueryPoolResults  compute fill / qcopy jobs (mali_meta)
 *   CmdWriteTimestamp2                           ops frame (mali_event.c): STORE_STATE timestamp + WAIT + availability
 *   CmdBeginQuery/EndQuery                       occlusion: the draws of the open render pass write the counter at the query's value word
 *                                                (DCD occlusion mode 3, OQ register; patches/occlusion-cs-emit.patch), EndQuery sets availability.
 * Hooks into mali_cmd.c (weak: without wiring the commands fail with VK_ERROR_FEATURE_NOT_PRESENT on the command buffer, R3-PLAN.md M13):
 *   mali_cmd_push_ops(cmd, ops, n)     push an ops frame; inside a render pass it must be deferred until the render pass frame was pushed
 *   mali_cmd_set_occlusion(cmd, va, m) the occlusion counter address / mode copied into every following mali_draw (occ_va/occ_mode) */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mali_cmd.h"
#include "mali_meta_vk.h"
#include "mali_query.h"
#include "mali_queue.h"
#include "mali_query_core.h"
#include "vk_buffer.h"
#include "vk_query_pool.h"

__attribute__((weak)) void mali_cmd_push_ops(struct mali_cmd_buffer *cmd, const struct mali_op *ops, unsigned n);
__attribute__((weak)) void mali_cmd_set_occlusion(struct mali_cmd_buffer *cmd, uint64_t va, unsigned mode);

struct mali_query_pool {
   struct vk_query_pool vk;
   struct mali_qpool q;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(mali_query_pool, vk.base, VkQueryPool, VK_OBJECT_TYPE_QUERY_POOL)

#define CMD(h) struct mali_cmd_buffer *cmd = mali_cmd_buffer_from_vk(vk_command_buffer_from_handle(h))

VKAPI_ATTR VkResult VKAPI_CALL
mali_CreateQueryPool(VkDevice _dev, const VkQueryPoolCreateInfo *ci, const VkAllocationCallbacks *alloc, VkQueryPool *out)
{
   struct vk_device *dev = vk_device_from_handle(_dev);
   if (ci->queryType != VK_QUERY_TYPE_OCCLUSION && ci->queryType != VK_QUERY_TYPE_TIMESTAMP)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   struct mali_query_pool *p = vk_query_pool_create(dev, ci, alloc, sizeof *p);
   if (!p)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   p->q.count = ci->queryCount;
   p->q.type = ci->queryType == VK_QUERY_TYPE_OCCLUSION ? MALI_QT_OCCLUSION : MALI_QT_TIMESTAMP;
   p->q.cpu = mali_hw_meta_alloc(container_of(dev, struct mali_device, vk), mali_qpool_size(ci->queryCount ? ci->queryCount : 1), 64, &p->q.va);
   if (!p->q.cpu) {
      vk_query_pool_destroy(dev, alloc, &p->vk);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   *out = mali_query_pool_to_handle(p);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
mali_DestroyQueryPool(VkDevice _dev, VkQueryPool h, const VkAllocationCallbacks *alloc)
{
   struct vk_device *dev = vk_device_from_handle(_dev);
   struct mali_query_pool *p = mali_query_pool_from_handle(h);
   if (!p)
      return;
   mali_hw_meta_free(container_of(dev, struct mali_device, vk), p->q.cpu);
   vk_query_pool_destroy(dev, alloc, &p->vk);
}

VKAPI_ATTR void VKAPI_CALL
mali_ResetQueryPool(VkDevice _dev, VkQueryPool h, uint32_t first, uint32_t count)
{
   mali_qpool_host_reset(&mali_query_pool_from_handle(h)->q, first, count);
}

struct qwait { struct mali_device *dev; uint64_t deadline_ns; };

static uint64_t
qnow_ns(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

static int
qwait_poll(void *u)
{
   struct qwait *w = u;
   if (mali_device_pdev(w->dev)->backend->device_lost(w->dev->kdev))
      return MALI_QWAIT_LOST;
   if (qnow_ns() > w->deadline_ns)
      return MALI_QWAIT_TIMEOUT;
   nanosleep(&(struct timespec){ 0, 50000 }, NULL);
   return 0;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_GetQueryPoolResults(VkDevice _dev, VkQueryPool h, uint32_t first, uint32_t count, size_t size, void *data, VkDeviceSize stride, VkQueryResultFlags flags)
{
   struct mali_device *dev = container_of(vk_device_from_handle(_dev), struct mali_device, vk);
   /* the deadline only bounds a query that was never submitted (application bug); a submitted one completes or the device is lost */
   struct qwait w = { dev, qnow_ns() + 10ull * 1000000000ull };
   int r = mali_qpool_get_results_wait(&mali_query_pool_from_handle(h)->q, first, count, size, data, stride, flags, qwait_poll, &w);
   if (r == MALI_QWAIT_LOST)
      return VK_ERROR_DEVICE_LOST;
   return r == 0 ? VK_SUCCESS : (r == 1 || r == MALI_QWAIT_TIMEOUT) ? VK_NOT_READY : VK_ERROR_UNKNOWN;
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdResetQueryPool(VkCommandBuffer h, VkQueryPool pool, uint32_t first, uint32_t count)
{
   CMD(h);
   struct mali_meta_list l = { 0 };
   int r = mali_qpool_plan_reset(&l, &mali_query_pool_from_handle(pool)->q, first, count);
   mali_meta_vk_emit(cmd, &l, r);
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdCopyQueryPoolResults(VkCommandBuffer h, VkQueryPool pool, uint32_t first, uint32_t count, VkBuffer dstb, VkDeviceSize off, VkDeviceSize stride, VkQueryResultFlags flags)
{
   CMD(h);
   struct vk_buffer *b = vk_buffer_from_handle(dstb);
   struct mali_meta_list l = { 0 };
   struct mali_meta_surf d = { vk_buffer_address(b, 0), b->size, 0, 0, 0 };
   int r = mali_qpool_plan_copy(&l, &mali_query_pool_from_handle(pool)->q, &d, first, count, off, stride, flags);
   mali_meta_vk_emit(cmd, &l, r);
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdWriteTimestamp2(VkCommandBuffer h, VkPipelineStageFlags2 stage, VkQueryPool pool, uint32_t q)
{
   CMD(h);
   struct mali_query_pool *p = mali_query_pool_from_handle(pool);
   if (!mali_cmd_push_ops || p->q.type != MALI_QT_TIMESTAMP || q >= p->q.count) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_FEATURE_NOT_PRESENT);
      return;
   }
   struct mali_op op = mali_qpool_op_timestamp(&p->q, q);
   mali_cmd_push_ops(cmd, &op, 1);
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdBeginQuery(VkCommandBuffer h, VkQueryPool pool, uint32_t q, VkQueryControlFlags flags)
{
   CMD(h);
   struct mali_query_pool *p = mali_query_pool_from_handle(pool);
   if (!mali_cmd_set_occlusion || p->q.type != MALI_QT_OCCLUSION || q >= p->q.count) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_FEATURE_NOT_PRESENT);
      return;
   }
   mali_cmd_set_occlusion(cmd, mali_qpool_value_va(&p->q, q), MALI_OCC_MODE_COUNTER);
}

VKAPI_ATTR void VKAPI_CALL
mali_CmdEndQuery(VkCommandBuffer h, VkQueryPool pool, uint32_t q)
{
   CMD(h);
   struct mali_query_pool *p = mali_query_pool_from_handle(pool);
   if (!mali_cmd_set_occlusion || !mali_cmd_push_ops || p->q.type != MALI_QT_OCCLUSION || q >= p->q.count) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_FEATURE_NOT_PRESENT);
      return;
   }
   mali_cmd_set_occlusion(cmd, 0, 0);
   struct mali_op op = mali_qpool_op_end(&p->q, q);
   mali_cmd_push_ops(cmd, &op, 1);
}
