#!/bin/bash
# SPDX-License-Identifier: MIT
# Offline check of the ICD (no GPU, no phone). Prints one line per case and "OK" or "FAILED".
#   ./check.sh [--no-build] [-v] [--bless]
#     --bless   regenerate vk/tests/golden/*.txt (decoded command streams of t_vk_draw/t_vk_feat/t_vk_meta on both fake GPUs) from the
#               current driver; review `git diff vk/tests/golden` and commit it with the change that caused it
#     --deqp-host  also run deqp/host-run.sh (host dEQP-VK vs the ASan fake-backend ICD, ~2 min); skipped with a message if the host deqp-vk
#               build (third_party/build-vk-x86-host) is absent
#   MALI_SAN=1 ./check.sh   same cases with ASan+UBSan driver objects (out/native-san; the standalone build-t_*.sh tests are skipped)
set -uo pipefail
V=$(cd "$(dirname "$0")" && pwd)
SAN=${MALI_SAN:-0}
if [ "$SAN" = 1 ]; then export MALI_TARGET=native-san; OUT=$V/out/native-san
elif [ "${MALI_COV:-0}" = 1 ]; then export MALI_TARGET=native-cov; OUT=$V/out/native-cov   # gcov build (tools/coverage.sh)
else OUT=$V/out/native; fi
build=1; verbose=0; bless=0; deqp_host=0
for a in "$@"; do
   case $a in --no-build) build=0 ;; -v) verbose=1 ;; --bless) bless=1 ;; --deqp-host) deqp_host=1 ;; *) echo "usage: check.sh [--no-build] [-v] [--bless] [--deqp-host]"; exit 2 ;; esac
done
if [ "$SAN" = 1 ]; then
   # The test executables are not sanitized; the sanitized driver .so needs the runtime preloaded. Leaks are reported by LSan at exit.
   export LD_PRELOAD="$(gcc -print-file-name=libasan.so)"
   export ASAN_OPTIONS=detect_leaks=${MALI_SAN_LEAKS:-0}:abort_on_error=0:halt_on_error=1
   export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
fi
# The compile cache (mali_compile.c, ~/.cache by default) must not decide a golden: a stale entry from an older compiler made main's own goldens
# fail on a clean cache (vk_draw FS FAU 0x40 vs the fresh 0x09000000). Every check run starts from an empty private cache.
# MALI_OPT_GCM=1 bypasses the cache (mali_compile.c use_cache), which the cold/warm and damaged-entry checks below need active: do not inherit it.
unset MALI_OPT_GCM MALI_CULLDIST
export MALI_SHADER_CACHE=$OUT/shader-cache; rm -rf "$MALI_SHADER_CACHE"; mkdir -p "$MALI_SHADER_CACHE"
if [ $build = 1 ]; then "$V/build.sh" >/dev/null || { echo "FAILED: build"; exit 1; }; fi

fail=0
export MALI_DIRTY_CHECK=1   # trap: build_draw_descs aborts when the record-time dirty mask reuses state the old compares would not
# Only our ICD, no implicit layers, so the result does not depend on the laptop's drivers.
# Number of DXVK-required core/1.1-1.3 features still false (of 47); lower it only with a passing test for the new feature.
DXVK_MISSING=4            # measured with t_dxvk_caps. sampleRateShading is advertised at 1 sample (t_vk_feat samplerate); MALI_EXPERIMENTAL_MSAA only adds 4x draws
DXVK_MISSING_MSAA=$DXVK_MISSING
LOADER_ENV="VK_DRIVER_FILES=$OUT/mali_icd.json VK_ICD_FILENAMES=$OUT/mali_icd.json VK_LOADER_LAYERS_DISABLE=~all~"

run() { # name, expected exit status, command...
   local name=$1 want=$2 out rc; shift 2
   out=$(timeout -k 5 120 env "$@" 2>&1); rc=$?
   if [ $rc -eq "$want" ]; then echo "ok    $name"
   elif [ $rc -eq 124 ] || [ $rc -eq 137 ]; then echo "FAIL  $name TIMEOUT (120 s, exit $rc)"; fail=1
   else echo "FAIL  $name (exit $rc, want $want)"; fail=1; fi
   if [ $verbose = 1 ] || [ $rc -ne "$want" ]; then echo "$out" | sed 's/^/        /'; fi
}

