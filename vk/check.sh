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
   # LD_PRELOAD also reaches gcc/sed/bash spawned by the cases; their own leaks are not ours.
   export LSAN_OPTIONS=suppressions=$V/tests/lsan.supp:print_suppressions=0
   export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
fi
# The compile cache (mali_compile.c, ~/.cache by default) must not decide a golden: a stale entry from an older compiler made main's own goldens
# fail on a clean cache (vk_draw FS FAU 0x40 vs the fresh 0x09000000). Every check run starts from an empty private cache.
# MALI_OPT_GCM=1 bypasses the cache (mali_compile.c use_cache), which the cold/warm and damaged-entry checks below need active: do not inherit it.
unset MALI_OPT_GCM
export MALI_SHADER_CACHE=$OUT/shader-cache; rm -rf "$MALI_SHADER_CACHE"; mkdir -p "$MALI_SHADER_CACHE"
if [ $build = 1 ]; then env -u LD_PRELOAD "$V/build.sh" >/dev/null || { echo "FAILED: build"; exit 1; }; fi

fail=0
export MALI_DIRTY_CHECK=1   # trap: build_draw_descs aborts when the record-time dirty mask reuses state the old compares would not
# Only our ICD, no implicit layers, so the result does not depend on the laptop's drivers.
# Number of DXVK-required core/1.1-1.3 features still false (of 47); lower it only with a passing test for the new feature.
DXVK_MISSING=3            # measured with t_dxvk_caps. sampleRateShading is advertised at 1 sample (t_vk_feat samplerate); MALI_EXPERIMENTAL_MSAA only adds 4x draws
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
# heap 0 is the commit cap: with a cap below the fixed 1 GiB default, maxMemoryAllocationSize must be clamped to
# the heap we report (VkPhysicalDeviceLimits), and the whole heap must still be allocatable.
run "vk_mem cap 256 MiB v11" 0 MALI_VRAM_CAP_MB=256 MALI_BACKEND=fake MALI_FAKE_GPU=v11 "$OUT/t_vk_mem" --direct "$OUT/libvulkan_mali.so"
# The dma-buf fd check (mali_dmabuf.c) both ways: statfs magic, the ioctl fallback with the magic arm forced off, and
# the refusals (memfd, pipe, regular file, /dev/kvm). SKIPs the accept cases on a host with no dma-buf provider.
run "dmabuf fd" 0 "$OUT/t_dmabuf"
for g in v11 v13; do run "vk_vram $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_vram" --direct "$OUT/libvulkan_mali.so"; done
for g in v11 v13; do run "vk_qerr $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_qerr" --direct "$OUT/libvulkan_mali.so"; done
for g in v11 v13; do run "dxvk_wsi $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_dxvk_wsi" --direct "$OUT/libvulkan_mali.so"; done
for g in v11 v13; do run "dxvk_caps $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_dxvk_caps" --direct "$OUT/libvulkan_mali.so" --expect-missing "$DXVK_MISSING"; done
for g in v11 v13; do run "dxvk_api $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_dxvk_api" --direct "$OUT/libvulkan_mali.so"; done
for g in v11 v13; do run "angle_seq $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_angle_seq" --direct "$OUT/libvulkan_mali.so"; done
run "nokopper shim"      0 sh "$V/tests/t_nokopper.sh"
run "banner status file" 0 sh "$V/tests/t_banner.sh" "$OUT"
# Steam session (STEAM-GAPS.md): none of gamescope's vkCreateDevice requirements is missing any more.
STEAM_MISSING=0
# zink (STEAM-GAPS.md): logicOp is on main; EXT_physical_device_drm is advertised only together with a DRM alias of the
# GPU node (t_vk_drm below), which the fake backend has none of, so it stays in this count.
ZINK_MISSING=1
for g in v11 v13; do run "vk_headless $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_headless" --direct "$OUT/libvulkan_mali.so"; done
for g in v11 v13; do run "steam_caps $g" 0 -u BL_DRM_STANDIN MALI_DRM_DIR=/nonexistent MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_steam_caps" --direct "$OUT/libvulkan_mali.so" --expect-missing "$STEAM_MISSING" --expect-zink-missing "$ZINK_MISSING"; done
# VK_EXT_physical_device_drm: the extension exists only with a node that stat() proves is the GPU char device (/dev/mali0;
# the guest's /dev/dri/renderD<N> alias, BL_DRM_STANDIN). Symlinks stand in for the guest's nodes; /dev/null is 1:3.
DRMR=$OUT/drm-render; DRMB=$OUT/drm-both; DRMX=$OUT/drm-bad; DRMY=$OUT/drm-x4
rm -rf "$DRMR" "$DRMB" "$DRMX" "$DRMY"; mkdir -p "$DRMR" "$DRMB" "$DRMX" "$DRMY"
ln -s /dev/null "$DRMR/renderD42"; ln -s /dev/zero "$DRMR/renderD43"; ln -s /dev/zero "$DRMR/card0"
ln -s /dev/null "$DRMB/renderD42"; ln -s /dev/null "$DRMB/card1"
ln -s /dev/zero "$DRMX/renderD43"
# X4's guest: /dev/dri holds card0 (226:0) and nothing else - no alias of the GPU node at all (1:5 here, another device).
ln -s /dev/zero "$DRMY/card0"
DRMT="$OUT/t_vk_drm --direct $OUT/libvulkan_mali.so"
run "drm absent v11"        0 -u BL_DRM_STANDIN $LOADER_ENV MALI_DRM_DIR=/nonexistent MALI_BACKEND=fake MALI_FAKE_GPU=v11 $DRMT --none
run "drm render v11"       0 $LOADER_ENV BL_DRM_STANDIN=/dev/null MALI_DRM_AUTO_STANDIN=0 MALI_DRM_DIR="$DRMR" MALI_BACKEND=fake MALI_FAKE_GPU=v11 $DRMT --node "$DRMR/renderD42"
run "drm render v13"       0 $LOADER_ENV BL_DRM_STANDIN=/dev/null MALI_DRM_AUTO_STANDIN=0 MALI_DRM_DIR="$DRMR" MALI_BACKEND=fake MALI_FAKE_GPU=v13 $DRMT --node "$DRMR/renderD42"
# Auto stand-in: the same node without MALI_DRM_AUTO_STANDIN=0 is reported as primary and render (stock app path).
run "drm auto standin v11" 0 $LOADER_ENV BL_DRM_STANDIN=/dev/null MALI_DRM_AUTO_STANDIN=1 MALI_DRM_DIR="$DRMR" MALI_BACKEND=fake MALI_FAKE_GPU=v11 $DRMT --node "$DRMR/renderD42"
# The stock app's own values (SessionService.kt:1044-1053): /dev/mali0 does not exist under that name here, so the
# driver has to resolve it to the /dev/dri alias of the GPU node we opened - render *and* primary.
run "drm stock standin v11" 0 $LOADER_ENV BL_DRM_STANDIN=/dev/null MALI_DRM_AUTO_STANDIN=1 DD_OPEN_DRIVER=1 DD_DRM_STANDIN=/dev/mali0 MALI_DRM_DIR="$DRMR" MALI_BACKEND=fake MALI_FAKE_GPU=v11 $DRMT --node "$DRMR/renderD42" --primary "$DRMR/renderD42"
# X4 (rung 1): the guest's /dev/dri is card0 and nothing else, so the app's stand-in (/dev/mali0) has no alias.
# Its rdev is the node the backend opened (BL_DRM_STANDIN stands for that opened node: /dev/null = 1:3), so it is
# still our own device and is reported as render *and* primary - otherwise gamescope stops at "physical device has
# no primary node" (session.log:54). Without the rdev equality this case reports no identity.
run "drm standin is device v11" 0 $LOADER_ENV BL_DRM_STANDIN=/dev/null MALI_DRM_DIR="$DRMY" MALI_BACKEND=fake MALI_FAKE_GPU=v11 DD_DRM_STANDIN=/dev/null $DRMT --node /dev/null --primary /dev/null
# #291 survives: a stand-in naming an unrelated node is still dropped. The extension is advertised (the app asked
# for it), but no identity is reported (/dev/full is 1:7, our node 1:3, and no /dev/dri entry matches either).
run "drm standin unrelated v11" 0 $LOADER_ENV BL_DRM_STANDIN=/dev/null MALI_DRM_DIR="$DRMY" MALI_BACKEND=fake MALI_FAKE_GPU=v11 DD_DRM_STANDIN=/dev/full $DRMT --no-identity
# The no-alias exception needs the app path to be a char device whose rdev is our node: a path stat() cannot use as a
# device (/etc is a directory) must still be dropped, even though the saved rdev equals our node.
run "drm standin non-chardev v11" 0 $LOADER_ENV BL_DRM_STANDIN=/dev/null MALI_DRM_DIR="$DRMY" MALI_BACKEND=fake MALI_FAKE_GPU=v11 DD_DRM_STANDIN=/etc $DRMT --no-identity
# And with no stand-in nothing is reported, even when /dev/dri exists: card0 there is another device.
run "drm no standin v11"     0 -u BL_DRM_STANDIN -u DD_DRM_STANDIN MALI_DRM_DIR="$DRMY" MALI_BACKEND=fake MALI_FAKE_GPU=v11 $DRMT --none
run "drm render+primary"   0 $LOADER_ENV BL_DRM_STANDIN=/dev/null MALI_DRM_DIR="$DRMB" MALI_BACKEND=fake MALI_FAKE_GPU=v11 $DRMT --node "$DRMB/renderD42" --primary "$DRMB/card1"
run "drm mismatch absent"  0 $LOADER_ENV BL_DRM_STANDIN=/dev/null MALI_DRM_DIR="$DRMX" MALI_BACKEND=fake MALI_FAKE_GPU=v11 $DRMT --none
# With a node identity zink's list loses that item: only logicOp remains (0 hard).
run "steam_caps v11 drm node" 0 $LOADER_ENV BL_DRM_STANDIN=/dev/null MALI_DRM_DIR="$DRMR" MALI_BACKEND=fake MALI_FAKE_GPU=v11 "$OUT/t_steam_caps" --direct "$OUT/libvulkan_mali.so" --expect-missing "$STEAM_MISSING" --expect-zink-missing 0
rm -rf "$DRMR" "$DRMB" "$DRMX" "$DRMY"
# 4x MSAA is opt-in (MALI_EXPERIMENTAL_MSAA=1): the plain `vk_info $g` run above asserts the default 1x, these assert the gate and
# that switching it off again (0) matches the default.
for g in v11 v13; do run "dxvk_caps $g msaa gate" 0 MALI_EXPERIMENTAL_MSAA=1 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_dxvk_caps" --direct "$OUT/libvulkan_mali.so" --expect-missing "$DXVK_MISSING_MSAA"; done
for g in v11 v13; do run "vk_info $g msaa gate" 0 MALI_EXPERIMENTAL_MSAA=1 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_info" --direct "$OUT/libvulkan_mali.so"; done
for g in v11 v13; do run "vk_info $g msaa off" 0 MALI_EXPERIMENTAL_MSAA=0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_info" --direct "$OUT/libvulkan_mali.so"; done

