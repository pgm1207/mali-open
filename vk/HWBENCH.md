# hwbench: which resource limits this frame?

Three microbenchmarks that answer "memory bandwidth, shader cores, or tiler?" with a slope instead of a guess. They run
through our own Vulkan API (no loader: `--direct` like `t_vk_draw`), so the same binary is a plumbing test on the fake
backend and a measurement on a phone.

```
tools/hwbench --phone cmf          # or --fake v13 on the host (plumbing only)
```

The table is one row per configuration:

```
mode case   target     texture    vertices   frames wall ms/f   cpu ms gpu ms/f  ns/unit texture MB
bw   tex    512x512    128x128    -               6     0.004    0.003     0.002     0.02    0.06
```

* `wall ms/f` — submit-to-fence time for one frame, with the fixed cost of a submission (kick, fence, timestamp pair)
  removed by measuring the same workload at `f` and `2f` frames and taking the slope. This is what the app would see
  with a GPU that never has to wait for the CPU.
* `cpu ms` — the same slope for `vkQueueSubmit` alone: the CPU cost of building and kicking the frame.
* `gpu ms/f` — the slope of `VK_QUERY_TYPE_TIMESTAMP` values written by the GPU around the frame loop. This separates
  "the CPU is slow at submitting" from "the GPU is slow at executing". `n/a` means the backend has no GPU timestamp
  (the fake backend), not that the GPU took zero time.
* `ns/unit` — `ns` per pixel (`bw`, `alu`) or per vertex (`geom`).
* `texture MB` — the bytes the fragment shader has to read if every fetch misses (the working-set footprint).

## (a) `bw` — bandwidth

Full-screen fragment shader, one `NEAREST` fetch per pixel, `vec4(texel.rgb, 1.0)` out: minimal ALU, 4 bytes written
per pixel.

Two sweeps:

1. **`tex`**: target fixed at 512x512, texture 128² → 4096² (64 KB → 64 MB).
   * Flat `ns/unit` while the texture is small: every fetch hits L2, the cost is the fetch itself.
   * The knee is the cache size. Above it the slope is the DRAM bandwidth the GPU actually sustains for this access
     pattern (`read_gbps` in `--json` assumes every fetch misses, so it is an *upper* bound when some still hit).
   * A flat curve all the way to 64 MB means the driver/GPU is not bandwidth-limited at this access pattern; a steep
     rise says it is.
2. **`target`**: texture fixed at 2048² (4 MB, past most Mali L2s), target 256² → 1080p.
   * Constant `ns/unit` = the cost is per pixel (fragment rate) and the frame scales with resolution.
   * `ms/f` growing super-linearly with pixels points at the write/tile path rather than at the reads.

## (b) `alu` — shader cores

64 *dependent* FMAs per pixel plus one fetch from a 4x4 texture (always in cache), swept over the target size
256² → 1080p.

* Constant `ns/unit` across resolutions: every pixel costs the same, so the shader cores are the limit and the frame
  time is `pixels x ns/pixel`. That is what an ALU-bound frame looks like.
* `ns/unit` falling as the resolution grows means fixed per-frame overhead (submission, tiler setup) still dominates
  the small sizes, so read the large sizes only.
* Compare its `ns/unit` with `bw`'s at the same resolution: the larger one names the resource that binds.

## (c) `geom` — vertex/tiler path

Very high vertex counts (1023 → 1M, one 2-pixel triangle per three vertices) with a constant-colour fragment shader:
no texture, no shading work, almost no rasterised pixels, so what is left is vertex processing plus the tiler's
per-primitive work. Vertex counts are swept at a fixed 512x512 target.

* Constant `ns/vertex` with `ms/f` growing linearly: the vertex pipe is the limit (per-vertex ALU or vertex-cache
  bandwidth), and "reduce vertices / move work to the fragment stage" is the fix.
* `ns/vertex` falling and `ms/f` flattening as the count grows: vertices per second has hit a ceiling that does not
  depend on the count — the tiler/primitives-per-second limit. At that point more geometry is free only until the
  next bin overflows; the frame is tiler-bound, not vertex-bound.
* `gpu ms/f` much larger than `cpu ms` throughout: the geometry path is on the GPU; if instead `cpu ms` tracks
  `ms/f`, the draw submission itself is the cost (that is what `t_vk_draw --only many_draws` measures).

## Reading it against a real frame

Take the real frame's resolution and vertex count, find the closest rows in the three sweeps, and compare:

| real frame | closest hwbench row | if that row is the largest `ns/unit` |
|---|---|---|
| 1080p, few vertices, heavy shader | `alu` 1920x1080 vs `bw` target 1920x1080 | shader cores |
| 1080p, many textures | `bw` tex sweep at the footprint that matches | bandwidth |
| 100k+ vertices, cheap pixels | `geom` at that vertex count | vertex pipe or tiler |

A real frame is usually a mix; the point of the sweeps is that the *shape* of the curve identifies which resource
saturates first when the resolution or the vertex count is increased.

## GPU time vs wall time

`gpu ms/f` comes from `VK_QUERY_TYPE_TIMESTAMP` queries that the driver implements as a GPU-side timestamp store
(`MALI_OP_TIMESTAMP` in `mali_event.c`), so it is the GPU's own clock around the submitted frame loop, not a CPU
sample. It is still an elapsed-time span, not a busy counter: the GPU timestamp counter keeps running while the GPU
is idle, and all frames of a measurement are queued back-to-back, so the span is GPU busy time plus small gaps.

For per-frame CPU-submit, GPU-estimate and present-interval numbers of a *real* workload, `MALI_FRAME_PROF` (PR #249,
`vk/FRAME-PROF.md`) and `tools/frameprof` remain the tool; hwbench's `gpu ms/f` is independent of it and works with
`MALI_FRAME_PROF` unset.

## Caveats

* The fake backend (`MALI_BACKEND=fake`) executes nothing: rows are plumbing only, `gpu ms/f` is `n/a`, and the
  timings say nothing about hardware. That mode is for `check.sh`, not for a report.
* Textures are `VK_IMAGE_TILING_LINEAR` and uploaded through the mapped linear layout (the same path as `t_vk_draw`'s
  texture case) so the working-set sweep is exact; a tiled or AFBC-compressed texture can have a different knee, and
  compressed content would move it further.
* The fragment shader writes a 4-byte RGBA8 target with no blending: write bandwidth is one full target per frame,
  which is the smallest a full-screen pass can do.
* One submission per measurement, one target, one pipeline. Draw-call cost is measured by `t_vk_draw`, not here.
