#!/bin/bash
# SPDX-License-Identifier: MIT
# The lane artefact: build the aarch64 X11+Wayland (glibc-wsi) ICD from THIS checkout and print
# path + sha256 + commit, so a deploy can be checked against the branch head before it happens.
#
#   tools/build-artefact.sh                build (only from integration), write <artefact>.provenance, print
#   tools/build-artefact.sh --check        no build: verify the .so on disk against its .provenance, HEAD
#                                          and integration's head, then print one OK: or FAIL: line
#   tools/build-artefact.sh --allow-branch  off-fleet build/check on another branch (it is NOT deployable)
#
# ONE BRANCH FEEDS THE PHONES: integration. A record that only matched HEAD would happily bless an
# artefact from any other branch, so the branch rule lives here instead of in people's memory. Without
# --allow-branch the tool refuses to build and `--check` fails unless HEAD is the tip of integration.
#
# One artefact per branch. The provenance record next to the .so holds the sha256 of the built file, the
# full commit, the branch, whether vk/ or libkbase/ had tracked modifications at build time (dirty=1),
# and the UTC build timestamp. A check that only looked at the embedded driverInfo string would bless an
# artefact built from another commit or swapped out afterwards; the record is what makes it verifiable.
set -euo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
TARGET=a64-glibc-wsi
OUT=$R/vk/out/$TARGET/libvulkan_mali.so
PROV=$OUT.provenance
DEPLOY_BRANCH=integration

allow_branch=0
offfleet=
mode=build
for a in "$@"; do
   case "$a" in
      --check) mode=check ;;
      --allow-branch) allow_branch=1; offfleet=", off-fleet build" ;;
      *) echo "FAIL: unknown argument '$a' (usage: tools/build-artefact.sh [--check] [--allow-branch])" >&2; exit 2 ;;
   esac
done

WT=$R
commit=$(git -C "$WT" rev-parse HEAD)
branch=$(git -C "$WT" rev-parse --abbrev-ref HEAD)
# Only the driver tree matters to the .so, and vk/build.sh stamps driverInfo "-dirty" from exactly this
# set (vk/ + libkbase/ tracked modifications). The record says which worktree it read and keeps the two
# counts apart, so a worktree mix-up or a change in the rule shows up here instead of in a phone log.
dirty_vk=$(git -C "$WT" status --porcelain --untracked-files=no -- vk | wc -l)
dirty_kb=$(git -C "$WT" status --porcelain --untracked-files=no -- libkbase | wc -l)
dirty=$((dirty_vk + dirty_kb))

# the head of integration as this checkout knows it: origin/ when the remote is there, else the local branch
if git -C "$R" rev-parse --verify -q "refs/remotes/origin/$DEPLOY_BRANCH" >/dev/null; then
   DEPLOY_REF=origin/$DEPLOY_BRANCH
elif git -C "$R" rev-parse --verify -q "refs/heads/$DEPLOY_BRANCH" >/dev/null; then
   DEPLOY_REF=$DEPLOY_BRANCH
else
   DEPLOY_REF=
fi
DEPLOY_COMMIT=$(git -C "$R" rev-parse -q --verify "$DEPLOY_REF" 2>/dev/null || true)

guard_branch() {  # the fleet rule: only integration's tip may be built or deployed
   [ "$allow_branch" = 1 ] && return 0
   if [ "$branch" != "$DEPLOY_BRANCH" ]; then
      echo "FAIL: HEAD is on branch '$branch', but the phones are fed from '$DEPLOY_BRANCH' only (git checkout $DEPLOY_BRANCH; or --allow-branch for an off-fleet build)"
      exit 1
   fi
   [ -n "$DEPLOY_COMMIT" ] || { echo "FAIL: no $DEPLOY_BRANCH ref in this checkout to compare against (git fetch origin)"; exit 1; }
   # HEAD must be on integration and contain integration's published tip. Being ahead is normal while an
   # integration stack is assembled locally (merged PR heads not pushed yet); being behind is not.
   if [ "$commit" != "$DEPLOY_COMMIT" ] && ! git -C "$R" merge-base --is-ancestor "$DEPLOY_COMMIT" "$commit"; then
      echo "FAIL: HEAD ${commit:0:7} does not contain $DEPLOY_REF ${DEPLOY_COMMIT:0:7}: update $DEPLOY_BRANCH before building (git fetch origin && git merge --ff-only $DEPLOY_REF; or --allow-branch)"
      exit 1
   fi
}

prov_get() { [ -f "$PROV" ] && sed -n "s/^$1[[:space:]]\{1,\}//p" "$PROV" | head -1 || true; }

