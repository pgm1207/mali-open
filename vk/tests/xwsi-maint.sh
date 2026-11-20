#!/bin/bash
# SPDX-License-Identifier: MIT
# Host test of the WSI contract (see t_wsi_maint.c): an X server from `gamescope --backend headless` (its Xwayland has
# DRI3/Present/MIT-SHM) and our ICD on the fake backend. Checks VK_EXT_swapchain_maintenance1 (create with the present
# modes/scaling structs, per-present mode switch, present fence, vkReleaseSwapchainImagesEXT) and DRM modifiers for
# NV12/BGRA8/RGBA8 (LINEAR with the right memory-plane count and both NV12 plane layouts).
# Usage: xwsi-maint.sh <out dir with libvulkan_mali.so, mali_icd.json, t_wsi_maint> [gpu v10|v11|v13]
# Skips (exit 0, "skipped") without gamescope/Xwayland or when the ICD lacks VK_KHR_xcb_surface.
set -u
OUT=$1; GPU=${2:-v11}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
command -v gamescope >/dev/null && command -v Xwayland >/dev/null || { echo "skipped: no gamescope/Xwayland for a headless X server"; exit 0; }
[ -x "$OUT/t_wsi_maint" ] || { echo "skipped: no t_wsi_maint in $OUT"; exit 0; }
cat >"$W/inner.sh" <<EOF
#!/bin/sh
export ENABLE_GAMESCOPE_WSI=0 MALI_BACKEND=fake MALI_FAKE_GPU=$GPU VK_DRIVER_FILES="$OUT/mali_icd.json" VK_ICD_FILENAMES="$OUT/mali_icd.json"
timeout 60 "$OUT/t_wsi_maint" --direct "$OUT/libvulkan_mali.so" >"$W/maint.log" 2>&1; echo \$? >"$W/maint.rc"
EOF
chmod +x "$W/inner.sh"
timeout 150 gamescope --backend headless -W 640 -H 480 -- "$W/inner.sh" >"$W/gs.log" 2>&1
[ -f "$W/maint.rc" ] || { echo "FAIL: the test did not run inside gamescope"; tail -20 "$W/gs.log"; exit 1; }
grep -v alvr "$W/maint.log"
rc=$(cat "$W/maint.rc")
if [ "$rc" = 77 ]; then echo "skipped: $OUT/t_wsi_maint exit 77"; exit 0; fi
[ "$rc" = 0 ] && grep -q '^PASS' "$W/maint.log" || { echo "FAIL: t_wsi_maint rc=$rc"; exit 1; }
echo "xwsi-maint: OK"
