/* SPDX-License-Identifier: MIT */
/* M12 offline tests: meta planner + software reference vs analytic expectations, shader compile (v11/v13), descriptor/push bytes.
 * Usage: t_meta [-v]. No GPU, GPU VAs are host pointers. */
#include "../src/mali_meta.h"
#include "../src/mali_compile.h"

#include <math.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern int mali_meta_ref_oob;
static int g_fail, g_verbose;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void *d_alloc(void *u, size_t sz, size_t al, uint64_t *va)
{
   void *p = aligned_alloc(64, (sz + 63) & ~63ull);
   memset(p, 0, sz);
   *va = (uint64_t)(uintptr_t)p;
   return p;
}
static void d_free(void *u, void *cpu, uint64_t va, size_t sz) { free(cpu); }
static int d_up(void *u, const uint8_t *b, uint32_t sz, uint64_t *va) { *va = 0x7f0000001000ull; return 0; }

static struct mali_meta_surf S(void *p, uint64_t size, uint64_t off, uint32_t row, uint32_t slice)
{
   return (struct mali_meta_surf){ (uint64_t)(uintptr_t)p, size, off, row, slice };
}
static void run(struct mali_meta_list *l)
{
   for (unsigned i = 0; i < l->n; i++)
      mali_meta_ref_run(&l->j[i]);
}
static uint8_t *mk(size_t n, int seed)
{
   uint8_t *p = aligned_alloc(64, (n + 63 + 4096) & ~63ull);
   for (size_t i = 0; i < n + 4096 - 4096 + 0; i++) p[i] = (uint8_t)(i * 31 + seed);
   return p;
}

static void t_compile(unsigned arch)
{
   struct mali_meta_dev dev = { NULL, arch, d_alloc, d_free, d_up };
   mali_meta_set_dev(&dev);
   for (int s = 0; s < MALI_META_BC_FIRST; s++) {
      const struct mali_compiled *c = NULL;
      uint64_t va = 0;
      int r = mali_meta_shader_get(s, &c, &va);
      CHECK(r == 0, "arch %u shader %d compile r=%d", arch, s, r);
      if (!r) {
         CHECK(c->bin_size > 0 && c->stage == MALI_STAGE_COMPUTE, "arch %u shader %d bin", arch, s);
         if (g_verbose) printf("  arch %u shader %d: %u B, fau %u, push %u\n", arch, s, c->bin_size, c->fau_count, c->push_const_bytes);
         static const unsigned pmax[] = { 64, 128, 20 };
         CHECK(c->push_const_bytes <= pmax[s], "shader %d push use %u > %u", s, c->push_const_bytes, pmax[s]);
      }
   }
}

static void t_materialize(void)
{
   struct mali_meta_dev dev = { NULL, 11, d_alloc, d_free, d_up };
   mali_meta_set_dev(&dev);
   uint8_t *a = mk(256, 1), *b = mk(256, 2);
   struct mali_meta_list l = { 0 };
   struct mali_meta_surf d = S(b, 256, 8, 0, 0), s = S(a, 256, 4, 0, 0);
   CHECK(mali_meta_plan_buffer_copy(&l, &d, &s, 100) == 0 && l.n >= 1, "plan");
   struct mali_meta_mem mem = { 0 };
   struct mali_dispatch disp;
   CHECK(mali_meta_materialize(&l.j[0], &mem, &disp) == 0, "materialize");
   CHECK(disp.nsets == 1 && disp.set_bytes[0] == 64, "set bytes %u", disp.set_bytes[0]);
   const uint32_t *set = (const uint32_t *)(uintptr_t)disp.set_va[0];
   CHECK((set[0] == (9u | 16u)) && set[1] == l.j[0].bind[0].size, "desc0 w0 %x w1 %u", set[0], set[1]);
   uint64_t va = ((uint64_t)set[3] << 32) | set[2];
   CHECK(va == l.j[0].bind[0].va && (va & 3) == 0, "desc0 va");
   CHECK(set[8] == (9u | 16u) && (((uint64_t)set[11] << 32) | set[10]) == l.j[0].bind[1].va, "desc1");
   CHECK(!memcmp(disp.push, l.j[0].push, l.j[0].push_bytes), "push");
   CHECK(disp.groups[0] >= 1, "groups");
   mali_meta_mem_release(&mem);
   mali_meta_list_free(&l);
   free(a); free(b);
}

