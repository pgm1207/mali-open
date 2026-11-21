/* SPDX-License-Identifier: MIT */
/* M13, Mesa-free core of query pools (layout in mali_event.h: 16 B entries {u32 avail, pad, u64 value}). Result flags use the Vulkan values
 * (64BIT 1, WAIT 2, WITH_AVAILABILITY 4, PARTIAL 8). Everything here is testable with a host-pointer pool (GPU VA = CPU pointer). */
#ifndef MALI_QUERY_CORE_H
#define MALI_QUERY_CORE_H

#include <stddef.h>
#include <stdint.h>
#include "mali_event.h"
#include "mali_meta.h"

#ifdef __cplusplus
extern "C" {
#endif

enum mali_qtype { MALI_QT_OCCLUSION = 0, MALI_QT_TIMESTAMP = 1 };
#define MALI_QF_64BIT 1u
#define MALI_QF_WAIT 2u
#define MALI_QF_AVAIL 4u
#define MALI_QF_PARTIAL 8u

struct mali_qpool { void *cpu; uint64_t va; uint32_t count; enum mali_qtype type; };

static inline uint64_t mali_qpool_size(uint32_t count) { return (uint64_t)count * MALI_QUERY_ENTRY; }
static inline uint64_t mali_qpool_entry_va(const struct mali_qpool *p, uint32_t q) { return p->va + (uint64_t)q * MALI_QUERY_ENTRY; }
static inline uint64_t mali_qpool_value_va(const struct mali_qpool *p, uint32_t q) { return mali_qpool_entry_va(p, q) + MALI_QUERY_VALUE_OFF; }

/* vkResetQueryPool (host): zero the range. -EINVAL when out of range. */
int mali_qpool_host_reset(const struct mali_qpool *p, uint32_t first, uint32_t count);
/* vkGetQueryPoolResults. 0 = VK_SUCCESS, 1 = VK_NOT_READY (a query was unavailable and PARTIAL not set), -EINVAL bad args (range, stride, data too small).
 * Never blocks: an unavailable query is NOT_READY whatever the flags. */
int mali_qpool_get_results(const struct mali_qpool *p, uint32_t first, uint32_t count, size_t data_size, void *data, uint64_t stride, uint32_t flags);
/* Same, but honours WAIT: while a query is unavailable, call poll(user) (which should sleep briefly) and re-read. poll returns 0 to keep
 * waiting, nonzero to give up (returned as is, e.g. 2 = device lost, 3 = deadline). Without WAIT, or with poll NULL, identical to the above. */
int mali_qpool_get_results_wait(const struct mali_qpool *p, uint32_t first, uint32_t count, size_t data_size, void *data, uint64_t stride, uint32_t flags,
                                int (*poll)(void *), void *user);
#define MALI_QWAIT_LOST 2
#define MALI_QWAIT_TIMEOUT 3
/* vkCmdResetQueryPool: a fill job over the range. */
int mali_qpool_plan_reset(struct mali_meta_list *l, const struct mali_qpool *p, uint32_t first, uint32_t count);
/* vkCmdCopyQueryPoolResults: the qcopy compute job (dst = buffer surface, dst_off added). */
int mali_qpool_plan_copy(struct mali_meta_list *l, const struct mali_qpool *p, const struct mali_meta_surf *dst, uint32_t first, uint32_t count,
                         uint64_t dst_off, uint64_t stride, uint32_t flags);
/* vkCmdWriteTimestamp2: one ops-frame op (STORE_STATE timestamp, WAIT, availability). */
struct mali_op mali_qpool_op_timestamp(const struct mali_qpool *p, uint32_t q);
/* vkCmdEndQuery (occlusion): availability op (after the frames that counted, they retire in order). */
struct mali_op mali_qpool_op_end(const struct mali_qpool *p, uint32_t q);
/* vkCmdBeginQuery (occlusion): counter address the draws use (DCD occlusion mode 3, patches/occlusion-cs-emit.patch) */
#define MALI_OCC_MODE_COUNTER 3u

#ifdef __cplusplus
}
#endif
#endif
