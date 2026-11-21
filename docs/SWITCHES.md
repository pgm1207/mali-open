# MALI_* switches

Scope: `origin/main` at the commit this file was written against (compile version 22). A switch is **in code** only
if the string is read with `getenv` (or set by a test/script) on main; PR-only switches are listed separately.
"Verified" means a named test exercises it; hardware verification is stated only where a note or doc says so.
Defaults were read from the cited line. Anything not read is marked `?`.

## Driver switches in `vk/src` (main)

| switch | default | effect | where | verified |
|---|---|---|---|---|
| `MALI_EXPERIMENTAL_MSAA` | off | `=1` advertises 4x sample counts and allows 4x pipelines; without it 4x pipelines are rejected | `mali_msaa.h:13`, `mali_pipeline.c:176` | host: `t_vk_draw msaa_*`, `check.sh` "msaa gate"; hardware: README says experimental, 4x `a2r10g10b10` flakes |
| `MALI_WSI` | auto | WSI backend select (`sw` needed on Xwayland, no DRM node) | `mali_wsi.c:51` | X8 guest run per steam-resume note (value `sw`); other values ? |
| `MALI_BACKEND` | kbase | `fake` selects the host fake backend | `mali_backend_fake.c:570` | used by every host test |
| `MALI_TRACE` | off | trace file path | `mali_instance.c:145` | `t_vk_trace` |
| `MALI_SHADER_CACHE` | disk cache default | cache directory | `mali_compile.c:220` | `t_compile`, `check.sh` |
| `MALI_OPT_GCM` | off | run `nir_opt_gcm` (compute only; bypasses disk cache) | `mali_compile.c`, `t_compile.c:202` | host only |
| `MALI_NO_MERGE` | off | disable workgroup merging for compute | `mali_compile.c:543` | ? |
| `MALI_IMAGE_TILED` | off | tiled image layout experiment | `mali_image.c:24` | ? hardware A/B in `vk/GPU-GAP.md` |
| `MALI_RING` | 1 MiB | kbase ring size | `mali_backend_kbase.c:122` | measured use in GPU-GAP; no test |
| `MALI_SUBMIT_DBG` | off | submit debug print | `mali_backend_kbase.c:119` | n/a (debug) |
| `MALI_INT_NOCOH` | on for arch 11 only | interconnect non-coherent flag | `mali_backend_kbase.c:291` | hardware A/B, GPU-GAP |
| `MALI_BO_COHERENT_LOCAL` / `MALI_BO_POOL_MB` | ? | BO coherency / pool size | `t_bopool.c:24-25` (test sets them) | `t_bopool`; the read site is not in the listing above, so defaults are `?` |
| `MALI_FRAME_TIMEOUT_MS` | ? | frame wait timeout | `mali_queue.c:435` | `t_fault`/fault harness (PR #220, unmerged) |
| `MALI_HEAP_CHUNK`, `MALI_HEAP_INIT` | `HEAP_CHUNK` / 4 | tiler heap sizing | `mali_queue.c:446-447` | ? |
| `MALI_SYNC_SUBMIT` | off | disables async submit when frames are in flight | `mali_queue.c:293` | ? |
| `MALI_FENCE_ALWAYS`, `MALI_QUEUE_CPU_WAIT`, `MALI_OPS_FLUSH` | off | submit/fence behaviour toggles | `mali_queue.c:578,517`, `mali_event.c:93` | ? |
| `MALI_PROF`, `MALI_PIPE_DBG`, `MALI_META_DBG`, `MALI_CSDUMP`, `MALI_ALLOC_STATS`, `MALI_IOCTL_PROF` | off | debug/profile output | `mali_queue.c:342,347,331`, `mali_meta_vk.c:99`, `libkbase/src/ctx.c:42` | n/a (diagnostics) |
| `MALI_GEOM_SIZE`, `MALI_TASK_THREADS`, `MALI_CHF`, `MALI_HF`, `MALI_CV`, `MALI_CV_OTHER`, `MALI_ZR`, `MALI_NOTF`, `MALI_DIRTY_CHECK` | see source | command-stream emit tuning/A-B knobs (`MALI_HF`/`MALI_CHF` default 0x332) | `mali_cs_emit.c:235,893-1100` | hardware A/B experiments only |

## Fake-backend / test-only switches

`MALI_FAKE_GPU` (`v11`/`v13`), `MALI_FAKE_SLOW_US`, `MALI_FAKE_DUMP`, `MALI_FAKE_BADFLAGS` (`mali_backend_fake.c:120,287`),
`MALI_FAULT_INJECT` (`t_queue.c:538`), `MALI_PRESENT_FENCE` (`t_queue.c:661`), `MALI_SKIP_CASES`, `MALI_MANY_COUNTS|DYN|REPS`
(`t_vk_draw.c`), `MALI_TEST_FORCE_DIDX_BUF` (`t_vk_feat.c:2839`), `MALI_ICD` (`vk/deqp/vkshim.c:12`), `MALI_SHADERDB_REPS`.
Host-only by construction.

## Farm switches (`tools/farm`)

`MALI_FARM_*` (DEVICES, STATE, SWEEP_*, MENUBENCH, RUNAWAY_SECS, HB_*, DOWN_AFTER, AUTO_*, CONFIG, ...) are read by the farm
scripts. Not individually verified here; see `tools/farm/README.md`.

## Documented but NOT in main's code

| switch | where documented | finding |
|---|---|---|
| `MALI_ADVERTISE_GS`, `MALI_ADVERTISE_CULL`, `MALI_ADVERTISE_DESCIDX` | `tools/megabonk/README.md:17,24`, `run.sh:101-103` | **Do not exist on main.** They are implemented only in unmerged PRs #230 (GS), #164 (cull), #166 (descidx). On main, setting them does nothing. |
| `MALI_ZS_CTW`, `MALI_WSI_HEAP`, `MALI_MEMSYNC_EX`, `MALI_COH_ON` | `vk/GPU-GAP.md:90-102` | Experiment knobs from past A/B sessions; no `getenv` on main. Historical. |
| `MALI_META_BC_FIRST`, `MALI_META_NSHADERS`, `MALI_DYN_TABLE`, `MALI_BO_EVENT/EXEC`, `MALI_DEVICE_API_VERSION` | R3/R4 plans, `vk/package/README.md:63` | C macros / enum names, not env vars. |
| `MALI_SAN`, `MALI_TARGET`, `MALI_COMPILER`, `MALI_FARM_DIR`, `MALI_FARM_RUNAWAY_CPU` | `vk/deqp/README.md`, farm README | Build/script variables outside `vk/src` (not checked further). |

## Exist on main, previously undocumented

`MALI_ALLOC_STATS`, `MALI_BO_POOL_MB`, `MALI_CSDUMP`, `MALI_CV_OTHER`, `MALI_DIRTY_CHECK`, `MALI_FENCE_ALWAYS`,
`MALI_MANY_*`, `MALI_OPS_FLUSH`, `MALI_QUEUE_CPU_WAIT`, `MALI_SKIP_CASES`, `MALI_SYNC_SUBMIT`, `MALI_TEST_FORCE_DIDX_BUF`,
`MALI_ZR` (all listed above).

## Switches that exist only in open PRs (not on main)

| switch | PR | default | note |
|---|---|---|---|
| `MALI_ADVERTISE_GS` | #230 | off | GS shim; pipelines still refused until a draw path exists (gs-plan PR 4) |
| `MALI_ADVERTISE_CULL` | #164 (#233 advertises it directly) | off | cull-distance writes ignored |
| `MALI_ADVERTISE_DESCIDX` | #166 | off | descriptorIndexing shim; #244 is the honest-subset alternative |
| `MALI_CULLDIST=1` | #203 | off | cull as clip, stage 1 |
| `MALI_AFBC` | #169 and the AFBC stack (#190-#209) | off, unused on main | layout maths only; hardware evidence in #216 |
| `MALI_OPT_GCM=post` | #159/#161 | off | register-pressure guard |
| `MALI_DESCIDX_FLAGS` | #176 (stages in #177, #188) | off | per-binding descriptor-indexing flags; read at `getenv` on lane/descidx2 |
| `MALI_TSAN` build flag | #211 | n/a | build option |
