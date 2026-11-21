# steam-menu: one-command Steam menu iteration

`tools/steam-menu/cycle.sh` runs the whole "change a web-helper flag, see if the menu renders and
how fast" loop on a phone and prints one JSON line.

**It touches a phone** (stops and starts the DroidDeck session, edits one guest file, taps the
screen in the fallback path). **Run it only when the owner's phone is free**: it takes no farm lock,
so coordinate first. It never installs, roots, or kills adb.

    tools/steam-menu/cycle.sh <serial> [--args "BL_WEBHELPER_ARGS value"] [--env K=V [K=V ...]] \
        [--wait 120] [--seconds 30] [--runs 2] [--dry-run]
    # e.g.
    tools/steam-menu/cycle.sh FABY... --args "--ozone-platform=wayland --use-gl=angle --use-angle=vulkan" --env FOO=1

Needs `droiddeckctl` (`$QUICK_ROOT/tools/droiddeckctl`, default `~/Projects/mali-gaming`) and
`tools/steam-fps-host/steam_fps_host.py` (PR #254; the script fails with verdict `no-tool` if absent).

## What it does

1. Stops a running session.
2. Applies `--args` / `--env` through the guest hook `/usr/local/lib/mali-open/wayland.env`
   (`--hook-path` to change), which `droiddeck-session` sources after its own exports. The block is
   appended as `export K='v'` lines, so it wins. The guest filesystem is only writable from a live
   session, so a bootstrap session is started, the file written (original read first, copy kept in
   `/tmp/cycle-<serial>-wayland.env.orig`), then the session is stopped.
3. Starts Steam with `droiddeckctl start steam --wait`. If the provider is unavailable
   ("Could not find provider") it launches the app, screenshots (`/tmp/cycle-<serial>-launch.png`),
   finds the Play button in `uiautomator dump` and taps it (`--play-xy X,Y` if the node has no label).
4. Waits up to `--wait` s for `firstFrame` (provider) or a `SessionActivity` SurfaceFlinger layer (no
   provider). While waiting it checks `/proc` in the guest: every `--args` token must be in a
   `steamwebhelper` cmdline and every `--env K=V` in the environ of `steam`/`steamwebhelper`.
   Otherwise the verdict is `env-not-applied` (loud failure, exit 1).
5. If it renders: `steam_fps_host.py <seconds> -s <serial>` `--runs` times, combined (mean of fps and
   p50, max of p99, worst verdict). If not: the GPU-process error lines are taken verbatim from the
   latest session log (`droiddeckctl logs latest`) into `error`.
6. Always (`finally`, also on SIGINT/SIGTERM): restores the hook file (deleting it if it did not
   exist) and stops the session. A failed restore is reported in `error` and on stderr.
7. Prints `{"session_reached","first_frame","fps_mean","p50_ms","p99_ms","verdict","error"}` as the
   only stdout line (progress and commands go to stderr). Exit 0 if frames were measured, else 1.

Verdicts: `120-paced`, `90-capped`, `60-capped`, `dropped-frames` (from the fps tool), `no-frames`,
`no-first-frame`, `no-session`, `no-provider`, `hook-write-failed`, `env-not-applied`, `no-tool`.

`--dry-run` prints the exact commands (`+ ...` on stderr) and touches nothing.

## Tests

    python3 -I tools/steam-menu/test_cycle.py

Phone-free: argument parsing, hook rendering (including a real `sh` round trip of the quoting),
`/proc` verification, GPU-error extraction, Play-button lookup, JSON assembly, `--dry-run`.
