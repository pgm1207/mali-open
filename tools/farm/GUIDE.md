# Farm user guide and troubleshooting

Task-oriented companion to `README.md` (which is the reference). Everything here is
derived from that README and `farm <cmd> --help`; if they disagree, the README and the
code win. Examples use `<device>`, `<job>`, `<ref>` placeholders.

Rule zero: never poll or loop against a phone (`while ...; sleep`, `flock -w`, raw
`adb` sessions on a phone the farm may own). Use `farm submit`, `farm wait`, `farm hold`.
Never run `adb kill-server`.

## 1. Submit a job

```
git fetch origin                      # submit does NOT fetch
farm submit <ref> --suite smoke --need v13
farm submit <ref> --suite daily --need v11,v13
```

| Suite | Use | Default prio |
|---|---|---|
| `smoke` | merge gate, `vk/tests/valrun.sh` | 0 |
| `daily` | dEQP daily sample vs the baseline, per case | 0 |
| `cases` | named dEQP cases, `--case N [--case M] --repeat R` | 20 |
| `bench` | triangle/cube ours vs bridge | 10 |
| `menu` | Steam menu benchmark (MediaDeck) | 10 |

Useful flags: `--need <classes>` (comma list; default is every class), `--env "K=V ..."`,
`--only` (smoke test subset), `--prio N`, `--merge-with-main`, `--allow-stale`, `--rerun`,
`--keep-old`.

### Stack gate (one run for a PR stack)

`farm submit --stack PR1,PR2,PR3 --suite smoke --need v13` merges the heads in the order
given (base PR first) into a throwaway commit and gates it as ONE job: a 4-PR stack costs one
X8 run, not four. Refs are branches/shas (`origin/lane-x`). Listing a ref after one that contains it is
rejected (wrong order); a merge conflict is reported at submit. Same heads = same merge commit,
so an already-passed stack is reused (`--rerun` forces). The ref shows as `stack:a+b+c`.
If it fails, farmd gates each prefix (PR1, PR1+PR2, ...; the full stack is the failed job) and
`farm batch JOB` prints `first failing prefix: <ref>`, i.e. the PR that introduced the break.
Unlike `--batch` (independent branches, each re-run alone), a stack never runs a dependent PR
without its base. Gate the stack on the top PR's real head only after each PR is rebased; the
throwaway merge is not what lands on main.

Things that surprise people:
- Only **committed** state is tested; the run JSON records `dirty`.
- A smoke that already passed for the same commit/inputs/classes is **not requeued**: the
  existing job is printed. `--rerun` forces. Daily, bench and menu are never reused.
- A ref more than 5 commits behind local `origin/main` is rejected: rebase, use
  `--merge-with-main`, or `--allow-stale`.
- A new smoke/daily cancels the *pending* parts of older gate jobs of the same suite,
  repo, env and `--only` on the same branch line. Running/finished parts are untouched.
  `--keep-old` opts out.
- Errors are not retried automatically; resubmit by hand. `invalid` runs (sha mismatch,
  `UI-BLOCKED`, restart-killed) are requeued by the farm (max 2 attempts).
- Tools-only PRs: v13 smoke is enough (see the merge gate in the mali-open skill).

## 2. Wait for the verdict

```
farm wait <job> --timeout 1800        # one job: prints bare verdict
farm wait <jobA> <jobB> --any         # first verdict; --all (default) waits for every job
```

Exit codes for one job: 0 pass, 1 fail, 2 error/invalid, 3 timeout. Several jobs print one
line each: `<job> <suite> <commit> <verdict> v11:pass v13:fail(2 new) v10:pending`.
Timeout 3 means "still running", not failure: wait again.

Then read the result:
- `farm results <job>` per-case counts (`--json` for the run JSON)
- `farm log <job>` kept test output of a smoke part (last 200 lines per test)
- `farm diff <jobA> <jobB> [--class v11]` case-level status changes
- `farm cancel <job>... [--running]` pending parts; `--running` also stops a running part cleanly

## 3. Hold a phone for ad-hoc work

```
farm hold <device> --max 20 -- <command...>     # blocks until granted, runs, releases
farm hold <device> --max 20 --lease             # returns JSON; release with --release <id>
farm hold --status <id>
```

