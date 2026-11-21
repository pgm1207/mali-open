#!/bin/bash
# SPDX-License-Identifier: MIT
# Unpack a mali-open zip into a scratch dir and check it the way DroidDeck's importer would (LinuxVulkanDriverManager.adopt):
# meta.json kind, library name allowlist, ELF class, "libc.so.6" string. A native (x86-64, TESTONLY) zip is additionally run through
# the system Vulkan loader on the fake backend (v11 and v13) with t_icd and vulkaninfo.
#   ./verify-zip.sh <zip> [--t-icd path]      default t_icd: ../out/native/t_icd
set -uo pipefail
P=$(cd "$(dirname "$0")" && pwd); V=$(dirname "$P")
[ $# -ge 1 ] || { echo "usage: verify-zip.sh <zip> [--t-icd path]"; exit 2; }
zip=$(realpath "$1"); shift
ticd=$V/out/native/t_icd
[ "${1:-}" = "--t-icd" ] && ticd=$(realpath "$2")
fail=0
ok()   { echo "ok    $*"; }
bad()  { echo "FAIL  $*"; fail=1; }

scratch=$(mktemp -d); trap 'rm -rf "$scratch"' EXIT
unzip -q "$zip" -d "$scratch" || { echo "FAIL  unzip"; exit 1; }
cd "$scratch" || exit 1   # scratch dir is ours; nothing in it is executed except via explicit paths below

for f in meta.json libvulkan_panfrost.so icd.json LICENSE NOTICE-MESA.txt VERSION SHA256SUMS; do
   [ -f "$f" ] && ok "has $f" || bad "missing $f"
done
sha256sum -c --quiet SHA256SUMS 2>/dev/null && ok "SHA256SUMS" || bad "SHA256SUMS"

kind=$(jq -r .kind meta.json 2>/dev/null); lib=$(jq -r .libraryName meta.json 2>/dev/null)
[ "$kind" = linux-vulkan-icd ] && ok "meta kind=$kind" || bad "meta kind=$kind"
[ "$lib" = libvulkan_panfrost.so ] && [ -f "$lib" ] && ok "meta libraryName=$lib present" || bad "libraryName=$lib"
jq -e '.name and .driverVersion and .minGlibc' meta.json >/dev/null && ok "meta name/driverVersion/minGlibc" || bad "meta fields"
jq -r '.ICD.library_path' icd.json | grep -q '^\./libvulkan_panfrost.so$' && ok "icd.json relative library_path" || bad "icd.json library_path"

testonly=$(jq -r .testOnly meta.json)
arch=$(jq -r .arch meta.json)
mach=$(readelf -h "$lib" | awk -F: '/Machine/{gsub(/^ +/,"",$2);print $2}')
echo "# machine: $mach  arch(meta): $arch  testOnly: $testonly"
if [ "$testonly" = false ]; then
   case $mach in AArch64*) ok "aarch64 ELF (importer isAarch64Elf)";; *) bad "not AArch64: $mach";; esac
   grep -aq 'libc.so.6' "$lib" && ok "contains libc.so.6 (importer isGlibc)" || bad "no libc.so.6 string"
fi
# WSI: ANGLE's VerifyExtensionsPresent needs VK_EXT_headless_surface (with the headless platform's vkCreateHeadlessSurfaceEXT compiled in)
for n in VK_EXT_headless_surface vkCreateHeadlessSurfaceEXT VK_EXT_swapchain_colorspace; do
   grep -aq "$n" "$lib" && ok "contains $n" || bad "no $n string (headless/colorspace WSI missing)"
done
# exports: loader entry points only
syms=$(nm -D --defined-only "$lib" | awk '{print $3}' | sort | tr '\n' ' ')
want="vk_icdGetInstanceProcAddr vk_icdGetPhysicalDeviceProcAddr vk_icdNegotiateLoaderICDInterfaceVersion "
[ "$syms" = "$want" ] && ok "exports = 3 loader entry points" || bad "exports: $syms"
echo "# NEEDED: $(readelf -d "$lib" | grep -o 'Shared library: \[[^]]*\]' | sed 's/.*\[\(.*\)\]/\1/' | tr '\n' ' ')"
echo "# highest GLIBC: $(readelf -V "$lib" | grep -o 'GLIBC_[0-9.]*' | sort -V | tail -1)  (meta minGlibc: $(jq -r .minGlibc meta.json))"
# WSI variant: the X11 surface names are what Steam's CEF/ANGLE client asks for through the loader (VK_KHR_xlib/xcb_surface). A Wayland-only
# library in that slot is the failure this check exists for, so a wsi zip must carry both names and the libxcb NEEDED entry.
variant=$(jq -r .variant meta.json)
case $variant in
   *-wsi)
      for n in VK_KHR_surface VK_KHR_xcb_surface VK_KHR_xlib_surface VK_KHR_wayland_surface; do
         grep -aq "$n" "$lib" && ok "wsi: advertises $n" || bad "wsi: $n not in the library"
      done
      readelf -d "$lib" | grep -q 'libxcb.so.1' && ok "wsi: links libxcb.so.1 (wsi_common_x11)" || bad "wsi: no libxcb.so.1 NEEDED"
      ;;
   *-wl)
      grep -aq 'VK_KHR_wayland_surface' "$lib" && ok "wl: advertises VK_KHR_wayland_surface" || bad "wl: no wayland surface"
      echo "# note: $variant has no X11 surface (Steam's CEF client needs the -wsi variant)"
      ;;
esac

if [ "$testonly" = true ] && [ "$mach" = "Advanced Micro Devices X86-64" -o "$arch" = x86_64 ]; then
   [ -x "$ticd" ] || { bad "t_icd not found at $ticd (run ../build.sh)"; exit 1; }
   env_base="VK_DRIVER_FILES=$scratch/icd.json VK_ICD_FILENAMES=$scratch/icd.json VK_LOADER_LAYERS_DISABLE=~all~ MALI_BACKEND=fake"
   for g in "v11 Mali-G615" "v13 Mali-G925"; do
      set -- $g
      out=$(env $env_base MALI_FAKE_GPU=$1 "$ticd" --expect-name "$2" 2>&1) && ok "t_icd via loader $1 ($2)" || { bad "t_icd loader $1"; echo "$out" | sed 's/^/        /'; }
      out=$(env MALI_BACKEND=fake MALI_FAKE_GPU=$1 "$ticd" --direct "$scratch/$lib" --expect-name "$2" 2>&1) && ok "t_icd direct $1" || { bad "t_icd direct $1"; echo "$out" | sed 's/^/        /'; }
      if command -v vulkaninfo >/dev/null; then
         out=$(env $env_base MALI_FAKE_GPU=$1 vulkaninfo --summary 2>&1); echo "$out" | grep -q "$2" && ok "vulkaninfo $1 sees $2" || { bad "vulkaninfo $1"; echo "$out" | tail -8 | sed 's/^/        /'; }
      else echo "# vulkaninfo not installed, skipped"; fi
   done
else
   echo "# aarch64 zip: loader run skipped on this host (no qemu-user); verify the native TESTONLY zip for the runtime check, or on a phone via driver/vk/tests/m2guest.sh"
fi
[ $fail = 0 ] && echo "OK" || echo "FAILED"
exit $fail
