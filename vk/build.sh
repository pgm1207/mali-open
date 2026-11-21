#!/bin/bash
# SPDX-License-Identifier: MIT
# Builds the Vulkan front end against a pinned, unmodified Mesa build tree.
#   ./build.sh            native x86-64 -> out/native/{libvulkan_mali.so,mali_icd.json,t_icd}
# Env: MESA_SRC (default ../third_party/mesa), MESA_BUILD (default ../third_party/build-vk).
# Compile and link flags are taken from the Mesa build tree (the PanVK target is used only as the
# source of flags and the library list; no PanVK object or header is used), so the same script
# works for any Mesa build dir, including the aarch64 glibc and bionic cross builds (R3-PLAN.md M2).
#
# One-time Mesa setup (2.5 min on 4 cores): see README.md.
set -euo pipefail
D=$(cd "$(dirname "$0")/.." && pwd)
V=$D/vk
MESA=${MESA_SRC:-$D/third_party/mesa}
B=${MESA_BUILD:-$D/third_party/build-vk}
# Native targets prefer the Mesa tree configured with -Dplatforms=wayland,x11 (README: "X11 WSI") when it exists, so the host ICD has
# VK_KHR_xcb_surface; without it the X11 tests skip. Cross targets and an explicit MESA_BUILD are never overridden.
case ${MALI_TARGET:-native} in
   native|native-san|native-cov) [ -z "${MESA_BUILD:-}" ] && [ -f "$D/third_party/build-vk-x86-x11/build.ninja" ] && B=$D/third_party/build-vk-x86-x11 ;;
esac
OUT=$V/out/${MALI_TARGET:-native}
GEN=$OUT/gen
OBJ=$OUT/obj
export PATH=$D/third_party/venv/bin:$PATH
PY=$D/third_party/venv/bin/python
ORACLE=src/panfrost/vulkan/libvulkan_panfrost.so

[ -f "$B/build.ninja" ] || { echo "no Mesa build tree at $B (see README.md)"; exit 1; }
mkdir -p "$GEN" "$OBJ"

# 1. Entry point tables. mali_* = arch-independent, mali_v10_*/mali_v11_*/mali_v13_* = per-arch device entry
#    points (weak: whatever is not implemented resolves to NULL and falls back to vk_common_*).
"$PY" "$MESA/src/vulkan/util/vk_entrypoints_gen.py" --xml "$MESA/src/vulkan/registry/vk.xml" \
   --proto --weak --out-h "$GEN/mali_entrypoints.h" --out-c "$GEN/mali_entrypoints.c" \
   --prefix mali --device-prefix mali_v10 --device-prefix mali_v11 --device-prefix mali_v13 --beta false

# MALI_EXTRA_LD: extra objects/libs (bionic: -llog -lsync and the libdrm objects of the Mesa wrap).
# 2. Flags from the Mesa tree.
cd "$B"
CMDS=$(ninja -t commands "$ORACLE")
CCLINE=$(grep -m1 -- 'panvk_instance\.c$' <<<"$CMDS")
LDLINE=$(tail -1 <<<"$CMDS")
[ -n "$CCLINE" ] || { echo "cannot find the reference compile line in $B"; exit 1; }
CC=$(sed -E 's# -MD -MQ [^ ]+ -MF [^ ]+##; s# -o [^ ]+ -c [^ ]+$##; s# -I[^ ]*src/panfrost/vulkan[^ ]*##g' <<<"$CCLINE")
LDSYS=$(tr ' ' '\n' <<<"$LDLINE" | grep -E '^(/.*\.so(\.[0-9.]+)?|/usr/.*\.a|-l[A-Za-z0-9_+.-]+|-pthread)$' | tr '\n' ' ')
CXX=$(cut -d' ' -f1 <<<"$LDLINE")
# A Meson tree configured with ccache installed prefixes every compile line with the launcher; callers below use the first word as the
# compiler ("${CC%% *}" -> "ccache: invalid option -- t"), so drop the launcher (the objects built here are not cached anyway).
CC=$(sed -E 's#^([^ ]*/)?ccache +##' <<<"$CC"); CXX=$(sed -E 's#^([^ ]*/)?ccache +##' <<<"$CXX")
# MALI_SAN=1: ASAN+UBSAN driver objects (use with LD_PRELOAD=$(gcc -print-file-name=libasan.so) and MALI_TARGET=native-san)
if [ "${MALI_SAN:-0}" = 1 ]; then CC="$CC -fsanitize=address,undefined -fno-omit-frame-pointer -g"; CXX="$CXX -fsanitize=address,undefined"; fi

