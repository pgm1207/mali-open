# tools/guest-driver — load our ICD inside the DroidDeck Steam guest

The app picks the guest's Vulkan ICD from its own switch (`vkBridge`, `linuxDriver`) and, with the
Mali bridge on, exports `VK_ICD_FILENAMES`/`VK_DRIVER_FILES` to `/usr/local/share/vkbridge/droidbridge_icd.json`
at every session start. `install-guest-driver.sh` makes the guest load the Mali-open ICD from a
`mali-open-<variant>.zip` **without** touching the app's selection or any pref.

## What it does inside the rootfs

| step | path | note |
|---|---|---|
| driver | `/usr/local/lib/mali-open/libvulkan_panfrost.so` | the `a64-glibc-wl` build (guest is a proot glibc rootfs) |
| manifest | `/usr/share/vulkan/icd.d/mali-open_icd.json` | absolute `library_path` |
| other manifests | `/usr/share/vulkan/icd.d.disabled/` | loader has exactly one choice on the default path |
| bridge manifest | `*.disabled` | the app rewrites it each session; the hook below wins |
| session hook | 4 lines in `/usr/local/bin/droiddeck-session` | sources `mali-open.env` just before the session starts |
| drop-in | `/usr/local/lib/mali-open/mali-open.env` | `VK_ICD_FILENAMES`/`VK_DRIVER_FILES` = our manifest, `BL_VKBRIDGE=0`, `BL_OPEN_DRIVER=1` |
| stage ledger | `/usr/local/.droiddeck-staged` | that one line is rewritten to the patched script's `length mtime canExecute`, otherwise `SessionFiles` re-stages the APK copy at the next start |

Rollback: `sh install-guest-driver.sh --uninstall`. Deleting `mali-open.env` alone is **not** enough: the install
moved every other `icd.d/*.json` into `icd.d.disabled/`, so with the bridge off the app would still pick our
manifest (the first `*.json` in `icd.d`). `--uninstall` removes `mali-open_icd.json` and the drop-in and moves the
displaced `icd.d` and bridge manifests back.

## Usage

```sh
# host: payload into the guest-visible Download dir. The directory must exist first: adb push with several
# sources requires the destination to be an existing directory, otherwise it copies nothing.
adb shell mkdir -p /sdcard/Download/mali-open-guest
adb push <zip-dir>/libvulkan_panfrost.so <zip-dir>/VERSION \
         tools/guest-driver/install-guest-driver.sh tools/guest-driver/vkprobe.py \
         /sdcard/Download/mali-open-guest/

# host: run inside the running session (Debugging tools on)
droiddeckctl guest -- sh /storage/emulated/0/Download/mali-open-guest/install-guest-driver.sh

# restart the session, then prove what the loader picked
droiddeckctl guest -- sh -c 'export VK_LOADER_DEBUG=error,warn,driver; python3 /usr/local/bin/mali-open-vkprobe'
```

Expected: `deviceName=Mali-G615 MC2 (mali-own)`, `driverName=mali-own`, `driverID=20 (MESA_PANVK)`
(there is no Mali-open `VkDriverId`; the driver reports PanVK's value, the nearest sibling, and the probe
prints its name), `driverVersion=0.1.0`, and
`Found ICD manifest file /usr/share/vulkan/icd.d/mali-open_icd.json` in the loader debug.

`droiddeck-env.example` is the app's own env-switch file
(`Android/data/com.droiddeck.launcher/files/droiddeck-env`); it is a secondary belt for sessions
started without the bridge, not a replacement for the hook.