# M4: memory, sync, queue (fake backend; MALI_FAKE_SLOW_US makes the fences really block).
TQ="$OUT/t_queue --direct $OUT/libvulkan_mali.so"
run "queue all v11"      0 MALI_BACKEND=fake MALI_FAKE_GPU=v11 $TQ
run "queue teardown v13"  0 MALI_BACKEND=fake MALI_FAKE_GPU=v13 MALI_FAKE_SLOW_US=400000 $TQ --only teardown
run "queue all v13 slow" 0 MALI_BACKEND=fake MALI_FAKE_GPU=v13 MALI_FAKE_SLOW_US=200 $TQ --submits 1000
# present ordering (queue-order): the commit never precedes the frame; with a fence it may (MALI_PRESENT_FENCE forces the kernel capability)
for m in 0 1; do run "present order fence=$m" 0 MALI_BACKEND=fake MALI_FAKE_SLOW_US=100000 MALI_PRESENT_FENCE=$m $TQ --only order; done
run "timeout-0 poll does not queue behind a blocking wait" 0 MALI_BACKEND=fake MALI_FAKE_SLOW_US=300000 $TQ --only poll0
run "signal-less submit skips the fence" 0 MALI_BACKEND=fake MALI_FAULT_INJECT=5 $TQ --only nofence
run "MALI_FENCE_ALWAYS restores it (expected fail)" 1 MALI_BACKEND=fake MALI_FAULT_INJECT=5 MALI_FENCE_ALWAYS=1 $TQ --only nofence
for n in 1 50; do run "fault inject $n" 0 MALI_BACKEND=fake MALI_FAULT_INJECT=$n $TQ --only fault; done
# WSI audit regressions: fd validation, sync export waits for a queued signal, DRM modifier/bind checks, failed submit still signals
for c in wsifd syncexport wsiimg; do run "wsi $c" 0 MALI_BACKEND=fake $TQ --only $c; done
run "wsi failsignal" 0 MALI_BACKEND=fake MALI_FAULT_INJECT=5 $TQ --only failsignal

