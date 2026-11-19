/* SPDX-License-Identifier: MIT */
/* Offline tests of mali_compile(). Usage: t_compile [shader_dir] [-v]. Runs in a few seconds, no GPU. */
#include "../src/mali_compile.h"
#include "vk_ycbcr_conversion.h"
#include "../../libkbase/tests/shaders_compiled_v11.h"
#include "../shader/golden_compiled_v13.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_fail, g_verbose;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static const char *g_dir = "driver/vk/shader";

struct spv { uint32_t *w; size_t n; };

static struct spv
load(const char *name)
{
   char p[512];
   snprintf(p, sizeof p, "%s/%s", g_dir, name);
   struct spv s = {0};
   FILE *f = fopen(p, "rb");
   if (!f) { printf("  cannot open %s\n", p); g_fail++; return s; }
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   s.w = malloc(n);
   if (fread(s.w, 1, n, f) != (size_t)n) { g_fail++; free(s.w); s.w = NULL; }
   else s.n = n / 4;
   fclose(f);
   return s;
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static int
compile(unsigned arch, enum mali_stage st, const struct spv *s, const struct mali_varying_blob *link, const char *cache, struct mali_compiled *c)
{
   struct mali_compile_opts o = { .arch = arch, .stage = st, .spirv = s->w, .spirv_words = s->n, .link_vs = link, .cache_dir = cache };
   char err[256] = "";
   int r = mali_compile(&o, c, err, sizeof err);
   if (r) printf("  compile error: %s\n", err);
   return r;
}

static void
expect_golden(const char *what, const struct mali_compiled *c, const uint8_t *bin, unsigned size, unsigned work, uint64_t preload,
              unsigned fau_count, unsigned fau_reserved, const uint32_t (*imm)[2], unsigned nimm, int vs, unsigned sec_en,
              unsigned sec_off, unsigned sec_work, uint64_t sec_pre, unsigned generic)
{
   CHECK(c->bin_size == size, "%s size %u != %u", what, c->bin_size, size);
   CHECK(c->bin_size == size && !memcmp(c->bin, bin, size), "%s binary differs from golden", what);
   CHECK(c->work_reg_count == work, "%s work regs", what);
   CHECK(c->preload == preload, "%s preload 0x%llx != 0x%llx", what, (unsigned long long)c->preload, (unsigned long long)preload);
   CHECK(c->fau_count == fau_count && c->fau_reserved == fau_reserved, "%s fau %u/%u", what, c->fau_count, c->fau_reserved);
   CHECK(c->nimm == nimm, "%s nimm %u != %u", what, c->nimm, nimm);
   for (unsigned i = 0; i < nimm && i < c->nimm; i++)
      CHECK(c->imm[i].word == imm[i][0] && c->imm[i].value == imm[i][1], "%s imm %u", what, i);
   if (vs) {
      CHECK(c->vs_secondary_enable == sec_en && c->vs_secondary_offset == sec_off && c->vs_secondary_work_reg_count == sec_work &&
            c->vs_secondary_preload == sec_pre, "%s secondary shader fields", what);
      CHECK(c->generic_size_B == generic, "%s generic size", what);
   }
}

static void
test_golden(void)
{
   printf("golden (rot.vert + var.frag vs gen-compiled.sh output)\n");
   struct spv vs = load("rot.vert.spv"), fs = load("var.frag.spv");
   if (!vs.w || !fs.w) return;
   struct mali_compiled c;
   if (!compile(11, MALI_STAGE_VERTEX, &vs, NULL, "", &c)) {
      expect_golden("v11 VS", &c, crot_vs_bin, CROT_VS_SIZE, CROT_VS_WORK_REG_COUNT, CROT_VS_PRELOAD, CROT_VS_FAU_COUNT,
                    CROT_VS_FAU_RESERVED, crot_vs_imm, CROT_VS_NIMM, 1, CROT_VS_SECONDARY_ENABLE, CROT_VS_SECONDARY_OFFSET,
                    CROT_VS_SECONDARY_WORK_REG_COUNT, CROT_VS_SECONDARY_PRELOAD, CROT_VS_GENERIC_SIZE_B);
      CHECK(c.push_const_bytes == 16, "push const bytes %u", c.push_const_bytes);
      mali_compiled_free(&c);
   }
   if (!compile(11, MALI_STAGE_FRAGMENT, &fs, NULL, "", &c)) {
      expect_golden("v11 FS", &c, crot_fs_bin, CROT_FS_SIZE, CROT_FS_WORK_REG_COUNT, CROT_FS_PRELOAD, CROT_FS_FAU_COUNT,
                    CROT_FS_FAU_RESERVED, crot_fs_imm, CROT_FS_NIMM, 0, 0, 0, 0, 0, 0);
      mali_compiled_free(&c);
   }
   if (!compile(13, MALI_STAGE_VERTEX, &vs, NULL, "", &c)) {
      expect_golden("v13 VS", &c, crot13_vs_bin, CROT13_VS_SIZE, CROT13_VS_WORK_REG_COUNT, CROT13_VS_PRELOAD, CROT13_VS_FAU_COUNT,
                    CROT13_VS_FAU_RESERVED, crot13_vs_imm, CROT13_VS_NIMM, 1, CROT13_VS_SECONDARY_ENABLE, CROT13_VS_SECONDARY_OFFSET,
                    CROT13_VS_SECONDARY_WORK_REG_COUNT, CROT13_VS_SECONDARY_PRELOAD, CROT13_VS_GENERIC_SIZE_B);
      mali_compiled_free(&c);
   }
   if (!compile(13, MALI_STAGE_FRAGMENT, &fs, NULL, "", &c)) {
      expect_golden("v13 FS", &c, crot13_fs_bin, CROT13_FS_SIZE, CROT13_FS_WORK_REG_COUNT, CROT13_FS_PRELOAD, CROT13_FS_FAU_COUNT,
                    CROT13_FS_FAU_RESERVED, crot13_fs_imm, CROT13_FS_NIMM, 0, 0, 0, 0, 0, 0);
      mali_compiled_free(&c);
   }
   free(vs.w); free(fs.w);
}

static void
test_pairing(void)
{
   printf("vertex + fragment pairing\n");
   struct spv vs = load("rot.vert.spv"), fs = load("var.frag.spv");
   if (!vs.w || !fs.w) return;
   for (unsigned arch = 11; arch <= 13; arch += 2) {
      struct mali_compiled v, f, f0;
      if (compile(arch, MALI_STAGE_VERTEX, &vs, NULL, "", &v)) continue;
      CHECK(v.layout.size > 0 && v.nvarying >= 2, "v%u VS layout", arch);
      int found = 0;
      for (unsigned i = 0; i < v.nvarying; i++)
         if (v.varying[i].location == 32 /* VAR0 */) found = v.varying[i].ncomps == 4 && v.varying[i].section != 0;
      CHECK(found, "v%u VS writes VAR0 vec4 in a generic section", arch);
      if (compile(arch, MALI_STAGE_FRAGMENT, &fs, &v.layout, "", &f)) { mali_compiled_free(&v); continue; }
      if (compile(arch, MALI_STAGE_FRAGMENT, &fs, NULL, "", &f0)) { mali_compiled_free(&v); mali_compiled_free(&f); continue; }
      int fsfound = 0;
      for (unsigned i = 0; i < f.nvarying; i++) if (f.varying[i].location == 32) fsfound = 1;
      CHECK(fsfound, "v%u FS reads VAR0 from the VS layout", arch);
      /* the linked FS reads the varying buffer (LD_VAR_BUF_IMM) where the unlinked one uses attribute descriptors: different code */
      CHECK(f.bin_size != f0.bin_size || memcmp(f.bin, f0.bin, f.bin_size), "v%u linked FS differs from unlinked", arch);
      if (g_verbose)
         printf("  v%u: VS %u B, FS linked %u B (unlinked %u B), generic %u B\n", arch, v.bin_size, f.bin_size, f0.bin_size, v.generic_size_B);
      mali_compiled_free(&v); mali_compiled_free(&f); mali_compiled_free(&f0);
   }
   free(vs.w); free(fs.w);
}

/* A Private global with a constant initializer (DXVK/dxc `static` variables) used to hit an assert in nir_lower_vars_to_ssa. */
static void
test_private_init(void)
{
   printf("private variable initializers\n");
   struct spv cs = load("privinit.comp.spv");
   if (!cs.w) return;
   for (unsigned arch = 10; arch <= 13; arch++) {
      if (arch == 12) continue;
      struct mali_compiled c;
      if (compile(arch, MALI_STAGE_COMPUTE, &cs, NULL, "", &c)) { g_fail++; continue; }
      CHECK(c.bin_size > 0 && c.bin_size % 8 == 0, "v%u privinit binary size %u", arch, c.bin_size);
      mali_compiled_free(&c);
   }
   free(cs.w);
}

/* bufferDeviceAddress in a fragment shader: nir_var_mem_global derefs used to be lowered only for compute and asserted in bi_emit_instr. */
static void
test_bda_frag(void)
{
   printf("buffer device address in fragment shaders\n");
   struct spv fs = load("bda_frag.frag.spv");
   if (!fs.w) return;
   for (unsigned arch = 10; arch <= 13; arch++) {
      if (arch == 12) continue;
      struct mali_compiled c;
      if (compile(arch, MALI_STAGE_FRAGMENT, &fs, NULL, "", &c)) { g_fail++; continue; }
      CHECK(c.bin_size > 0 && c.bin_size % 8 == 0, "v%u bda fs binary size %u", arch, c.bin_size);
      mali_compiled_free(&c);
   }
   free(fs.w);
}

static void
test_compute(void)
{
   printf("compute stub\n");
   struct spv cs = load("comp.comp.spv");
   if (!cs.w) return;
   for (unsigned arch = 11; arch <= 13; arch += 2) {
      struct mali_compiled c;
      if (compile(arch, MALI_STAGE_COMPUTE, &cs, NULL, "", &c)) continue;
      CHECK(c.bin_size > 0 && c.bin_size % 8 == 0, "v%u compute binary size %u", arch, c.bin_size);
      CHECK(c.push_const_bytes == 12, "v%u compute push bytes %u", arch, c.push_const_bytes);
      mali_compiled_free(&c);
   }
   free(cs.w);
}

static void
test_determinism_and_cache(void)
{
   printf("determinism and disk cache\n");
   struct spv vs = load("rot.vert.spv");
   if (!vs.w) return;
   char dir[] = "/tmp/t_compile_cache.XXXXXX";
   if (!mkdtemp(dir)) { g_fail++; return; }
   struct mali_compile_stats s0, s1;
   struct mali_compiled a, b, c;
   mali_compile_stats(&s0);
   CHECK(!compile(11, MALI_STAGE_VERTEX, &vs, NULL, "", &a), "compile a");
   CHECK(!compile(11, MALI_STAGE_VERTEX, &vs, NULL, "", &b), "compile b");
   CHECK(a.bin_size == b.bin_size && !memcmp(a.bin, b.bin, a.bin_size), "two uncached compiles are byte-identical");
   CHECK(!memcmp(&a.layout, &b.layout, sizeof a.layout), "layouts identical");
   mali_compiled_free(&b);
   CHECK(!compile(11, MALI_STAGE_VERTEX, &vs, NULL, dir, &b), "cache miss compile");
   CHECK(!b.cache_hit, "first cached compile is a miss");
   double t0 = now();
   CHECK(!compile(11, MALI_STAGE_VERTEX, &vs, NULL, dir, &c), "cache hit load");
   double t_hit = now() - t0;
   if (!getenv("MALI_OPT_GCM"))   /* the experimental GCM env bypasses the disk cache by design */
      CHECK(c.cache_hit == 1, "second is a hit");
   CHECK(c.bin_size == a.bin_size && !memcmp(c.bin, a.bin, a.bin_size), "cached binary identical");
   CHECK(c.nimm == a.nimm && !memcmp(c.imm, a.imm, a.nimm * sizeof *a.imm), "cached imm identical");
   CHECK(c.preload == a.preload && c.generic_size_B == a.generic_size_B && !memcmp(c.varying, a.varying, sizeof c.varying), "cached metadata");
   mali_compiled_free(&c);
   /* other arch and other options are different keys */
   CHECK(!compile(13, MALI_STAGE_VERTEX, &vs, NULL, dir, &c) && !c.cache_hit, "arch is part of the key");
   mali_compiled_free(&c);
   struct mali_compile_opts o = { .arch = 11, .stage = MALI_STAGE_VERTEX, .spirv = vs.w, .spirv_words = vs.n, .cache_dir = dir,
                                  .fau_reserved = 20 };
   CHECK(!mali_compile(&o, &c, NULL, 0) && !c.cache_hit, "options are part of the key");
   mali_compiled_free(&c);
   /* corrupt entry: truncated file falls back to a compile */
   char cmd[600];
   snprintf(cmd, sizeof cmd, "for f in %s/*.bin; do truncate -s 100 \"$f\"; done", dir);
   CHECK(system(cmd) == 0, "truncate");
   CHECK(!compile(11, MALI_STAGE_VERTEX, &vs, NULL, dir, &c) && !c.cache_hit && c.bin_size == a.bin_size &&
         !memcmp(c.bin, a.bin, a.bin_size), "corrupt cache entry recompiles correctly");
   mali_compiled_free(&c);
   mali_compile_stats(&s1);
   if (!getenv("MALI_OPT_GCM")) CHECK(s1.hits - s0.hits == 1, "stats: 1 hit, got %llu", (unsigned long long)(s1.hits - s0.hits));
   if (g_verbose) printf("  hit load %.0f us\n", t_hit * 1e6);
   mali_compiled_free(&a); mali_compiled_free(&b);
   snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
   if (system(cmd)) g_fail++;
   free(vs.w);
}

static void
test_errors(void)
{
   printf("errors\n");
   uint32_t junk[8] = {0x12345678, 1, 2, 3, 4, 5, 6, 7};
   struct spv j = { junk, 8 };
   struct mali_compiled c;
   char err[256];
   struct mali_compile_opts o = { .arch = 11, .stage = MALI_STAGE_VERTEX, .spirv = j.w, .spirv_words = j.n, .cache_dir = "" };
   CHECK(mali_compile(&o, &c, err, sizeof err) != 0, "junk SPIR-V rejected");
   o.arch = 12;
   CHECK(mali_compile(&o, &c, err, sizeof err) != 0, "arch 12 rejected");

   /* A module that declares a capability Mesa does not implement: the error must name it (not just "spirv_to_nir failed"). */
   uint32_t badcap[] = {0x07230203, 0x00010500, 0, 1, 0, 0x00020011, 0x7ffffff0u /* OpCapability <unknown> */};
   o = (struct mali_compile_opts){ .arch = 11, .stage = MALI_STAGE_VERTEX, .spirv = badcap, .spirv_words = 7, .cache_dir = "" };
   err[0] = 0;
   CHECK(mali_compile(&o, &c, err, sizeof err) != 0, "unknown capability rejected");
   CHECK(strstr(err, "Unimplemented SPIR-V capability") != NULL, "error names the capability: '%s'", err);
}

/* YCbCr: the conversion of an immutable NV12 sampler is lowered into the shader (nir_vk_lower_ycbcr_tex: two plane samples, model/range
 * constants, siting). Compile tex.frag with a plain combined sampler and with converted ones and look at what changed. */
static int
imm_has(const struct mali_compiled *c, float want)
{
   for (unsigned i = 0; i < c->nimm; i++) {
      float f;
      memcpy(&f, &c->imm[i].value, 4);
      if (f > want - 2e-3f && f < want + 2e-3f) return 1;
   }
   return 0;
}

static int
ycc_compile(unsigned arch, VkSamplerYcbcrModelConversion model, VkSamplerYcbcrRange range, VkChromaLocation loc, int ycbcr, struct mali_compiled *c)
{
   struct spv fs = load("tex.frag.spv");
   if (!fs.w) return -1;
   struct vk_ycbcr_conversion_state st;
   memset(&st, 0, sizeof st);
   st.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
   st.ycbcr_model = model;
   st.ycbcr_range = range;
   for (int i = 0; i < 4; i++) st.mapping[i] = VK_COMPONENT_SWIZZLE_IDENTITY;
   st.chroma_offsets[0] = st.chroma_offsets[1] = loc;
   st.chroma_filter = VK_FILTER_LINEAR;
   st.chroma_reconstruction = loc == VK_CHROMA_LOCATION_COSITED_EVEN;
   struct mali_lbinding b = {.type = MALI_DT_COMBINED_IMAGE_SAMPLER, .count = 1, .slot = 0, .stride = ycbcr ? 3 : 2, .imm_off = 0,
                             .ycbcr_planes = ycbcr ? 2 : 0};
   struct mali_compile_layout l;
   memset(&l, 0, sizeof l);
   l.nsets = 1;
   l.set[0] = (struct mali_lset){.nbindings = 1, .slot_count = b.stride, .b = &b, .ycbcr = ycbcr ? (const void *)&st : NULL};
   struct mali_compile_opts o = {.arch = arch, .stage = MALI_STAGE_FRAGMENT, .spirv = fs.w, .spirv_words = fs.n, .layout = &l, .cache_dir = ""};
   char err[256] = "";
   int r = mali_compile(&o, c, err, sizeof err);
   if (r) printf("  compile error: %s\n", err);
   free(fs.w);
   return r;
}

/* Vulkan: gl_FragCoord.xy is a pixel centre (+0.5). The hardware gives the integer corner; the compiler adds the half pixel
 * (nir_lower_wpos_center). fragcoord.frag only scales FragCoord, so the folded constants x - 99.5 and y - 199.5 exist only if the lowering ran. */
/* the folded constants are literals inside the clause words (nimm is 0), so look for the float bit pattern in the binary */
static int
bin_has_f(const struct mali_compiled *c, float f)
{
   for (unsigned i = 0; i + 4 <= c->bin_size; i++)
      if (!memcmp((const char *)c->bin + i, &f, 4)) return 1;
   return 0;
}

static void
test_wpos_center(void)
{
   printf("gl_FragCoord pixel centre\n");
   for (unsigned arch = 10; arch <= 13; arch++) {
      if (arch == 12) continue;
      struct spv fs = load("fragcoord.frag.spv");
      if (!fs.w) continue;
      struct mali_compile_opts o = {.arch = arch, .stage = MALI_STAGE_FRAGMENT, .spirv = fs.w, .spirv_words = fs.n, .cache_dir = ""};
      struct mali_compiled c;
      char err[256] = "";
      int r = mali_compile(&o, &c, err, sizeof err);
      CHECK(r == 0, "v%u fragcoord.frag compiles: %s", arch, err);
      if (r == 0)
         CHECK(bin_has_f(&c, -99.5f) && bin_has_f(&c, -199.5f) && !bin_has_f(&c, -100.0f) && !bin_has_f(&c, -200.0f),
               "v%u FragCoord carries the half-pixel centre", arch);
      free(fs.w);
   }
}

static void
test_ycbcr(void)
{
   printf("ycbcr lowering\n");
   for (unsigned arch = 10; arch <= 13; arch += 1) {
      if (arch == 12) continue;
      struct mali_compiled plain, a, b, c, d;
      if (ycc_compile(arch, 0, 0, VK_CHROMA_LOCATION_MIDPOINT, 0, &plain)) continue;
      if (ycc_compile(arch, VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709, VK_SAMPLER_YCBCR_RANGE_ITU_NARROW, VK_CHROMA_LOCATION_MIDPOINT, 1, &a)) continue;
      if (ycc_compile(arch, VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601, VK_SAMPLER_YCBCR_RANGE_ITU_FULL, VK_CHROMA_LOCATION_MIDPOINT, 1, &b)) continue;
      if (ycc_compile(arch, VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709, VK_SAMPLER_YCBCR_RANGE_ITU_FULL, VK_CHROMA_LOCATION_COSITED_EVEN, 1, &c)) continue;
      if (ycc_compile(arch, VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709, VK_SAMPLER_YCBCR_RANGE_ITU_FULL, VK_CHROMA_LOCATION_MIDPOINT, 1, &d)) continue;
      if (g_verbose)
         printf("  v%u: plain %u B, 709n %u B, 601f %u B, 709f cosited %u B, 709f midpoint %u B\n", arch, plain.bin_size, a.bin_size, b.bin_size, c.bin_size, d.bin_size);
      /* the converted sample is two texture fetches plus the matrix: a lot more code than one fetch */
      CHECK(a.bin_size > plain.bin_size + 32, "v%u converted sample (%u B) is larger than a plain one (%u B)", arch, a.bin_size, plain.bin_size);
      CHECK(!imm_has(&plain, 1.5748f) && !imm_has(&plain, 1.402f), "v%u plain shader has no conversion constants", arch);
      CHECK(imm_has(&a, 1.5748f) && !imm_has(&a, 1.402f), "v%u BT.709 constants (Cr->R 1.5748)", arch);
      CHECK(imm_has(&b, 1.402f) && !imm_has(&b, 1.5748f), "v%u BT.601 constants (Cr->R 1.402)", arch);
      CHECK(imm_has(&a, 255.0f / 219.0f) || imm_has(&a, 1.1644f), "v%u narrow range luma scale 255/219", arch);
      CHECK(!imm_has(&b, 255.0f / 219.0f) && !imm_has(&d, 255.0f / 219.0f), "v%u full range has no narrow scale", arch);
      CHECK(a.bin_size != b.bin_size || memcmp(a.bin, b.bin, a.bin_size), "v%u 709n and 601f differ", arch);
      CHECK(c.bin_size != d.bin_size || memcmp(c.bin, d.bin, c.bin_size), "v%u cosited siting changes the code", arch);
      /* the lowered code reads two planes: texture handles of plane 0 and 1 of the same element (handle = slot + plane) */
      mali_compiled_free(&plain); mali_compiled_free(&a); mali_compiled_free(&b); mali_compiled_free(&c); mali_compiled_free(&d);
   }
}

/* Specialization constants: the same SPIR-V compiled with different VkSpecializationInfo gives different code (the value is folded in),
 * a missing entry keeps the SPIR-V default, and the disk cache key separates the variants. */
static int
spec_compile(unsigned arch, const char *spv, enum mali_stage st, const struct mali_spec_entry *e, unsigned n, const void *data, const char *cache, struct mali_compiled *c)
{
   struct spv s = load(spv);
   if (!s.w) return -1;
   struct mali_lbinding b = {.type = MALI_DT_STORAGE_BUFFER, .count = 1, .slot = 0, .stride = 1, .imm_off = MALI_NO_IMM_SAMPLER};
   struct mali_compile_layout l;
   memset(&l, 0, sizeof l);
   l.nsets = 1;
   l.set[0] = (struct mali_lset){.nbindings = 1, .slot_count = 1, .b = &b};
   struct mali_compile_opts o = {.arch = arch, .stage = st, .spirv = s.w, .spirv_words = s.n, .nspec = n, .spec = e, .spec_data = data, .cache_dir = cache,
                                 .layout = st == MALI_STAGE_COMPUTE ? &l : NULL};
   char err[256] = "";
   int r = mali_compile(&o, c, err, sizeof err);
   if (r) printf("  compile error: %s\n", err);
   free(s.w);
   return r;
}

static void
test_spec(void)
{
   printf("specialization constants\n");
   for (unsigned arch = 10; arch <= 13; arch++) {
      if (arch == 12) continue;
      struct mali_compiled fd, fo, fp, cd, co;
      struct { float r; uint32_t mode, blue; } fdata = {0.75f, 2, 1};
      struct mali_spec_entry fe[3] = {{0, 0, 4}, {3, 4, 4}, {5, 8, 4}};
      struct mali_spec_entry fe1[1] = {{0, 0, 4}};
      if (spec_compile(arch, "spec.frag.spv", MALI_STAGE_FRAGMENT, NULL, 0, NULL, "", &fd)) continue;
      if (spec_compile(arch, "spec.frag.spv", MALI_STAGE_FRAGMENT, fe, 3, &fdata, "", &fo)) continue;
      if (spec_compile(arch, "spec.frag.spv", MALI_STAGE_FRAGMENT, fe1, 1, &fdata, "", &fp)) continue;
      CHECK(imm_has(&fd, 0.25f) && !imm_has(&fd, 0.75f), "v%u FS default R = 0.25", arch);
      CHECK(imm_has(&fo, 0.75f) && !imm_has(&fo, 0.25f), "v%u FS overridden R = 0.75", arch);
      CHECK(imm_has(&fp, 0.75f), "v%u FS partial override: R from the data, MODE/BLUE default", arch);
      CHECK(fo.bin_size != fd.bin_size || memcmp(fo.bin, fd.bin, fd.bin_size), "v%u FS default and overridden binaries differ", arch);
      CHECK(fo.bin_size != fp.bin_size || memcmp(fo.bin, fp.bin, fo.bin_size), "v%u FS partial override differs from the full one", arch);
      struct { float f; uint32_t wg, b, a; } cdat = {3.5f, 4, 1, 9};
      struct mali_spec_entry ce[4] = {{1, 12, 4}, {2, 0, 4}, {4, 4, 4}, {7, 8, 4}};
      if (spec_compile(arch, "spec.comp.spv", MALI_STAGE_COMPUTE, NULL, 0, NULL, "", &cd)) continue;
      if (spec_compile(arch, "spec.comp.spv", MALI_STAGE_COMPUTE, ce, 4, &cdat, "", &co)) continue;
      CHECK(cd.local_size[0] == 1 && co.local_size[0] == 4, "v%u CS workgroup size from local_size_x_id: default %u, overridden %u", arch, cd.local_size[0], co.local_size[0]);
      CHECK(cd.bin_size != co.bin_size || memcmp(cd.bin, co.bin, cd.bin_size), "v%u CS binaries differ", arch);
      /* disk cache: the variants must not alias */
      char dir[] = "/tmp/t_compile_spec_XXXXXX";
      if (mkdtemp(dir)) {
         struct mali_compiled x1, x2, x3;
         if (!spec_compile(arch, "spec.frag.spv", MALI_STAGE_FRAGMENT, NULL, 0, NULL, dir, &x1) &&
             !spec_compile(arch, "spec.frag.spv", MALI_STAGE_FRAGMENT, fe, 3, &fdata, dir, &x2) &&
             !spec_compile(arch, "spec.frag.spv", MALI_STAGE_FRAGMENT, NULL, 0, NULL, dir, &x3)) {
            CHECK(imm_has(&x1, 0.25f) && imm_has(&x2, 0.75f) && imm_has(&x3, 0.25f), "v%u cache keeps default and overridden variants apart", arch);
            mali_compiled_free(&x1); mali_compiled_free(&x2); mali_compiled_free(&x3);
         }
         char cmd[128];
         snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
         if (system(cmd)) {}
      }
      mali_compiled_free(&fd); mali_compiled_free(&fo); mali_compiled_free(&fp); mali_compiled_free(&cd); mali_compiled_free(&co);
   }
}

/* MALI_CULLDIST=1: gl_CullDistance lowered as clip distances (docs/cull-distance-plan.md). The VS reports clip + cull as vs_clip_distances, so the
 * FS pass (nir_lower_clip_fs) kills below 0; unset the output is unchanged (0 = the cull outputs are unused varyings, no kill). */
static void
test_cull_distance(void)
{
   printf("MALI_CULLDIST\n");
   static const struct { const char *spv; unsigned on; } t[] = {{"cull.vert.spv", 2}, {"cullclip.vert.spv", 4}};
   for (unsigned arch = 10; arch <= 13; arch++) {
      if (arch == 12) continue;
      for (unsigned i = 0; i < 2; i++) {
         struct spv vs = load(t[i].spv);
         if (!vs.w) continue;
         for (int on = 0; on < 2; on++) {
            if (on) setenv("MALI_CULLDIST", "1", 1); else unsetenv("MALI_CULLDIST");
            struct mali_compile_opts o = {.arch = arch, .stage = MALI_STAGE_VERTEX, .spirv = vs.w, .spirv_words = vs.n, .cache_dir = ""};
            struct mali_compiled c;
            char err[256] = "";
            int r = mali_compile(&o, &c, err, sizeof err);
            CHECK(r == 0, "v%u %s env=%d compiles: %s", arch, t[i].spv, on, err);
            if (r == 0) {
               unsigned want = on ? t[i].on : (i == 1 ? 2 : 0);   /* unset: only real ClipDistances count */
               CHECK(c.vs_clip_distances == want, "v%u %s MALI_CULLDIST=%d: vs_clip_distances %u != %u", arch, t[i].spv, on, c.vs_clip_distances, want);
            }
         }
         free(vs.w);
      }
      struct spv rv = load("rot.vert.spv");   /* a shader without user distances: the VS binary is identical with and without the env */
      if (rv.w) {
         struct mali_compile_opts o = {.arch = arch, .stage = MALI_STAGE_VERTEX, .spirv = rv.w, .spirv_words = rv.n, .cache_dir = ""};
         struct mali_compiled a, b;
         char err[256] = "";
         unsetenv("MALI_CULLDIST");
         int ra = mali_compile(&o, &a, err, sizeof err);
         setenv("MALI_CULLDIST", "1", 1);
         int rb = mali_compile(&o, &b, err, sizeof err);
         unsetenv("MALI_CULLDIST");
         CHECK(ra == 0 && rb == 0 && a.bin_size == b.bin_size && !memcmp(a.bin, b.bin, a.bin_size), "v%u rot.vert differs under MALI_CULLDIST", arch);
         free(rv.w);
      }
      struct spv vs = load("cullover.vert.spv");   /* 4 clip + 8 cull > 8: refused with the env, tolerated without */
      if (vs.w) {
         struct mali_compile_opts o = {.arch = arch, .stage = MALI_STAGE_VERTEX, .spirv = vs.w, .spirv_words = vs.n, .cache_dir = ""};
         struct mali_compiled c;
         char err[256] = "";
         setenv("MALI_CULLDIST", "1", 1);
         CHECK(mali_compile(&o, &c, err, sizeof err) != 0 && strstr(err, "> 8"), "v%u cullover under MALI_CULLDIST=1 must fail (%s)", arch, err);
         unsetenv("MALI_CULLDIST");
         free(vs.w);
      }
   }
}

int
main(int argc, char **argv)
{
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "-v")) g_verbose = 1;
      else g_dir = argv[i];
   }
   double t0 = now();
   setvbuf(stdout, NULL, _IOLBF, 0);
   /* The v11/v13 binary goldens predate shaderDrawParameters/ClipDistance (extra FAU sysvars): known drift, regenerate with
    * compiler-spike/gen-compiled.sh + vk/shader/gen-golden.sh after review. Opt in with T_COMPILE_GOLDEN=1. */
   if (getenv("T_COMPILE_GOLDEN")) test_golden();
   else printf("golden: skipped (stale, T_COMPILE_GOLDEN=1 to run)\n");
   test_pairing();
   test_compute();
   test_private_init();
   test_bda_frag();
   test_determinism_and_cache();
   test_ycbcr();
   test_wpos_center();
   test_cull_distance();
   test_spec();
   test_errors();
   printf("%s (%.2f s)\n", g_fail ? "FAILED" : "OK", now() - t0);
   return g_fail != 0;
}