run "loader v11"         0 $LOADER_ENV MALI_BACKEND=fake MALI_FAKE_GPU=v11 "$OUT/t_icd" --expect-name "Mali-G615"
run "loader v13"         0 $LOADER_ENV MALI_BACKEND=fake MALI_FAKE_GPU=v13 "$OUT/t_icd" --expect-name "Mali-G925"
run "direct v11"         0 MALI_BACKEND=fake MALI_FAKE_GPU=v11 "$OUT/t_icd" --direct "$OUT/libvulkan_mali.so" --expect-name "Mali-G615"
run "direct v13"         0 MALI_BACKEND=fake MALI_FAKE_GPU=v13 "$OUT/t_icd" --direct "$OUT/libvulkan_mali.so" --expect-name "Mali-G925"
run "direct no backend"  0 -u MALI_BACKEND "$OUT/t_icd" --direct "$OUT/libvulkan_mali.so" --expect-none
run "direct bad fake gpu" 0 MALI_BACKEND=fake MALI_FAKE_GPU=v99 "$OUT/t_icd" --direct "$OUT/libvulkan_mali.so" --expect-none
for g in v11 v13; do run "vk_info $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_info" --direct "$OUT/libvulkan_mali.so"; done
for g in v11 v13; do run "vk_mem $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_mem" --direct "$OUT/libvulkan_mali.so"; done
for g in v11 v13; do run "vk_qerr $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_qerr" --direct "$OUT/libvulkan_mali.so"; done
for g in v11 v13; do run "dxvk_caps $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_dxvk_caps" --direct "$OUT/libvulkan_mali.so" --expect-missing "$DXVK_MISSING"; done
for g in v11 v13; do run "dxvk_api $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_dxvk_api" --direct "$OUT/libvulkan_mali.so"; done
for g in v11 v13; do run "angle_seq $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_angle_seq" --direct "$OUT/libvulkan_mali.so"; done
run "nokopper shim"      0 sh "$V/tests/t_nokopper.sh"
# Steam session (STEAM-GAPS.md): none of gamescope's vkCreateDevice requirements is missing any more.
STEAM_MISSING=0
# zink (STEAM-GAPS.md): logicOp and EXT_physical_device_drm still missing; lower only together with the passing test.
ZINK_MISSING=2
for g in v11 v13; do run "steam_caps $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_steam_caps" --direct "$OUT/libvulkan_mali.so" --expect-missing "$STEAM_MISSING" --expect-zink-missing "$ZINK_MISSING"; done
for g in v11 v13; do run "dxvk_caps $g msaa gate" 0 MALI_EXPERIMENTAL_MSAA=1 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_dxvk_caps" --direct "$OUT/libvulkan_mali.so" --expect-missing "$DXVK_MISSING_MSAA"; done
for g in v11 v13; do run "vk_info $g msaa gate" 0 MALI_EXPERIMENTAL_MSAA=1 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_info" --direct "$OUT/libvulkan_mali.so"; done

# M4: memory, sync, queue (fake backend; MALI_FAKE_SLOW_US makes the fences really block).
TQ="$OUT/t_queue --direct $OUT/libvulkan_mali.so"
run "queue all v11"      0 MALI_BACKEND=fake MALI_FAKE_GPU=v11 $TQ
run "queue teardown v13"  0 MALI_BACKEND=fake MALI_FAKE_GPU=v13 MALI_FAKE_SLOW_US=400000 $TQ --only teardown
run "queue all v13 slow" 0 MALI_BACKEND=fake MALI_FAKE_GPU=v13 MALI_FAKE_SLOW_US=200 $TQ --submits 1000
# present ordering (queue-order): the commit never precedes the frame; with a fence it may (MALI_PRESENT_FENCE forces the kernel capability)
for m in 0 1; do run "present order fence=$m" 0 MALI_BACKEND=fake MALI_FAKE_SLOW_US=100000 MALI_PRESENT_FENCE=$m $TQ --only order; done
run "signal-less submit skips the fence" 0 MALI_BACKEND=fake MALI_FAULT_INJECT=5 $TQ --only nofence
run "MALI_FENCE_ALWAYS restores it (expected fail)" 1 MALI_BACKEND=fake MALI_FAULT_INJECT=5 MALI_FENCE_ALWAYS=1 $TQ --only nofence
for n in 1 50; do run "fault inject $n" 0 MALI_BACKEND=fake MALI_FAULT_INJECT=$n $TQ --only fault; done

