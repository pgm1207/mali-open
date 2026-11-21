# tools/dxvk — pinned DXVK build for the Mali driver

This directory is the single home of the DXVK build we run on the Mali (mali-open)
Vulkan driver in a DroidDeck/Proton guest. It was consolidated from the retired
`pgm1207/DroidDeck` lane (`lane/dxvk-async-pipeline`, stacked on
`lane/dxvk-compat-shim`); that fork is no longer needed for DXVK work.

`build-aarch64.sh` fetches the pinned upstream commit, applies every patch in
`patches/` in filename order, runs the source checks, cross-builds the five
aarch64 Windows DLLs (PE machine `0xaa64`) and writes `MANIFEST` and
`SHA256SUMS` into an empty output directory. It does not touch a phone, an APK
or an installed Proton prefix.

```
# one-time: llvm-mingw + meson/ninja (third_party/venv from CONTRIBUTING.md)
export PATH=/path/to/llvm-mingw/bin:$PATH        # or DXVK_MINGW_BIN=/path/to/llvm-mingw/bin
tools/dxvk/build-aarch64.sh /tmp/dxvk-aarch64    # must not exist or must be empty
sha256sum -c /tmp/dxvk-aarch64/SHA256SUMS
```

## Pin

`source.env`: `DXVK_REVISION=25ca63f17f34bdc05a39873ee63907d3cbbfa030`, DXVK
3.1.1. Every patch applies to that commit; `build-aarch64.sh` verifies the
fetched `HEAD` before patching and runs `git apply --check` per patch, so a
rebased or dropped hunk fails the build instead of producing a DLL.

## Patch set

The aarch64 build used for the Megabonk fast-path work carries six patches. All
of them are needed to reproduce the staged hashes; 0005 and 0006 are the ones
this consolidation is about, and 0001–0004 were already on the DroidDeck tree
(commit `3733c509`, "non-Adreno graphics stack") and are carried here so the
toolchain survives the fork's deletion.

| patch | why it exists |
|---|---|
| `0001-check-direct3d-bc-formats.patch` | report the BC formats the driver can actually sample, instead of advertising the whole set |
| `0002-present-semaphore-per-swapchain-image.patch` | one present semaphore per swapchain image; the single shared semaphore can be re-signalled while still in use |
| `0003-hud-explicit-r8-texel-format.patch` | the HUD frame-time compute shader needs an explicit `r8` texel format for the Mali compiler |
| `0004-d3d9-force-single-sample.patch` | D3D9 MSAA resolve paths on this driver: force single-sample for the depth/stencil and lockable paths that cannot be resolved safely |
| `0005-relax-required-features.patch` | **local shim**: let DXVK device creation continue without two `featureRequired` entries the driver cannot provide (below) |
| `0006-dxvk-gplasync-3.1.1-1.patch` | **upstream gplasync**: compile missing pipelines on a worker thread and skip that draw instead of stalling the submit thread (below) |

The five checks in this directory run inside `build-aarch64.sh` before the
build: `check_present_sync.py` (0002), `check_compat_relaxation.py` (0005),
`check_async_pipeline.py` (0006), `check_hud_storage_format.py` (0003) and
`check_d3d9_single_sample.py` (0004). They refuse a tree that no longer looks
like the one reviewed; they do not run a game or a GPU.

## The compatibility shim (0005)

`patches/0005-relax-required-features.patch` is **a local shim, not upstream
DXVK**, and it is the only patch here that changes DXVK's notion of a
compatible device.

`DxvkDeviceCapabilities::checkDeviceCompatibility()`
(`src/dxvk/dxvk_device_info.cpp`) fails device creation when any entry flagged
`featureRequired` is absent. On the Mali driver we run, two of those entries
can never be true, so every D3D11 title fails with `Device does not support
required feature ...` before a single draw. The shim turns their absence into a
warning and keeps `robustBufferAccess`, `robustBufferAccess2`,
`sampleRateShading` and the push-constant check as hard requirements. Anything
not listed still fails device creation.

| relaxed | why it is harmless for what we run |
|---|---|
| `geometryShader` | Valhall/Bifrost have no geometry stage; the driver also refuses a GS pipeline at `vkCreateGraphicsPipelines`, so a title that really used one fails loudly at pipeline creation instead of rendering wrongly. |
| `shaderCullDistance` | No shipped title writes `SV_CullDistance`; the driver ignores cull writes (`MALI_ADVERTISE_CULL` is off by default). A title that wrote one renders without the cull, not a crash. |

Each relaxation logs exactly one warning per device creation:

```
warn:  mali-open: dxvk compat relaxation: geometryShader absent, allowed
```

`check_compat_relaxation.py` refuses a tree whose relaxed set is not exactly
those two names with their log lines, or whose `checkDeviceCompatibility()`
does not consult the shim.

### Push constants are not a gate any more

This section used to end at a 256 B push-constant wall: DXVK requires
`MaxTotalPushDataSize` (256 B) and the driver reported `maxPushConstantsSize =
128`, so device creation failed before the shim mattered. That work merged in
mali-open PR #242, and `t_dxvk_caps` reports the remaining DXVK-required
features as `DXVK_MISSING=4`. The shim relaxes no push-constant check and needs
no new one.

### Risks: a title that uses geometry shaders