- Queued fairly with jobs by `--prio` (default 0 = like gates), then submission order. Use
  `--wait-max <min>` to give up if it never starts.
- Max 20 min per hold, and hold only while the phone is actually running something.
- The command runs with `QSAFE_HELD`, `ANDROID_SERIAL`, `ADB_SERIAL`, `ADB_SERIAL_<LOCK>`,
  `FARM_HOLD`, `FARM_HOLD_DEVICE` set, so qsafe/menubench/qrestore do not lock again.
- Exit code is the command's; **124** = the hold ended while it ran (`--max` ran out, the
  owner disabled the phone, or farmd restarted). The command was killed.
- With `--lease`, use `--watch-pid <pid>` so it releases if your process dies. A caller with
  no heartbeat for 45 s is released automatically.
- On release farmd kills leftover `steam`, `steamwebhelper`, `gamescope-wl` and runs
  `qrestore`. stderr says `restored clean` or `RESTORE PROBLEM`. After a hold the phone
  must have none of those processes and `vkBridge=True` with an empty env; check if you
  saw `RESTORE PROBLEM`.
- Never touch a session you did not start: a farm job may own it. Exit 73 / `PHONE BUSY`
  means you do not hold the lock; do not use `QSAFE_LOCK_OVERRIDE=1` outside emergencies.

## 4. Priorities and queue

Lower number runs first, FIFO inside a level, explicit before auto at equal level.
Gates 0, bench/menu 10, `cases` 20; auto bench/menu sit below
explicit jobs; the idle sweep only runs when nothing is pending.

```
farm prio <job> <n>          # change later
farm submit ... --prio 0     # at submit
```

Use prio sparingly: do not give exploratory jobs gate priority.

## 5. Digest, status and finding things

- `farm digest`: one screen. Devices, queue by class with positions (`v11#3`), latest
  smoke/daily per ref/class with the difference vs main's latest daily, newest bench row
  per phone, flaky counts. Start here.
- `farm status`: devices (`up`/`down`, battery %, temperature, thermal, transport; also `disabled by owner`, `on battery:
  paused`) and, under a down phone, the recovery steps.
- `farm case <substring>`: status of matching cases in every stored daily, newest last.
  Use it to tell "new regression" from "was always failing".
- `farm flaky [--class v11] [--seed]`: cases that fail but pass a rerun. A daily
  automatically reruns main-passing Fails up to 3 times; a pass makes the case flaky (not
  a new failure). `known-flaky.txt` cases are ignored by the verdict but reported.
- `farm util [--since today|6h|2d]`: where phone time went (busy, sweep, gated, queue gap).
- `farm sweep status|failures|on|off`: idle full-list sweep; it yields to any queued job.

Reading a daily verdict: `fail +N new` means N cases Pass in main's baseline (or the
branch's own) and now Fail, after flaky reruns. Known acceptable: the
`a2r10g10b10 sample_count_4` fail. Anything else new must be explained or fixed. A job
killed by a farmd restart can show a false fail; check whether the run is `invalid`/was
running at a restart time (`<state>/logs/farm-queue.log`, `alerts.log`) and resubmit.

## 6. Thermal and power rules

Defaults (gate, before each job, between chunks, every ~10 s in a run; per-device overrides
`batt_stop batt_resume temp_stop temp_resume thermal_stop thermal_resume` in devices.toml):

| Condition | Gate state | Resumes |
|---|---|---|
| battery < 20 % | `charging` | >= 50 % |
| battery temp >= 43 C or thermal status >= SEVERE | `cooling` | <= 38 C and status <= LIGHT |
| `require_charging` phone on battery | paused | when a charger is attached |

A gated phone keeps its jobs pending; the job stops cleanly (rc 75 for dEQP, resumes from
its TSV), attempts are not counted. Per phone:
- **X8:** keep it on AC with passive cooling; stop at 43 C battery temperature.
- **X4:** skin sensor. Do not start above 40 C (`temp_start`), resume per `temp_resume`,
  stop mid-job at 50 C (`temp_stop`; the job returns to pending, device cooling). Hold
  commands you run yourself are not auto-stopped: watch it, and end the hold at 50 C.
