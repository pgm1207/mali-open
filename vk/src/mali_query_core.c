/* SPDX-License-Identifier: MIT */
/* M13 query pool core, see mali_query_core.h. */
#include <errno.h>
#include <string.h>

#include "mali_query_core.h"

static int
range_ok(const struct mali_qpool *p, uint32_t first, uint32_t count)
{
   return first <= p->count && count <= p->count - first;
}

int
mali_qpool_host_reset(const struct mali_qpool *p, uint32_t first, uint32_t count)
{
   if (!range_ok(p, first, count))
      return -EINVAL;
   memset((uint8_t *)p->cpu + (uint64_t)first * MALI_QUERY_ENTRY, 0, mali_qpool_size(count));
   return 0;
}

int
mali_qpool_get_results(const struct mali_qpool *p, uint32_t first, uint32_t count, size_t data_size, void *data, uint64_t stride, uint32_t flags)
{
   bool is64 = flags & MALI_QF_64BIT;
   uint32_t es = is64 ? 8 : 4, elem = es * ((flags & MALI_QF_AVAIL) ? 2 : 1);
   if (!range_ok(p, first, count) || (count > 1 && (stride < elem || (stride & (es - 1)))))
      return -EINVAL;
   if (count && (uint64_t)(count - 1) * stride + elem > data_size)
      return -EINVAL;
   int ret = 0;
   for (uint32_t i = 0; i < count; i++) {
      const uint32_t *e = (const uint32_t *)((const uint8_t *)p->cpu + (uint64_t)(first + i) * MALI_QUERY_ENTRY);
      uint32_t avail = __atomic_load_n(&e[0], __ATOMIC_ACQUIRE), lo = e[2], hi = e[3];
      uint8_t *o = (uint8_t *)data + i * stride;
      if (avail || (flags & MALI_QF_PARTIAL)) {
         if (is64) {
            memcpy(o, &lo, 4);
            memcpy(o + 4, &hi, 4);
         } else {
            uint32_t v = hi ? 0xffffffffu : lo;
            memcpy(o, &v, 4);
         }
      }
      /* WAIT means wait for availability even with PARTIAL: PARTIAL only changes what a non-waiting call returns */
      if (!avail && (!(flags & MALI_QF_PARTIAL) || (flags & MALI_QF_WAIT)))
         ret = 1;
      if (flags & MALI_QF_AVAIL) {
         uint64_t a = avail ? 1 : 0;
         memcpy(o + es, &a, es);   /* little endian: low word first, high word 0 */
      }
   }
   return ret;
}

int
mali_qpool_get_results_wait(const struct mali_qpool *p, uint32_t first, uint32_t count, size_t data_size, void *data, uint64_t stride, uint32_t flags,
                            int (*poll)(void *), void *user)
{
   int r = mali_qpool_get_results(p, first, count, data_size, data, stride, flags);
   /* WAIT: submits retire asynchronously, so an unavailable query is normal right after vkQueueSubmit: block until the GPU marks it */
   while (r == 1 && (flags & MALI_QF_WAIT) && poll) {
      int pr = poll(user);
      if (pr)
         return pr;   /* gave up (device lost / deadline): caller maps it */
      r = mali_qpool_get_results(p, first, count, data_size, data, stride, flags);
   }
   return r;
}

int
mali_qpool_plan_reset(struct mali_meta_list *l, const struct mali_qpool *p, uint32_t first, uint32_t count)
{
   if (!range_ok(p, first, count))
      return -EINVAL;
   struct mali_meta_surf s = { p->va, mali_qpool_size(p->count), (uint64_t)first * MALI_QUERY_ENTRY, 0, 0 };
   return mali_meta_plan_fill(l, &s, mali_qpool_size(count), 0);
}

int
mali_qpool_plan_copy(struct mali_meta_list *l, const struct mali_qpool *p, const struct mali_meta_surf *dst, uint32_t first, uint32_t count,
                     uint64_t dst_off, uint64_t stride, uint32_t flags)
{
   if (!range_ok(p, first, count))
      return -EINVAL;
   struct mali_meta_surf s = { p->va, mali_qpool_size(p->count), 0, 0, 0 };
   return mali_meta_plan_qcopy(l, &s, dst, first, count, dst_off, stride, flags);
}

struct mali_op
mali_qpool_op_timestamp(const struct mali_qpool *p, uint32_t q)
{
   return (struct mali_op){ MALI_OP_TIMESTAMP, mali_qpool_value_va(p, q), mali_qpool_entry_va(p, q), 1 };
}

struct mali_op
mali_qpool_op_end(const struct mali_qpool *p, uint32_t q)
{
   return (struct mali_op){ MALI_OP_AVAIL, mali_qpool_entry_va(p, q), 0, 1 };
}
