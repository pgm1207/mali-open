# shaderCullDistance plan

Status: `maxCullDistances` is 0 and `shaderCullDistance` is false (#54). DXVK 2.x lists the feature as required (/tmp/oc notes: megapath, geom), and shim PR #164 (`MALI_ADVERTISE_CULL`) advertises it while ignoring the values. This file records what the hardware gives us and the staged plan. Only stage 1 is implemented.

## What the hardware and compiler give us

- No native clip or cull distance on Valhall v10/v11/v13. `panfrost/genxml/v10.xml` has only depth-range culling (`Low/High Depth Cull`) and facing culling (`Cull Front/Back Face`). PanVK reports `shaderClipDistance`/`shaderCullDistance` false and all three limits 0.
- ClipDistance today (`mali_compile.c`): the VS writes `VARYING_SLOT_CLIP_DIST0/1` as ordinary varyings, and the FS kills fragments where the interpolated value is < 0 (`nir_lower_clip_fs`, count passed as `vs_clip_distances` -> `mali_compile_opts.clip_distances`). This is exact for clipping.
- CullDistance on main: nothing special. The gl_PerVertex block is split by member only when a ClipDistance member exists; cull outputs travel as extra, unused varyings and are never tested. Shaders using them compile but the cull has no effect.

## Semantics that are hard

Vulkan culls a primitive when, for any index i, the distance is negative at ALL its vertices. Per-fragment interpolation cannot recover "all vertices", so exact culling needs per-primitive information.

## Options

| Stage | Approach | Exactness | Cost |
|---|---|---|---|
| 1 (done, opt-in) | Cull distances lowered as clip distances: FS kills where the interpolated value < 0 | Exact if all vertices of a primitive are negative (whole primitive killed) or none is. A primitive straddling 0 is clipped to its positive part instead of kept whole. | One extra varying vec4 slot; FS discard. Nothing advertised. |
| 2 | Compute pre-pass: run the cull part of the VS per primitive into a visibility buffer, then draw via compacted index buffer / indirect draw | Exact | New pass, indexed-draw rewrite, extra latency; 3-5 days |
| 3 | Geometry-stage-in-compute (GS plan) | Exact | Reuses the GS work (see geom notes); do GS first |

Why clip semantics rather than ignoring the values (#164): the error of clipping is confined to primitives straddling the plane, where it looks like a plane clip. Ignoring leaves every primitive that should vanish visible.

## Stage 1 (`MALI_CULLDIST=1`, default off)

- `mali_compile.c`: with the env set, `CULL_DIST0` outputs are split out of gl_PerVertex like ClipDistance, relocated to `CLIP_DIST0` when there is no ClipDistance array, merged after the clip array otherwise (`nir_merge_clip_cull_distance_vars`), and counted in `vs_clip_distances` (clip + cull). More than 8 combined fails the compile with a clear error.
- Shader cache: the key knows only `MALI_COMPILE_VERSION`, so the cache is bypassed when the env is set (same as `MALI_OPT_GCM`). Default output is unchanged, so no version bump.
- Nothing advertised: limits, features and `DXVK_MISSING` stay as they are. Enabling the advertisement is a separate decision after stage 2, or an explicit compatibility shim.
- Tests: `t_compile` (`test_cull_distance`: clip counts with/without the env, 4+8 > 8 refused, plain VS binary identical); `t_vk_draw --only cull_dist` under `MALI_CULLDIST=1` on v11/v13 via `check.sh`.

## Caveats

- Not conformant: mixed-sign primitives are clipped, not kept (stage 1).
- Pixel behaviour is unverified on hardware; the fake backend does not execute GPU work.
- An FS compiled with a clip count kills below 0 for every counted distance, so cull distances count against the 8 clip slots.

## Stage 2 evaluation: no cheap conformant approach (decision: stop at stage 1)

Goal: exact culling (primitive dropped iff some distance is negative at ALL its vertices) for the common DXVK/Unity uses, host-testable, behind `MALI_CULLDIST=1`. Every candidate was checked against what a VS or FS can see.

1. **FS test on an interpolated per-vertex flag** (`flag_i = d_i >= 0`, discard when the interpolated flag is 0). Idea: the interpolated flag is 0 exactly when every vertex is negative. Not provably right:
   - The flag is interpolated at the pixel centre. Where a pixel is only partly covered (every primitive edge, MSAA covered samples, centroid-less), the centre lies outside the primitive and the barycentric weights are extrapolated. A primitive with flags (1,0,0) then yields a value <= 0 at covered edge pixels and loses them: visible erosion of every straddling primitive and of edges generally.
   - A pixel centre exactly on an edge has weight 0 for the opposite vertex, so a lone non-negative vertex is ignored there: holes along axis-aligned edges, which is what UI quads produce.
   - Helper invocations get the same extrapolation and corrupt derivatives.
2. **FS reading the other vertices' values**: Valhall exposes only the interpolated value and the provoking vertex (flat). Nothing gives an FS the other two vertices.
3. **VS re-evaluating the siblings and writing a NaN/off-screen position when all three are negative**: needs the other vertices' inputs. Attributes are preloaded by the hardware per invocation, so this needs vertex pulling (explicit attribute loads for `3*(vid/3)+k`), and it only works for non-indexed triangle lists without instancing tricks (an indexed draw has no primitive id in the VS; DXVK and Unity draw mostly indexed). Large, fragile, subset too small.
4. **Draw-uniform distances** (every vertex gets the same value, provable with `nir_divergence_analysis`): exact and cheap (push the position off-screen when negative), but nobody writes cull distances that way; it is not the common case, so it is not implemented.
5. **Compute pre-pass** (run the position and distance part per primitive into a visibility buffer, then draw from a compacted index buffer or indirect draw): the only general exact route (stage 2 in the table above). It restructures draw submission (indexed and non-indexed, instancing, primitive restart, strip and fan topologies, indirect draws) and costs a pass per draw; 3-5 days, same machinery as the GS-in-compute plan, so do the two together.

**Conclusion:** exact culling cannot be had from a vertex or fragment shader transform; a conformant implementation is the compute pre-pass (or GS-in-compute) and is not stage-sized. Stage 1 (cull as clip) stays the opt-in approximation, nothing is advertised, and `shaderCullDistance` stays false until the pre-pass exists. #164 remains a compatibility shim, not conformance.
