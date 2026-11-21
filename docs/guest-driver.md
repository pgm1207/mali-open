# Running our ICD in the DroidDeck Steam guest

Status: **works on the CMF (v11)**, verified 2026-11-19 — see the evidence below. Untested on v13
(the X8 has no DroidDeck/MediaDeck installed, and installing an APK is out of scope).

## Why the guest never loaded us

The guest is a proot glibc rootfs (`/dev/mali0` visible, pipewire/pulseaudio, wayland).
Its Vulkan loader is the rootfs' own `libvulkan.so.1` and the app decides the ICD for it:

- `SessionService.kt:891` — with the bridge off, `VK_ICD_FILENAMES=<rootfs>/usr/share/vulkan/icd.d/<first *.json>`
  (`LinuxRuntime.vulkanIcd()` prefers `freedreno*`). With the bridge **on** (the default on Mali) the app
  adds nothing and `droiddeck-session` (`tools/linuxfs/overlay/usr/local/bin`, bridge block at ~line 264)
  exports `VK_ICD_FILENAMES=VK_DRIVER_FILES=/usr/local/share/vkbridge/droidbridge_icd.json`.
- `LinuxVulkanDriver.resolveIcdPath` → `BL_VK_DRIVER` is only consulted when the bridge is off; the
  bridge only uses it as `BL_VKBRIDGE_FALLBACK_ICD` after a failed self-test.
- So the app's "Linux driver" pref cannot beat the bridge, and the PanVK folder never appeared because
  no Linux driver bundle was ever imported (`files/linux_vulkan_drivers/` does not exist on the CMF).

## What sticks and what does not (measured)

| path | survives a session restart? |
|---|---|
| `/usr/share/vulkan/icd.d/*` | yes (11 packaged Mesa manifests moved to `icd.d.disabled/` stayed moved) |
| `/etc/vulkan/icd.d/*` | yes |
| `/usr/local/**` (app overlay) | **no** unless the stage ledger is updated — `SessionFiles` re-stages from the APK when `length mtime canExecute` differs from `/usr/local/.droiddeck-staged` |
| `/usr/local/share/vkbridge/droidbridge_icd.json` | no: rewritten at every start |
| new files anywhere | yes |
| `Android/data/com.droiddeck.launcher/files/droiddeck-env` | yes, read as extra env, but the bridge export still wins for the ICD keys |

Hence the install: payload + manifest outside `/usr/local`, all other manifests disabled, and a 4-line
hook in `droiddeck-session` (sourcing a drop-in) whose stage-ledger line is rewritten so the app keeps it.

## Evidence (CMF, 192.168.3.111, 2026-11-19, session READY, `vkBridge=true` still set in the app)

`droiddeck-env`-free run, session restarted, `VK_LOADER_DEBUG=error,warn,driver`, twice:

```
Found ICD manifest file /usr/share/vulkan/icd.d/mali-open_icd.json, version 1.0.1
physical devices: 1
[0] deviceName=Mali-G615 MC2 (mali-own)
    driverID=20 (VK_DRIVER_ID_SAMSUNG_PROPRIETARY)  driverName=mali-own
    driverInfo=mali-open 35e22d9c29 (mali-own), not conformant
    driverVersion=0.1.0 (0x00001000)  apiVersion=1.3.363  deviceType=integrated-gpu  vendor=0x13b5 device=0xb8a3
```

and the session's own process tree carries the same env:

```
23491 /bin/bash /usr/local/bin/droiddeck-session steam -> /usr/share/vulkan/icd.d/mali-open_icd.json
23612 gamescope ...                                      -> /usr/share/vulkan/icd.d/mali-open_icd.json
```

Payload sha256 `1eafe04a0a82274470b8d775e26b781ec7feb91c5bc0e7b952e89caf93cb7edf`
(zip `mali-open-a64-glibc-wl-20261119-main35e22d9.zip`, sha256 `8c408567…7eb264e`).

Known gaps seen here: `driverID` is our own slot (20) instead of `MESA_PANVK`/`ARM_PROPRIETARY`, and
`driverInfo` says "not conformant" — cosmetic for DXVK/Proton, tracked separately.

## Repeat it

`tools/guest-driver/README.md` has the exact host commands; in short: push the payload and
`tools/guest-driver/install-guest-driver.sh` to `/sdcard/Download/mali-open-guest/`, run the script through
`droiddeckctl guest`, restart the session, run `tools/guest-driver/vkprobe.py`.

Revert: delete `/usr/local/lib/mali-open/mali-open.env` and touch `/usr/local/bin/droiddeck-session`
(the app then re-stages its own copy); restore `icd.d.disabled/*` if the default path is wanted back.
