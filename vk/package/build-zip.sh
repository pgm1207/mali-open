#!/bin/bash
# SPDX-License-Identifier: MIT
# Package the own Vulkan driver as a DroidDeck/MediaDeck "Linux Vulkan driver" zip.
#   ./build-zip.sh [-o outdir] [--bundle <PanVK-Android adrenotools zip>] [variant...]     variants: a64-glibc a64-glibc-wsi a64-glibc-wl native   (default: a64-glibc a64-glibc-wsi)
# Output: <outdir>/mali-open-<variant>-<YYYYmmdd>.zip   (outdir default: ./dist)
# a64-* zips are importable in the app. The "native" zip is x86-64, for verify-zip.sh on the laptop ONLY (the app rejects non-aarch64).
#
# --bundle: ALSO write mali-open-bundle-<variant>-<date>.zip (manifest.json + android/ + linux/, the "driver pair" format of DriverBundle.kt).
#   The bundle id is "PanVK-MaliOpen-<date>" on purpose: DriverPairs.isNonAdreno() only accepts ids matching ^PanVK-..., and only then does
#   the app set BL_OPEN_DRIVER=1 and count the open driver as able to draw (DeviceSupport.openDriverActive/canDraw). The android half
#   (display side) is a stock PanVK adrenotools driver from the --bundle zip; it is only used for display on non-system-Vulkan phones.
#
# Layout (flat, the shape LinuxVulkanDriverManager.installDriver() reads: first libvulkan_{panfrost,freedreno,radeon}*.so + meta.json):
#   libvulkan_panfrost.so  our libvulkan_mali.so under the NAME THE IMPORTER ALLOWS (isDriverLibrary). SONAME inside stays libvulkan_mali.so.
#   meta.json              kind=linux-vulkan-icd, glibc, minGlibc
#   icd.json               reference ICD manifest (relative library_path); the app writes its own with an absolute path
#   LICENSE, NOTICE-MESA.txt, VERSION, NEEDED.txt (readelf NEEDED + ldd), SHA256SUMS, README.txt
set -euo pipefail
P=$(cd "$(dirname "$0")" && pwd); V=$(dirname "$P"); MESA=$V/../third_party/mesa
out=$P/dist
android_zip=
while [ "${1:-}" = "-o" ] || [ "${1:-}" = "--bundle" ]; do
   case $1 in -o) out=$(realpath -m "$2");; --bundle) android_zip=$(realpath "$2");; esac; shift 2
done
# a64-glibc-wsi (Wayland + X11) is the shipping WSI variant: Steam's CEF/ANGLE client and GLX/zink are X11 clients of Xwayland.
# a64-glibc-wl (Wayland only) stays importable but cannot create an X11 surface (VK_KHR_xlib/xcb_surface absent).
variants=("$@"); [ ${#variants[@]} -gt 0 ] || variants=(a64-glibc a64-glibc-wsi)
date=$(date +%Y%m%d)
mkdir -p "$out"

for var in "${variants[@]}"; do
   so=$V/out/$var/libvulkan_mali.so
   [ -f "$so" ] || { echo "missing $so (build: ../cross/build-cross.sh all / ../build.sh)"; exit 1; }
   case $var in native) arch=x86_64; test_only=true ;; *) arch=aarch64; test_only=false ;; esac
   w=$(mktemp -d); trap 'rm -rf "$w"' EXIT
   cp "$so" "$w/libvulkan_panfrost.so"
   if [ $arch = aarch64 ]; then
      readelf -h "$w/libvulkan_panfrost.so" | grep -q 'AArch64' || { echo "$var: not AArch64"; exit 1; }
      grep -aq 'libc.so.6' "$w/libvulkan_panfrost.so" || { echo "$var: no libc.so.6 string (importer would reject: not glibc)"; exit 1; }
   fi
   # highest GLIBC symbol version needed -> minGlibc
   minglibc=$(readelf -V "$so" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -V | tail -1 || true)
   [ -n "$minglibc" ] || minglibc=2.38
   mesa_ver=$(cat "$MESA/VERSION" 2>/dev/null || echo unknown)
   src_id=$(cd "$V" && git rev-parse --short HEAD 2>/dev/null || echo nogit)
   cat > "$w/meta.json" <<J
{
  "schemaVersion": 1,
  "kind": "linux-vulkan-icd",
  "name": "Mali-open ($var)",
  "driverVersion": "own Vulkan 1.0 ICD 0.1.0 (Mesa $mesa_ver runtime, $src_id)",
  "libc": "glibc",
  "minGlibc": "$minglibc",
  "libraryName": "libvulkan_panfrost.so",
  "arch": "$arch",
  "variant": "$var",
  "testOnly": $test_only,
  "hwGenerations": ["v11 (G615/G715)", "v13 (G925)"]
}
J
   cat > "$w/icd.json" <<'J'
{"file_format_version":"1.0.1","ICD":{"library_path":"./libvulkan_panfrost.so","api_version":"1.3.0"}}
J
   echo "0.1.0 $date $var mesa-$mesa_ver $src_id" > "$w/VERSION"
   cat > "$w/LICENSE" <<'J'
