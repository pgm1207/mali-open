/* SPDX-License-Identifier: MIT */
/* M13 offline tests (fake backend = host-pointer "GPU"): query pool core, query result copy shader (reference) vs the host path, reset,
 * timestamp / occlusion / event lifecycles by interpreting the emitted ops-frame CS against host memory. Usage: t_qpool [-v]. */
#include "../src/mali_query_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
extern int mali_meta_ref_oob;

struct ent { uint32_t avail, pad; uint64_t v; };
static struct mali_qpool mkpool(struct ent *e, uint32_t n, enum mali_qtype t) { return (struct mali_qpool){ e, (uint64_t)(uintptr_t)e, n, t }; }

/* Interprets an ops CS with VAs as host pointers. Returns 0 done, 1 blocked on SYNC_WAIT32. *async_ok: STORE_STATE results observed only after a WAIT. */
static int interp(const uint64_t *cs, unsigned n, uint64_t fake_ts, int *hazards)
{
   uint32_t r[256] = { 0 };
   int pend = 0;
   for (unsigned i = 0; i < n; i++) {
      uint64_t w = cs[i];
      unsigned op = w >> 56, a = (w >> 48) & 0xff;
      if (op == 1) { r[a] = (uint32_t)w; r[a + 1] = (w >> 32) & 0xffff; }
      else if (op == 2) r[a] = (uint32_t)w;
      else if (op == 3) pend = 0;
      else if (op == 36) pend = 1;
      else if (op == 40) { uint64_t va = r[(w >> 40) & 0xff] | (uint64_t)r[((w >> 40) & 0xff) + 1] << 32; memcpy((void *)(uintptr_t)va, &fake_ts, 8); pend = 1; }
      else if (op == 38 || op == 51) {
         if (pend) (*hazards)++;
         unsigned ar = (w >> 40) & 0xff, dr = (w >> 32) & 0xff;
         uint64_t va = r[ar] | (uint64_t)r[ar + 1] << 32;
         if (op == 38) memcpy((void *)(uintptr_t)va, &r[dr], 4);
         else { uint64_t x; memcpy(&x, (void *)(uintptr_t)va, 8); x += r[dr] | (uint64_t)r[dr + 1] << 32; memcpy((void *)(uintptr_t)va, &x, 8); }
      } else if (op == 39) {
         unsigned ar = (w >> 40) & 0xff, dr = (w >> 32) & 0xff;
         uint32_t m; memcpy(&m, (void *)(uintptr_t)(r[ar] | (uint64_t)r[ar + 1] << 32), 4);
         if (!(((w >> 28) & 0xf) == 1 && m > r[dr])) return 1;
      }
   }
   return 0;
}

static int run_ops(const struct mali_op *ops, unsigned n, uint64_t ts, uint64_t *sync)
{
   struct mali_env env = { .arch = 11, .sync_va = (uint64_t)(uintptr_t)sync };
   uint64_t *cs; unsigned k;
   if (mali_ops_build(&env, ops, n, &cs, &k)) return -1;
   int haz = 0, blocked = interp(cs, k, ts, &haz);
   CHECK(haz == 0, "async-hazard in emitted CS");
   free(cs);
   return blocked;
}