# MALI_PROF=1 (with MALI_TARGET=native-prof): debug info + frame pointers for the host sampling profiler (tools/prof-sample.c).
if [ "${MALI_PROF:-0}" = 1 ]; then CC="$CC -g -fno-omit-frame-pointer"; fi

# MALI_COV=1: gcov instrumented driver objects (MALI_TARGET=native-cov; see tools/coverage.sh). -O0 so line counts map to source.
if [ "${MALI_COV:-0}" = 1 ]; then CC="$CC --coverage -fprofile-update=atomic -O0 -g"; CXX="$CXX --coverage"; fi

# 3. Compile (cwd = Mesa build dir: its -I paths are relative).
# Shared ccache for every worktree (one cache dir, size-capped) when ccache is installed; MALI_NO_CCACHE=1 turns it off.
# BASEDIR rewrites absolute paths under this checkout to relative ones so identical sources in different worktrees hit the same entry.
CCACHE=""
if [ -z "${MALI_NO_CCACHE:-}" ] && command -v ccache >/dev/null 2>&1; then
   CCACHE=ccache
   export CCACHE_DIR=${CCACHE_DIR:-$HOME/.cache/mali-ccache} CCACHE_MAXSIZE=${CCACHE_MAXSIZE:-10G} CCACHE_BASEDIR=$D CCACHE_COMPILERCHECK=content
   mkdir -p "$CCACHE_DIR"
fi
SRCS="$V/src/mali_instance.c $V/src/mali_physical_device.c $V/src/mali_device.c
      $V/src/mali_memory.c $V/src/mali_sync.c $V/src/mali_queue.c $V/src/mali_fprof.c
      $V/src/mali_backend_fake.c $V/src/mali_backend_kbase.c
      $V/src/mali_format.c $V/src/mali_image.c $V/src/mali_buffer.c $V/src/mali_sampler.c
      $V/src/mali_descriptor.c $V/src/mali_descriptor_lower.c $V/src/mali_pipeline_layout.c
      $V/src/mali_compile.c $V/src/mali_resources.c
      $V/src/mali_wsi.c $V/src/mali_meta.c $V/src/mali_bc.c $V/src/mali_meta_vk.c $V/src/mali_query.c $V/src/mali_query_core.c
      $V/src/mali_event.c $V/src/mali_event_vk.c $V/src/mali_cmd.c $V/src/mali_render.c $V/src/mali_cs_emit.c $V/src/mali_pipeline.c $V/src/mali_maint5.c $V/src/mali_vb3.c $V/src/mali_soft.c $V/src/mali_dmabuf.c $GEN/mali_entrypoints.c"
# libkbase (own, MIT, libc only) is compiled in with the same flags as the rest, warnings not fatal.
KB=$D/libkbase
KBSRCS="$KB/src/ctx.c $KB/src/bo.c $KB/src/queue.c $KB/src/sync.c $KB/src/heap.c
        $KB/src/draw.c $KB/src/draw_v13.c $KB/src/fb.c"
OBJS=""
# Compile in parallel (MALI_JOBS, default nproc); any failed compile fails the build.
JOBS=${MALI_JOBS:-$(nproc 2>/dev/null || echo 4)}
if ! [[ $JOBS =~ ^0*[1-9][0-9]*$ ]]; then echo "MALI_JOBS must be a positive integer (got '$JOBS')"; exit 1; fi   # 007 ok, 0 / 00 rejected
cfail=0; running=0
reap() { wait -n || cfail=1; running=$((running - 1)); }
for s in $KBSRCS; do
   o=$OBJ/kb_$(basename "$s" .c).o
   [ $running -ge "$JOBS" ] && reap
   eval "$CCACHE $CC -D_GNU_SOURCE -I$KB/include -I$KB/src -w -o $o -c $s" & running=$((running + 1))
   OBJS="$OBJS $o"
