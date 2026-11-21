# mali-open status (no spin)

Written 2026-11-19 against `origin/main` (MALI_COMPILE_VERSION 22) from the lane notes and a read of main's code.
Source tags: `PR #n` (open unless stated), `test name`, or a measured number with its note. `unverified` = no source found.
Everything described as "merged" is on main; the 150+ open PRs are **not** on main and are listed as such.

## 1. What the driver supports on main today

| area | state | source |
|---|---|---|
| Vulkan device / API version | 1.3 (reports 1.3.363), "not conformant" | `vk/src/mali_private.h:69`; guest-driver.md log |
| GPUs | G615 (v11), G925 (v13) full path; G610 (v10) partial, on `feat/v10` only | `docs/progress.md` |
| Draw, compute, copy/blit/clear/queries, WSI (Wayland) | done on v11; v13 WSI "pending", meta partial | README table, `docs/progress.md`, `t_vk_draw`, `t_vk_compute`, `t_vk_meta`, `t_vk_present` |
| Colour attachments per pass | **exactly 1** (advertises `maxColorAttachments` 4) | mrt.md; `mali_cmd.c` rejects others; PR #75..#239 chain unmerged |
| Sample counts | 1 and 4 (4 only with `MALI_EXPERIMENTAL_MSAA=1`) | `mali_physical_device.c:20`, README |
| `sampleRateShading` | advertised, a no-op at 1 sample | `mali_physical_device.c:286`, `t_vk_feat samplerate` |
| Push constants | 128 B (`maxPushConstantsSize`) | `mali_physical_device.c:39`; 256 B is PR #242 |
| `maxBoundDescriptorSets` | 4 (8 is PR #235) | `mali_physical_device.c:43` |
| DXVK required features still false | 4 of 47: `shaderCullDistance`, `robustBufferAccess`, full `descriptorIndexing`, `geometryShader` | `vk/check.sh` `DXVK_MISSING=4`, README |
| BC1-BC7 | offline done, hardware wiring pending | README |
| Geometry/tessellation, GPL, `logicOp` | not advertised | README |

## 2. Behind an env flag (off by default)

| flag | on main? | detail |
|---|---|---|
| `MALI_EXPERIMENTAL_MSAA=1` | yes | 4x draws + sample counts; hardware evidence is PR #215 (unmerged); README: 4x `a2r10g10b10` still flakes |
| `MALI_OPT_GCM` | yes | compute-only GCM; host only |
| `MALI_ADVERTISE_GS/CULL/DESCIDX` | **no** (PRs #230, #164, #166) | documented in `tools/megabonk` but inert on main |
| `MALI_AFBC`, `MALI_CULLDIST`, `MALI_DESCIDX_FLAGS` | no (PRs #169.., #203, #176) | off, unused or host-only |

Full list with defaults: `docs/SWITCHES.md`.

## 3. Verified on hardware

| claim | arch | source |
|---|---|---|
| draw/compute/copy/query/dEQP daily sample (1381 cases) | G615 v11 (CMF), G925 v13 (Poco X8) | `docs/progress.md`; `vk/deqp/ours-daily-sample-{cmf,poco}.tsv` |
| cube-720p bench | CMF **62.4 fps** (46631edd8, 2026-11-19), X8 **492.8 fps** (453cd8d0c, 2026-11-18) | README bench table |
| Steam launcher shell under our ICD | X8 v13, **77.5 fps** (19130→21534 over 31.0 s, 120 Hz panel) | steam-resume.md |
| ICD loads inside the DroidDeck Steam guest | CMF v11 (`Mali-G615 MC2 (mali-own)`), X4 v10 loads; X8 v13 `Mali-G925 MC11` | guest-driver.md, steam-resume.md |
| Steam UI on X4 v10 | **blocked** (regression vs the app's bridge path) | guest-driver.md |

## 4. Host-only (no phone run)

- **MRT chain** #75/#83/#87/#91 → #229 → #236 → #237 → #239: tests `mrt2/3/4_mixed`, `mrt_rt1_load`, `mrt_blend_*`, `mrt_msaa_gate`, `t_cmd test_fbd_rt_store`, each mutation-checked (mrt.md). "No phone run has been made for the MRT chain." Matrix: fixed blend, CLEAR/LOAD/STORE per RT up to 4 RT; dual-source/advanced blend and MSAA with MRT refused at pipeline creation (#237, #239); resolve on RT>0 refused.
- **DXVK 3.1.1 device creation**: modelled by `t_dxvk_caps --expect-first` (PR #238) on the fake backend. On default build first refusal is `geometryShader` (`dxvk_device_info.cpp:855`); with GS and cull shims it is `descriptorIndexing` (`:899`); then 128 < 256 push constants (`:790`). apiVersion 1.3 **passes** (1.4 is not required, `dxvk_instance.h:12`).
- **GS shim** (#230) and the compile-side GS work (#241): `t_gs`; no GS draw path exists (gs-plan.md PRs 2-4 not started; est. 10-15 days).
- **Push 256 B** (#242): `t_compile test_push256`, `t_vk_feat push256`; v13 hardware leg passed, v11 pending (merge-train-status.md).
- Per-RT FBD words and v13 per-RT blend FAU slots: unverified on G615/G925 (mrt.md).

## 5. Known broken / blocked

| item | evidence |
|---|---|
| `vkCreateDevice` for DXVK 3.1.1 fails on today's driver (and on every unmerged shim combination) | #238 |
| Steam's ANGLE under Wayland-ozone: `eglInitialize ... Internal Vulkan error (-7): A requested extension is not supported`; session stays STARTING_STEAM, no menu fps | steam-resume.md; cause (which extension) unidentified; angle-requirements.md points to `VK_EXT_headless_surface` (PR #250) as first candidate, unverified |
| Steam client first-run download is wiped by the guest launcher (not our driver): `rm -rf` of partial parts at start counter >= 2; 243/361 MB lost | steam-resume.md; upstream fix not started |
| Steam UI on X4 v10 | guest-driver.md |
| Merge train 2026-11-19: **nothing merged**; every candidate had an unresolved review thread or was stacked on an unmerged base | merge-train-status.md |
| Notes `memory.md` and `zink.md` were not present; claims about memory types/heaps (PR #240) and zink are `unverified` here | — |

## 6. Compile-version numbering (`MALI_COMPILE_VERSION`)

Reality: main = **22** (`vk/src/mali_compile.c:35`, `vk/tests/compile-guard.txt`: `version 22`; last bump #151 `nir_lower_wpos_center`).
The plan in `/tmp/oc/vers.md` numbers 23-37 in merge order (#26=23, #44=24, #52=25, #71=26, #72=27, #75=28, #76=29, #84=30, #148=31, #89=32, #114=33, #91=34, #159=35, #161=36, #164=37; stacked #78/#83/#87/#131/#146 none).
`compile-guard.sh` forces a bump whenever `mali_compile.c`, `mali_descriptor_lower.{c,h}` or `mali_pipeline_layout_flat.h` change (it hashes the files, not shader output), so the list below is "PRs that change guarded sources or Mesa patches", which includes changes that may not alter shader output. Generated by diffing each PR head against its own base. **Nobody's PR code was renumbered; these are conflicts to report.**

| PR | base | version (base→head) | changes | verdict vs plan |
| #26 | main | 21→22 | guarded compile sources | CONFLICT: claims 22, plan 23 |
| #44 | main | 22→24 | guarded compile sources | OK |
| #52 | main | 21→22 | guarded compile sources | CONFLICT: claims 22, plan 25 |
| #71 | main | 21→22 | guarded compile sources | CONFLICT: claims 22, plan 26 |
| #72 | main | 21→22 | guarded compile sources | CONFLICT: claims 22, plan 27 |
| #75 | main | 21→22 | guarded compile sources | CONFLICT: claims 22, plan 28 |
| #76 | main | 21→22 | guarded compile sources | CONFLICT: claims 22, plan 29 |
| #84 | main | 21→22 | Mesa patches | CONFLICT: claims 22, plan 30 |
| #86 | main | 22→22 | guarded compile sources | not in plan; no bump although it changes guarded sources |
| #89 | pipeline-cache-uuid | 24→24 | guarded compile sources | CONFLICT: claims 24, plan 32 |
| #91 | lane/multi-rt-load | 20→21 | guarded compile sources | CONFLICT: claims 21, plan 34 |
| #114 | lane/robust-size-lowering | 21→22 | guarded compile sources | CONFLICT: claims 22, plan 33 |
| #131 | lane/pcache-disk-pr | 24→24 | guarded compile sources | plan: no bump (stacked); OK |
| #136 | main | 22→22 | guarded compile sources | not in plan; no bump although it changes guarded sources |
| #146 | lane/b100-41 | 22→22 | guarded compile sources | plan: no bump (stacked); OK |
| #148 | lane/mesa-pinbump | 22→23 | guarded compile sources, Mesa patches | CONFLICT: claims 23, plan 31 |
| #152 | lane/robust2-probe | 22→22 | guarded compile sources | not in plan; no bump although it changes guarded sources |
| #159 | main | 22→35 | guarded compile sources | OK |
| #161 | lane/gcm-post | 35→36 | guarded compile sources | OK |
| #164 | main | 22→37 | guarded compile sources | OK |
| #172 | main | 22→22 | guarded compile sources | not in plan; no bump although it changes guarded sources |
| #178 | lane/mesa-rollin-prep | 23→23 | Mesa patches | not in plan; claims 23, unplanned |
| #181 | main | 22→22 | guarded compile sources | not in plan; no bump although it changes guarded sources |
| #185 | lane/compilecost-loopblocks | 23→23 | Mesa patches | not in plan; claims 23, unplanned |
| #188 | lane/descidx3 | 22→22 | guarded compile sources | not in plan; no bump although it changes guarded sources |
| #189 | main | 22→39 | guarded compile sources | not in plan; claims 39, unplanned |
| #197 | main | 22→38 | guarded compile sources | not in plan; claims 38, unplanned |
| #202 | lane/wsi3 | 23→23 | Mesa patches | not in plan; claims 23, unplanned |
| #203 | main | 22→22 | guarded compile sources | not in plan; no bump although it changes guarded sources |
| #210 | main | 22→22 | guarded compile sources | not in plan; no bump although it changes guarded sources |
| #213 | compilecost-ra | 23→23 | Mesa patches | not in plan; claims 23, unplanned |
| #219 | main | 22→22 | guarded compile sources | not in plan; no bump although it changes guarded sources |
| #221 | main | 22→22 | guarded compile sources | not in plan; no bump although it changes guarded sources |
| #224 | main | 22→22 | guarded compile sources | not in plan; no bump although it changes guarded sources |
| #229 | main | 22→22 | guarded compile sources | not in plan; no bump although it changes guarded sources |
| #231 | feat/gs-shim-doc | 22→22 | guarded compile sources | not in plan; no bump although it changes guarded sources |
| #233 | feat/robust2 | 22→23 | guarded compile sources | not in plan; claims 23, unplanned |
| #242 | main | 22→23 | guarded compile sources | not in plan; claims 23, unplanned |
| #244 | main | 22→23 | guarded compile sources | not in plan; claims 23, unplanned |

Conflicts in plain terms:
- Eight main-based PRs (#26 #52 #71 #72 #75 #76 #84 and the stale #114) claim **22, which main already has**. Whichever merges first breaks the rest: they need 23-33 per the plan.
- #44 is the only plan-conformant early PR (24) but **#89 and #131's base also claim 24** (#89 plan 32).
- **Two PRs claim 23 for different work chains**: the Mesa-pinbump stack (#148/#178/#185/#199/#202/#213) and #233/#234/#238/#242/#244 (push256, descidx-subset, cull). Plan has #148=31.
- #189 (39) and #197 (38) claim numbers above the plan with no gap-filling order; #159/#161/#164 (35/36/37) are plan-correct.
- ~14 PRs change guarded sources and carry no bump (#86 #136 #152 #172 #181 #188 #203 #210 #219 #221 #224 #229 #231); the guard will fail them at merge. They need numbers after 39 (unassigned here).
- Docs: `CHANGELOG.md:55` says "MALI_COMPILE_VERSION 21" for PR #50 (true when written; main is now 22). `vk/DXVK-GAP.md:13` claimed DXVK 3.x needs Vulkan 1.4 (wrong, fixed in this PR).
