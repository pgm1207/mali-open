#!/bin/bash
# SPDX-License-Identifier: MIT
# Two X connections in one process are two servers (CR #323 N2): the no-DRI3 software decision must be per connection,
# not per physical device. gamescope's Xwayland ($DISPLAY, has DRI3) is connection A and a second Xwayland started
# without DRI3 is connection B; MALI_WSI=hw keeps the fake backend on the dma-buf path so A's swapchain is decided
# dma-buf (its DRI3 import then fails on the fake backend's memfd "dma-bufs" - expected, the mode is what is read)
# while B must be created on the software path. Before this fix, B's query set the device-wide flag and A was pushed
# onto the software path too (log: both sw=1).
# Usage: xwsi-two-conns.sh <out dir with libvulkan_mali.so, mali_icd.json, t_vk_xcb> [gpu v10|v11|v13]
set -u
OUT=$1; GPU=${2:-v10}
T=$(cd "$(dirname "$0")" && pwd); V=$(cd "$T/.." && pwd)
[ -x "$OUT/t_vk_xcb" ] || { echo "skipped: no t_vk_xcb in $OUT"; exit 0; }
for c in gamescope Xwayland xdpyinfo; do command -v $c >/dev/null || { echo "skipped: no $c"; exit 0; }; done
OUT=$(cd "$OUT" && pwd)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
cat >"$W/inner.sh" <<INNER
#!/bin/sh
sock=\${GAMESCOPE_WAYLAND_DISPLAY:-}
[ -n "\$sock" ] || sock=\$(ls "\$XDG_RUNTIME_DIR" 2>/dev/null | grep -E '^gamescope-[0-9]+\$' | head -1)
[ -n "\$sock" ] && export WAYLAND_DISPLAY="\$sock"
xdpyinfo -display "\$DISPLAY" >"$W/xdpy_a.txt" 2>&1
grep -qx '[[:space:]]*DRI3' "$W/xdpy_a.txt" || { echo "skipped: the DRI3 connection has no DRI3"; echo 77 >"$W/rc"; exit 0; }
XWAYLAND_NO_GLAMOR=1 Xwayland :77 -nolisten tcp -extension DRI3 >"$W/xw.log" 2>&1 &
i=0; while [ \$i -lt 60 ]; do xdpyinfo -display :77 >/dev/null 2>&1 && break; sleep 0.25; i=\$((i+1)); done
xdpyinfo -display :77 >"$W/xdpy_b.txt" 2>&1 || { echo "FAIL: no X server on :77"; echo 1 >"$W/rc"; exit 1; }
grep -qx '[[:space:]]*DRI3' "$W/xdpy_b.txt" && { echo "FAIL: :77 has DRI3"; echo 1 >"$W/rc"; exit 1; }
echo "connA: \$DISPLAY (DRI3)  connB: :77 (no DRI3)"
export TVK_DISPLAY_A="\$DISPLAY" TVK_DISPLAY_B=:77
export MALI_BACKEND=fake MALI_FAKE_GPU=$GPU
export VK_DRIVER_FILES=$OUT/mali_icd.json VK_ICD_FILENAMES=$OUT/mali_icd.json
export MALI_WSI=hw MALI_WSI_DEBUG=1
timeout 120 "$OUT/t_vk_xcb" --direct "$OUT/libvulkan_mali.so" --two-conns >"$W/xcb.log" 2>&1
echo \$? >"$W/rc"
INNER
chmod +x "$W/inner.sh"
timeout 200 gamescope --backend headless -W 640 -H 480 -- "$W/inner.sh" >"$W/gs.log" 2>&1
rc=$(cat "$W/rc" 2>/dev/null || echo 1)
if [ "$rc" = 77 ]; then echo "skipped: $(grep -m1 skipped "$W/xcb.log" "$W/gs.log" 2>/dev/null | head -1)"; exit 0; fi
grep -E "^(connA|connB)" "$W/xcb.log"
[ "$rc" = 0 ] || { echo "FAIL: t_vk_xcb --two-conns exit $rc"; tail -8 "$W/xcb.log"; exit 1; }
# The driver's own lines, in order: A first (dma-buf, sw=0), then B (software, sw=1).
grep -E "mali_vk: wsi create_swapchain: platform=xcb" "$W/xcb.log" >"$W/modes.txt"
# A is decided dma-buf (dri3=1), B software (dri3=0). On the fake backend A's DRI3 import fails, so #328's retry
# finishes A on the software device: the create line then says sw=1 for both, and the retry line names only A.
dec=$(grep -o "dri3=[01]" "$W/modes.txt" | tr '\n' ' ')
sw=$(grep -o "sw=[01]" "$W/modes.txt" | tr '\n' ' ')
echo "modes: $sw decisions: $dec"
[ "$dec" = "dri3=1 dri3=0 " ] || { echo "FAIL: expected A decided dma-buf (dri3=1) and B software (dri3=0), got: $dec"; exit 1; }
[ "$sw" = "sw=1 sw=1 " ] || { echo "FAIL: expected both to end on software (A after #328's retry), got: $sw"; exit 1; }
[ "$(grep -c "retrying on the software" "$W/xcb.log")" -ge 1 ] || { echo "FAIL: A did not report the #328 retry"; exit 1; }
echo "ok    two conns ($GPU): the no-DRI3 connection took the software path, the DRI3 one stayed dma-buf"
