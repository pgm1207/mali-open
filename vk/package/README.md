# Packaging the own Vulkan driver for DroidDeck / MediaDeck

    ./build-zip.sh [-o outdir] [--bundle <PanVK-Android-adrenotools.zip>] [a64-glibc a64-glibc-wsi a64-glibc-wl native]
    ./verify-zip.sh dist/mali-open-<variant>-<date>.zip

Output in `dist/`: `mali-open-<variant>-<date>.zip` (plain Linux driver), `mali-open-bundle-<variant>-<date>.zip` (with `--bundle`),
`mali-open-native-TESTONLY-<date>.zip` (x86-64, laptop verification only; the app rejects it).
Variants: `a64-glibc` (no WSI platform), `a64-glibc-wsi` (**Wayland + X11 WSI, the one to ship**), `a64-glibc-wl` (Wayland only).
The default is `a64-glibc a64-glibc-wsi`. Steam's CEF/ANGLE web helper and GLX (zink/kopper) are **X11 clients of Xwayland**, so an
X11-less build dies there with "VK_KHR_xlib_surface missing"; `a64-glibc-wl` is kept for A/B only.
Build inputs come from `../out/<variant>/` (`../cross/build-cross.sh all`, `../build.sh`).

## How the app imports a driver (upstream/DroidDeck sources)

* Plain zip: `LinuxVulkanDriverManager.installDriver()` takes the **first** `libvulkan_{freedreno,panfrost,radeon}*.so` and `meta.json`
  (`isDriverLibrary`). Anything else, including `libvulkan_mali.so`, is ignored. So the package ships our library as
  **`libvulkan_panfrost.so`** (SONAME inside stays `libvulkan_mali.so`; harmless). `adopt()` requires an aarch64 ELF containing the string
  `libc.so.6`, then writes `files/<dir>/<id>/{libvulkan_panfrost.so,icd.json,meta.json}`; `icd.json` has an absolute `library_path` and
  `api_version 1.1.274`. Our own `icd.json`, LICENSE etc. in the zip are ignored by the importer (kept for humans and the verifier).
* Bundle zip: `DriverBundle.install()` needs `manifest.json` + `android/` + `linux/` halves; the Android half must be an adrenotools-style
  driver (`meta.json` with `libraryName`, aarch64 ELF), so `--bundle` copies a stock PanVK Android half (third-party, Mesa MIT). The bundle
  tag is `PanVK-MaliOpen-<date>` because `DriverPairs.isNonAdreno()` only recognises ids matching `^PanVK-...`; only such bundles make
  `DeviceSupport.openDriverActive()` true (sets `BL_OPEN_DRIVER=1`, counts as able to draw).
* Guest session: `SessionService.kt` ~l.944-960: `LinuxVulkanDriver.resolveIcdPath()` -> env `BL_VK_DRIVER=<.../icd.json>`; the runtime's
  loader wrapper turns that into the ICD file. **If `DeviceSupport.vkBridge()` is true (the default on Mali with `/dev/mali0` and no
  system Vulkan), the ICD env is not set at all**: MaliBridge is used and the imported driver is ignored.

## Import and select

Needs a phone/app on-device (not testable offline).
1. Copy the zip to the phone (`/sdcard/Download`).
2. Preferred, full open-driver treatment: import `mali-open-bundle-*.zip` via the driver bundle import (Setup > GPU drivers). Plain zip:
   Setup > GPU drivers > Linux driver > import zip (`DriverMenus.kt` -> `installDriver`).
3. Turn **MaliBridge off** (pref `vkBridge=false`), select the imported driver (bundle: it sets `androidDriver` + `linuxDriver`; plain zip:
   `linuxDriver` only). Takes effect at the next session start.
4. Check `driver-setup.txt` and the session log for `Linux session draws with imported driver <id>`.
   Without the app hook (`mali-open-app-hook.patch`) a plain zip never sets `BL_OPEN_DRIVER`, so use the bundle until it lands.

## Roll back

Setup: select "Runtime default" (empty `linuxDriver`) and/or switch MaliBridge back on; remove the imported driver from the same menu
(`removeDriver` / `DriverBundle.remove`, which also clears `linuxDriver`). Nothing outside the app's `files/` is touched. A vanished
driver falls back to the runtime default on non-PowerVR devices.

## Contents and runtime libs

`meta.json` (kind `linux-vulkan-icd`, glibc, `minGlibc` from the highest `GLIBC_` symbol: 2.38), `icd.json`, `LICENSE` (MIT),
`NOTICE-MESA.txt` (Mesa runtime/NIR/compiler statically linked, MIT; no PanVK code), `VERSION`, `NEEDED.txt`, `SHA256SUMS`.
Both GPU generations are in one library; v11 (G615/G715, `mali_v11_*`) or v13 (G925, `mali_v13_*`) is chosen at runtime from the GPU id.

Not bundled (importer copies a single `.so`); must exist in the guest rootfs, as PanVK already requires most of them:
`a64-glibc`: libdrm.so.2 libm libz.so.1 libzstd.so.1 libstdc++.so.6 libc ld-linux-aarch64.so.1.
`a64-glibc-wl` adds **libwayland-client.so.0**. `a64-glibc-wsi` adds libwayland-client.so.0 plus what `wsi_common_x11` links against:
libxcb.so.1, libX11-xcb.so.1, libxcb-dri3.so.0, libxcb-present.so.0, libxcb-xfixes.so.0, libxcb-sync.so.1, libxcb-randr.so.0,
libxcb-shm.so.0, libxshmfence.so.1 (all of them are dependencies of Xwayland in the Steam/Debian guest image).
The laptop has no aarch64 loader/qemu, so `ldd` of the aarch64 libs is not runnable here; `NEEDED.txt` carries `readelf` output.

## Verifier

`verify-zip.sh` unpacks into a scratch dir, checks the importer's conditions (meta kind, library name, aarch64, `libc.so.6`, checksums,
3 exported loader symbols), and for the native TESTONLY zip runs `t_icd` (loader and `--direct`) and `vulkaninfo --summary` with
`VK_DRIVER_FILES/VK_ICD_FILENAMES` set to the unpacked `icd.json`, `MALI_BACKEND=fake`, `MALI_FAKE_GPU=v11` and `v13`.
Last result: all three zips pass.

## Known limits

* Vulkan 1.0 device (`MALI_DEVICE_API_VERSION`); ICD json advertises 1.3.0 loader-side only.
* Works today (fake backend and R-series tests): enumeration/device on v11 and v13, buffers/images/descriptors, graphics draws on v11 and
  v13, compute, queries, command-buffer paths in `check.sh`. Pixel results need hardware; the zip itself is untested in the app.
* WSI (Wayland + X11 present, `a64-glibc-wsi`, M14) is work in progress; `a64-glibc` has no presentation path and `a64-glibc-wl` has no X11 surface.
* Needs `/dev/mali0` reachable in the guest (the app's `BL_DRM_STANDIN`); real kbase backend, otherwise zero devices.
* Version string is `0.1.0`; no conformance claim.

## App side (still open)

`mali-open-app-hook.patch` (applies with `git apply -p1` in upstream/DroidDeck, **not applied**, APK owned by another worker): a driver
whose `meta.json` name starts with `Mali-open` switches MaliBridge off while selected and counts as the open driver, so a plain zip works
without the `PanVK-*` bundle id trick.
