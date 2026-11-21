#!/bin/bash
# SPDX-License-Identifier: MIT
# Host: push the build + spv files to the Poco and run t_vk_draw --only $1 (default fx) via qdrv. Usage: r4m1run.sh [case]
set -e; export PATH=$PATH:$HOME/Android/Sdk/platform-tools
V=$(cd "$(dirname "$0")/.." && pwd); S=$("$V/../tools/dev-serial" poco); DL=/storage/emulated/0/Download
for f in libvulkan_mali.so t_vk_draw; do timeout 100 adb -s $S push $V/out/a64-glibc/$f $DL/r4m1_$f >/dev/null; done
for f in rot.vert var.frag fxz.vert ubo_big.frag; do timeout 100 adb -s $S push $V/shader/$f.spv $DL/r4m1_$f.spv >/dev/null; done
echo "${1:-fx}" >/tmp/r4m1_only.txt; adb -s $S push /tmp/r4m1_only.txt $DL/r4m1_only.txt >/dev/null
cd ~/Projects/mali-gaming; timeout 200 scripts/quick/qdrv poco $V/tests/r4m1draw.sh --keep 2>&1 | tail -1
cat "$(ls -t runs/quick/*drv-r4m1draw.sh.jsonl | head -1)"