# M5/M7: buffers, images, views, samplers, memory binding, descriptor sets, updates, templates (white-box, static link).
for g in v11 v13; do run "resources $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_resources"; done

# M9: client draws through the ICD (fake backend: API sequence, compile, CS build and submit; pixels only on hardware).
for g in v11 v13; do
   run "vk_draw $g"       0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_draw" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader" --loop 200
   run "vk_draw cull $g"  0 MALI_CULLDIST=1 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_draw" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader" --only cull_dist   # gl_CullDistance VS as clip distances (default off)
   run "vk_draw many $g"  0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_draw" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader" --only many_draws
   run "vk_draw reset $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_draw" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader" --loop 1000 --loop-reset
done

# R4-B2: DXVK-required core feature tests (pixels only on hardware).
for g in v11 v13; do
   run "vk_feat $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_feat" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader"
done

# M11: compute through the ICD (fake backend: compile, CS build and submit; results only on hardware).
for g in v11 v13; do
   run "vk_compute $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_compute" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader" --loop 200
   run "vk_meta $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_meta" --direct "$OUT/libvulkan_mali.so" --loop 20
done

# Golden command streams + hardware-bug rules (tests/golden.py): the fake backend dumps every submitted CS with the live BOs
# (MALI_FAKE_DUMP); the dump is rendered address-normalised and diffed against tests/golden/<test>-<gpu>.txt. BO flag rules run in the
# fake backend's bo_alloc (a violation fails the test itself).
GOLD=$V/tests/golden; GDIR=$OUT/golden; mkdir -p "$GOLD" "$GDIR"
golden() { # test, gpu, extra args...
   local t=$1 g=$2 name out rc; shift 2
   name=$t-$g; rm -f "$GDIR/$name.jsonl" "$GDIR/$name.txt"
   out=$(MALI_FAKE_DUMP="$GDIR/$name.jsonl" timeout -k 5 120 env MALI_EXPERIMENTAL_MSAA=${GOLD_MSAA:-0} MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_$t" --direct "$OUT/libvulkan_mali.so" "$@" 2>&1); rc=$?
   if [ $rc -ne 0 ]; then echo "FAIL  golden $name (test exit $rc)"; echo "$out" | tail -15 | sed 's/^/        /'; fail=1; return; fi
   out=$(GOLDEN_ARCH=${g#v} timeout -k 5 120 python3 -I "$V/tests/golden.py" "$GDIR/$name.jsonl" "$GDIR/$name.txt" 2>&1); rc=$?
   if [ $rc -ne 0 ]; then echo "FAIL  rules $name"; echo "$out" | head -20 | sed 's/^/        /'; fail=1; return; fi
   [ $verbose = 1 ] && echo "$out" | sed 's/^/        /'
   if [ $bless = 1 ]; then cp "$GDIR/$name.txt" "$GOLD/$name.txt"; echo "bless $name ($(wc -l <"$GOLD/$name.txt") lines)"; return; fi
   if [ ! -f "$GOLD/$name.txt" ]; then echo "FAIL  golden $name: no tests/golden/$name.txt (run ./check.sh --bless)"; fail=1
   elif cmp -s "$GOLD/$name.txt" "$GDIR/$name.txt"; then echo "ok    golden $name ($(grep -c '^kick' "$GOLD/$name.txt") kicks)"
   else echo "FAIL  golden $name differs (./check.sh --bless if intended):"; diff -u "$GOLD/$name.txt" "$GDIR/$name.txt" | head -30 | sed 's/^/        /'; fail=1; fi
}
for g in v11 v13; do
   golden vk_draw $g --spv-dir "$V/shader"
   golden vk_feat $g --spv-dir "$V/shader"
   GOLD_MSAA=1 golden vk_meta $g
done
# Shader cache, cold vs warm: the same test on an empty private cache and again on the cache that run left must give byte-identical decoded
# streams (a cached shader that yields different GPU state than a fresh compile would hide behind every host run that has a warm cache; see
# tests/compile-guard.sh for the cause that was found). The warm run must really hit the cache (no new entry) and a damaged entry must be seen.
cwdump() { # test, gpu, cache dir, out name
   rm -f "$GDIR/$4.jsonl" "$GDIR/$4.txt"
   MALI_SHADER_CACHE=$3 MALI_FAKE_DUMP="$GDIR/$4.jsonl" timeout -k 5 120 env MALI_BACKEND=fake MALI_FAKE_GPU=$2 "$OUT/t_$1" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader" >/dev/null 2>&1 || return 1
   GOLDEN_ARCH=${2#v} timeout -k 5 120 python3 -I "$V/tests/golden.py" "$GDIR/$4.jsonl" "$GDIR/$4.txt" >/dev/null 2>&1
}
for t in vk_draw vk_feat; do for g in v11 v13; do
   CW=$OUT/cw-cache-$t-$g; rm -rf "$CW"; mkdir -p "$CW"
   if ! cwdump $t $g "$CW" cw-cold-$t-$g; then echo "FAIL  cache cold/warm $t $g (cold run failed)"; fail=1; continue; fi
   n0=$(ls "$CW" | wc -l); sum0=$(cat "$CW"/*.bin 2>/dev/null | sha256sum)
   if ! cwdump $t $g "$CW" cw-warm-$t-$g; then echo "FAIL  cache cold/warm $t $g (warm run failed)"; fail=1; continue; fi
   if [ "$n0" -gt 0 ] && [ "$n0" = "$(ls "$CW" | wc -l)" ] && [ "$sum0" = "$(cat "$CW"/*.bin | sha256sum)" ] && cmp -s "$GDIR/cw-cold-$t-$g.txt" "$GDIR/cw-warm-$t-$g.txt"; then
      echo "ok    shader cache cold == warm $t $g ($n0 entries)"
   else echo "FAIL  shader cache cold != warm $t $g (entries $n0 -> $(ls "$CW" | wc -l)):"; diff -u "$GDIR/cw-cold-$t-$g.txt" "$GDIR/cw-warm-$t-$g.txt" | head -20 | sed 's/^/        /'; fail=1; fi
done; done
# teeth: an entry whose immediates differ (what a stale entry of an older lowering looks like) must change the warm dump
CW=$OUT/cw-cache-vk_draw-v11
for f in "$CW"/*.bin; do python3 -I -c 'import sys; b=bytearray(open(sys.argv[1],"rb").read()); b[-1]^=0x5a; open(sys.argv[1],"wb").write(b)' "$f"; done
if cwdump vk_draw v11 "$CW" cw-bad-vk_draw-v11 && ! cmp -s "$GDIR/cw-cold-vk_draw-v11.txt" "$GDIR/cw-bad-vk_draw-v11.txt"; then echo "ok    cache cold/warm self-test fires (damaged entries change the dump)"
else echo "FAIL  cache cold/warm self-test: damaged cache entries did not change the vk_draw dump"; fail=1; fi
rm -rf "$OUT"/cw-cache-* "$GDIR"/cw-*
# The cache key knows only MALI_COMPILE_VERSION: lowering sources and the recorded version must move together.
if [ $bless = 1 ]; then "$V/tests/compile-guard.sh" --bless
elif out=$("$V/tests/compile-guard.sh" 2>&1); then echo "ok    $out"; else echo "FAIL  compile guard:"; echo "$out" | sed 's/^/        /'; fail=1; fi
# The rules must have teeth: each one is made to fire on a deliberately damaged dump / fault-injected backend.
out=$(MALI_GEOM_SIZE=4194304 MALI_FAKE_DUMP="$GDIR/geom.jsonl" MALI_BACKEND=fake MALI_FAKE_GPU=v13 timeout 120 "$OUT/t_vk_draw" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader" 2>&1 >/dev/null)
out=$(GOLDEN_ARCH=13 python3 -I "$V/tests/golden.py" "$GDIR/geom.jsonl" "$GDIR/geom.txt" 2>&1)
if grep -q "R-GEOM" <<<"$out"; then echo "ok    rule self-test R-GEOM fires (4 MiB geometry buffer)"; else echo "FAIL  rule self-test: R-GEOM did not fire on MALI_GEOM_SIZE=4194304"; fail=1; fi
out=$(MALI_FAKE_BADFLAGS=1 MALI_BACKEND=fake MALI_FAKE_GPU=v13 timeout 120 "$OUT/t_vk_meta" --direct "$OUT/libvulkan_mali.so" 2>&1)
if grep -q "RULE VIOLATION" <<<"$out"; then echo "ok    rule self-test R-FLAGS fires (EVENT|CACHED BO rejected)"; else echo "FAIL  rule self-test: R-FLAGS did not fire with MALI_FAKE_BADFLAGS=1"; fail=1; fi
out=$(python3 -I "$V/tests/selftest.py" "$GDIR/vk_draw-v11.jsonl" "$GDIR/vk_draw-v13.jsonl" "$GDIR" 2>&1 | grep -v '^warn:'); echo "$out"
grep -q '^FAIL' <<<"$out" && fail=1
rm -f "$GDIR/geom.jsonl"

# Standalone offline tests (each builds itself against the Mesa tree, then runs; they exit non-zero on failure).
if [ "$SAN" = 1 ]; then
   echo "skip  build-t_*.sh tests (not sanitized)"
else
   for t in bc bopool cmd compile desc image memsync meta qpool query txf_cc; do run "t_$t" 0 "$V/tests/build-t_$t.sh"; done
fi

# v10 (Mali-G610, Poco X4 GT): every fake-backend case above, once more on MALI_FAKE_GPU=v10.
G=v10; F="MALI_BACKEND=fake MALI_FAKE_GPU=v10"; DL="--direct $OUT/libvulkan_mali.so"
run "loader v10"      0 $LOADER_ENV $F "$OUT/t_icd" --expect-name "Mali-G610"
run "direct v10"      0 $F "$OUT/t_icd" $DL --expect-name "Mali-G610"
run "vk_info v10"     0 $F "$OUT/t_vk_info" $DL
run "dxvk_caps v10"   0 $F "$OUT/t_dxvk_caps" $DL --expect-missing "$DXVK_MISSING"
run "steam_caps v10"   0 $F "$OUT/t_steam_caps" $DL --expect-missing "$STEAM_MISSING" --expect-zink-missing "$ZINK_MISSING"
run "vk_trace v10"    0 $F "$OUT/t_vk_trace" $DL
run "angle_seq v10"   0 $F "$OUT/t_angle_seq" $DL
run "queue all v10"   0 $F $TQ
run "resources v10"   0 $F "$OUT/t_resources"
run "vk_draw v10"       0 $F "$OUT/t_vk_draw" $DL --spv-dir "$V/shader" --loop 200
run "vk_draw many v10"  0 $F "$OUT/t_vk_draw" $DL --spv-dir "$V/shader" --only many_draws
run "vk_draw reset v10" 0 $F "$OUT/t_vk_draw" $DL --spv-dir "$V/shader" --loop 1000 --loop-reset
run "zink host triangle v10" 0 "$V/tests/zink-host.sh" "$OUT" v10
# X11 WSI (skips itself without gamescope, t_vk_xcb, or an ICD built from a Mesa tree with -Dplatforms=x11): xcb swapchain + glxinfo -B via zink
for g in v10 v13; do run "x11 wsi host $g" 0 "$V/tests/xwsi-host.sh" "$OUT" $g; done
run "vk_feat v10"     0 $F "$OUT/t_vk_feat" $DL --spv-dir "$V/shader"
run "vk_compute v10"  0 $F "$OUT/t_vk_compute" $DL --spv-dir "$V/shader" --loop 200
run "vk_meta v10"     0 $F "$OUT/t_vk_meta" $DL --loop 20
cc -std=gnu11 -I"$V/../libkbase/include" -I"$V/../libkbase/src" -o "$OUT/t_v10_cs" "$V/tests/t_v10_cs.c" "$V/../libkbase/src/draw.c" && run "v10 cs/zsd packers" 0 "$OUT/t_v10_cs" || { echo "FAIL  t_v10_cs build"; fail=1; }

# Exported symbols: the three loader entry points and nothing else.
syms=$(nm -D --defined-only "$OUT/libvulkan_mali.so" | awk '{print $3}' | grep -v '^$' | sort | tr '\n' ' ')
want="vk_icdGetInstanceProcAddr vk_icdGetPhysicalDeviceProcAddr vk_icdNegotiateLoaderICDInterfaceVersion "
if [ "$syms" = "$want" ]; then echo "ok    exported symbols"; else echo "FAIL  exported symbols: $syms"; fail=1; fi

# No LGPL libelf on the link line (R3-PLAN.md section 7).
if readelf -d "$OUT/libvulkan_mali.so" | grep NEEDED | grep -qi libelf; then echo "FAIL  libelf in NEEDED"; fail=1; else echo "ok    no libelf in NEEDED"; fi

# No PanVK code in the binary.
if nm "$OUT/libvulkan_mali.so" 2>/dev/null | grep -qi 'panvk'; then echo "FAIL  panvk symbols linked"; fail=1; else echo "ok    no panvk symbols"; fi

if command -v vulkaninfo >/dev/null; then
   out=$(env $LOADER_ENV MALI_BACKEND=fake MALI_FAKE_GPU=v13 vulkaninfo --summary 2>&1); rc=$?
   if [ $rc -eq 0 ] && grep -q "Mali-G925 MC11 (mali-own, fake backend)" <<<"$out"; then echo "ok    vulkaninfo --summary"; else echo "FAIL  vulkaninfo --summary (exit $rc)"; fail=1; echo "$out" | tail -20; fi
   out=$(env $LOADER_ENV MALI_BACKEND=fake MALI_FAKE_GPU=v11 vulkaninfo 2>&1); rc=$?
   if [ $rc -eq 0 ] && grep -q "maxImageDimension2D" <<<"$out"; then echo "ok    vulkaninfo (full)"; else echo "FAIL  vulkaninfo full (exit $rc)"; fail=1; echo "$out" | tail -20; fi
else
   echo "skip  vulkaninfo (not installed)"
fi

# progress diagram (docs/progress.svg) is current: derived from the tree, offline, well under a second
if [ -n "${MALI_SKIP_PROGRESS_CHECK:-}" ]; then echo "skip  progress diagram (CI refreshes it on main)"
elif out=$(python3 "$V/../tools/progress.py" --check 2>&1); then echo "ok    progress diagram"; else echo "FAIL  progress diagram"; echo "$out" | sed 's/^/        /'; fail=1; fi

# no private identifiers (serials, home paths, IPs, emails) in tracked files
if env -u LD_PRELOAD "$V/../tools/private-lint.sh" >/dev/null; then echo "ok    private-lint"; else echo "FAIL  private-lint"; env -u LD_PRELOAD "$V/../tools/private-lint.sh" | sed 's/^/        /'; fail=1; fi

# opt-in: host dEQP-VK gate (slow; needs the host deqp-vk build, see deqp/README.md)
if [ $deqp_host = 1 ]; then
   hd=${DEQP_VK:-$V/../third_party/build-vk-x86-host/external/vulkancts/modules/vulkan/deqp-vk}
   if [ ! -x "$hd" ]; then echo "skip  host dEQP (--deqp-host): no deqp-vk at ${hd#$V/../} -- build it first, see deqp/README.md"
   else
      [ -f "$V/out/native-san/libvulkan_mali.so" ] || (MALI_SAN=1 MALI_TARGET=native-san "$V/build.sh" >/dev/null 2>&1)
      out=$(timeout 900 "$V/deqp/host-run.sh" 2>&1); rc=$?
      if [ $rc -eq 0 ]; then echo "ok    host dEQP (no new Crash/Timeout/NotSupported)"; else echo "FAIL  host dEQP gate"; echo "$out" | grep -v '^  v1[13]: ' | tail -25 | sed 's/^/        /'; fail=1; fi
   fi
fi

[ $fail = 0 ] && echo OK || { echo FAILED; exit 1; }