done
# Unmistakable driver identity: short git sha (+dirty) reported in driverInfo; "unknown" outside a git checkout.
if MALI_SHA=$(git -C "$V" rev-parse --short=10 HEAD 2>/dev/null) && [ -n "$MALI_SHA" ]; then
   rc=0; git -C "$V" diff --quiet HEAD -- 2>/dev/null || rc=$?
   case $rc in
      0) ;;
      1) MALI_SHA="$MALI_SHA-dirty" ;;
      *) echo "build.sh: git diff failed (rc=$rc); not marking -dirty" >&2 ;;
   esac
else
   MALI_SHA=unknown
fi
for s in $SRCS; do
   o=$OBJ/$(basename "$s" .c).o
   [ $running -ge "$JOBS" ] && reap
   eval "$CCACHE $CC -DMALI_GIT_SHA=\\\"$MALI_SHA\\\" -I$GEN -I$V/src -I$KB/include -Werror -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -o $o -c $s" & running=$((running + 1))
   OBJS="$OBJS $o"
done
while [ $running -gt 0 ]; do reap; done
[ $cfail = 0 ] || { echo "compile failed"; exit 1; }

# 4. Link. Runtime libs need --whole-archive (weak entry point overloads), as Mesa does it.
WHOLE="src/vulkan/runtime/libvulkan_lite_runtime.a src/vulkan/runtime/libvulkan_runtime.a
       src/vulkan/runtime/libvulkan_instance.a src/vulkan/wsi/libvulkan_wsi.a"
# M5/M6: pan_format tables, panfrost compiler (Valhall) and what they pull in (no PanVK object).
PANLIBS="src/panfrost/compiler/libpanfrost_compiler.a src/panfrost/compiler/midgard/libpanfrost_midgard.a
      src/panfrost/compiler/midgard/libpanfrost_midgard_disasm.a src/panfrost/compiler/bifrost/libpanfrost_bifrost.a
      src/panfrost/compiler/bifrost/libpanfrost_bifrost_disasm.a src/panfrost/compiler/bifrost/valhall/libpanfrost_valhall_disasm.a
      src/panfrost/model/libpanfrost_model_lib.a src/panfrost/lib/libpanfrost_lib.a
      src/panfrost/lib/libpan-format-v10.a src/panfrost/lib/libpan-format-v11.a src/panfrost/lib/libpan-format-v13.a
      src/panfrost/lib/libpan-arch-v10.a src/panfrost/lib/libpan-arch-v11.a src/panfrost/lib/libpan-arch-v13.a src/panfrost/lib/kmod/libpankmod_lib.a src/panfrost/genxml/libpanfrost_decode.a
      src/panfrost/genxml/libpandecode-arch-v10.a src/panfrost/genxml/libpandecode-arch-v11.a src/panfrost/genxml/libpandecode-arch-v13.a"
LIBS="$PANLIBS src/vulkan/util/libvulkan_util.a src/compiler/nir/libnir.a src/compiler/libcompiler.a
      src/compiler/spirv/libvtn.a src/util/libmesa_util.a src/util/libmesa_util_simd.a
      src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a src/util/libxmlconfig.a"
for l in src/loader/libloader_wayland_helper.a src/x11/libloader_x11.a src/loader/libloader.a; do   # M14: -Dplatforms=wayland; X11 WSI: -Dplatforms=x11
   [ -f "$l" ] && LIBS="$LIBS $l"
done
for l in src/util/libmesa_util_clflush.a src/util/libmesa_util_clflushopt.a; do
   [ -f "$l" ] && LIBS="$LIBS $l"     # x86 only
done
for l in $WHOLE $LIBS; do
   [ -f "$l" ] || { echo "missing $B/$l: build the Mesa targets listed in README.md"; exit 1; }