static void t_buffer_copy(void)
{
   static const struct { uint64_t so, doff, n; } cs[] = { { 0, 0, 256 }, { 4, 8, 100 }, { 1, 3, 77 }, { 3, 0, 5 }, { 7, 1, 1 }, { 0, 4096 + 2, 129 } };
   for (unsigned i = 0; i < sizeof cs / sizeof *cs; i++) {
      uint8_t *a = mk(8192, 1), *b = mk(8192, 99), *e = malloc(8192);
      memcpy(e, b, 8192);
      memcpy(e + cs[i].doff, a + cs[i].so, cs[i].n);
      struct mali_meta_list l = { 0 };
      struct mali_meta_surf d = S(b, 8192, cs[i].doff, 0, 0), s = S(a, 8192, cs[i].so, 0, 0);
      CHECK(mali_meta_plan_buffer_copy(&l, &d, &s, cs[i].n) == 0, "plan %u", i);
      run(&l);
      CHECK(!memcmp(b, e, 8192), "buffer copy case %u (so %lu do %lu n %lu) differs", i, (unsigned long)cs[i].so, (unsigned long)cs[i].doff, (unsigned long)cs[i].n);
      mali_meta_list_free(&l);
      free(a); free(b); free(e);
   }
}

static void t_fill_update(void)
{
   uint8_t *b = mk(4096, 5), *e = malloc(4096);
   memcpy(e, b, 4096);
   for (int i = 0; i < 64; i++) memcpy(e + 16 + i * 4, &(uint32_t){ 0xdeadbeef }, 4);
   struct mali_meta_list l = { 0 };
   struct mali_meta_surf d = S(b, 4096, 16, 0, 0);
   CHECK(mali_meta_plan_fill(&l, &d, 256, 0xdeadbeef) == 0, "fill plan");
   run(&l);
   CHECK(!memcmp(b, e, 4096), "fill");
   mali_meta_list_free(&l);

   uint8_t data[200];
   for (int i = 0; i < 200; i++) data[i] = (uint8_t)(i * 7 + 3);
   memcpy(e + 3800, data, 200);
   d = S(b, 4096, 3800, 0, 0);
   CHECK(mali_meta_plan_update(&l, &d, 200, data) == 0, "update plan");
   data[0] ^= 0xff;   /* planner must have copied it */
   run(&l);
   CHECK(!memcmp(b, e, 4096), "update");
   mali_meta_list_free(&l);
   CHECK(mali_meta_plan_update(&l, &d, 65540, data) != 0, "update > 64k rejected");
   mali_meta_list_free(&l);
   free(b); free(e);
}

/* image rect copy: 2D region with pitches, 3D slices, mixed bpt */
static void t_image_copy(void)
{
   static const uint32_t bpts[] = { 1, 2, 4, 8, 16 };
   for (unsigned k = 0; k < 5; k++) {
      uint32_t bpt = bpts[k], ex = 7, ey = 5, ez = 3;
      uint32_t srow = 16 * bpt + 8, ssl = srow * 9, drow = 12 * bpt, dsl = drow * 8;
      size_t sn = ssl * 4, dn = dsl * 4;
      uint8_t *a = mk(sn, 3), *b = mk(dn, 77), *e = malloc(dn);
      memcpy(e, b, dn);
      uint64_t so = 2 * bpt + 1 * srow + 12, doff = 3 * bpt + 1 * drow;
      so = (so / bpt) * bpt;   /* texel aligned like a real image */
      for (uint32_t z = 0; z < ez; z++) for (uint32_t y = 0; y < ey; y++)
         memcpy(e + doff + z * dsl + y * drow, a + so + z * ssl + y * srow, ex * bpt);
      struct mali_meta_list l = { 0 };
      struct mali_meta_surf d = S(b, dn, doff, drow, dsl), s = S(a, sn, so, srow, ssl);
      CHECK(mali_meta_plan_copy(&l, &d, &s, bpt, ex, ey, ez) == 0, "plan bpt %u", bpt);
      run(&l);
      CHECK(!memcmp(b, e, dn), "image copy bpt %u differs", bpt);
      mali_meta_list_free(&l);
      free(a); free(b); free(e);
   }
}

