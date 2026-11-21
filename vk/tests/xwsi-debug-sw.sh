#!/bin/bash
# SPDX-License-Identifier: MIT
# MESA_VK_WSI_DEBUG=sw on an X server that HAS DRI3 (STEAM-GAPS.md "X11 WSI"; regression #323).
#
# Mesa's software request is an input to wsi_device->sw: wsi_common.c:89 sets
# `wsi->sw = device_options->sw_device || (WSI_DEBUG & WSI_DEBUG_SW)`, and wsi_common.c:221 fetches the
# dma-buf callbacks (GetMemoryFdKHR) only when it is FALSE at init. #323 made the X11 no-DRI3 decision a
# per-call value derived from mali_wsi_use_sw()/pdev->wsi_x11_sw, which on a DRI3 server is false: the
# device was built for software (no GetMemoryFdKHR), the swapchain was then created on the dma-buf path,
# and wsi_common_x11 dereferenced the NULL export callback -> SIGSEGV in the client (CMF Mali-G615 and X8
# Mali-G925 on integration@20bb6c6; before #323 the same env gave "x11 present: shm=1 putimage=0").
#
# Here: gamescope's own Xwayland (it HAS DRI3 - xdpyinfo is checked), MESA_VK_WSI_DEBUG=sw, MALI_WSI=hw so
# that the only reason to be in software is Mesa's request. t_vk_xcb must present through our MIT-SHM
# segment (shm=1) and write a MALI_WSI_DUMP frame; a crash (exit 139) or a dma-buf swapchain fails.
# Usage: xwsi-debug-sw.sh <out dir with libvulkan_mali.so, mali_icd.json, t_vk_xcb> [gpu v10|v11|v13]
# Skips (exit 0, "skipped") without gamescope/Xwayland/xdpyinfo/t_vk_xcb.
set -u
OUT=$1; GPU=${2:-v10}
T=$(cd "$(dirname "$0")" && pwd); V=$(cd "$T/.." && pwd)
[ -x "$OUT/t_vk_xcb" ] || { echo "skipped: no t_vk_xcb in $OUT"; exit 0; }
for c in gamescope Xwayland xdpyinfo; do command -v $c >/dev/null || { echo "skipped: no $c"; exit 0; }; done
OUT=$(cd "$OUT" && pwd)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT

SV=$V
cat >"$W/inner.sh" <<INNER
#!/bin/sh
xdpyinfo -display \$DISPLAY >"$W/xdpy.txt" 2>&1 || { echo "FAIL: no X server inside gamescope"; echo 1 >"$W/rc"; exit 1; }
# The point of this case is a server that DOES have DRI3: on the no-DRI3 path the crash cannot happen.
grep -qx '[[:space:]]*DRI3' "$W/xdpy.txt" || { echo "skipped: this X server has no DRI3 (xwsi-nodri3 covers it)"; echo 77 >"$W/rc"; exit 0; }
echo "x11-dri3: server has DRI3"
export MALI_BACKEND=fake MALI_FAKE_GPU=$GPU
export VK_DRIVER_FILES="$OUT/mali_icd.json" VK_ICD_FILENAMES="$OUT/mali_icd.json"
export MALI_WSI=hw MALI_WSI_DEBUG=1 MALI_SOFT=1   # MALI_SOFT: the fake backend clears on the CPU, so --pixels sees the frame
export MESA_VK_WSI_DEBUG=sw MALI_WSI_DUMP="$W/frame" MALI_WSI_DUMP_EVERY=1
timeout 120 "$OUT/t_vk_xcb" --direct "$OUT/libvulkan_mali.so" --pixels >"$W/xcb.log" 2>&1
echo \$? >"$W/rc"
INNER
chmod +x "$W/inner.sh"
timeout 200 gamescope --backend headless -W 640 -H 480 -- "$W/inner.sh" >"$W/gs.log" 2>&1
rc=$(cat "$W/rc" 2>/dev/null || echo 1)
if [ "$rc" = 77 ]; then echo "skipped: $(grep -m1 skipped "$W/xcb.log" "$W/gs.log" 2>/dev/null | head -1)"; exit 0; fi
grep -vE 'alvr|CAP_SYS_NICE|Performance will|radvk|radv|xkbcomp|^>|^Errors from' "$W/gs.log" | grep -vE '^\[gamescope\]' | head -8
grep -E "x11-dri3|mali_vk: (x11 present|wsi create_swapchain)" "$W/xcb.log" | head -4
[ "$rc" = 0 ] || { echo "FAIL: t_vk_xcb exit $rc (139 = the #323 crash: software device, dma-buf swapchain, NULL GetMemoryFdKHR)"; tail -6 "$W/xcb.log"; exit 1; }
grep -q "x11 present: shm=1" "$W/xcb.log" || { echo "FAIL: Mesa's MESA_VK_WSI_DEBUG=sw did not give the MIT-SHM present path:"; grep -E "present path|create_swapchain" "$W/xcb.log" | head -3; exit 1; }
grep -q "platform=xcb .* sw=1 " "$W/xcb.log" || { echo "FAIL: the X11 swapchain was not created on the software path"; grep "create_swapchain" "$W/xcb.log" | head -2; exit 1; }
ls "$W"/frame*.ppm >/dev/null 2>&1 || { echo "FAIL: MALI_WSI_DUMP wrote no frame (the present did not go through our SHM segment)"; exit 1; }
# The dump names the process and the chain, <prefix>-<comm>-<pid>-sc<N>-<frame>.ppm, and one .txt line per dump carries
# comm, pid, platform, surface, size, VkFormat number and name, layout and the mean. Before, the name was
# <prefix>-NNNNN.ppm from a per-process counter, so Steam's UI and a game overwrote each other's frames.
ls "$W"/frame-t_vk_xcb-*-sc0-*.ppm >/dev/null 2>&1 \
   || { echo "FAIL: dump files are not named <prefix>-<comm>-<pid>-sc<N>-<frame>.ppm:"; ls "$W" | head -5; exit 1; }
grep -qE "^dump frame=[0-9]+ sc=0 comm=t_vk_xcb pid=[0-9]+ platform=xcb surface=0x[0-9a-f]+ [0-9]+x[0-9]+ format=44\(VK_FORMAT_B8G8R8A8_UNORM\) layout=linear .*mean=51\.0,102\.0,204\.0 file=" "$W/frame.txt" \
   || { echo "FAIL: the .txt line does not describe the dumped frame (comm/pid/surface/format/layout/mean):"; sed -n "1,2p" "$W/frame.txt"; exit 1; }
echo "ok    debug sw on a DRI3 server ($GPU): MESA_VK_WSI_DEBUG=sw kept the MIT-SHM path, $(ls "$W"/frame*.ppm | wc -l) dump(s), mean 51,102,204"