done
# shellcheck disable=SC2086
$CXX -o "$OUT/libvulkan_mali.so" -shared -fPIC -Wl,--as-needed -Wl,--no-undefined -Wl,-O1 \
   -Wl,-soname,libvulkan_mali.so $OBJS \
   -Wl,--whole-archive -Wl,--start-group $WHOLE -Wl,--no-whole-archive $LIBS \
   -Wl,--version-script "$MESA/src/vulkan/vulkan.sym" -Wl,-Bsymbolic -Wl,--gc-sections \
   -Wl,--build-id=sha1 $LDSYS ${MALI_EXTRA_LD:-} -Wl,--end-group

# 4b. t_resources: M5/M7 end-to-end, linked statically with the driver objects (white-box, reads descriptor memory).
eval "$CC -I$GEN -I$V/src -I$KB/include -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -o $OBJ/t_resources.o -c $V/tests/t_resources.c"
# shellcheck disable=SC2086
$CXX -o "$OUT/t_resources" $OBJ/t_resources.o $OBJS \
   -Wl,--wrap=memset -Wl,--whole-archive -Wl,--start-group $WHOLE -Wl,--no-whole-archive $LIBS $LDSYS ${MALI_EXTRA_LD:-} -Wl,--end-group

# 4c. shaderdb: offline compile statistics over a SPIR-V corpus (tools/shaderdb/run.sh).
eval "$CC -I$GEN -I$V/src -I$KB/include -Wno-unused-parameter -o $OBJ/shaderdb.o -c $D/tools/shaderdb/shaderdb.c"
# shellcheck disable=SC2086
$CXX -o "$OUT/shaderdb" $OBJ/shaderdb.o $OBJS \
   -Wl,--whole-archive -Wl,--start-group $WHOLE -Wl,--no-whole-archive $LIBS $LDSYS ${MALI_EXTRA_LD:-} -Wl,--end-group

# 4c. t_soft_frame: the desktop software rasteriser end to end (MALI_SOFT=1), linked statically like t_resources so it can
#     register CPU programs per pipeline and read the images through the driver's own layout query.
eval "$CC -I$GEN -I$V/src -I$KB/include -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -o $OBJ/t_soft_frame.o -c $V/tests/t_soft_frame.c"
# shellcheck disable=SC2086
$CXX -o "$OUT/t_soft_frame" $OBJ/t_soft_frame.o $OBJS \
   -Wl,--whole-archive -Wl,--start-group $WHOLE -Wl,--no-whole-archive $LIBS $LDSYS ${MALI_EXTRA_LD:-} -Wl,--end-group

# 5. Loader manifest + test.
cat > "$OUT/mali_icd.json" <<J
{"file_format_version":"1.0.1","ICD":{"library_path":"$OUT/libvulkan_mali.so","api_version":"1.3.0"}}
J
${CC%% *} -std=c11 -O2 -Wall -Wextra -Werror -I"$MESA/include" -o "$OUT/t_icd" "$V/tests/t_icd.c" -ldl
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -I"$MESA/include" -o "$OUT/t_queue" "$V/tests/t_queue.c" -ldl -pthread