static void t_const(void)
{
   /* clear colour rect of 4 bpt, 8 bpt (words replicate), 3D */
   for (unsigned bpt = 4; bpt <= 16; bpt *= 2) {
      uint32_t ex = 5, ey = 4, ez = 2, row = 8 * bpt, sl = row * 6;
      uint8_t *b = mk(sl * 2, 9), *e = malloc(sl * 2);
      memcpy(e, b, sl * 2);
      uint32_t c[4] = { 0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00 };
      for (uint32_t z = 0; z < ez; z++) for (uint32_t y = 0; y < ey; y++) for (uint32_t x = 0; x < ex; x++)
         for (uint32_t w = 0; w < bpt / 4; w++)
            memcpy(e + z * sl + y * row + (x + 1) * bpt + w * 4, &c[w], 4);
      struct mali_meta_list l = { 0 };
      struct mali_meta_surf d = S(b, sl * 2, bpt, row, sl);
      CHECK(mali_meta_plan_const(&l, &d, bpt, ex, ey, ez, c) == 0, "const plan");
      run(&l);
      CHECK(!memcmp(b, e, sl * 2), "const bpt %u", bpt);
      mali_meta_list_free(&l);
      free(b); free(e);
   }
   /* 1 and 2 bpt (byte path) */
   for (unsigned bpt = 1; bpt <= 2; bpt++) {
      uint8_t *b = mk(256, 4), *e = malloc(256);
      memcpy(e, b, 256);
      uint32_t c[4] = { 0xabcd, 0, 0, 0 };
      for (int y = 0; y < 3; y++) for (int x = 0; x < 6; x++) for (unsigned k = 0; k < bpt; k++)
         e[2 * bpt + y * 40 + x * bpt + k] = (c[0] >> (8 * k)) & 0xff;
      struct mali_meta_list l = { 0 };
      struct mali_meta_surf d = S(b, 256, 2 * bpt, 40, 0);
      CHECK(mali_meta_plan_const(&l, &d, bpt, 6, 3, 1, c) == 0, "const plan small");
      run(&l);
      CHECK(!memcmp(b, e, 256), "const bpt %u byte path", bpt);
      mali_meta_list_free(&l);
      free(b); free(e);
   }
}

static void mkblit(struct mali_meta_blit *bl, uint8_t *src, uint32_t sw, uint32_t sh, uint8_t *dst, uint32_t dw, uint32_t dh, unsigned n, bool f32)
{
   uint32_t bpp = f32 ? 4 * n : n;
   memset(bl, 0, sizeof *bl);
   bl->src = S(src, sw * sh * bpp, 0, sw * bpp, 0);
   bl->dst = S(dst, dw * dh * bpp, 0, dw * bpp, 0);
   bl->src_w = sw; bl->src_h = sh;
   bl->ncomp = n; bl->f32 = f32;
   bl->s[0] = 0; bl->s[1] = 0; bl->s[2] = sw; bl->s[3] = sh;
   bl->d[0] = 0; bl->d[1] = 0; bl->d[2] = dw; bl->d[3] = dh;
   bl->clamp[0] = 0; bl->clamp[1] = 0; bl->clamp[2] = dw; bl->clamp[3] = dh;
}

