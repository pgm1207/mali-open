# bench2: triangle + cube, ours vs the Mali bridge, inside the DroidDeck guest

Only two benchmarks, both run INSIDE the guest (Linux arm64 / glibc a64 build under proot, started like a game via
`reaper SteamLaunch AppId=769`, Wayland WSI through gamescope). Never native Android binaries.

    tools/bench2 <alias|class> [--driver ours|bridge|both] [--bench triangle|cube|all] [--sweep] [--reps 5] [--secs 8] [--wait N]
    tools/bench2 --report        # regenerate RESULTS.md from results.jsonl

Flow per phone: `--sweep` (one short run per settings combination, writes `best.json` + `sweeps.jsonl`), then the final run (default,
5 interleaved reps of the stored best presented + offscreen config per driver) which appends rows to `results.jsonl` and rewrites
`RESULTS.md`. Both steps are resumable: calls stop after `--budget` s (default 230, rule: chunks of at most 4 min) with
`CHUNK DONE`; run the same command again (finished runs are cached in `raw/`, untracked, keyed by phone + commit + .so sha).
Phone access: qsafe lock (`/tmp/qsafe-<lock>.lock`, busy = pending, nothing is retried in a loop; `--wait N` = one blocking flock like
qsafe), owner disable switch, battery 20/50 % and 43/38 C gate (`tools/farm/gate.py`), focus rule (launcher/settings only), farm adb routing.
poco-x4 has no guest route (shell route only): it stays pending until it has one. Nothing is installed on a phone.
A run where a foreground dialog (e.g. a permission prompt) covers the app prints `UI-BLOCKED`: bench2 dismisses permission dialogs, the run is INVALID (no rows) and the farm requeues it once.

## Workloads (`mdbench/`, one binary, a vendored copy of the mission repo's mdbench + `--draw-idx`)
* **triangle** (CPU-heavy): `--scene drawcalls --n 3000 --draw-idx 3`: 3000 draws per frame, one triangle each, a dynamic-UBO descriptor
  offset per draw. Headline: CPU ms/frame (record + submit, mean), also fps and GPU ms.
* **cube** (GPU-heavy): `--scene cube --layers 8 --iters 24`: a rotating textured cube plus 8 full-screen overdraw layers plus a
  24-iteration ALU fragment shader, depth on. `cube-720p` = fixed 1280x720 window, `cube-native` = compositor size. Headline: GPU ms/frame
  (timestamps), fps, 1 % low, frames over 50 ms.
* Each runs presented (wayland, 120 Hz cap) and offscreen (headless 1280x720, no cap). Compare drivers on CPU ms and GPU ms, not on capped fps.

## Settings swept (rows record the winner)
Present mode mailbox/fifo/immediate x swapchain images 2/3/4, frames in flight 1/2/3. Ours additionally env sets (`MALI_RING`+`MALI_GEOM_SIZE`,
`MALI_HF=0 MALI_NOTF=1` (unvalidated), `MALI_NO_MERGE=1`). The bridge exposes no knobs in the guest beyond the above.
Ranking: triangle presented = fps then CPU ms, offscreen = CPU ms; cube presented = 1 % low then fps, offscreen = GPU ms.

## Known limits (open driver items; the harness records them, it does not hide them)
* Frames over ~3000 draws used to fail with `QueueSubmit = VK_ERROR_DEVICE_LOST`: the 32 MiB descriptor arena (~4.6 KB/draw) ran out. Fixed on `perf/draw-path` by retiring in-flight frames and spilling into overflow BOs (test: `t_vk_draw --only many_draws`, 3k-50k draws).
* Host (fake v11, 3000 dyn-UBO draws, median of 7): 1.43 us/draw before, 0.72 after descriptor sharing between consecutive draws.
  `tools/bench2 <phone> --variant triangle-20k` runs the large one once the driver can (5k/10k/20k run on CMF now; no poco-x8 rows yet).
* **Fixed:** `VK_ERROR_DEVICE_LOST` above ~3000 draws/frame (descriptor arena exhaustion); frames that do not fit spill into pooled overflow BOs. Scaling below.

## Draw-count scaling (triangle, one frame of N draws, offscreen, CMF v11, 2026-11-18)