- Idle sweep is stricter: charger only, battery temp <= 36 C (`sweep_temp_max`), >= 80 %
  if on battery is allowed.
- Do not "fix" a `cooling` gate by overriding it. Wait (`farm wait`), or cool the phone.

## 7. Down, absent or stuck phones

Diagnosis order (read-only first):

1. `farm status` and `<state>/ALERTS`, `<state>/alerts.log`. A phone with waiting work that
   is missing gets `adb reconnect` / `reconnect offline` / `connect <wireless>` by farmd
   itself; after 10 min it is marked **down** and the recovery steps are written.
2. Is the host on the same Wi-Fi network as the phones (the farm owner knows which)? Wireless adb only works there.
3. `adb devices` (plain, never `kill-server`). `offline`/missing: `adb connect <ip>:5555`.
4. `adb tcpip 5555` is lost on reboot until USB returns: plug in USB, run
   `adb tcpip 5555`, then `adb connect <ip>:5555`. farmd re-stores the route
   (`wireless` list in devices.toml) once the phone is idle on USB.
5. Pairing Wireless Debugging itself is the owner's job; the farm never toggles
   `adb_wifi_enabled`.
6. Phone alive but gated: `farm status` shows `charging`, `cooling` or `on battery`.
   Fix the cause (charger, cooling); nothing to restart.
7. `disabled by owner`: someone ran `farm disable` (or touched
   `<state>/disabled/<alias>`). `farm enable <device>` only if you know it is yours to
   enable. A running job was stopped when it was disabled.
8. Phone busy with a user app: shell-route phones pause above load 8; guest-route phones
   wait while the owner is in another app or the screen is locked. dEQP daily has no
   locked-phone fallback (it ends invalid/pending); smoke falls back to the shell route on
   `smoke_fallback = "shell"` phones.
9. Stale lock / `PHONE BUSY`: find the holder first: `ls <state>/holds` (files are
   `<id>.json`), then `farm hold --status <id>`. A hold from a dead caller frees itself in 45 s.
10. `RUNAWAY-APP` in alerts.log: farmd force-stopped a foreground app above 90 % CPU for
    15 min on an idle `foreground_launch` phone.

After the phone returns, pending parts start by themselves. Resubmit only errored jobs.

### Restarting farmd (safely)

A restart kills the running part and any hold. Only when nothing runs:
1. Note which phones are enabled (`farm status` marks owner-disabled ones `disabled by owner`), then
   `farm disable <dev>` for those only; leave already-disabled phones alone.
2. Wait until `farm status` shows no `:running`.
3. `systemctl --user restart mali-farm.service`.
4. `farm enable <dev>` only for the phones you disabled in step 1; a phone the owner
   had disabled stays disabled.

Jobs killed by a restart are INVALID and requeued; holds are released.

## 8. Common failure messages

| You see | Meaning | Do |
|---|---|---|
| submit rejects "behind origin/main" | stale base | rebase, or `--merge-with-main` |
| submit prints an old job id | idempotent smoke reuse | `--rerun` if you need a fresh run |
| `farm wait` exit 3 | timeout, job still going | wait again |
| `farm wait` exit 2 | error or invalid | `farm log`/`results`, alerts.log, resubmit |
| exit 73 `PHONE BUSY` | you do not hold the lock | use `farm hold <device> --max <min> -- <command...>` |
| hold exit 124 | hold expired / disabled / farmd restarted | re-request a hold |
| run `invalid`, sha mismatch | driver .so on phone differs | farmd re-pushes once and requeues; resubmit if it repeats |
| gate `cooling` / `charging` | thermal or battery gate | wait or fix the cause |
| `UI-BLOCKED` (bench) | foreground dialog on the phone | dismiss it as the owner; job requeues once |
| `RESTORE PROBLEM` after hold | leftovers after release | check for steam/gamescope-wl, vkBridge state, tell the owner |

## 9. Do not

- Edit state files under `~/.local/state/mali-farm/` by hand, except the documented flags
  (`disabled/`, `bench/off`, `bench/menu-off`, `baseline/off`).
- Write secrets (sudo password) anywhere, or change package ID/signing key.
- Put real IPs in docs: use `<ip:port>`; run `tools/private-lint.sh` before pushing.