static void t_blit(void)
{
   /* identity 4 comps */
   uint8_t *a = mk(8 * 8 * 4, 1), *b = calloc(1, 8 * 8 * 4);
   struct mali_meta_blit bl;
   struct mali_meta_list l = { 0 };
   mkblit(&bl, a, 8, 8, b, 8, 8, 4, false);
   CHECK(mali_meta_plan_blit(&l, &bl) == 0, "blit plan");
   run(&l);
   CHECK(!memcmp(a, b, 256), "identity nearest blit");
   mali_meta_list_free(&l);

   /* 2x nearest upscale of a 4x4 R8 */
   uint8_t s4[16], d8[64], e8[64];
   for (int i = 0; i < 16; i++) s4[i] = (uint8_t)(i * 16 + 1);
   for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) e8[y * 8 + x] = s4[(y / 2) * 4 + x / 2];
   mkblit(&bl, s4, 4, 4, d8, 8, 8, 1, false);
   CHECK(mali_meta_plan_blit(&l, &bl) == 0, "plan 2x");
   run(&l);
   CHECK(!memcmp(d8, e8, 64), "2x nearest");
   mali_meta_list_free(&l);

   /* mirror x: d x1 < x0 */
   uint8_t m[16];
   mkblit(&bl, s4, 4, 4, m, 4, 4, 1, false);
   bl.d[0] = 4; bl.d[2] = 0;
   CHECK(mali_meta_plan_blit(&l, &bl) == 0, "plan mirror");
   run(&l);
   int ok = 1;
   for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) ok &= m[y * 4 + x] == s4[y * 4 + 3 - x];
   CHECK(ok, "mirror x");
   mali_meta_list_free(&l);

   /* linear 2x down of constant-gradient f32: average of 2x2 */
   float f4[16], fd[4];
   for (int i = 0; i < 16; i++) f4[i] = (float)i;
   mkblit(&bl, (uint8_t *)f4, 4, 4, (uint8_t *)fd, 2, 2, 1, true);
   bl.mode = 1;
   CHECK(mali_meta_plan_blit(&l, &bl) == 0, "plan linear");
   run(&l);
   float ef[4] = { 2.5f, 4.5f, 10.5f, 12.5f };
   for (int i = 0; i < 4; i++) CHECK(fd[i] > ef[i] - 1e-4 && fd[i] < ef[i] + 1e-4, "linear f32 %d: %f vs %f", i, fd[i], ef[i]);
   mali_meta_list_free(&l);

   /* trilinear: two f32 slices (1x1 each, values 10 and 20) 16 bytes... slice stride 4 B; weight 0.25 -> 12.5, weight 0 -> bilinear of slice 0 */
   float zs2[2] = { 10.0f, 20.0f }, zd1[1];
   for (int k = 0; k < 2; k++) {
      mkblit(&bl, (uint8_t *)zs2, 1, 1, (uint8_t *)zd1, 1, 1, 1, true);
      bl.mode = 1;
      bl.zw = k ? 0.25f : 0.0f;
      bl.sample_stride = k ? 4 : 0;
      bl.src.size = 8;
      CHECK(mali_meta_plan_blit(&l, &bl) == 0, "plan trilinear");
      run(&l);
      CHECK(fabsf(zd1[0] - (k ? 12.5f : 10.0f)) < 1e-4f, "trilinear %d: %f", k, zd1[0]);
      mali_meta_list_free(&l);
   }

   /* clamped (scissored) region only touches the rect */
   uint8_t z[64];
   memset(z, 0xee, 64);
   mkblit(&bl, s4, 4, 4, z, 8, 8, 1, false);
   bl.clamp[0] = 2; bl.clamp[1] = 3; bl.clamp[2] = 6; bl.clamp[3] = 5;
   CHECK(mali_meta_plan_blit(&l, &bl) == 0, "plan clamp");
   run(&l);
   ok = 1;
   for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
      int in = x >= 2 && x < 6 && y >= 3 && y < 5;
      ok &= in ? z[y * 8 + x] == s4[(y / 2) * 4 + x / 2] : z[y * 8 + x] == 0xee;
   }
   CHECK(ok, "clamp rect");
   mali_meta_list_free(&l);

   /* BGRA swap */
   uint8_t px[4] = { 10, 20, 30, 40 }, out[4];
   mkblit(&bl, px, 1, 1, out, 1, 1, 4, false);
   bl.swap_rb = true;
   CHECK(mali_meta_plan_blit(&l, &bl) == 0, "plan swap");
   run(&l);
   CHECK(out[0] == 30 && out[1] == 20 && out[2] == 10 && out[3] == 40, "swap rb %d %d %d %d", out[0], out[1], out[2], out[3]);
   mali_meta_list_free(&l);
   free(a); free(b);

   /* MSAA resolve (4 samples, 4x4 RGBA8, sample surfaces 64 B apart) */
   uint8_t ms[4 * 64], rs[64], er[64];
   for (int s = 0; s < 4; s++) for (int i = 0; i < 16; i++) { ms[s * 64 + i * 4] = (uint8_t)(i * 8 + s * 4); ms[s * 64 + i * 4 + 1] = 100; ms[s * 64 + i * 4 + 2] = 0; ms[s * 64 + i * 4 + 3] = 255; }
   mkblit(&bl, ms, 4, 4, rs, 4, 4, 4, false);
   bl.src.size = sizeof ms;
   bl.mode = 2; bl.nsamples = 4; bl.sample_stride = 64;
   CHECK(mali_meta_plan_blit(&l, &bl) == 0, "plan resolve");
   run(&l);
   ok = 1;
   for (int i = 0; i < 16; i++) {
      int avg = (int)((i * 8 * 4 + 4 * (0 + 1 + 2 + 3)) / 4.0f / 255.0f * 255.0f + 0.5f);
      ok &= rs[i * 4] == avg && rs[i * 4 + 1] == 100 && rs[i * 4 + 3] == 255;
   }
   (void)er;
   CHECK(ok, "resolve average");
   mali_meta_list_free(&l);
}