# M15: frame profiler (mali_fprof.c) on the fake backend. The last case leaves MALI_FRAME_PROF unset and requires
# silent output: the unset path is the normal driver path, and the test fails if anything is printed or written.
for g in v11 v13; do
   run "frameprof $g"       0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_frameprof" --direct "$OUT/libvulkan_mali.so"
   run "frameprof $g unset" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_frameprof" --direct "$OUT/libvulkan_mali.so" --off
done

# M5/M7: buffers, images, views, samplers, memory binding, descriptor sets, updates, templates (white-box, static link).
for g in v11 v13; do run "resources $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_resources"; done

# M9: client draws through the ICD (fake backend: API sequence, compile, CS build and submit; pixels only on hardware).
for g in v11 v13; do
   run "vk_draw $g"       0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_draw" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader" --loop 200
   run "vk_draw many $g"  0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_draw" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader" --only many_draws
   run "vk_draw sets8 $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_draw" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader" --only sets8
   run "vk_draw reset $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_draw" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader" --loop 1000 --loop-reset
done

# Reference scene (docs/desktop-gpu.md): the real scene, recorded through the ICD with an OPTIMAL-tiled (AFBC on Mali)
# colour attachment read back by vkCmdCopyImageToBuffer. The fake backend executes nothing, so this is the record-side gate
# that keeps the client honest; the RADV reference run and the phone comparison are documented commands, not check.sh cases.
for g in v11 v13; do
   run "vk_ref $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_ref" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader"
