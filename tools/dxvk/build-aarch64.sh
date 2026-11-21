#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# tools/dxvk/build-aarch64.sh   pinned DXVK, aarch64 Windows DLLs, manifest.
#
# Fetches DXVK_REVISION (source.env), applies every patch in patches/ in
# filename order, runs the source checks below, cross-builds the five aarch64
# PE DLLs the Android guest installs into a Proton prefix, and writes
# MANIFEST + SHA256SUMS into the (empty) output directory.
#
# Needs meson, ninja, python3 and llvm-mingw's aarch64-w64-mingw32-* tools.
# Put the toolchain on PATH, or point DXVK_MINGW_BIN at its bin/ directory.
# No device, phone or game is touched.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
. "$root/tools/dxvk/source.env"

out=${1:?usage: tools/dxvk/build-aarch64.sh <empty output directory>}
# lld stamps PE files with the wall clock unless this is set; the pinned epoch
# keeps every rebuild bit-identical to the verified reference stage.
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$DXVK_BUILD_EPOCH}
mkdir -p "$out"
out=$(cd "$out" && pwd)
shopt -s nullglob dotglob
existing=("$out"/*)
if ((${#existing[@]})); then
  printf '%s\n' 'output directory must be empty; use a fresh build directory' >&2
  exit 1
fi
shopt -u nullglob dotglob

if [[ -n ${DXVK_MINGW_BIN:-} ]]; then
  export PATH="$DXVK_MINGW_BIN:$PATH"
fi
for tool in meson ninja python3 aarch64-w64-mingw32-clang aarch64-w64-mingw32-strip; do
  command -v "$tool" >/dev/null || {
    printf 'missing %s: install llvm-mingw and meson, or set DXVK_MINGW_BIN\n' "$tool" >&2
    exit 1
  }
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

git init -q "$work/source"
git -C "$work/source" fetch --depth=1 https://github.com/doitsujin/dxvk.git "$DXVK_REVISION"
git -C "$work/source" checkout -q --detach FETCH_HEAD
test "$(git -C "$work/source" rev-parse HEAD)" = "$DXVK_REVISION"
git -C "$work/source" submodule update --init --recursive --depth=1

for patch in "$root"/tools/dxvk/patches/*.patch; do
  git -C "$work/source" apply --check "$patch"
  git -C "$work/source" apply "$patch"
done

python3 "$root/tools/dxvk/check_present_sync.py" "$work/source/src/dxvk/dxvk_presenter.cpp"
python3 "$root/tools/dxvk/check_compat_relaxation.py" "$work/source/src/dxvk/dxvk_device_info.cpp"
python3 "$root/tools/dxvk/check_async_pipeline.py" "$work/source"
python3 "$root/tools/dxvk/check_hud_storage_format.py" "$work/source/src/dxvk/hud/shaders/hud_frame_time_eval.comp"
python3 "$root/tools/dxvk/check_d3d9_single_sample.py" "$work/source"

meson setup "$work/build" "$work/source" \
  --cross-file "$root/tools/dxvk/aarch64-w64-mingw32.txt" \
  --buildtype release --prefix "$out" --bindir aarch64 --libdir aarch64 \
  -Db_ndebug=if-release --strip
meson compile -C "$work/build" -j "${JOBS:-$(nproc)}"
meson install -C "$work/build" --no-rebuild

toolchain=$(aarch64-w64-mingw32-clang --version | head -n 1)
python3 - "$out" "$DXVK_REVISION" "$toolchain" "$root/tools/dxvk/patches" <<'PY'
import datetime
import hashlib
import os
import struct
import sys

out, revision, toolchain, patchdir = sys.argv[1:5]
dlls = ["d3d10core.dll", "d3d11.dll", "d3d8.dll", "d3d9.dll", "dxgi.dll"]
markers = [
    b"mali-open: dxvk compat relaxation: geometryShader absent, allowed",
    b"mali-open: dxvk compat relaxation: shaderCullDistance absent, allowed",
    b"dxvk.enableAsync",
    b"DXVK_ASYNC",
]


def machine(path):
    with open(path, "rb") as f:
        data = f.read(0x400)
    if data[:2] != b"MZ":
        raise SystemExit(f"{path}: not a PE file")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise SystemExit(f"{path}: missing PE signature")
    return struct.unpack_from("<H", data, e_lfanew + 4)[0]


rows = []
seen = set()
for name in dlls:
    path = os.path.join(out, "aarch64", name)
    if not os.path.isfile(path):
        raise SystemExit(f"missing built DLL: {path}")
    with open(path, "rb") as f:
        data = f.read()
    mach = machine(path)
    if mach != 0xAA64:
        raise SystemExit(f"{path}: machine 0x{mach:04x}, expected 0xaa64")
    digest = hashlib.sha256(data).hexdigest()
    rows.append((name, mach, len(data), digest))
    seen.add(name)

with open(os.path.join(out, "aarch64", "d3d11.dll"), "rb") as f:
    binary = f.read()
missing = [m.decode() for m in markers if m not in binary]
if missing:
    raise SystemExit("d3d11.dll is missing markers: " + ", ".join(missing))

patches = []
for name in sorted(os.listdir(patchdir)):
    if name.endswith(".patch"):
        with open(os.path.join(patchdir, name), "rb") as f:
            patches.append((name, hashlib.sha256(f.read()).hexdigest()))

lines = []
lines.append("DXVK aarch64 build - Mali driver (async pipelines + compatibility shim)")
lines.append("built: " + datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"))
lines.append(f"source: doitsujin/dxvk {revision} (DXVK 3.1.1)")
lines.append("")
lines.append("patches applied in order (tools/dxvk/patches):")
for name, digest in patches:
    lines.append(f"  {name:48s} {digest}")
lines.append("0006 rebase note: upstream gplasync v3.1.1-1 (9a835c1) sha256 "
             "2542bf4f00091278e6274ecba5017e826a330451af2b0c1a085e0ad70cd40f3d;")
lines.append("  one context line differs (status.store memory_order_release), hunks unchanged")
lines.append("")
lines.append(f"toolchain: {toolchain}")
lines.append("config: dxvk.enableAsync=true (default), DXVK_ASYNC unset; relaxation shim active")
lines.append("")
lines.append("files (PE machine, size, sha256):")
for name, mach, size, digest in rows:
    lines.append(f"  aarch64/{name:16s} 0x{mach:04x} {size:9d}  {digest}")
lines.append("")
lines.append("all five DLLs are 0xaa64; SHA256SUMS in this directory matches the table above.")
lines.append("markers verified in aarch64/d3d11.dll: " + "; ".join(m.decode() for m in markers))
manifest = "\n".join(lines) + "\n"
with open(os.path.join(out, "MANIFEST"), "w", encoding="utf-8") as f:
    f.write(manifest)
with open(os.path.join(out, "SHA256SUMS"), "w", encoding="utf-8") as f:
    for name, _, _, digest in rows:
        f.write(f"{digest}  aarch64/{name}\n")
print(manifest, end="")
PY