static void t_host(void)
{
   struct ent e[6] = { { 1, 0, 5 }, { 0, 0, 7 }, { 1, 0, 0x100000009ull }, { 1, 0, 11 }, { 1, 0, 12 }, { 0, 0, 0 } };
   struct mali_qpool p = mkpool(e, 6, MALI_QT_OCCLUSION);
   for (unsigned fl = 0; fl < 16; fl++) {
      uint8_t h[256], s[256];
      memset(h, 0xcc, 256); memset(s, 0xcc, 256);
      int hr = mali_qpool_get_results(&p, 1, 4, 256 - 8, h + 8, 24, fl);
      /* shader path through the reference */
      struct mali_meta_list l = { 0 };
      struct mali_meta_surf d = { (uint64_t)(uintptr_t)s, 256, 0, 0, 0 };
      CHECK(mali_qpool_plan_copy(&l, &p, &d, 1, 4, 8, 24, fl) == 0, "plan copy");
      for (unsigned i = 0; i < l.n; i++) mali_meta_ref_run(&l.j[i]);
      mali_meta_list_free(&l);
      bool part = fl & MALI_QF_PARTIAL;
      /* shader writes values of unavailable queries only with PARTIAL, as the host path; availability identical */
      CHECK(!memcmp(h, s, 256), "host vs copy-shader differ for flags %u", fl);
      CHECK(hr == (part ? 0 : 1), "NOT_READY expectation flags %u: %d", fl, hr);
   }
   uint64_t v64[2] = { 0, 0 };
   CHECK(mali_qpool_get_results(&p, 2, 1, 8, v64, 8, MALI_QF_64BIT) == 0 && v64[0] == 0x100000009ull, "64-bit value");
   uint32_t v32 = 0;
   CHECK(mali_qpool_get_results(&p, 2, 1, 4, &v32, 4, 0) == 0 && v32 == 0xffffffffu, "32-bit clamp");
   CHECK(mali_qpool_get_results(&p, 5, 2, 64, v64, 8, 1) == -22, "range error");
   CHECK(mali_qpool_get_results(&p, 0, 2, 8, v64, 8, 1) == -22, "data too small");
   CHECK(mali_qpool_host_reset(&p, 4, 2) == 0 && e[4].avail == 0 && e[5].v == 0 && e[3].avail == 1, "host reset range");
}

static void t_reset(void)
{
   struct ent e[8]; memset(e, 0x5a, sizeof e);
   struct mali_qpool p = mkpool(e, 8, MALI_QT_TIMESTAMP);
   struct mali_meta_list l = { 0 };
   CHECK(mali_qpool_plan_reset(&l, &p, 2, 3) == 0, "plan reset");
   for (unsigned i = 0; i < l.n; i++) mali_meta_ref_run(&l.j[i]);
   mali_meta_list_free(&l);
   int ok = 1;
   for (int i = 0; i < 8; i++) { int in = i >= 2 && i < 5; ok &= in ? (e[i].avail == 0 && e[i].v == 0 && e[i].pad == 0) : (e[i].avail == 0x5a5a5a5a && e[i].v == 0x5a5a5a5a5a5a5a5aull); }
   CHECK(ok, "reset zeroes exactly the range");
   CHECK(mali_qpool_plan_reset(&l, &p, 6, 3) < 0, "reset range error");
   mali_meta_list_free(&l);
}

static void t_lifecycle(void)
{
   uint64_t sync = 0;
   struct ent e[4]; memset(e, 0, sizeof e);
   struct mali_qpool ts = mkpool(e, 4, MALI_QT_TIMESTAMP);
   struct mali_op t1 = mali_qpool_op_timestamp(&ts, 1), t2 = mali_qpool_op_timestamp(&ts, 2);
   CHECK(run_ops(&t1, 1, 1000, &sync) == 0 && run_ops(&t2, 1, 1600, &sync) == 0, "timestamp frames");
   CHECK(e[1].avail == 1 && e[1].v == 1000 && e[2].avail == 1 && e[2].v == 1600 && e[0].avail == 0 && e[3].avail == 0, "timestamp results");
   CHECK(sync == 2, "frame completion sync incremented once per frame (%lu)", (unsigned long)sync);
   uint64_t out[2] = { 0, 0 };
   CHECK(mali_qpool_get_results(&ts, 1, 2, 32, out, 8, 1 | 2) == 0 && out[1] - out[0] == 600, "timestamp delta");

   /* occlusion: the HW adds the sample count at the value address while the draws run; EndQuery sets availability afterwards */
   struct mali_qpool oq = mkpool(e, 4, MALI_QT_OCCLUSION);
   e[3].avail = 0; e[3].v = 0;
   uint64_t va = mali_qpool_value_va(&oq, 3);
   CHECK(va == (uint64_t)(uintptr_t)&e[3].v && (va & 7) == 0, "occlusion counter address 8-aligned");
   *(uint64_t *)(uintptr_t)va += 1234;   /* draws */
   CHECK(mali_qpool_get_results(&oq, 3, 1, 8, out, 8, 1) == 1, "before End: not ready");
   struct mali_op en = mali_qpool_op_end(&oq, 3);
   CHECK(run_ops(&en, 1, 0, &sync) == 0, "end frame");
   CHECK(mali_qpool_get_results(&oq, 3, 1, 8, out, 8, 1) == 0 && out[0] == 1234, "after End: 1234 samples");

   /* events: wait blocks until set, reset blocks again */
   uint32_t ev = 0;
   struct mali_op w = { MALI_OP_EVENT_WAIT, (uint64_t)(uintptr_t)&ev, 0, 0 };
   struct mali_op set = { MALI_OP_AVAIL, (uint64_t)(uintptr_t)&ev, 0, 1 }, rst = { MALI_OP_AVAIL, (uint64_t)(uintptr_t)&ev, 0, 0 };
   CHECK(run_ops(&w, 1, 0, &sync) == 1, "wait on unset event blocks");
   CHECK(run_ops(&set, 1, 0, &sync) == 0 && ev == 1, "set");
   CHECK(run_ops(&w, 1, 0, &sync) == 0, "wait on set event passes");
   CHECK(run_ops(&rst, 1, 0, &sync) == 0 && ev == 0 && run_ops(&w, 1, 0, &sync) == 1, "reset blocks again");
}

