# steam-fps-host

Host-only Steam menu frame-rate check from unprivileged `adb shell dumpsys SurfaceFlinger`.
Use it when the DroidDeck agent provider is missing ("Could not find provider") and the guest
`/run/droiddeck-hud/frames` is unreadable from adb shell.

**Not `tools/steam-present/` (PR #227).** That tool drives the phone: it starts the menu, sweeps
the D-pad and samples 30 s. This one touches nothing; it only reads SurfaceFlinger while the menu is
already up. Run the other tool when you want a session driven for you.

Run once the menu is up:

    python3 tools/steam-fps-host/steam_fps_host.py 10 -s <serial>

Prints JSON: `frames`, `meanMs`/`p50Ms`/`p95Ms`/`p99Ms`, `fps` (from mean interval), `fpsWall`
(frames / window), `histogram` (6.7 / 8.33 / 11.1 / 16.7 / 33 ms; 33 includes slower), `source`,
`idleGaps`, `layers`, and `verdict`: `120-paced`, `90-capped`, `60-capped`, `dropped-frames`,
`no-frames` (exit 1). `--layer REGEX` overrides the default `SessionActivity` match.

How: each poll re-runs `--list` (layer `#id` changes on session restart), prefers the SurfaceView
(BLAST) layer and polls `--latency <exact layer>`, de-duplicating actual-present timestamps. In
parallel `--timestats` is enabled and dumped at the end. If `--latency` gives under 10 frames the
per-layer present2present histogram is used (1 ms bins, bucketed by bin midpoint). Gaps over
100 ms count as `idleGaps`, not drops (BLAST presents on change, so a static menu shows few frames).

Real-capture note: on the CMF/X8 captures under `/tmp/oc` the BLAST layer's `--latency` returned
only the refresh-period line, so expect `source: timestats` for that surface.

Tests (synthetic cadences, empty buffers, rename, fallback): `python3 tools/steam-fps-host/test_steam_fps_host.py`
