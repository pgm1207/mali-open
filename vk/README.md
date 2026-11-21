# driver/vk: Vulkan front end (R3)

Own ICD on Mesa's `vk_*` runtime. Plan and milestones: `../R3-PLAN.md`. State: M2 (cross builds, kbase backend, both phones enumerate and create a device).

## One-time Mesa setup (pinned, unmodified mesa-26.3.0)

    cd ../third_party
    PATH=$PWD/venv/bin:$PATH meson setup build-vk mesa -Dbuildtype=release -Dgallium-drivers= \
        -Dvulkan-drivers=panfrost -Dtools= -Dglx=disabled -Dplatforms= -Degl=disabled -Dgbm=disabled \
        -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dllvm=enabled -Dvalgrind=disabled \
        -Dlibunwind=disabled -Dpanfrost-rust=false -Dmesa-clc=enabled -Db_ndebug=false
    PATH=$PWD/venv/bin:$PATH ninja -C build-vk src/panfrost/vulkan/libvulkan_panfrost.so

`-Dplatforms=` is empty until M14 (WSI), which reconfigures with `-Dplatforms=wayland`.

### X11 WSI (round 6, optional)

For `VK_KHR_xcb_surface` / `xlib_surface` (zink/GLX, Steam's X11 clients without the gamescope layer) Mesa must be configured with the x11 platform
(`wsi_common_x11`). Host (`build.sh` picks this tree for `native`/`native-san` when it exists; otherwise the X11 tests skip):

    cd ../third_party
    PATH=$PWD/venv/bin:$PATH meson setup build-vk-x86-x11 mesa <same options as above> -Dplatforms=wayland,x11
    PATH=$PWD/venv/bin:$PATH ninja -C build-vk-x86-x11 src/panfrost/vulkan/libvulkan_panfrost.so

Guest (aarch64 glibc, Wayland + X11): `./cross/build-cross.sh glibc-wsi` -> `out/a64-glibc-wsi/` (not part of `all`). Present path: see `MALI_WSI` in
`STEAM-GAPS.md` "X11 WSI". Tests: `tests/xwsi-host.sh` (in `check.sh`), `tests/x11guest.sh` (hardware, via qdrv; pushes `x11wsi_libvulkan_mali.so`, `x11wsi_t_vk_xcb`).

The PanVK target is built only because it pulls in every static library we link (runtime, WSI, NIR, SPIR-V, panfrost
compiler and lib) and gives `build.sh` a reference compile/link line. No PanVK object, header or symbol ends up in our
library (`check.sh` verifies that).

## Build and check

    ./build.sh     # out/native/{libvulkan_mali.so, mali_icd.json, t_icd}
    ./check.sh     # loader + direct dlopen on fake v11/v13, export list, vulkaninfo

## Cross builds and phones (M2)

    ./cross/build-cross.sh all        # out/a64-glibc (guest) and out/a64-bionic (adb shell direct); 0 Mesa patches
    # bionic, adb shell:  t_icd --direct ./libvulkan_mali.so --expect-name G615     (no loader, uses vk_icdGetInstanceProcAddr)
    # guest:  push out/a64-glibc/{libvulkan_mali.so,t_icd} to Download/m2vk_*, then  scripts/quick/qdrv <cmf|poco> driver/vk/tests/m2guest.sh

`t_icd` fails when the process holds more fds after vkDestroyDevice than before vkCreateInstance. Without `MALI_BACKEND=fake`
the kbase backend is used (opens `/dev/mali0`; zero devices on machines without it).

## Environment

| Variable | Meaning |
|---|---|
| `MALI_BACKEND=fake` | No hardware; GPU identity from `MALI_FAKE_GPU=v11` (G615 MC2) or `v13` (G925 MC11). Without it: real kbase backend. |
| `MESA_SRC`, `MESA_BUILD` | Mesa source and build dir for `build.sh` (cross builds use another build dir, see M2). |
| `MALI_TARGET` | Output subdir name under `out/` (default `native`). |

## Layout

| File | Content |
|---|---|
| `src/mali_instance.c` | Instance, the 3 `vk_icd*` loader exports, physical device enumeration callback. |
| `src/mali_physical_device.c` | Properties, features, limits (Vulkan 1.0 minimums), queue family, memory heap/types. |
| `src/mali_device.c` | Device and queue create/destroy; picks the `mali_v11_*` or `mali_v13_*` entry point table. |
| `src/mali_backend.h`, `mali_backend_fake.c` | The seam to libkbase (`mali_backend_kbase.c`: libkbase probe, per-device context + queue). |
| `tests/t_icd.c` | Loader-free and loader-based smoke test. |

## M6: `mali_compile()` (offline, no GPU)

`src/mali_compile.{c,h}`: SPIR-V → NIR (`spirv_to_nir`) → the spike's lowering (I/O, varying layout, push constants at `push_base` into
the FAU table, sysvals) → Mesa `bi_*` backend → `struct mali_compiled` (binary, work regs, preload, FAU count/reserved + promoted immediates,
varying slots, VS secondary shader, FS flags). v11 and v13, VS/FS/compute stub (compute: descriptor-free, base ids 0; descriptors = M7).
Pairing: compile the VS, pass `vs.layout` as `link_vs` of the FS (FS reads the varying buffer); `link_vs = NULL` = R2's attribute-descriptor FS.
Disk cache: `$MALI_SHADER_CACHE`, else `$XDG_CACHE_HOME`/`~/.cache/mali-gaming/shaders`; `cache_dir=""` disables. Key = blake3(SPIR-V, arch,
stage, fau_reserved, push_base, VS layout, entry, Mesa tag, `MALI_COMPILE_VERSION`); atomic write, corrupt entries recompile.
Test: `tests/build-t_compile.sh [-v]` (x86 Mesa tree, ~1.5 s build, 0.01 s run). `rot.vert`/`var.frag` via SPIR-V are byte-identical to
`libkbase/tests/shaders_compiled_v11.h` and to `shader/golden_compiled_v13.h` (`shader/gen-golden.sh`); SPIR-V from `shader/gen-spv.sh` (glslang).
Not yet: blend shaders, descriptor lowering (M7), `vk_device_shader_ops`/`vk_pipeline_cache` wiring, the 300-module dEQP cache corpus (needs M3).