static void t_qcopy(void)
{
   struct { uint32_t avail, pad; uint64_t v; } pool[6] = { { 1, 0, 5 }, { 0, 0, 7 }, { 1, 0, 0x100000009ull }, { 1, 0, 11 }, { 1, 0, 12 }, { 0, 0, 0 } };
   for (unsigned fl = 0; fl < 16; fl++) {
      uint8_t dst[256], exp[256];
      memset(dst, 0xcc, 256);
      memset(exp, 0xcc, 256);
      bool is64 = fl & 1, wait = fl & 2, avl = fl & 4, part = fl & 8;
      uint32_t first = 1, count = 4, stride = 24, doff = 8;
      for (uint32_t i = 0; i < count; i++) {
         uint8_t *p = exp + doff + i * stride;
         uint32_t av = pool[first + i].avail;
         if (av || part) {
            if (is64) memcpy(p, &pool[first + i].v, 8);
            else { uint32_t v = (pool[first + i].v >> 32) ? 0xffffffffu : (uint32_t)pool[first + i].v; memcpy(p, &v, 4); }
         }
         if (avl) { uint8_t *q = p + (is64 ? 8 : 4); uint32_t v = av ? 1 : 0; memcpy(q, &v, 4); if (is64) memset(q + 4, 0, 4); }
      }
      (void)wait;
      struct mali_meta_list l = { 0 };
      struct mali_meta_surf ps = S(pool, sizeof pool, 0, 0, 0), ds = S(dst, 256, 0, 0, 0);
      CHECK(mali_meta_plan_qcopy(&l, &ps, &ds, first, count, doff, stride, fl) == 0, "qcopy plan %u", fl);
      run(&l);
      /* the planner may require the WAIT bit to be passed through, only compare written bytes */
      CHECK(!memcmp(dst, exp, 256), "qcopy flags %u differ", fl);
      mali_meta_list_free(&l);
   }
}

/* ---- threaded device seam: two devices swapped while another thread materialises jobs (CR #223 group) ----
 * The invariant is the one those findings are about: a dispatch may come from either device, but the executable VA
 * must be the one *that* device uploaded - never arch 11's compiled shader paired with arch 13's VA. A watchdog
 * (timed join) turns a hang into a failure. */
#define SVA11 0x7f0000001000ull
#define SVA13 0x7f0000002000ull
struct sdev { struct mali_meta_dev dev; uint64_t va; };
static int s_up(void *u, const uint8_t *b, uint32_t sz, uint64_t *va) { (void)b; (void)sz; *va = ((struct sdev *)u)->va; return 0; }

struct sctx { struct sdev d[2]; unsigned swaps, jobs; volatile int bad; };