/* WAIT_BIT: the "GPU" retires the query only after a few host polls, as an async submit does */
struct fakegpu { struct ent *e; unsigned polls, done_at; };
static int fake_poll(void *u) { struct fakegpu *g = u; if (++g->polls == g->done_at) { g->e->v = 77; __atomic_store_n(&g->e->avail, 1, __ATOMIC_RELEASE); } return g->polls > 1000 ? MALI_QWAIT_TIMEOUT : 0; }
static int dead_poll(void *u) { (void)u; return MALI_QWAIT_LOST; }

static void t_wait(void)
{
   struct ent e[2] = { { 0, 0, 0 }, { 0, 0, 0 } };
   struct mali_qpool p = mkpool(e, 2, MALI_QT_OCCLUSION);
   uint64_t v = 0;
   struct fakegpu g = { &e[0], 0, 3 };
   CHECK(mali_qpool_get_results_wait(&p, 0, 1, 8, &v, 8, 1, fake_poll, &g) == 1 && g.polls == 0, "no WAIT: NOT_READY without polling");
   CHECK(mali_qpool_get_results_wait(&p, 0, 1, 8, &v, 8, 1 | MALI_QF_WAIT, fake_poll, &g) == 0 && g.polls == 3 && v == 77, "WAIT blocks until available (polls %u v %lu)", g.polls, (unsigned long)v);
   g.polls = 0;
   CHECK(mali_qpool_get_results_wait(&p, 0, 1, 8, &v, 8, 1 | MALI_QF_WAIT, fake_poll, &g) == 0 && g.polls == 0, "already available: no polling");
   CHECK(mali_qpool_get_results_wait(&p, 1, 1, 8, &v, 8, 1 | MALI_QF_WAIT, dead_poll, NULL) == MALI_QWAIT_LOST, "device lost surfaces");
   CHECK(mali_qpool_get_results_wait(&p, 1, 1, 8, &v, 8, 1 | MALI_QF_WAIT | MALI_QF_PARTIAL, dead_poll, NULL) == MALI_QWAIT_LOST, "PARTIAL+WAIT still polls (device lost surfaces)");
   struct fakegpu g2 = { &e[1], 0, 2 };
   v = 0;
   CHECK(mali_qpool_get_results_wait(&p, 1, 1, 8, &v, 8, 1 | MALI_QF_WAIT | MALI_QF_PARTIAL, fake_poll, &g2) == 0 && g2.polls == 2 && v == 77, "PARTIAL+WAIT blocks until available (polls %u v %lu)", g2.polls, (unsigned long)v);
   struct ent e3[1] = { { 0, 0, 0 } };
   struct mali_qpool p3 = mkpool(e3, 1, MALI_QT_OCCLUSION);
   CHECK(mali_qpool_get_results_wait(&p3, 0, 1, 8, &v, 8, 1 | MALI_QF_PARTIAL, dead_poll, NULL) == 0, "PARTIAL without WAIT returns at once");
}

int main(int argc, char **argv)
{
   struct timespec a, b;
   clock_gettime(CLOCK_MONOTONIC, &a);
   t_host();
   t_wait();
   t_reset();
   t_lifecycle();
   CHECK(mali_meta_ref_oob == 0, "oob %d", mali_meta_ref_oob);
   clock_gettime(CLOCK_MONOTONIC, &b);
   double dt = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) * 1e-9;
   printf("t_qpool: %s (%.2fs)\n", g_fail ? "FAIL" : "ok", dt);
   return g_fail != 0 || dt > 5;
}