check_artefact() {
   [ -f "$OUT" ] || { echo "FAIL: no artefact at $OUT (run tools/build-artefact.sh)"; exit 1; }
   [ -f "$PROV" ] || { echo "FAIL: no provenance record at $PROV (rebuild with tools/build-artefact.sh)"; exit 1; }
   guard_branch
   sum=$(sha256sum "$OUT" | cut -d' ' -f1)
   f=$(file -b "$OUT")
   p_commit=$(prov_get commit)
   p_sum=$(prov_get sha256)
   p_branch=$(prov_get branch)
   p_dirty=$(prov_get dirty)
   p_built=$(prov_get built)
   p_wt=$(prov_get worktree)
   p_dvk=$(prov_get dirty.vk)
   p_dkb=$(prov_get dirty.libkbase)
   [ -n "$p_commit" ] && [ -n "$p_sum" ] || { echo "FAIL: $PROV is not a provenance record (no commit/sha256 line)"; exit 1; }
   [ "$allow_branch" = 1 ] || [ "$p_branch" = "$DEPLOY_BRANCH" ] || { echo "FAIL: artefact was built on branch '$p_branch', not $DEPLOY_BRANCH: rebuild it on $DEPLOY_BRANCH"; exit 1; }
   case "$f" in *aarch64*) ;; *) echo "FAIL: $OUT is not aarch64: do not deploy this"; exit 1 ;; esac
   [ "$p_sum" = "$sum" ] || { echo "FAIL: provenance sha256 $p_sum != artefact $sum: the .so was replaced after the build"; exit 1; }
   # the two records of "was the tree dirty" must be the same fact, read from the same worktree
   [ "$p_wt" = "$WT" ] || { echo "FAIL: provenance was written in worktree '$p_wt', not $WT: it describes another tree"; exit 1; }
   [ "$p_dirty" = "$(( ${p_dvk:-0} + ${p_dkb:-0} ))" ] || { echo "FAIL: provenance dirty=$p_dirty does not equal dirty.vk=$p_dvk + dirty.libkbase=$p_dkb"; exit 1; }
   short=$(git -C "$WT" rev-parse --short=10 HEAD)
   want=$short; other="$short-dirty"
   if [ "$p_dirty" != 0 ]; then want="$short-dirty"; other=$short; fi
   # the .so must carry exactly the driverInfo string these counts imply: no silent disagreement.
   # (strings is captured first: `strings | grep -q` breaks under pipefail when grep exits early.)
   di=$(strings -a "$OUT")
   grep -qxF -- "$want" <<<"$di" || { echo "FAIL: driverInfo '$want' not found in the .so (provenance says dirty=$p_dirty)"; exit 1; }
   if grep -qxF -- "$other" <<<"$di"; then
      echo "FAIL: driverInfo carries both '$want' and '$other': stale objects in $OUT"; exit 1
   fi
   [ "$p_commit" = "$commit" ] || { echo "FAIL: provenance commit ${p_commit:0:7} != HEAD ${commit:0:7}: rebuild before deploying"; exit 1; }
   [ "$p_dirty" = 0 ] || { echo "FAIL: artefact was built from a dirty driver tree (dirty=$p_dirty): rebuild from a clean tree"; exit 1; }
   [ "$dirty" = 0 ] || { echo "FAIL: $dirty tracked file(s) under vk/ or libkbase/ modified: this .so is not the branch head, do not deploy it"; exit 1; }
   echo "OK: deploy $OUT at $sum from ${p_branch:-?}@${p_commit:0:7} (built ${p_built:-?}${offfleet})"
}

if [ "$mode" = check ]; then
   check_artefact
   exit 0
fi

guard_branch
cd "$R/vk"
./cross/build-cross.sh glibc-wsi >/tmp/build-artefact.log 2>&1 || { tail -20 /tmp/build-artefact.log; exit 1; }
grep -E 'warning:|error:' /tmp/build-artefact.log | grep -v 'tools/shaderdb' || true

sum=$(sha256sum "$OUT" | cut -d' ' -f1)
f=$(file -b "$OUT")
built=$(date -u +%Y-%m-%dT%H:%M:%SZ)
echo "branch   $branch"
echo "commit   $commit"
echo "artefact $OUT"
echo "sha256   $sum"
echo "file     $f"
case "$f" in *aarch64*) ;; *) echo "FAIL: $OUT is not aarch64: do not deploy this"; exit 1 ;; esac
# record before judging, so a refused (dirty) build still leaves the evidence of what it was
printf 'sha256  %s\ncommit  %s\nbranch  %s\nworktree  %s\ndirty   %s\ndirty.vk  %s\ndirty.libkbase  %s\nbuilt   %s\n' \
   "$sum" "$commit" "$branch" "$WT" "$dirty" "$dirty_vk" "$dirty_kb" "$built" >"$PROV.tmp" && mv "$PROV.tmp" "$PROV"
echo "provenance $PROV"
if [ "$dirty" != 0 ]; then
   echo "FAIL: $dirty tracked file(s) under vk/ or libkbase/ modified: this .so is not the branch head, do not deploy it"
   exit 1
fi
echo "OK: deploy $OUT at $sum from $branch@${commit:0:7} (built $built${offfleet})"