`tools/bench2 cmf --bench triangle --tri-n N`, best env `MALI_RING=67108864 MALI_GEOM_SIZE=67108864`, one rep per cell (the CMF is bimodal: the 3k row is the best of 5). CPU = record + submit per frame. main = 74a582f (before the draw-path stack), draw-path = `perf/draw-path` with dirty mask + small draw records (5c3daca). The bridge column is the MediaDeck bridge on the same phone.

| draws | main CPU ms | draw-path CPU ms | draw-path fps | draw-path GPU ms | bridge CPU ms | bridge fps |
|---|---|---|---|---|---|---|
| 3000 | 4.78 | 0.79 | 893 | 0.73 | 0.80 | 235 |
| 5000 | QueueSubmit -4 | 1.44 | 456 | 1.40 | 1.34 | 168 |
| 10000 | QueueSubmit -4 | 5.14 | 141 | 4.20 | 3.27 | 88 |
| 20000 | QueueSubmit -4 | 10.66 | 76 | 6.46 | 5.61 | 48 |

main's `QueueSubmit` -4 (VK_ERROR_DEVICE_LOST, 32 MiB descriptor arena exhausted at ~7k draws) is fixed by c02338c (a frame that does not fit spills into overflow BOs) and 44fd974 (overflow BOs pooled), merged in 068a170. Above 3k the draw-path rows are slower than the bridge on CPU (10k/20k) because the per-draw cost stops being the limit and CS/heap growth dominates; not yet profiled.

* CMF timings are bimodal: medians of interleaved runs only. The bridge's presented fps is display-paced (about 120): compare CPU/GPU ms.
* poco-x8 runs MaliDeck p19 (sessions sometimes die early; p21 fixes it but is not installed). A run that fails is retried only via the cache/rerun.

## poco-x4 (v10, MediaDeck p22 guest) caveat, 2026-11-17
The `ours` cube rows on poco-x4 (GPU 0.09-0.17 ms, 650-3800 fps vs bridge ~7.3 ms) are NOT valid speed: the sweep winner env `MALI_RING=67108864+MALI_GEOM_SIZE=67108864` makes the cube draw (almost) nothing on v10. The default env of the same sweep gives cube-720p ~104 fps / 7.9 ms GPU (bench/sweeps.jsonl, poco-x4 ours cube-720p), on par with the bridge. Triangle-3k rows are plausible (ours cpu ~4.6 ms vs bridge 0.46 ms). The sweep should reject winners whose GPU ms is far below the default's.

## Megabonk frame-time bench (`tools/frametime`, `mdbench --scene megabonk`)
Not part of bench2 / RESULTS.md: a diagnostic for the 120 fps goal (8.33 ms budget). Per frame: a 2048² depth-only **shadow pass** (all cubes), an offscreen
**scene pass** (1500 small cube draws + 8192 blended instanced particles from the compute shader), and a **post pass** (5-tap blur + vignette + tonemap
sampling the scene image, into the swapchain). `mdbench` reports per-pass GPU time (`gpu_pass_ms`, timestamps), `frame_time_ms` p50/p99 and
`over_budget` (frames above `1000/--target-hz` ms). Knobs: `--mega-skip shadow,particles,post`, `--mega-scissor-px N` (scene pass scissored to NxN px: keeps the draws,
drops fragment work), `--shadow-size`, `--target-hz`, `--n` (cubes), `--groups` (particles/64). CSV has `gpu_shadow_ms,gpu_scene_ms,gpu_post_ms` columns.

    farm hold poco-x8 --max 25 -- tools/frametime poco-x8 --so <libvulkan_mali.so> --env "MALI_AFBC=1" --label afbc
    tools/frametime --summarize /tmp/oc/frametime-*.json

`tools/frametime` runs full / mailbox / offscreen / noshadow / noparticles / nopost / scissor64 and derives buckets (fragment = scene minus scissor64-scene;
tiler+CPU = scissor64-scene; present/acquire = `cpu_ms.present_acquire`). Timestamps are bottom-of-pipe, and on Mali the tiler job of one pass overlaps the
fragment job of the previous one, so the per-pass numbers are attribution rather than a strict partition; `gpu_ms` (whole frame) is exact.
Test: `python3 -I tools/test_frametime.py`.
