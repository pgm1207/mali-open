#!/bin/bash
# SPDX-License-Identifier: MIT
# MALI_WSI_DUMP names the process and the chain, and MALI_WSI_DEBUG sums up a chain every N frames (not per frame).
#
# The dump used to be <prefix>-NNNNN.ppm from one per-process counter and it only ran in the X11 MIT-SHM present
# shortcut, so (a) two processes (Steam's UI and a game, or a game and a second run) overwrote each other's frames and
# (b) the product path - a Wayland dma-buf through the gamescope WSI layer - wrote nothing at all. Here two t_vk_xcb
# processes present into the same prefix and every frame is identifiable by comm, pid and swapchain slot.
# Usage: xwsi-dump.sh <out dir with libvulkan_mali.so, mali_icd.json, t_vk_xcb> [gpu v10|v11|v13]
# Skips (exit 0, "skipped") without gamescope/Xwayland/t_vk_xcb.
set -u
OUT=$1; GPU=${2:-v10}
T=$(cd "$(dirname "$0")" && pwd); V=$(cd "$T/.." && pwd)
[ -x "$OUT/t_vk_xcb" ] || { echo "skipped: no t_vk_xcb in $OUT"; exit 0; }
for c in gamescope Xwayland; do command -v $c >/dev/null || { echo "skipped: no $c"; exit 0; }; done
OUT=$(cd "$OUT" && pwd)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
mkdir -p "$W/two" "$W/quiet"

cat >"$W/inner.sh" <<INNER
#!/bin/sh
# MESA_VK_WSI_DEBUG=sw: the MIT-SHM present path, which the fake backend can copy; MALI_SOFT=1: real pixels.
export MALI_BACKEND=fake MALI_FAKE_GPU=$GPU MALI_SOFT=1 MALI_WSI=hw MESA_VK_WSI_DEBUG=sw
export VK_DRIVER_FILES="$OUT/mali_icd.json" VK_ICD_FILENAMES="$OUT/mali_icd.json"
export MALI_WSI_DEBUG=1 MALI_WSI_DEBUG_EVERY=1 MALI_WSI_DUMP="$W/two/f" MALI_WSI_DUMP_EVERY=1
timeout 120 "$OUT/t_vk_xcb" --direct "$OUT/libvulkan_mali.so" >"$W/a.log" 2>&1 &
a=\$!
timeout 120 "$OUT/t_vk_xcb" --direct "$OUT/libvulkan_mali.so" >"$W/b.log" 2>&1 &
b=\$!
wait \$a; echo \$? >"$W/a.rc"
wait \$b; echo \$? >"$W/b.rc"
# ...and with MALI_WSI_DUMP unset nothing may be written: the present path's only cost is the cached configuration.
# The baseline is taken BEFORE this run, so a file or .txt line it wrongly writes is visible in the comparison
# (CR thread, xwsi-dump.sh:73).
ls -1 "$W/two" >"$W/before.ls"; grep -c '^dump ' "$W/two/f.txt" >"$W/before.n"
unset MALI_WSI_DUMP MALI_WSI_DUMP_EVERY
timeout 120 "$OUT/t_vk_xcb" --direct "$OUT/libvulkan_mali.so" >"$W/q.log" 2>&1
echo \$? >"$W/q.rc"
ls -1 "$W/two" >"$W/after.ls"
INNER
chmod +x "$W/inner.sh"
timeout 300 gamescope --backend headless -W 640 -H 480 -- "$W/inner.sh" >"$W/gs.log" 2>&1

for x in a b q; do
   rc=$(cat "$W/$x.rc" 2>/dev/null || echo 1)
   [ "$rc" = 0 ] || { echo "FAIL: run $x exit $rc"; tail -5 "$W/$x.log"; exit 1; }
done

# One naming scheme for every kind: <prefix>-<comm>-<pid>-sc<N>c<creation id>-<frame>.ppm. The creation id is what
# keeps a replacement chain (same slot, frames restarting at 1) from truncating its predecessor's dumps.
ls "$W"/two/f-t_vk_xcb-*-sc0c*-1.ppm >/dev/null 2>&1 \
   || { echo "FAIL: the dumps are not named <prefix>-<comm>-<pid>-sc<N>c<id>-<frame>.ppm:"; ls "$W/two" | head -5; exit 1; }