/* Watchdog: the workers must finish well inside this; a hang is a failure (exit 99), not a stuck check.sh. */
static volatile int *g_wd_done;
static void *watch_dog(void *a)
{
   unsigned sec = *(unsigned *)a;
   for (unsigned i = 0; i < sec * 10; i++) {   /* poll so a finished stress does not wait out the watchdog */
      if (*g_wd_done)
         return NULL;
      struct timespec ts = { 0, 100 * 1000 * 1000 };
      nanosleep(&ts, NULL);
   }
   fprintf(stderr, "t_meta: stress did not finish in %u s (watchdog)\n", sec);
   _exit(99);
}

static void *swap_thr(void *a)
{
   struct sctx *c = a;
   for (unsigned i = 0; i < c->swaps; i++)
      mali_meta_set_dev(&c->d[i & 1].dev);
   return NULL;
}

static void *job_thr(void *a)
{
   struct sctx *c = a;
   uint8_t *src = mk(256, 3), *dst = mk(256, 4);
   unsigned done = 0;
   for (unsigned i = 0; i < c->jobs && !c->bad; i++) {
      struct mali_meta_list l = { 0 };
      struct mali_meta_surf d = S(dst, 256, 8, 0, 0), s = S(src, 256, 4, 0, 0);
      if (mali_meta_plan_buffer_copy(&l, &d, &s, 100) != 0) { c->bad = 1; break; }
      struct mali_meta_mem mem = { 0 };
      struct mali_dispatch disp;
      int r = mali_meta_materialize(&l.j[0], &mem, &disp);
      if (r == 0) {
         done++;
         /* the shader and the VA must belong to the same device */
         if (!disp.cs || disp.cs_va != (disp.cs->arch == 13 ? SVA13 : SVA11))
            c->bad = 2;
      } else if (r != -ENODEV) {
         c->bad = 3;
      }
      mali_meta_mem_release(&mem);
   }
   free(src);
   free(dst);
   if (!done && !c->bad)
      c->bad = 4;
   return NULL;
}

static void
t_stress(void)
{
   struct sctx c = { .swaps = 200, .jobs = 200 };
   c.d[0] = (struct sdev){ .dev = { &c.d[0], 11, d_alloc, d_free, s_up }, .va = SVA11 };
   c.d[1] = (struct sdev){ .dev = { &c.d[1], 13, d_alloc, d_free, s_up }, .va = SVA13 };
   mali_meta_set_dev(&c.d[0].dev);
   static volatile int done;
   g_wd_done = &done;
   unsigned wait_s = 60;
   pthread_t ts, tj, tw;
   done = 0;
   CHECK(pthread_create(&ts, NULL, swap_thr, &c) == 0, "stress: swap thread");
   CHECK(pthread_create(&tj, NULL, job_thr, &c) == 0, "stress: job thread");
   CHECK(pthread_create(&tw, NULL, watch_dog, &wait_s) == 0, "stress: watchdog");
   pthread_join(ts, NULL);
   pthread_join(tj, NULL);
   done = 1;
   pthread_join(tw, NULL);
   CHECK(c.bad == 0, "stress: every dispatch paired its shader with its own device (bad=%d)", c.bad);
   printf("  stress: %u swaps, %u jobs, no mixed device\n", c.swaps, c.jobs);
}

int main(int argc, char **argv)
{
   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);
   g_verbose = argc > 1 && !strcmp(argv[1], "-v");
   t_compile(11);
   t_compile(13);
   t_materialize();
   t_buffer_copy();
   t_fill_update();
   t_image_copy();
   t_const();
   t_blit();
   t_qcopy();
   if (!getenv("MALI_META_NO_STRESS"))
      t_stress();
   CHECK(mali_meta_ref_oob == 0, "reference ran %d out-of-bounds / short-grid accesses", mali_meta_ref_oob);
   mali_meta_set_dev(NULL);
   clock_gettime(CLOCK_MONOTONIC, &t1);
   double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
   printf("t_meta: %s (%.2fs)\n", g_fail ? "FAIL" : "ok", dt);
   return g_fail ? 1 : 0;
}
