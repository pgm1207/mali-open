#!/bin/bash
# SPDX-License-Identifier: MIT
# Host test of the MIT-SHM segment's lifetime (design-shm.md, decision B): the segment is unlinked with
# shmctl(IPC_RMID) right after a successful xcb_shm_attach, so
#   1. a live client has no segment listed in /proc/sysvipc/shm (the old code listed one for its whole life), and a
#      client that exits without vkDestroySwapchainKHR (SIGKILL, exit(), a killed Steam game) leaves nothing behind -
#      the kernel reclaims it at the last detach, and
#   2. destroying a swapchain with a present still in flight cannot pull pages from under the server (the server holds
#      its own attach until it processes our xcb_shm_detach, which the X stream orders behind the PutImage).
# The observable of RMID-at-attach is the kernel's own "destroyed" mark: ipcs -m lists the segment with status `dest`
# while the client's chain is alive (it is unlinked, but the server still holds an attach, so the pages live until our
# detach). With shmctl only at destroy the status column is empty for the client's whole life.
# Before this change the segment was only unlinked in mali_x11_shm_destroy(): one segment per run leaked until reboot.
# Usage: xwsi-shm-leak.sh <out dir with libvulkan_mali.so, mali_icd.json, t_vk_xcb> [gpu v10|v11|v13]
set -u
OUT=$1; GPU=${2:-v10}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
command -v gamescope >/dev/null && command -v Xwayland >/dev/null || { echo "skipped: no gamescope/Xwayland for a headless X server"; exit 0; }
[ -x "$OUT/t_vk_xcb" ] || { echo "skipped: no t_vk_xcb in $OUT"; exit 0; }
[ -r /proc/sysvipc/shm ] || { echo "skipped: no /proc/sysvipc/shm on this kernel"; exit 0; }

cat >"$W/inner.sh" <<EOF
#!/bin/sh
export ENABLE_GAMESCOPE_WSI=0 MALI_BACKEND=fake MALI_FAKE_GPU=$GPU MALI_WSI_DEBUG=1
export VK_DRIVER_FILES=$OUT/mali_icd.json VK_ICD_FILENAMES=$OUT/mali_icd.json
# 1. alive, no destroy: our segment must already be unlinked (status dest) while the chain exists
# A width of its own for this run, so a segment leaked by an *older* build (same size) can never be counted.
WID=\$((100 + RANDOM % 60))
timeout 60 "$OUT/t_vk_xcb" --expect-name Mali --size \${WID}x61 --shm-alive-ms 6000 >"$W/alive.log" 2>&1 &
ap=\$!
for i in \$(seq 80); do grep -q "x11 present: shm=1" "$W/alive.log" 2>/dev/null && break; sleep 0.1; done
sz=\$(sed -n 's/.*images, \([0-9]*\) B segment.*/\1/p' "$W/alive.log" | head -1)
echo "\${sz:-0}" >"$W/seg.size"
ipcs -m | awk -v s="\${sz:-0}" 'NR>2 && \$5==s {print \$7}' >"$W/alive.status"   # column 6 = status
ipcs -m | awk -v s="\${sz:-0}" 'NR>2 && \$5==s {n++} END {print n+0}' >"$W/alive.count"
wait \$ap
# 2. exit without destroy: the kernel must reclaim it (nothing of that size left behind)
timeout 60 "$OUT/t_vk_xcb" --expect-name Mali --size \${WID}x61 --shm-leak-exit >"$W/leak.log" 2>&1; echo \$? >"$W/leak.rc"
sz2=\$(sed -n 's/.*images, \([0-9]*\) B segment.*/\1/p' "$W/leak.log" | head -1)
# The X server processes our connection's close asynchronously; with RMID-at-attach its own detach then frees the
# pages. Give it 2 s, then read the final count (0 = reclaimed, >0 = a live segment the kernel had to keep).
for i in \$(seq 20); do
   n=\$(ipcs -m | awk -v s="\${sz2:-0}" 'NR>2 && \$5==s {n++} END {print n+0}')
   [ "\$n" = 0 ] && break
   sleep 0.1
done
echo "\$n" >"$W/leak.count"
# 3. control: create, present, destroy
timeout 60 "$OUT/t_vk_xcb" --expect-name Mali --size \${WID}x61 >"$W/normal.log" 2>&1; echo \$? >"$W/normal.rc"
EOF
chmod +x "$W/inner.sh"
timeout 250 gamescope --backend headless -W 640 -H 480 -- "$W/inner.sh" >"$W/gs.log" 2>&1
[ -f "$W/leak.rc" ] || { echo "FAIL: the test did not run inside gamescope"; tail -20 "$W/gs.log"; exit 1; }
grep -q "x11 present: shm=1 putimage=0" "$W/alive.log" \
   || { echo "FAIL: the alive run did not take the MIT-SHM path"; grep "x11 present" "$W/alive.log" | head -3; exit 1; }
[ "$(cat "$W/alive.count")" -ge 1 ] \
   || { echo "FAIL: while the chain is alive ipcs -m has no segment of our size ($(cat "$W/seg.size") B): it leaked/vanished?"; ipcs -m | tail -3; exit 1; }
grep -qx "dest" "$W/alive.status" \
   || { echo "FAIL: the live segment is not marked dest: IPC_RMID still happens at destroy, not at attach"; exit 1; }
[ "$(cat "$W/leak.rc")" = 0 ] && grep -q '^PASS' "$W/leak.log" \
   || { echo "FAIL: --shm-leak-exit rc=$(cat "$W/leak.rc")"; tail -5 "$W/leak.log"; exit 1; }
if [ "$(cat "$W/leak.count")" -ne 0 ]; then
   echo "FAIL: exit without vkDestroySwapchainKHR left $(cat "$W/leak.count") segment(s) behind"; exit 1
fi
[ "$(cat "$W/normal.rc")" = 0 ] && grep -q '^PASS' "$W/normal.log" \
   || { echo "FAIL: the destroy control run rc=$(cat "$W/normal.rc")"; tail -5 "$W/normal.log"; exit 1; }
echo "shm segment lifetime: live segment dest (unlinked at attach), exit-without-destroy leaves nothing"
echo "xwsi-shm-leak: OK"