A title that creates a pipeline with a geometry shader is unsupported on this
driver regardless of the shim: `vkCreateGraphicsPipelines` returns
`VK_ERROR_FEATURE_NOT_PRESENT`, usually as a black frame or a device-lost
error, not silent wrong output. Tessellation-heavy UE4/Unity HDRP titles, some
emulators and a few DX11 ports are the likely candidates. Detection:

* DXVK log: a `mali-open: dxvk compat relaxation: geometryShader absent,
  allowed` line means only that the shim ran; a later `Device does not support`
  error or a black frame means the title wants the stage.
* The driver's own stderr line for a refused GS pipeline (one per process).

## Async pipelines (0006)

`patches/0006-dxvk-gplasync-3.1.1-1.patch` is upstream **gplasync v3.1.1-1**
(<https://gitlab.com/Ph42oN/dxvk-gplasync>, tag `v3.1.1-1`, commit `9a835c1`,
upstream patch sha256 `2542bf4f…`), not a local invention. The only change we
make is one context line: upstream was generated against a tree that has
`entry.first->second.status.store(status, std::memory_order_release);` in
`dxvk_graphics.cpp`; our pin has the same store without the explicit ordering.
No applied hunk depends on that line, and 0005 does not touch
`dxvk_graphics.cpp`, so the two patch sets are independent.

What it does: when a draw needs a pipeline that has not been created yet, DXVK
normally creates it on the submitting thread inside `getPipelineHandle()` and
busy-waits if another thread is already compiling it. That is the 2–6 ms
first-draw hitch Megabonk shows as stutter (each new Unity shader variant).
With the patch, `DxvkGraphicsPipeline::getPipelineHandle(state, async)` takes
the pipeline's own `m_asyncMutex`, hands the compile to the pipeline worker
pool at `DxvkPipelinePriority::High` and returns an empty handle, so
`DxvkContext` skips that draw and re-tries it on the next frame; by then the
worker has stored the instance and `m_async` is back to `false`. Async is
refused for a pipeline whose render-target attachments are not async-safe
(`DxvkContext::checkAsyncCompilationCompat()`), and fast-linked ("base")
pipelines still compile synchronously, so only variants that actually hitch
move off-thread.

Runtime switches (no rebuild): `dxvk.enableAsync = False` (or `0`) in
`dxvk.conf`, or `DXVK_ASYNC=0` in the environment. Default is enabled, matching
gplasync.

### Risks: async compilation artefacts

* **Skipped draws and pop-in.** A draw is skipped while its pipeline compiles,
  so for the one or two frames before the worker finishes a title can show a
  missing object, a frame without that material, or a pop-in. Titles that read
  back the current frame or render in strict order can notice.
* **Titles that rely on synchronous compile.** Anything that assumes a draw
  always executes (frame-perfect capture/replay, some anti-cheat screenshots,
  multi-pass effects that sample the previous pass's target, compute-style
  feedback loops) may produce wrong frames or a validation error instead of a
  stall. Megabonk (Unity 2023.2 built-in forward, no frame readback) is not one
  of them.
* **Shader caches.** A skipped pipeline is not logged to the state cache until
  it is compiled; closing a level in the first seconds leaves a colder cache
  than a synchronous run, so the next launch may hitch again.
* **Not a driver fix.** It hides pipeline-compile latency from the submit
  thread; total compile work is unchanged. Real GPL
  (`VK_EXT_graphics_pipeline_library`) remains the driver gap.

## Revert

Runtime, any prefix:

1. Async only: `dxvk.enableAsync = False` in `dxvk.conf`, or `DXVK_ASYNC=0`.
2. Async only, build-side: delete `patches/0006-dxvk-gplasync-3.1.1-1.patch`
   and rebuild. 0005 and the other four patches are untouched; no file the
   shim modifies is modified by 0006. Unpatched `d3d11.dll` no longer contains
   `dxvk.enableAsync` or `DXVK_ASYNC` (`strings -a aarch64/d3d11.dll | grep -c
   dxvk.enableAsync` → 1 patched, 0 reverted).
3. Shim only: delete `patches/0005-relax-required-features.patch`. Device
   creation then fails again on `geometryShader` (or `shaderCullDistance`),
   which is the intended behaviour for a driver that advertises the feature.
4. Both: restore a stock DXVK 3.1.1 build, or rebuild with an empty patch set.
   The guest-side swap backup (`dxvk-backup-*` next to the prefix) is the
   fastest revert for an installed prefix.

## Manifest and reproducibility

`MANIFEST` records the source commit, every patch name + sha256, the rebase
note for 0006, the toolchain, and for each DLL its PE machine, size and sha256.
`SHA256SUMS` lists the same five hashes. The source is pinned, and
`DXVK_BUILD_EPOCH` in `source.env` fixes `SOURCE_DATE_EPOCH` because lld
otherwise stamps each PE with the wall clock. With the pinned llvm-mingw, two
clean builds of the same revision and patch set are therefore bit-identical;
the aarch64 build staged for the Megabonk work (`d3d11.dll` sha256 `caa113e8…`)
is the reference. Compare with:

```
cd /tmp/dxvk-aarch64 && sha256sum -c SHA256SUMS
```

Install only per-file, never by copying a directory: every DLL must report
machine `0xaa64`, and `aarch64/d3d11.dll` must carry the two `mali-open: dxvk compat
relaxation:` strings and both `dxvk.enableAsync` / `DXVK_ASYNC`.