n=$(ls "$W"/two/f-t_vk_xcb-*-sc0c*-*.ppm | wc -l)
[ "$n" -ge 6 ] || { echo "FAIL: expected 2 processes x 3 frames of dumps, got $n"; ls "$W/two" | head; exit 1; }
pids=$(ls "$W"/two/f-t_vk_xcb-*-sc0c*.ppm | sed -E 's#.*f-t_vk_xcb-([0-9]+)-.*#\1#' | sort -u | wc -l)
[ "$pids" = 2 ] || { echo "FAIL: the dump names carry $pids distinct pid(s), want 2 (both processes must be identifiable)"; ls "$W/two" | head -5; exit 1; }
# One .txt line per dump, with the pid and the platform: two processes append to the same file without losing lines.
# Every dump must have exactly ONE metadata record with its own pid, creation id and frame: a duplicate record and a
# missing record could not be told apart by counting lines (CR thread, xwsi-dump.sh:54).
for f in "$W"/two/f-t_vk_xcb-*-sc0c*-*.ppm; do
   k=$(basename "$f" | sed -E 's#^f-t_vk_xcb-([0-9]+)-sc[0-9]+c([0-9]+)-([0-9]+)\.ppm$#\1 \2 \3#')
   set -- $k
   [ "$#" = 3 ] || { echo "FAIL: cannot parse the dump name $(basename "$f")"; exit 1; }
   rec=$(grep -c "^dump frame=$3 sc=0 chain=$2 comm=t_vk_xcb pid=$1 " "$W/two/f.txt")
   [ "$rec" = 1 ] || { echo "FAIL: $(basename "$f") has $rec metadata records, want exactly 1"; grep "^dump frame=$3 " "$W/two/f.txt" | head -3; exit 1; }
done
lines=$(grep -c '^dump ' "$W/two/f.txt")
[ "$lines" -ge 6 ] || { echo "FAIL: $lines .txt lines for $n dumps (want one per dump)"; head -3 "$W/two/f.txt"; exit 1; }
tpids=$(sed -n 's/^dump frame=[0-9]* sc=[0-9]* chain=[0-9]* comm=[^ ]* pid=\([0-9]*\).*/\1/p' "$W/two/f.txt" | sort -u | wc -l)
[ "$tpids" = 2 ] || { echo "FAIL: the .txt lines carry $tpids distinct pid(s), want 2"; head -3 "$W/two/f.txt"; exit 1; }
grep -qE '^dump frame=[0-9]+ sc=0 chain=[0-9]+ comm=t_vk_xcb pid=[0-9]+ platform=xcb surface=0x[0-9a-f]+ [0-9]+x[0-9]+ format=44\(VK_FORMAT_B8G8R8A8_UNORM\) layout=linear ' "$W/two/f.txt" \
   || { echo "FAIL: the .txt line has no comm/pid/platform/surface/format/layout:"; head -2 "$W/two/f.txt"; exit 1; }

# The per-chain summary: one line per chain per N presents (MALI_WSI_DEBUG_EVERY=1 here), never one per frame.
for x in a b; do
   grep -qE "mali_vk: wsi present: sc0 xcb comm=t_vk_xcb pid=[0-9]+ surface=0x[0-9a-f]+ [0-9]+x[0-9]+ VK_FORMAT_[A-Z0-9_]+ images=[0-9]+ usage=0x[0-9a-f]+ mode=[0-9]+ sw=1 frames=[0-9]+ image=[0-9]+" "$W/$x.log" \
      || { echo "FAIL: run $x has no per-chain 'wsi present' summary line:"; grep "wsi present" "$W/$x.log" | head -3; exit 1; }
done
per_frame=$(grep -c "mali_vk: wsi present: sc0" "$W/a.log")
frames=$(grep -o "frames=[0-9]*" "$W/a.log" | tail -1 | cut -d= -f2)
[ -z "$frames" ] || [ "$per_frame" -le "$frames" ] \
   || { echo "FAIL: $per_frame summary lines for $frames presents (the summary is per frame)"; exit 1; }

# Unset: no file may appear anywhere.
[ -z "$(ls -A "$W/quiet")" ] || { echo "FAIL: something was written with MALI_WSI_DUMP unset"; ls -A "$W/quiet"; exit 1; }
cmp -s "$W/before.ls" "$W/after.ls" \
   || { echo "FAIL: the no-dump run changed the dump directory"; diff "$W/before.ls" "$W/after.ls" | head -5; exit 1; }
nq=$(grep -c '^dump ' "$W/two/f.txt")
[ "$nq" = "$(cat "$W/before.n")" ] && [ "$nq" = "$lines" ] \
   || { echo "FAIL: the no-dump run added .txt lines ($nq, was $(cat "$W/before.n"), $lines before it)"; exit 1; }

echo "ok    wsi dump ($GPU): $n dumps of 2 processes named f-t_vk_xcb-<pid>-sc0-c<id>-<frame>.ppm, $lines .txt lines, no dump when unset"
