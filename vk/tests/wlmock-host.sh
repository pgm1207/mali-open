#!/bin/bash
# SPDX-License-Identifier: MIT
# Host test of the Wayland WSI against a MOCK compositor (tests/mock_wl_compositor.c, libwayland-server): no GPU, no display, our ICD on
# the fake backend (software present path = wl_shm). t_vk_present is the client. What it pins down, per compositor behaviour:
#   normal     30 wl_shm formats (>16 VkSurfaceFormats: the format query must be two-call), FIFO and MAILBOX, frames flow
#   noframe    wl_surface.frame callbacks never answered (hidden window, a guest compositor that does not draw): MAILBOX must keep presenting
#   stall      no frame callbacks and no wl_buffer.release: vkAcquireNextImage(timeout) must return VK_TIMEOUT in bounded time (exit 3)
#   KNOWN HANG noframe + FIFO: Mesa wsi_common_wayland's legacy FIFO throttle (queue_present: `while (!legacy_fifo_ready)
#              wl_display_dispatch_queue`) blocks vkQueuePresent forever. Not our code (Mesa 26.3, no wp_fifo in the compositor); recorded
#              as an expected hang so that a Mesa bump that fixes it shows up as "hang fixed" and this line can be tightened.
# Usage: wlmock-host.sh <out dir with libvulkan_mali.so> [gpu v10|v11|v13]
# Skips (exit 0, "skipped") without libwayland-server / wayland-scanner / libwayland-client.
set -u
OUT=$1; GPU=${2:-v10}
V=$(cd "$(dirname "$0")/.." && pwd)
command -v wayland-scanner >/dev/null && pkg-config --exists wayland-server wayland-client || { echo "skipped: no wayland-scanner/libwayland-server"; exit 0; }
[ -f "$V/../present/gen/xdg-shell-protocol.c" ] || { echo "skipped: no present/gen"; exit 0; }
W=$(mktemp -d); trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$W"' EXIT
export TMPDIR=$W
env -u LD_PRELOAD wayland-scanner server-header "$V/../present/protocols/xdg-shell.xml" "$W/xdg-shell-server-protocol.h" || exit 1
env -u LD_PRELOAD cc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -I"$W" -I"$V/../present/gen" \
   -o "$W/mock" "$V/tests/mock_wl_compositor.c" "$V/../present/gen/xdg-shell-protocol.c" -lwayland-server || { echo "FAIL: mock build"; exit 1; }
if [ ! -x "$V/out/t_vk_present-native" ] || [ "$V/tests/t_vk_present.c" -nt "$V/out/t_vk_present-native" ]; then
   env -u LD_PRELOAD "$V/tests/build-t_vk_present.sh" native >/dev/null || { echo "FAIL: t_vk_present build"; exit 1; }
fi
fail=0
# case <name> <mock mode> <nformats> <want rc> <client timeout s> <grep pattern in client output (optional)> -- client args
case_() {
   local name=$1 mode=$2 nf=$3 want=$4 to=$5 pat=$6; shift 7
   local rt; rt=$(mktemp -d -p "$W"); local sock=mock-$$-$RANDOM
   env -u LD_PRELOAD XDG_RUNTIME_DIR=$rt "$W/mock" $sock $mode $nf >"$rt/mock.log" 2>&1 & local m=$!
   for _ in $(seq 50); do grep -q READY "$rt/mock.log" && break; sleep 0.1; done
   { XDG_RUNTIME_DIR=$rt WAYLAND_DISPLAY=$sock MALI_BACKEND=fake MALI_FAKE_GPU=$GPU timeout -k 2 "$to" \
      "$V/out/t_vk_present-native" --icd "$OUT/libvulkan_mali.so" --spv-dir "$V/shader" "$@" >"$rt/client.log" 2>&1; } 2>/dev/null
   local rc=$?
   kill $m 2>/dev/null; wait $m 2>/dev/null
   if [[ "$rc" =~ ^($want)$ ]] && { [ -z "$pat" ] || grep -q "$pat" "$rt/client.log"; }; then echo "ok    $name (rc=$rc)"
   else echo "FAIL  $name (rc=$rc, want $want, pattern '$pat')"; tail -5 "$rt/client.log" | cut -c1-200 | sed 's/^/        /'; fail=1; fi
}
case_ "wl mock normal, 30 shm formats, fifo"    normal    30 0   30 '"pass":true'            -- --seconds 1 --mode fifo
case_ "wl mock normal, mailbox + resize"        normal    30 0   30 '"pass":true'            -- --seconds 1 --mode mailbox --resize-test
case_ "wl mock noframe, mailbox keeps going"    noframe    4 0   30 '"pass":true'            -- --seconds 1 --mode mailbox
# CR #288: with present ids (Steam/gamescope chain VkPresentIdKHR) vkQueuePresentKHR must return without waiting on the
# compositor. noframe never answers wl_surface.frame, so the vkWaitForPresentKHR this driver used to run per present
# waited its whole 20 ms cap -> every frame 20 ms (a 50 fps ceiling from the present path alone).
case_ "wl mock noframe, present-ids present does not wait" noframe 4 0 30 'present_ms' -- --seconds 2 --mode mailbox --present-ids --max-present-ms 5
case_ "wl mock stall, acquire times out"        stall      4 3   30 'acquire_timeout'        -- --seconds 5 --mode mailbox --acquire-ms 300
case_ "wl mock norelease, fifo acquire times out" norelease 4 3  30 'acquire_timeout'        -- --seconds 5 --mode fifo --acquire-ms 300
# KNOWN HANG: expected to be stuck at the timeout (124); 0 would mean Mesa no longer blocks.
case_ "wl mock noframe, fifo (KNOWN HANG, Mesa legacy FIFO throttle)" noframe 4 "124|137" 5 ''     -- --seconds 1 --mode fifo
[ $fail = 0 ] && echo "wlmock-host: OK"
exit $fail
