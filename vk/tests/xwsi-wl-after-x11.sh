#!/bin/bash
# SPDX-License-Identifier: MIT
# The X11 no-DRI3 decision must stay an X11 decision (STEAM-GAPS.md "X11 WSI"; regression #322 caused on X8).
#
# #322 answers vkGetPhysicalDeviceSurfaceSupportKHR TRUE for an X11 surface on a server without DRI3, so zink/kopper
# takes the MIT-SHM present path (zink_kopper.c:130). That path needs Mesa's wsi_device->sw, which is device-wide,
# and wsi_common_wayland.c:3940 turns EVERY Wayland swapchain into a wl_shm/CPU one while it is set. A process that
# asks about an X11 surface first therefore lost the dma-buf path for its Wayland swapchain too: on X8 that is the
# gamescope WSI layer's swapchain (Steam), and the compositor is then handed shm (memfd) buffers that no dma-buf
# import can take - 298 x "vkGetMemoryFdPropertiesKHR failed (VK_ERROR_INVALID_EXTERNAL_HANDLE)" in the
# x8-1121-005438 session, plus the zink "couldn't allocate memory" lines of the same run.
#
# t_vk_xcb --wl-after-x11: (1) an xcb surface on the no-DRI3 Xwayland must report support TRUE, (2) a Wayland surface
# of the same device (inside gamescope, which has zwp_linux_dmabuf_v1) must still create its swapchain on the dma-buf
# path - the driver prints "platform=wayland ... sw=0" (MALI_WSI_DEBUG=1) and sw=1 before the fix.
# Usage: xwsi-wl-after-x11.sh <out dir with libvulkan_mali.so, mali_icd.json, t_vk_xcb> [gpu v10|v11|v13]
# Skips (exit 0, "skipped") without gamescope/Xwayland/xdpyinfo/t_vk_xcb/wayland.
set -u
OUT=$1; GPU=${2:-v11}
T=$(cd "$(dirname "$0")" && pwd); V=$(cd "$T/.." && pwd)
[ -x "$OUT/t_vk_xcb" ] || { echo "skipped: no t_vk_xcb in $OUT"; exit 0; }
for c in gamescope Xwayland xdpyinfo; do command -v $c >/dev/null || { echo "skipped: no $c"; exit 0; }; done
OUT=$(cd "$OUT" && pwd)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT

SV=$V
cat >"$W/inner.sh" <<INNER
#!/bin/sh
# gamescope starts its own Xwayland on :1; a second one on :77, glamor off, gives the server without DRI3.
sock=\${GAMESCOPE_WAYLAND_DISPLAY:-}
[ -n "\$sock" ] || sock=\$(ls "\$XDG_RUNTIME_DIR" 2>/dev/null | grep -E '^gamescope-[0-9]+\$' | head -1)
[ -n "\$sock" ] || { echo "FAIL: no gamescope Wayland socket in \$XDG_RUNTIME_DIR"; echo 1 >"$W/rc"; exit 1; }
export WAYLAND_DISPLAY="\$sock"
XWAYLAND_NO_GLAMOR=1 Xwayland :77 -nolisten tcp -extension DRI3 >"$W/xw.log" 2>&1 &
i=0; while [ \$i -lt 60 ]; do xdpyinfo -display :77 >/dev/null 2>&1 && break; sleep 0.25; i=\$((i+1)); done
xdpyinfo -display :77 >"$W/xdpy.txt" 2>&1 || { echo "FAIL: no X server on :77"; tail -5 "$W/xw.log"; echo 1 >"$W/rc"; exit 1; }
if grep -qx '[[:space:]]*DRI3' "$W/xdpy.txt"; then
   echo "FAIL: the test X server has DRI3, so nothing is tested"; echo 1 >"$W/rc"; exit 1
