#!/bin/bash
# SPDX-License-Identifier: MIT
# Host test of the deferred image release (design-shm.md, decision A): after each PutImage the driver queues a
# GetGeometry on the same connection and releases the image only when that reply arrives, so
#   1. no present waits on the server (the drain is non-blocking), and
#   2. the app cannot acquire an image whose region the server may still be reading - if every image is gated, the
#      acquire refuses (VK_NOT_READY, timeout 0) instead of handing out a segment still in flight, and returns one only
#      when the gate is lifted, i.e. through the bounded wait in vkAcquireNextImageKHR.
# The MALI_WSI_SHM_HOLD hook (t_vk_xcb --shm-gate-hold) pretends the newest N replies have not arrived. On the old
# driver (immediate release_images) the second acquire returns an image while the server is still reading it.
# Usage: xwsi-shm-gate.sh <out dir with libvulkan_mali.so, mali_icd.json, t_vk_xcb> [gpu v10|v11|v13]
set -u
OUT=$1; GPU=${2:-v10}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
command -v gamescope >/dev/null && command -v Xwayland >/dev/null || { echo "skipped: no gamescope/Xwayland for a headless X server"; exit 0; }
[ -x "$OUT/t_vk_xcb" ] || { echo "skipped: no t_vk_xcb in $OUT"; exit 0; }
cat >"$W/inner.sh" <<EOF
#!/bin/sh
export ENABLE_GAMESCOPE_WSI=0 MALI_BACKEND=fake MALI_FAKE_GPU=$GPU MALI_WSI_DEBUG=1 MALI_WSI=shm
export VK_DRIVER_FILES=$OUT/mali_icd.json VK_ICD_FILENAMES=$OUT/mali_icd.json
timeout 60 "$OUT/t_vk_xcb" --expect-name Mali --shm-gate-hold 3 >"$W/gate.log" 2>&1; echo \$? >"$W/gate.rc"
# the escape hatch must give the old behaviour back (immediate release), so the case proves the gate is what holds
MALI_WSI_SHM_GATE=0 timeout 60 "$OUT/t_vk_xcb" --expect-name Mali --shm-gate-hold 3 >"$W/off.log" 2>&1; echo \$? >"$W/off.rc"
EOF
chmod +x "$W/inner.sh"
timeout 250 gamescope --backend headless -W 640 -H 480 -- "$W/inner.sh" >"$W/gs.log" 2>&1
[ -f "$W/gate.rc" ] || { echo "FAIL: the test did not run inside gamescope"; tail -20 "$W/gs.log"; exit 1; }
grep -E "gate held|gate lifted" "$W/gate.log"
[ "$(cat "$W/gate.rc")" = 0 ] && grep -q '^PASS' "$W/gate.log" \
   || { echo "FAIL: the gate did not hold (rc=$(cat "$W/gate.rc"))"; grep FAIL "$W/gate.log" | head -3; exit 1; }
grep -q "shm+gated" "$W/gate.log" \
   || { echo "FAIL: the present did not take the gated shm path"; grep "wsi present" "$W/gate.log" | head -3; exit 1; }
# MALI_WSI_SHM_GATE=0 restores the immediate release, i.e. exactly the old failure - that is the control.
[ "$(cat "$W/off.rc")" != 0 ] && grep -q "while 3 GetGeometry reply" "$W/off.log" \
   || { echo "FAIL: MALI_WSI_SHM_GATE=0 did not restore the immediate release"; tail -3 "$W/off.log"; exit 1; }
echo "shm gate: present never waits, images are released on the server's reply, acquire refuses a held image"
echo "xwsi-shm-gate: OK"