## M8: command buffers, rendering, CS emission (offline, no GPU)

`src/mali_cs_emit.{h,c}` (Mesa-free core: frame -> descriptors + CS, v11/v13 by runtime arch), `src/mali_render.c` (FBD), `src/mali_cmd.{h,c}` (`vk_command_buffer` ops and `mali_Cmd*` entrypoints; seams `mali_cmd_set_hw`, `mali_cmd_set_res`).
Test: `tests/build-t_cmd.sh [-v]` builds `t_cmd11`/`t_cmd13` (static ICD + libkbase fake backend), records the t_uniform scenes, diffs against libkbase's t_uniform dumps (`tests/cmp_cmd.py`) and runs the decoder rules.
Wired in M9 (below); limits: R3-PLAN.md "M8 status".

## M7: descriptors (offline, no GPU)

`src/mali_descriptor.{c,h}` set layouts (`mali_dsl` on `vk_descriptor_set_layout`), pools (first-fit in one 32-byte-slot arena), sets, writes/copies/templates, dynamic
buffers (kept as base+size, offset applied by `mali_dyn_buf_pack` at bind), hardware packers (texture, plane, sampler, buffer; bytes equal libkbase's v11 and v13).
Slots: SAMPLER/SAMPLED_IMAGE/STORAGE_IMAGE/texel buffer/UBO/SSBO 1, COMBINED_IMAGE_SAMPLER 2 (texture, sampler). Resource tables: 0 = driver (dummy sampler idx 63, dynamic buffers
from 64), set n = table n+1; push constants = FAU via `push_base`. `src/mali_pipeline_layout.{c,h}` flattens a `vk_pipeline_layout` (runtime's `vk_common_CreatePipelineLayout`) to
`mali_compile_layout` (`mali_pipeline_layout_flat.h`, plain C). `src/mali_descriptor_lower.{c,h}` NIR pass (resource_index/reindex/load_vulkan_descriptor, tex/image derefs -> `pan_res_handle`,
UBO/SSBO `vec2_index_32bit_offset` + scalar pack); `mali_compile_opts.layout` enables it (NULL keeps the descriptor-free stub; cache key includes the layout).
Seams for others: `mali_desc_set_res_ops` (image view/sampler/buffer -> hardware data, M5), `mali_desc_set_mem_ops` (pool memory, M2); entrypoints are `mali_create_descriptor_*` etc. taking `(dev, arch)`.
`build.sh` must add mali_descriptor.c, mali_descriptor_lower.c, mali_pipeline_layout.c and mali_compile.c. Test: `tests/build-t_desc.sh [-v]` (5 s build, 0.01 s run; shaders `desc.frag`, `desc.comp`).
Not yet: push descriptors, inline uniform blocks, image-size queries, immutable-sampler hardware check, dEQP.

## M5: buffers, images, views, samplers, formats (offline, no GPU)

`src/mali_format.{c,h}` (VkFormat -> hw word, RT/ZS codes, features; from the linked `pan_*_v11/v13` tables, v10+ encoding), `mali_image.{c,h}` (layout, 32 B texture + plane payload
descriptors, image/view objects on `vk_image`/`vk_image_view`), `mali_buffer.c` (buffers, texel buffer views, `mali_res_ops` for the descriptor code), `mali_sampler.c`.
Layout: linear (rows/slices/image 64 B aligned) by default; `MALI_IMAGE_TILED=1` selects 16x16 u-interleaved (unverified on hardware). Buffers align 256. D+S = depth plane + S8 plane.
Limits: 2D 4096, 3D 256, 256 layers, 4096 -> 13 mips, 1/4 samples. View payload comes from `mali_image_set_mem_ops` (default malloc).
Wiring (build.sh sources, entrypoints, `mali_desc_set_res_ops(&mali_res_ops)`, memory ops) is listed in R3-PLAN.md "M5 status". Test: `tests/build-t_image.sh [-v]` (no GPU, 0.00 s run).
Not yet: ASTC/YUV, sparse, DRM modifiers, hardware runs.

## M4: memory, sync, queue submit

`src/mali_memory.c` (heaps/types over kbase BOs, dma-buf export/import), `src/mali_sync.c` (binary sync_file vk_sync, emulated timeline), `src/mali_queue.c` (submit = empty CS flush + signal). Test: `out/<target>/t_queue [--direct lib.so] [--only mem|dmabuf|idle|binary|timeline|syncfile|submits|fault] [--submits N] [--burst N]`.
Env: `MALI_FAULT_INJECT=n` (n-th submit fails -> DEVICE_LOST), `MALI_FAKE_SLOW_US` (late fake fences), `MALI_BO_COHERENT_LOCAL=1` (BO flag 0x800).
`--burst N` = unwaited submits in `idle`; `MALI_SUBMIT_DBG=0x80` dumps queue state and ring on a failed submit (0x20: 128-byte submissions).
Status: offline all green; CMF all cases pass incl. 10000 unwaited and 10000 waited submits (2026-11-16). The ring must never be completely full
(`insert - extract == size` gives CS_CONFIG_FAULT): one 64-byte unit stays free, see R3-PLAN.md. Cached allocations are zeroed at alloc.

Per-frame profiling (`src/mali_fprof.c`): `MALI_FRAME_PROF=1` (stderr) or `=<path>` prints CPU submit time, a GPU busy
*estimate* and the present interval per frame, plus a summary at device teardown; `tools/frameprof <log>` summarises a log.
Off (no ioctl, no clock read) when unset. What it measures and its accuracy: `FRAME-PROF.md`.
Test: `out/<target>/t_frameprof --direct lib.so [--off]` (`--off` = no output at all when unset).

## M9: graphics pipelines, queue wiring, client draw test

`mali_pipeline.c` compiles VS+FS through `mali_compile`; `mali_queue_submit` runs the M8 command buffers via `mali_cmd_submit` (one frame at a time).
`tests/t_vk_draw --direct <icd.so> [--loop N] [--loop-reset] [--only triangle|two|depth]` draws through the Vulkan API and checks pixels (skipped on the fake
backend). Offline: `./check.sh`. CMF hardware: PASS for triangle, two draws, depth; texture/uniform clients pending. Details: R3-PLAN.md "M9 status".

## hwbench: bandwidth, ALU or tiler?

`tests/t_vk_bench --direct <icd.so> [--spv-dir dir] [--mode all|bw|alu|geom] [--frames N] [--quick] [--json]` runs
three microbenchmarks through the Vulkan API and sweeps the resource each one is about: (a) one large-texture fetch
per pixel over texture sizes (cache knee, then DRAM slope), (b) 64 dependent FMAs per pixel over target sizes
(ns/pixel), (c) 1k-1M vertices of 2-pixel triangles with a constant-colour shader (ns/vertex, primitives/second).
Rows carry wall ms/frame, CPU submit time and GPU time from `VK_QUERY_TYPE_TIMESTAMP` where the backend has it.
`tools/hwbench [--phone cmf] [--fake v13]` runs all three and prints one table; how to read it: `HWBENCH.md`.

## M12/M13: meta operations, barriers, events, timestamps (offline, not wired)
See `R3-PLAN.md` "M12/M13 status" for design, unsupported list, wiring list and HW test plan. Tests: `tests/build-t_meta.sh [-v]`, `tests/build-t_query.sh`,
`tests/check-meta-vk.sh mali_meta_vk`. Shaders: `shader/gen-meta-spv.sh` regenerates `src/mali_meta_spv.h` (glslang + spirv-val).

## M12/M13 (offline)
Meta ops (`mali_meta*`), query pools (`mali_query*`), events/ops frames (`mali_event*`). Tests: `tests/build-t_meta.sh`, `build-t_query.sh`, `build-t_qpool.sh`. Occlusion needs `patches/occlusion-cs-emit.patch`; see R3-PLAN.md.

## Known gaps in advertised features (feat-audit, 2026-11-18)
- **Colour attachments:** `maxColorAttachments`/`maxFragmentOutputAttachments` are the Vulkan minimum (4) but only ONE colour attachment renders; the check is on `colorAttachmentCount`, not on the number of non-null views: unless every slot is null (depth/stencil-only is fine) the count must be exactly 1 with a non-null view, so `colorAttachmentCount` 2 fails `vkEndCommandBuffer` even when only one view is non-null (`mali_cmd.c`, "need exactly one colour attachment"). Nulling the extra slots does not make a multi-slot rendering valid. `independentBlend` is true only because of that.
- **MSAA:** 4x sample counts (limits, image format properties), `sampleRateShading` and 4x pipelines are all gated on `MALI_EXPERIMENTAL_MSAA=1`; off by default the driver advertises 1x only (`t_vk_info` checks both modes). With the gate on, `vkCmdClearAttachments` inside an MSAA pass is still unsupported (M8 gap, `mali_cmd.c`) and resolve/LOAD/depth cases are limited.
- **multiViewport:** `maxViewports`=16 but viewports 1..15 are accepted and ignored (no geometry shader / ViewportIndex output).
- **Indirect draws** are read on the CPU from a mappable buffer (`mali_cmd.c`); GPU-written indirect arguments are not supported.
- **driverID** is `MESA_PANVK` on purpose: zink applies PanVK workarounds keyed on it.
- Device apiVersion is 1.3 (older notes saying 1.0 are stale) but not every 1.3-mandatory feature is set; see `vk/FEATURE-AUDIT.md`.