fi
echo "x11-no-dri3: server on :77 has no DRI3"
export DISPLAY=:77 MALI_BACKEND=fake MALI_FAKE_GPU=$GPU
export VK_DRIVER_FILES=$OUT/mali_icd.json VK_ICD_FILENAMES=$OUT/mali_icd.json
# MALI_SOFT=1: the fake backend renders on the CPU, so the cleared Wayland dma-buf image really holds its pixels and the
# MALI_WSI_DUMP written at vkQueuePresentKHR can be checked, not only its metadata. MALI_WSI_DUMP_EVERY=1: one frame.
export MALI_WSI=hw MALI_WSI_DEBUG=1 MALI_SOFT=1 MALI_WSI_DUMP="$W/wl" MALI_WSI_DUMP_EVERY=1
timeout 120 "$OUT/t_vk_xcb" --direct "$OUT/libvulkan_mali.so" --wl-after-x11 >"$W/xcb.log" 2>&1
echo \$? >"$W/rc"
INNER
chmod +x "$W/inner.sh"
timeout 200 gamescope --backend headless -W 640 -H 480 -- "$W/inner.sh" >"$W/gs.log" 2>&1
rc=$(cat "$W/rc" 2>/dev/null || echo 1)
if [ "$rc" = 77 ]; then
   grep -qE "skipped: (no X server|no Wayland display|ICD without|t_vk_xcb built without wayland-client)" "$W/xcb.log" && { echo "skipped: $(grep -m1 skipped "$W/xcb.log")"; exit 0; }
fi
grep -vE 'alvr|CAP_SYS_NICE|Performance will|radv|xkbcomp|^>|^Errors from' "$W/gs.log" | grep -vE '^\[gamescope\]' | head -10
grep -E "^(x11|wayland)" "$W/xcb.log" | head -5
wl=$(grep -E "mali_vk: wsi create_swapchain: platform=wayland" "$W/xcb.log" | tail -1)
[ "$rc" = 0 ] || { echo "FAIL: t_vk_xcb --wl-after-x11 exit $rc"; tail -15 "$W/xcb.log"; exit 1; }
[ -n "$wl" ] || { echo "FAIL: no wayland swapchain in the driver log"; tail -10 "$W/xcb.log"; exit 1; }
case "$wl" in
   *" sw=0 "*) ;;
   *) echo "FAIL: the wayland swapchain took wsi's software path after an X11 no-DRI3 query (the #322 leak):"; echo "      $wl"; exit 1 ;;
esac
# MALI_WSI_DEBUG names the process and the surface on every platform now, Wayland included: without those fields a
# Wayland create_swapchain line cannot be attributed to the game that asked for it (Megabonk on the product path).
echo "$wl" | grep -qE "platform=wayland comm=[^ ]+ pid=[0-9]+ surface=0x[0-9a-f]+ format=[0-9]+\(VK_FORMAT_[A-Z0-9_]+\) colorSpace=[0-9]+ usage=0x[0-9a-f]+ mode=[0-9]+ extent=[0-9]+x[0-9]+ minImageCount=[0-9]+ images=[0-9]+ " \
   || { echo "FAIL: the wayland create_swapchain line does not name comm/pid/surface/format/images:"; echo "      $wl"; exit 1; }
# The dump of that Wayland dma-buf swapchain: pixels at vkQueuePresentKHR, the product path the old SHM-only dump
# could not see at all.
ls "$W"/wl-t_vk_xcb-*-sc0-*.ppm >/dev/null 2>&1 \
   || { echo "FAIL: no MALI_WSI_DUMP frame for the Wayland dma-buf swapchain:"; grep -E "wsi dump|wsi present" "$W/xcb.log" | head -5; exit 1; }
line=$(grep -m1 "platform=wayland" "$W/wl.txt")
case "$line" in
   *" comm=t_vk_xcb pid="*" layout=linear "*) ;;
   *) echo "FAIL: the Wayland dump .txt line does not name the process and the layout:"; echo "      $line"; exit 1 ;;
esac
# The image is cleared to 0.2/0.4/0.8: an _SRGB swapchain stores that encoded (124,170,231), a UNORM one raw (51,102,204).
case "$line" in
   *"(VK_FORMAT_B8G8R8A8_SRGB)"*" mean=124.0,170.0,231.0 "*) want="124,170,231 (the sRGB-encoded clear)" ;;
   *" mean=51.0,102.0,204.0 "*)                              want="51,102,204 (the clear)" ;;
   *) echo "FAIL: the dumped Wayland frame does not hold the clear colour:"; echo "      $line"; exit 1 ;;
esac
grep -q "wayland present: result 0" "$W/xcb.log" \
   || echo "WARN: the Wayland present did not return VK_SUCCESS (the dump is written before wsi_common, so the checks above still hold)"
echo "ok    wl dump ($GPU): $want, sc0, comm+pid in the file name"
echo "ok    wl after x11 no-dri3 ($GPU): x11 support TRUE and the wayland swapchain stayed on the dma-buf path"