${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_frameprof" "$V/tests/t_frameprof.c" -ldl -lm -pthread
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_vk_draw" "$V/tests/t_vk_draw.c" -ldl -lm -pthread
# Reference scene (docs/desktop-gpu.md): one client, run against our ICD on the phone and against the desktop GPU for the
# reference image. Plain Vulkan, no driver internals, so it also builds for the aarch64 targets.
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_vk_ref" "$V/tests/t_vk_ref.c" -ldl -lm
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_vk_feat" "$V/tests/t_vk_feat.c" -ldl -lm -pthread
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_vk_compute" "$V/tests/t_vk_compute.c" -ldl -lm -pthread
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_vk_bench" "$V/tests/t_vk_bench.c" -ldl -lm -pthread
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_vk_meta" "$V/tests/t_vk_meta.c" -ldl -lm -pthread
# Uncapped benchmark pair (loader clients, offscreen targets: no presentation path to cap them).
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_bench_gpu" "$V/tests/t_bench_gpu.c" -ldl -lm
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_bench_cpu" "$V/tests/t_bench_cpu.c" -ldl -lm
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -I"$V/tests" -o "$OUT/t_vk_leak" "$V/tests/t_vk_leak.c" -ldl
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -DMALI_EXPECT_SHA=\"$MALI_SHA\" -o "$OUT/t_vk_info" "$V/tests/t_vk_info.c" -ldl
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_vk_mem" "$V/tests/t_vk_mem.c" -ldl
# The dma-buf fd check on its own (mali_dmabuf.c): a real dma-buf from /dev/udmabuf or the system dma-heap must pass,
# memfd/pipe/regular file//dev/kvm must not, and the ioctl fallback must accept the dma-buf with the statfs arm forced off.
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$V/src" -o "$OUT/t_dmabuf" "$V/tests/t_dmabuf.c" "$V/src/mali_dmabuf.c"
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_vk_vram" "$V/tests/t_vk_vram.c" -ldl
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_vk_qerr" "$V/tests/t_vk_qerr.c" -ldl
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_vk_dxvk_wsi" "$V/tests/t_vk_dxvk_wsi.c" -ldl
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_dxvk_caps" "$V/tests/t_dxvk_caps.c" -ldl
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_dxvk_api" "$V/tests/t_dxvk_api.c" -I"$V/tests" -ldl
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_steam_caps" "$V/tests/t_steam_caps.c" -ldl
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_vk_drm" "$V/tests/t_vk_drm.c" -ldl
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_vk_headless" "$V/tests/t_vk_headless.c" -ldl
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_vk_trace" "$V/tests/t_vk_trace.c" -ldl
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -I"$V/tests" -o "$OUT/t_angle_seq" "$V/tests/t_angle_seq.c" -ldl
# Two threads on one device: a vkAcquireNextImageKHR that waits must not stall the other thread (CR #323 N4).
rm -f "$OUT/t_vk_threads"
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" -o "$OUT/t_vk_threads" "$V/tests/t_vk_threads.c" -ldl -lxcb -lpthread 2>"$OBJ/t_vk_threads.err" \
   || { if grep -qE "xcb/xcb.h: No such file|xcb/xcb.h. file not found|cannot find -lxcb|unable to find library -lxcb" "$OBJ/t_vk_threads.err"; then
           echo "note: t_vk_threads not built (no xcb headers/libs for this target)"; else cat "$OBJ/t_vk_threads.err"; exit 1; fi; }
# X11 WSI test: needs libxcb headers and libs (absent for bionic: then there is simply no t_vk_xcb and the X11 cases skip)
# --wl-after-x11 also needs libwayland-client; without it that mode reports "skipped" and the case is skipped (HAVE_WAYLAND_CLIENT).
rm -f "$OUT/t_vk_xcb"
WL_CFLAGS=; WL_LIBS=
pkg-config --exists wayland-client 2>/dev/null && { WL_CFLAGS=$(pkg-config --cflags wayland-client); WL_LIBS=$(pkg-config --libs wayland-client); }
[ -n "$WL_LIBS" ] && case "${CC%% *}" in *bionic*|*android*) WL_CFLAGS=; WL_LIBS= ;; esac
${CC%% *} -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter -I"$MESA/include" $WL_CFLAGS -o "$OUT/t_vk_xcb" "$V/tests/t_vk_xcb.c" -ldl -lxcb $WL_LIBS ${WL_LIBS:+-DHAVE_WAYLAND_CLIENT=1} 2>"$OBJ/t_vk_xcb.err" \
   || { if grep -qE "xcb/xcb.h: No such file|xcb/xcb.h. file not found|cannot find -lxcb|unable to find library -lxcb" "$OBJ/t_vk_xcb.err"; then
           echo "note: t_vk_xcb not built (no xcb headers/libs for this target)"; else cat "$OBJ/t_vk_xcb.err"; exit 1; fi; }

[ "$OUT/libvulkan_mali.so" -nt "$V/src/mali_cs_emit.c" ] || { echo "FAILED: libvulkan_mali.so older than sources"; exit 1; }
echo "built $OUT/libvulkan_mali.so ($(stat -c %s "$OUT/libvulkan_mali.so") B) mali_icd.json t_icd"