done
run "refdiff selftest" 0 python3 -I "$V/tests/refdiff.py" --selftest

# R4-B2: DXVK-required core feature tests (pixels only on hardware).
for g in v11 v13; do
   run "vk_feat $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_feat" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader"
done

# VK_EXT_provoking_vertex: the fake backend asserts the two hardware copies of "First provoking vertex" (FBD word 11 bit 14, tiler ctx
# word 2 bit 18) in t_cmd --extra (build-t_cmd.sh, below); the t_vk_feat provoking case (flat v0 red / v2 blue) is named here so the
# fake-backend gate cannot silently drop it. Its pixel assertion only runs on hardware (FIRST=red, LAST=blue).
for g in v11 v13; do
   run "vk_feat provoking $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_feat" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader" --only provoking
done

# M11: compute through the ICD (fake backend: compile, CS build and submit; results only on hardware).
for g in v11 v13; do
   run "vk_compute $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_compute" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader" --loop 200
   # A batch of 8 compute pipelines in one call goes through the parallel create path (cache is cold here, so it really compiles). The
   # batch<seq timing assert stays off: MALI_SHADER_CACHE is a path, and a timing check would flake on a loaded host.
   run "vk_compute batch $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_compute" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader" --only batch
   run "vk_meta $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_meta" --direct "$OUT/libvulkan_mali.so" --loop 20
   # A meta op inside a render pass is deferred: recording succeeds, and on hardware the copy/fill runs after the
   # pass; the test asserts the ordering contract, not an immediate refusal.
   run "vk_meta inpass $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_meta" --direct "$OUT/libvulkan_mali.so" --only inpass
   run "vk_meta tiled_reject $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_meta" --direct "$OUT/libvulkan_mali.so" --only tiled_reject
   # packed A8B8G8R8 render target + a 2D view of one slice of a 3D image (zink's 3D render targets)
   run "vk_meta argb8srgb $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_meta" --direct "$OUT/libvulkan_mali.so" --only argb8srgb
   run "vk_meta view3d2d $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_meta" --direct "$OUT/libvulkan_mali.so" --only view3d2d
   # loadOp LOAD of the formats Source-engine/wined3d content uses: D24S8 depth (zs_format 2) and A8B8G8R8 colour.
   run "vk_draw load_d24s8 $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_draw" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader" --only load_depth24s8
   run "vk_draw load_argb8 $g" 0 MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_draw" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader" --only load_colour_argb8
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
# share_idx (tests/check_share_idx.py): the frame-build fast path skips the per-draw register packing when nothing differs
# (mali_cs_emit.c same_draw_words). vkCmdBindIndexBuffer sets no dirty bit, so the index-buffer VA is only protected by being part
# of that key: four draws with identical per-draw words and the index buffer alternating A B A B (an identical pipeline rebind
# before the third) must still write the index-buffer register four times. Explicit only (a new frame would change the goldens).
for g in v11 v13; do
   name=share_idx-$g; rm -f "$GDIR/$name.jsonl"
   out=$(MALI_FAKE_DUMP="$GDIR/$name.jsonl" timeout -k 5 120 env MALI_BACKEND=fake MALI_FAKE_GPU=$g "$OUT/t_vk_draw" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader" --only share_idx 2>&1); rc=$?
   if [ $rc -ne 0 ]; then echo "FAIL  share_idx CS $g (test exit $rc)"; echo "$out" | tail -10 | sed 's/^/        /'; fail=1; continue; fi
   out=$(timeout -k 5 120 python3 -I "$V/tests/check_share_idx.py" "$GDIR/$name.jsonl" "$g" 2>&1); rc=$?
   if [ $rc -eq 0 ]; then echo "ok    share_idx CS $g ($out)"
   else echo "FAIL  share_idx CS $g (missing per-draw index-buffer write: see same_draw_words)"; echo "$out" | sed 's/^/        /'; fail=1; fi
done
# Shader cache, cold vs warm: the same test on an empty private cache and again on the cache that run left must give byte-identical decoded
# streams (a cached shader that yields different GPU state than a fresh compile would hide behind every host run that has a warm cache; see
# tests/compile-guard.sh for the cause that was found). The warm run must really hit the cache (no new entry) and a damaged entry must be seen.
cwdump() { # test, gpu, cache dir, out name
   rm -f "$GDIR/$4.jsonl" "$GDIR/$4.txt"
   MALI_SHADER_CACHE=$3 MALI_FAKE_DUMP="$GDIR/$4.jsonl" timeout -k 5 120 env MALI_BACKEND=fake MALI_FAKE_GPU=$2 "$OUT/t_$1" --direct "$OUT/libvulkan_mali.so" --spv-dir "$V/shader" >/dev/null 2>&1 || return 1
   GOLDEN_ARCH=${2#v} timeout -k 5 120 python3 -I "$V/tests/golden.py" "$GDIR/$4.jsonl" "$GDIR/$4.txt" >/dev/null 2>&1
}
cwpair() { # test, gpu: one cold/warm pair; prints its result lines, exits 1 on failure
   local t=$1 g=$2 fail=0 CW n0 sum0
   CW=$OUT/cw-cache-$t-$g; rm -rf "$CW"; mkdir -p "$CW"
   if ! cwdump $t $g "$CW" cw-cold-$t-$g; then echo "FAIL  cache cold/warm $t $g (cold run failed)"; return 1; fi
   n0=$(ls "$CW" | wc -l); sum0=$(cat "$CW"/*.bin 2>/dev/null | sha256sum)
   if ! cwdump $t $g "$CW" cw-warm-$t-$g; then echo "FAIL  cache cold/warm $t $g (warm run failed)"; return 1; fi
   if [ "$n0" -gt 0 ] && [ "$n0" = "$(ls "$CW" | wc -l)" ] && [ "$sum0" = "$(cat "$CW"/*.bin | sha256sum)" ] && cmp -s "$GDIR/cw-cold-$t-$g.txt" "$GDIR/cw-warm-$t-$g.txt"; then
      echo "ok    shader cache cold == warm $t $g ($n0 entries)"
   else echo "FAIL  shader cache cold != warm $t $g (entries $n0 -> $(ls "$CW" | wc -l)):"; diff -u "$GDIR/cw-cold-$t-$g.txt" "$GDIR/cw-warm-$t-$g.txt" | head -20 | sed 's/^/        /'; fail=1; fi
   return $fail
}
# the four pairs are independent (own cache dir, own output names): run them concurrently, print in the original order
cwpids=""; cwtags=""
for t in vk_draw vk_feat; do for g in v11 v13; do cwpair $t $g >"$GDIR/cw-res-$t-$g.out" 2>&1 & cwpids="$cwpids $!"; cwtags="$cwtags $t:$g"; done; done
set -- $cwpids; for tg in $cwtags; do t=${tg%:*}; g=${tg#*:}; wait "$1" || fail=1; shift; cat "$GDIR/cw-res-$t-$g.out"; done
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
   for t in afbc bc cmd compile desc image memsync meta qpool query txf_cc; do run "t_$t" 0 "$V/tests/build-t_$t.sh"; done
   for t in bc bopool cmd compile desc image memsync meta qpool query txf_cc; do run "t_$t" 0 "$V/tests/build-t_$t.sh"; done
   # Unity 2023.2 capability probe: every format it hands to vkCreateImage, and the swapchain contract (surface rows only when the tree
   # has a WSI platform; the plain build skips them with a message, the x11 run is tests/xwsi-host.sh's job).
   for g in v11 v13; do run "unity_caps $g" 0 OUT_DIR="$OUT" "$V/tests/build-t_unity_caps.sh" $g; done
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
run "vk_draw sets8 v10" 0 $F "$OUT/t_vk_draw" $DL --spv-dir "$V/shader" --only sets8
run "vk_draw reset v10" 0 $F "$OUT/t_vk_draw" $DL --spv-dir "$V/shader" --loop 1000 --loop-reset
run "vk_ref v10"        0 $F "$OUT/t_vk_ref" $DL --spv-dir "$V/shader"
run "zink host triangle v10" 0 "$V/tests/zink-host.sh" "$OUT" v10
# X11 WSI (skips itself without gamescope, t_vk_xcb, or an ICD built from a Mesa tree with -Dplatforms=x11): xcb swapchain + glxinfo -B via zink
for g in v10 v13; do run "x11 wsi host $g" 0 "$V/tests/xwsi-host.sh" "$OUT" $g; done
# The same server without DRI3 (the X4's Xwayland: no DRM node, so MIT-SHM/PutImage is the only present path). zink asks
# surface support first and stops on FALSE (zink_kopper.c:130-132), so the switch has to be taken at that query and not in
# vkCreateSwapchainKHR: this case fails on such a driver with "surface not supported by the graphics queue".
for g in v10 v13; do run "x11 no-dri3 $g" 0 "$V/tests/xwsi-nodri3.sh" "$OUT" $g; done
# ...and it must stay an X11 decision: wsi.sw is device-wide, so the same flip used to turn every Wayland swapchain of the
# process into a wl_shm/CPU one (wsi_common_wayland.c:3940) - on X8 the gamescope WSI layer's swapchain, whose shm (memfd)
# buffers no dma-buf import can take (298 x vkGetMemoryFdPropertiesKHR -> VK_ERROR_INVALID_EXTERNAL_HANDLE in x8-1121-005438).
for g in v10 v13; do run "wl after x11 no-dri3 $g" 0 "$V/tests/xwsi-wl-after-x11.sh" "$OUT" $g; done
# Mesa's own software request (MESA_VK_WSI_DEBUG=sw) on a DRI3 server: the device is built for software (no
# GetMemoryFdKHR), so the X11 swapchain must stay software too - #323 pushed it onto the dma-buf path and
# wsi_common_x11 dereferenced the NULL export callback (SIGSEGV, exit 139, CMF + X8 on integration@20bb6c6).
for g in v10 v13; do run "debug sw on dri3 $g" 0 "$V/tests/xwsi-debug-sw.sh" "$OUT" $g; done
# CR #323 N2/N4/N5 on the X11 path: the software decision is per connection, a blocked acquire stalls nothing, and the
# *2KHR surface queries answer what the 1-forms answer.
for g in v10 v13; do run "two conns $g" 0 "$V/tests/xwsi-two-conns.sh" "$OUT" $g; done
for g in v10 v13; do run "threads $g" 0 "$V/tests/xwsi-threads.sh" "$OUT" $g; done
for g in v10 v13; do run "caps2 $g" 0 "$V/tests/xwsi-caps2.sh" "$OUT" $g; done
for g in v10 v13; do run "wl mock host $g" 0 "$V/tests/wlmock-host.sh" "$OUT" $g; done
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

# Desktop software rasteriser (MALI_SOFT=1, docs/host-harness.md): real frames rendered on the CPU from the frame record and
# compared tile by tile with tests/golden/soft-frame.txt. Deterministic across v10/v11/v13 (the CPU path never looks at the arch).
if [ -x "$OUT/t_soft_frame" ]; then
   if [ $bless = 1 ]; then
      # a failed render under --bless must fail the gate: the golden is then stale or untouched, not blessed
      if out=$(MALI_TARGET=${MALI_TARGET:-native} "$V/tests/soft-frame.sh" --bless 2>&1); then echo "ok    soft frames (golden re-blessed)"
      else echo "FAIL  soft frames (--bless):"; echo "$out" | sed 's/^/        /'; fail=1; fi
   elif out=$(MALI_TARGET=${MALI_TARGET:-native} "$V/tests/soft-frame.sh" 2>&1); then echo "ok    ${out#ok }"
   else echo "FAIL  soft frames:"; echo "$out" | sed 's/^/        /'; fail=1; fi
else
   echo "skip  soft frames (t_soft_frame not built)"
fi

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