MIT License

Copyright (c) 2026 the mali-open authors

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files
(the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge,
publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
J
   {
      echo "This binary statically links parts of Mesa $mesa_ver (https://gitlab.freedesktop.org/mesa/mesa, tag mesa-$mesa_ver):"
      echo "the Vulkan runtime (vk_*), WSI, NIR, SPIR-V, the Panfrost/Bifrost compiler and lib. Mesa is MIT-licensed (and a few"
      echo "files under other permissive licences: see Mesa's docs/license.rst and licenses/). No PanVK driver code is linked."
      echo "Mesa copyright notices apply to those parts; the MIT text in LICENSE applies to them as well."
      echo
      echo "Corresponding Mesa licence text, docs/license.rst:"; echo
      cat "$MESA/docs/license.rst" 2>/dev/null || echo "(not available when packaged)"
      [ -f "$MESA/licenses/MIT" ] && { echo; echo "---- licenses/MIT ----"; cat "$MESA/licenses/MIT"; }
   } > "$w/NOTICE-MESA.txt"
   {
      echo "# readelf -d NEEDED (libs the guest rootfs must provide; none are bundled, the importer copies one .so only)"
      readelf -d "$so" | grep NEEDED
      echo; echo "# ldd (host; aarch64 libs show 'not a dynamic executable' on x86-64)"
      ldd "$so" 2>&1 || true
      echo; echo "# highest GLIBC symbol version: $minglibc"
   } > "$w/NEEDED.txt"
   cat > "$w/README.txt" <<J
Mali-open $var: own Vulkan ICD for Mali (Valhall v11 and 5th-gen v13 picked at runtime from the GPU id).
Import in DroidDeck/MediaDeck: Setup > GPU drivers > import Linux driver, pick this zip. Details: driver/vk/package/README.md.
Needs in the guest: $(readelf -d "$so" | grep -o 'Shared library: \[[^]]*\]' | sed 's/.*\[\(.*\)\]/\1/' | tr '\n' ' ')
J
   (cd "$w" && sha256sum $(ls | grep -v SHA256SUMS) > SHA256SUMS)
   name=mali-open-$var-$date.zip
   [ "$test_only" = true ] && name=mali-open-$var-TESTONLY-$date.zip
   rm -f "$out/$name"
   (cd "$w" && zip -q -X -9 "$out/$name" meta.json libvulkan_panfrost.so icd.json LICENSE NOTICE-MESA.txt VERSION NEEDED.txt README.txt SHA256SUMS)
   echo "$out/$name  ($(du -h "$out/$name" | cut -f1); needs: $(readelf -d "$so" | grep -o 'Shared library: \[[^]]*\]' | sed 's/.*\[\(.*\)\]/\1/' | tr '\n' ' '))"
   if [ -n "$android_zip" ] && [ $arch = aarch64 ]; then
      b=$w/bundle; mkdir -p "$b/android" "$b/linux"
      unzip -q -j "$android_zip" libvulkan_panfrost.so meta.json -d "$b/android"
      cp "$w/libvulkan_panfrost.so" "$w/meta.json" "$w/icd.json" "$w/LICENSE" "$w/NOTICE-MESA.txt" "$w/VERSION" "$b/linux/"
      tag=PanVK-MaliOpen-$date
      cat > "$b/manifest.json" <<J
{"package":"PanVK-MaliOpen","version":"own ICD 0.1.0 ($var)","tag":"$tag","drivers":{"android":{"path":"android"},"linux":{"path":"linux"}}}
J
      bn=mali-open-bundle-$var-$date.zip; rm -f "$out/$bn"
      (cd "$b" && zip -q -X -9 -r "$out/$bn" manifest.json android linux)
      echo "$out/$bn  (bundle, id $tag, android half from $(basename "$android_zip"))"
   fi
   rm -rf "$w"; trap - EXIT
done
