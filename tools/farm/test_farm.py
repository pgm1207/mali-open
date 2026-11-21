#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Dry-run tests of queue, pending/resume, comparison and JSON logic (no phone, no build). Run: python3 tools/farm/test_farm.py"""
import hashlib, json, os, shutil, subprocess, sys, tempfile, threading, time, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import farmlib as L
import sweeplib as S  # noqa: E402
import gate  # noqa: E402
import benchlib as BL  # noqa: E402
import menulib as ML  # noqa: E402
import holdlib as H  # noqa: E402

TOML = """quick_root = "/nonexistent"
[[device]]
alias = "dev-a"
class = "v11"
serial = "SERIAL-A"
lock = "cmf"
baseline = "cmf"
[[device]]
alias = "dev-b"
class = "v13"
serial = "SERIAL-B"
lock = "poco"
baseline = "poco"
require_charging = true
"""


FAKE_MENUBENCH = r'''#!/usr/bin/env python3
# fake MediaDeck scripts/quick/menubench: same layout (results/menubench.jsonl two levels above scripts/), same exit codes
import json, os, sys, time
root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.realpath(__file__))))
mode = open(os.path.join(root, "mode")).read().strip() if os.path.exists(os.path.join(root, "mode")) else "ok"
vc = int(open(os.path.join(root, "vc")).read()) if os.path.exists(os.path.join(root, "vc")) else 22
open(os.path.join(root, "calls"), "a").write(" ".join(sys.argv[1:]) + " held=" + os.environ.get("QSAFE_HELD", "-") + "\n")
alias = sys.argv[1]
if mode == "busy":
    print("PHONE BUSY (qsafe lock held): not retrying in a loop; run again later")
    sys.exit(125)
profs = ["default", "uncapped"]
state = os.path.join(root, "state")
done = int(open(state).read()) if os.path.exists(state) else 0
todo = [(r, p) for r in (1, 2, 3) for p in profs][done:]
if mode == "chunks":
    todo = todo[:3]                      # one chunk = 3 reps, then exit 75 like the real budget stop
os.makedirs(os.path.join(root, "results"), exist_ok=True)
for rep, p in todo:
    bad = mode == "selfcheck" and p == "uncapped"
    fps = 118.0 + rep if p == "default" else 180.0 + rep
    rec = {"ts": time.strftime("%Y-%m-%dT%H:%M:%S+00:00", time.gmtime()), "alias": alias, "class": "v11", "profile": p, "rep": rep, "version_code": vc,
           "app_version": "1.0.0-p%d" % vc, "bridge_version": "0.5.5.4", "panel_hz": 120 if p == "uncapped" else 60,
           "render": {"avg_fps": fps, "median_fps": None if p == "default" else fps - 1, "low1_fps": fps / 2, "p99_ms": 20.0 + rep},
           "ok": not bad, "selfcheck": ["no gpu-process found in the guest"] if bad else [], "status": "FAILED after retries" if bad else "ok"}
    open(os.path.join(root, "results", "menubench.jsonl"), "a").write(json.dumps(rec) + "\n")
    done += 1
open(state, "w").write(str(done))
if done < 6:
    sys.exit(75)
os.unlink(state)
'''


FAKE_ADB = r'''#!/usr/bin/env python3
# fake adb for the shell route: the "phone" is a directory ($FAKE_ADB_ROOT); deqp-vk / t_vk_* are simulated
import hashlib, os, re, shutil, sys
R = os.environ["FAKE_ADB_ROOT"]
a = sys.argv[1:]
if a[:1] == ["-s"]:
    a = a[2:]
P = lambda p: R + p
op = a[0]
if op == "push":
    os.makedirs(os.path.dirname(P(a[2])), exist_ok=True)
    if os.environ.get("FAKE_ADB_CORRUPT") == "1" and a[2].endswith(".so"):
        open(P(a[2]), "w").write("tampered")
    else:
        shutil.copy(a[1], P(a[2]))
elif op == "pull":
    if os.path.exists(P(a[1])):
        shutil.copy(P(a[1]), a[2])
    else:
        print("error: remote object does not exist"); sys.exit(1)
elif op == "shell":
    c = a[1]
    open(R + "/adb-shell.log", "a").write(c + "\n")
    m = re.match(r"sha256sum (\S+)", c)
    if m:
        if os.path.exists(P(m.group(1))):
            print(hashlib.sha256(open(P(m.group(1)), "rb").read()).hexdigest(), m.group(1))
    elif c.startswith("ls "):
        print("\n".join(os.listdir(P(c[3:].strip()))))
    elif c == "echo alive":
        print("alive")
    elif "/proc/sys/kernel/random/boot_id" in c:                    # boot identity: FAKE_ADB_REBOOT_ON_READ=N -> the Nth read sees a new boot
        n = int(open(R + "/bootreads").read()) if os.path.exists(R + "/bootreads") else 0
        n += 1
        open(R + "/bootreads", "w").write(str(n))
        base = int(open(R + "/uptime").read()) if os.path.exists(R + "/uptime") else 5000
        if os.environ.get("FAKE_ADB_REBOOT_ON_READ") == str(n):
            print("2b2b2b2b-0000-4000-8000-000000000002"); print("7.00 3.00")
        else:
            print("1a1a1a1a-0000-4000-8000-000000000001"); print("%d.00 1.00" % (base + n))
    elif "tail -c" in c:
        m = re.search(r"tail -c \d+ (\S+)", c)
        if m and os.path.exists(P(m.group(1))):
            print(open(P(m.group(1))).read())
    elif "dumpsys battery" in c:
        print("  AC powered: true\n  level: 80\n  temperature: 300\nThermal Status: 0")
    elif "deqp-vk" in c:
        cf = re.search(r"--deqp-caselist-file=(\S+)", c).group(1)
        qf = re.search(r"--deqp-log-filename=(\S+)", c).group(1)
        open(R + "/deqp-runs.log", "a").write(str(len(open(P(cf)).read().split())) + "\n")     # one line per launch: the case count of that shard
        out = re.search(r">(\S+\.out)", c)
        if os.environ.get("FAKE_ADB_OOM") == "1":                    # the memory bound killed the run before it wrote a result
            if out:                                                  # the command cd's into /data/local/tmp/g first: the .out lives there
                open(P("/data/local/tmp/g/" + out.group(1)), "w").write("terminate called after throwing an instance of 'std::bad_alloc'\n  what():  std::bad_alloc\n")
            print("rc=134")
            raise SystemExit(0)
        crash = [x for x in os.environ.get("FAKE_ADB_CRASH", "").split(",") if x]       # these cases die mid-case (no result), FAKE_ADB_CRASH_TIMES times each
        times, rc = int(os.environ.get("FAKE_ADB_CRASH_TIMES", "1")), 0
        with open(P(qf), "w") as f:
            for case in open(P(cf)).read().split():
                cn = R + "/crashcount-" + case
                n = int(open(cn).read()) if os.path.exists(cn) else 0
                if case in crash and n < times:
                    open(cn, "w").write(str(n + 1))
                    f.write("#beginTestCaseResult %s\n" % case)
                    rc = 139
                    break
                f.write("#beginTestCaseResult %s\n<Result StatusCode=\"Pass\"/>\n#endTestCaseResult\n" % case)
        print("rc=%d" % rc)
    elif "./t_vk_" in c:
        d = re.split(r";|&&", c[len("cd "):])[0].strip()
        n = re.search(r"\./(t_vk_\w+)", c).group(1)
        open(P(d) + "/" + n + ".log", "w").write('{"pass":true}\n{"pass":true}\n')
        print("rc=0")
    else:
        for m in re.finditer(r"rm -(?:rf|f) ([^;&]+)", c):
            for p in m.group(1).split():
                shutil.rmtree(P(p), ignore_errors=True) if os.path.isdir(P(p)) else (os.path.exists(P(p)) and os.unlink(P(p)))
        for m in re.finditer(r"mkdir -p (\S+)", c):
            os.makedirs(P(m.group(1)), exist_ok=True)
'''


class ShellRoute(unittest.TestCase):
    """shellroute.py against a fake adb (the phone is a temp dir): per-run .so path, sha256 on the phone, lock guard, resume"""
    def setUp(self):
        self.d = tempfile.mkdtemp()
        os.makedirs(self.d + "/bin")
        open(self.d + "/bin/adb", "w").write(FAKE_ADB)
        os.chmod(self.d + "/bin/adb", 0o755)
        self.root = self.d + "/phone"
        for p in ("/data/local/tmp/g/lib/ld-linux-aarch64.so.1", "/data/local/tmp/g/deqp-vk"):
            os.makedirs(os.path.dirname(self.root + p), exist_ok=True)
            open(self.root + p, "w").write("x")
        self.tree = self.d + "/tree"
        for sub in ("a64-glibc", "a64-bionic"):
            os.makedirs(f"{self.tree}/vk/out/{sub}")
            open(f"{self.tree}/vk/out/{sub}/libvulkan_mali.so", "w").write("driver " + sub)
        os.makedirs(self.tree + "/vk/out/a64-bionic", exist_ok=True)
        for t in ("t_vk_draw", "t_vk_compute", "t_vk_meta", "t_vk_feat"):
            open(f"{self.tree}/vk/out/a64-bionic/{t}", "w").write("x")
        os.makedirs(self.tree + "/vk/shader")
        open(self.tree + "/vk/shader/a.spv", "w").write("x")
        open(self.d + "/cases.txt", "w").write("\n".join(f"dEQP-VK.t.c{i}" for i in range(5)) + "\n")
        self.env = dict(os.environ, PATH=self.d + "/bin:" + os.environ["PATH"], FAKE_ADB_ROOT=self.root, ANDROID_SERIAL="X", QSAFE_HELD="shelltest",
                        MALI_FARM_STATE=self.d + "/state", MALI_FARM_DEVICES=self.d + "/none.toml")

    def run_sr(self, mode, sha_of, extra=(), env=None):
        sha = hashlib.sha256(open(f"{self.tree}/vk/out/{sha_of}/libvulkan_mali.so", "rb").read()).hexdigest()
        r = subprocess.run([sys.executable, "-I", HERE + "/shellroute.py", mode, "--lock", "shelltest", "--tree", self.tree, "--sha", sha, "--run", "r1", *extra],
                           capture_output=True, text=True, env=dict(self.env, **(env or {})), timeout=60)
        return r.returncode, r.stdout

    def test_smoke_nodim_never_touches_screen_settings(self):
        """smoke fallback on a locked phone (SHELLROUTE_NODIM): no brightness/screen commands at all; the normal route still dims"""
        os.makedirs(self.root, exist_ok=True)
        rc, o = self.run_sr("smoke", "a64-bionic", env={"SHELLROUTE_NODIM": "1"})
        self.assertEqual(rc, 0, o)
        log = open(self.root + "/adb-shell.log").read()
        self.assertNotIn("settings ", log)
        self.assertNotIn("keyevent", log)
        self.assertNotIn("dim.state", log)
        os.unlink(self.root + "/adb-shell.log")
        self.assertEqual(self.run_sr("smoke", "a64-bionic")[0], 0)           # control: the normal route does read/set the screen state
        self.assertIn("settings ", open(self.root + "/adb-shell.log").read())

    def test_daily_per_run_so_and_resume(self):
        out = self.d + "/o.tsv"
        rc, o = self.run_sr("daily", "a64-glibc", ["--list", self.d + "/cases.txt", "--out", out, "--chunk", "2"])
        self.assertEqual(rc, 0, o)
        self.assertEqual(open(out).read().count("\tPass\t\n"), 5)
        self.assertTrue(os.path.exists(self.root + "/data/local/tmp/g/icd.r1.so"))      # per-run ICD path, left for farmd's final check
        self.assertIn("SO_ON_PHONE /data/local/tmp/g/icd.r1.so", o)
        rc, o = self.run_sr("daily", "a64-glibc", ["--list", self.d + "/cases.txt", "--out", out])        # resume: nothing left to run
        self.assertEqual(rc, 0)
        self.assertEqual(len(open(out).read().splitlines()), 5)

    def crash_run(self, crash, times, n=12, extra_env=None):
        open(self.d + "/many.txt", "w").write("\n".join(f"dEQP-VK.t.c{i}" for i in range(n)) + "\n")
        out = self.d + "/m.tsv"
        os.path.exists(out) and os.unlink(out)
        for x in os.listdir(self.root) if os.path.isdir(self.root) else []:
            x.startswith("crashcount-") and os.unlink(self.root + "/" + x)
        env = {"FAKE_ADB_CRASH": ",".join(f"dEQP-VK.t.c{i}" for i in crash), "FAKE_ADB_CRASH_TIMES": str(times), "SHELLROUTE_BURST_PAUSE": "0"}
        env.update(extra_env or {})
        rc, o = self.run_sr("daily", "a64-glibc", ["--list", self.d + "/many.txt", "--out", out, "--chunk", "3"], env)
        rows = dict(l.split("\t")[:2] for l in open(out).read().splitlines()) if os.path.exists(out) else {}
        return rc, o, rows

    def test_daily_isolated_crash_gets_a_second_run_and_stays_crash_if_real(self):
        rc, o, rows = self.crash_run([3], 99)                              # always dies: a real crash
        self.assertEqual(rc, 0, o)
        self.assertEqual(rows["dEQP-VK.t.c3"], "Crash")
        self.assertEqual(sum(1 for v in rows.values() if v == "Pass"), 11)
        self.assertNotIn("BURST", o)
        rc, o, rows = self.crash_run([3], 1)                               # died once (transient): the second run passes
        self.assertEqual(rows["dEQP-VK.t.c3"], "Pass", o)
        self.assertNotIn("Crash", rows.values())

    def test_daily_burst_pauses_rechecks_and_reruns_instead_of_recording_crashes(self):
        rc, o, rows = self.crash_run(range(0, 5), 1)                       # 5 chunks in a row die once each: a burst
        self.assertEqual(rc, 0, o)
        self.assertIn("BURST: 5 chunks in a row ended in a crash", o)
        self.assertEqual(set(rows.values()), {"Pass"}, rows)
        self.assertEqual(len(rows), 12)
        log = open(self.root + "/adb-shell.log").read()
        self.assertIn("echo alive", log)                                   # the device was re-checked before the rerun

    def test_daily_burst_that_keeps_coming_back_is_unstable_and_writes_nothing_for_it(self):
        rc, o, rows = self.crash_run(range(0, 12), 99, extra_env={"SHELLROUTE_BURST_MAX": "1"})
        self.assertEqual(rc, 81, o)
        self.assertIn("UNSTABLE", o)
        self.assertEqual(sorted(rows), [f"dEQP-VK.t.c{i}" for i in range(5)])         # only the second-run crashes of burst 1; burst 2's held cases never reach the TSV

    def test_daily_real_crash_cluster_after_a_pause_is_recorded_not_endless(self):
        rc, o, rows = self.crash_run(range(0, 5), 99, extra_env={"SHELLROUTE_BURST_MAX": "3"})
        self.assertEqual(rc, 0, o)                                         # same cases die again after the pause: real crashes
        self.assertEqual([rows[f"dEQP-VK.t.c{i}"] for i in range(5)], ["Crash"] * 5)
        self.assertEqual(rows["dEQP-VK.t.c11"], "Pass")

    def test_burst_guard_unit(self):
        sys.path.insert(0, HERE)
        import shellroute
        g = shellroute.BurstGuard(3, 2)
        for i in range(2):
            g.chunk_crashed([f"c{i}"])
        self.assertFalse(g.burst())
        self.assertEqual(g.release(), ["c0", "c1"])                       # clean chunk: second run, streak over
        self.assertFalse(g.holds("c0"))                                    # a second crash of c0 is final
        self.assertTrue(g.holds("c9"))
        g.chunk_crashed([])                                                # a chunk whose crashes were all second-run ones does not count
        self.assertEqual(g.streak, 0)
        for i in range(3):
            g.chunk_crashed([f"d{i}"])
        self.assertTrue(g.burst())
        self.assertEqual(g.start_pause(), ["d0", "d1", "d2"])
        g.end_pause()
        self.assertFalse(g.burst())
        self.assertEqual(g.held, {})

    def test_daily_foreign_so_is_refused(self):
        rc, o = self.run_sr("daily", "a64-glibc", ["--list", self.d + "/cases.txt", "--out", self.d + "/o2.tsv"], {"FAKE_ADB_CORRUPT": "1"})
        self.assertEqual(rc, 76)
        self.assertFalse(os.path.exists(self.d + "/o2.tsv") and open(self.d + "/o2.tsv").read())

    def test_crash_note_keeps_output_and_qpa_tails(self):
        sys.path.insert(0, HERE)
        import shellroute
        n = shellroute.crash_note("#beginTestCaseResult a.b.c\n<Text>x</Text>\n" * 40, "line1\nSIGSEGV in foo\n")
        self.assertTrue(n.startswith("CRASH out: line1 SIGSEGV in foo | qpa: "))
        self.assertNotIn("\n", n)
        self.assertLess(len(n), 800)

    def test_parse_notes_keeps_deqp_failure_messages(self):
        sys.path.insert(0, HERE)
        import shellroute
        qpa = ("#beginTestCaseResult a.b.ok\n<Result StatusCode=\"Pass\">Passed</Result>\n#endTestCaseResult\n"
               "#beginTestCaseResult a.b.bad\n<Result StatusCode=\"Fail\">Image comparison\n failed at (3,4)</Result>\n#endTestCaseResult\n"
               "#beginTestCaseResult a.b.ns\n<Result StatusCode=\"NotSupported\">not here</Result>\n#endTestCaseResult\n")
        self.assertEqual(shellroute.parse_notes(qpa), {"a.b.bad": "Image comparison failed at (3,4)", "a.b.ns": "not here"})

    def test_cpu_busy_pct_ignores_idle_phone_with_high_loadavg(self):
        sys.path.insert(0, HERE)
        import shellroute
        idle = "cpu  1000 0 500 8000 100 0 0 0\ncpu  1000 0 500 8800 100 0 0 0"      # 800 idle ticks of 800: 0 % busy (poco-x4 at load 12)
        busy = "cpu  1000 0 500 8000 100 0 0 0\ncpu  1700 0 600 8100 100 0 0 0"
        self.assertEqual(shellroute.cpu_busy_pct(idle), 0.0)
        self.assertAlmostEqual(shellroute.cpu_busy_pct(busy), 88.89, 1)
        self.assertIsNone(shellroute.cpu_busy_pct("garbage"))

    def test_refuses_without_the_phone_lock(self):
        import fcntl
        fd = open("/tmp/qsafe-shelltest.lock", "a")
        fcntl.flock(fd, fcntl.LOCK_EX)
        try:
            rc, _ = self.run_sr("daily", "a64-glibc", ["--list", self.d + "/cases.txt", "--out", self.d + "/o3.tsv"], {"QSAFE_HELD": ""})
            self.assertEqual(rc, 73)
        finally:
            fd.close()

    def test_smoke(self):
        rc, o = self.run_sr("smoke", "a64-bionic")
        self.assertEqual(rc, 0, o)
        self.assertIn('"test":"summary","ok":true', o)
        self.assertIn('"test":"t_vk_draw","rc":0,"fail_lines":0,"pass_lines":2', o)
        rc, o = self.run_sr("smoke", "a64-bionic", env={"FAKE_ADB_CORRUPT": "1"})
        self.assertEqual(rc, 77)


    # ---- resource guards: the 2026-11-19 kernel panic (oom_score_adj=-1000 inherited from adbd, unbounded deqp, blind retry after the reboot)
    def test_oom_protection_is_cleared_memory_capped_and_work_sharded(self):
        sys.path.insert(0, HERE)
        import shellroute
        pre = shellroute.limit_preamble("r1")
        self.assertIn("echo 0 > /proc/self/oom_score_adj", pre)
        self.assertIn("ulimit -v 3145728", pre)                       # 3 GiB default cap
        self.assertNotIn("-1000", pre)
        self.assertNotIn("'", pre)                                    # embedded in the device command as-is
        open(self.d + "/many.txt", "w").write("\n".join(f"dEQP-VK.t.c{i}" for i in range(90)) + "\n")
        rc, o = self.run_sr("daily", "a64-glibc", ["--list", self.d + "/many.txt", "--out", self.d + "/s.tsv", "--chunk", "100"],
                            {"SHELLROUTE_CHUNK_MAX": "40"})
        self.assertEqual(rc, 0, o)
        cmds = open(self.root + "/adb-shell.log").read()
        self.assertIn("echo 0 > /proc/self/oom_score_adj", cmds)      # every launch, not just the first
        self.assertIn("ulimit -v 3145728", cmds)
        self.assertNotIn("oom_score_adj=-1000", cmds)
        self.assertIn("CHUNK 100 capped to 40", o)
        self.assertEqual([int(x) for x in open(self.root + "/deqp-runs.log").read().split()], [40, 40, 10])

    def test_device_reboot_mid_run_gives_verdict_device_reboot_and_no_retry(self):
        open(self.d + "/many.txt", "w").write("\n".join(f"dEQP-VK.t.c{i}" for i in range(12)) + "\n")
        rc, o = self.run_sr("daily", "a64-glibc", ["--list", self.d + "/many.txt", "--out", self.d + "/rb.tsv", "--chunk", "3"],
                            {"FAKE_ADB_REBOOT_ON_READ": "3"})
        self.assertEqual(rc, 82, o)
        self.assertIn("verdict: device_reboot", o)
        self.assertIn("boot id changed", o)
        self.assertEqual(len(open(self.root + "/deqp-runs.log").read().split()), 1)     # chunk 1 ran; the run is not restarted
        self.assertEqual(len(open(self.d + "/rb.tsv").read().splitlines()), 3)          # what passed before the reboot is kept

    def test_memory_cap_breach_aborts_cleanly_with_no_retry(self):
        open(self.d + "/many.txt", "w").write("\n".join(f"dEQP-VK.t.c{i}" for i in range(6)) + "\n")
        rc, o = self.run_sr("daily", "a64-glibc", ["--list", self.d + "/many.txt", "--out", self.d + "/oom.tsv", "--chunk", "3"],
                            {"FAKE_ADB_OOM": "1"})
        self.assertEqual(rc, 83, o)
        self.assertIn("verdict: memory_cap", o)
        self.assertEqual(len(open(self.root + "/deqp-runs.log").read().split()), 1)     # one aborted launch, no blind retry
        self.assertFalse(os.path.exists(self.d + "/oom.tsv") and "Pass" in open(self.d + "/oom.tsv").read())


class DeviceStopGuard(unittest.TestCase):
    """a reboot mid-run or the memory bound ends the job: verdict device_reboot|memory_cap, other parts cancelled, no retry.
    2026-11-19: the farm resubmitted a sweep after every reboot of a phone whose kernel had panicked, the phone looped for hours."""
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.old = os.environ.get("MALI_FARM_STATE")
        os.environ["MALI_FARM_STATE"] = self.d
        import importlib.machinery, importlib.util
        ld = importlib.machinery.SourceFileLoader("farmd_guard", HERE + "/farmd")
        spec = importlib.util.spec_from_loader("farmd_guard", ld)
        self.fd = importlib.util.module_from_spec(spec)
        ld.exec_module(self.fd)
        self.f = object.__new__(self.fd.Farm)
        self.f.jl = threading.Lock()          # __init__ is bypassed; abort_job() takes the job-file lock

    def tearDown(self):
        if self.old is None:
            os.environ.pop("MALI_FARM_STATE", None)
        else:
            os.environ["MALI_FARM_STATE"] = self.old

    def job(self):
        L.new_job("HEAD", "0" * 40, False, "daily", ["v11", "v13"], "", "mali-open")
        return L.list_jobs()[0]["id"]

    def alerts(self):
        return open(os.path.join(L.state_dir(), "ALERTS")).read()

    def test_shell_rc_maps_reboot_and_memory_cap(self):
        self.assertEqual(self.f.shell_rc(82, "verdict: device_reboot uptime went backwards", "shellroute")["verdict"], "device_reboot")
        self.assertEqual(self.f.shell_rc(83, "verdict: memory_cap bad_alloc", "shellroute")["verdict"], "memory_cap")
        self.assertIsNone(self.f.shell_rc(0, "rc=0", "shellroute"))

    def test_reboot_aborts_the_whole_job_and_is_never_retried(self):
        jid = self.job()
        self.f.abort_job(jid, "v11", "dev-a", {"verdict": "device_reboot", "notes": "boot id changed"})
        j = L.read_json(L.job_path(jid))
        self.assertEqual((j["parts"]["v11"]["state"], j["parts"]["v11"]["verdict"]), ("done", "device_reboot"))
        self.assertEqual(j["parts"]["v13"]["state"], "cancelled")            # the other phone is not fed the same job
        self.assertEqual(j["parts"]["v11"]["attempts"], 0)                   # no retry is even counted
        self.assertEqual(L.job_verdict(j), "device_reboot")                  # never pending, never pass
        self.assertEqual([c for c, p in j["parts"].items() if p["state"] == "pending"], [])
        self.assertIn("DEVICE_REBOOT", self.alerts())
        self.assertTrue(os.path.exists(L.run_path(jid, "v11").replace(".json", ".terminal.json")))

    def test_two_reboots_in_30_min_are_a_loud_reboot_loop(self):
        jid = self.job()
        for _ in range(2):
            self.f.abort_job(jid, "v11", "dev-a", {"verdict": "device_reboot", "notes": "uptime went backwards"})
        self.assertIn("REBOOT-LOOP dev-a: 2 reboots in 30 min", self.alerts())

    def test_memory_cap_is_terminal_too(self):
        jid = self.job()
        self.f.abort_job(jid, "v11", "dev-a", {"verdict": "memory_cap", "notes": "std::bad_alloc"})
        self.assertEqual(L.job_verdict(L.read_json(L.job_path(jid))), "memory_cap")
        self.assertIn("MEMORY_CAP", self.alerts())


class T(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        os.environ.pop("MALI_FARM_SWEEP_LIST", None)                          # sweep_env() leaks it into os.environ: later tests would idle-sweep a stale list until farmd times out
        open(os.path.join(self.d, "devices.toml"), "w").write(TOML)
        self.env = dict(os.environ, MALI_FARM_STATE=self.d + "/state", MALI_FARM_DEVICES=self.d + "/devices.toml",
                        MALI_FARM_DOWN_AFTER="1")
        os.environ.update({k: self.env[k] for k in ("MALI_FARM_STATE", "MALI_FARM_DEVICES")})
        os.makedirs(self.d + "/state", exist_ok=True)

    def attach(self, *names):
        with open(self.d + "/state/fake-attached", "w") as f:
            f.write("\n".join(names))

    def farm(self, *a):
        r = subprocess.run([sys.executable, getattr(self, "root", HERE) + "/farm", *a], capture_output=True, text=True, env=self.env)
        return r.returncode, r.stdout.strip(), r.stderr

    def fixture_repo(self, n=1, origin_at=None):
        """a clean throwaway git checkout holding a copy of the farm tools, n commits deep; origin/main -> commit origin_at (default HEAD).
        self.root then points at it, so submit/farmd never depend on the developer's checkout, its refs or its tracked edits"""
        import shutil
        root = os.path.join(self.d, "fx")
        shutil.copytree(HERE, root, ignore=shutil.ignore_patterns("__pycache__", ".git"))
        g = lambda *c: subprocess.run(["git", "-C", root, "-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgSign=false", *c], capture_output=True, text=True, check=True).stdout.strip()
        g("init", "-q", "-b", "main")
        for dev in ("cmf", "x8", "poco"):                       # minimal passing daily baselines, so a daily job can reach `pass` in fake mode
            os.makedirs(os.path.join(root, "vk/deqp"), exist_ok=True)
            with open(os.path.join(root, "vk/deqp/ours-daily-sample-%s.tsv" % dev), "w") as f:
                f.write("".join("dEQP-VK.api.smoke.t%d\tPass\t\n" % i for i in range(20)))
        g("add", "-A")
        for i in range(n):
            g("commit", "-q", "--allow-empty", "-m", f"c{i}")
        g("update-ref", "refs/remotes/origin/main", origin_at or "HEAD")
        self.root = root
        return g

    def farmd(self):
        r = subprocess.run([sys.executable, getattr(self, "root", HERE) + "/farmd", "--fake", "--once"], capture_output=True, text=True, env=self.env, timeout=240)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout

    def runaway(self, pkg_cpu, secs="0", guard=True, age=0):
        if guard and "runaway_guard" not in open(self.d + "/devices.toml").read():
            t = open(self.d + "/devices.toml").read().replace('baseline = "cmf"', 'baseline = "cmf"\nrunaway_guard = true', 1)
            open(self.d + "/devices.toml", "w").write(t)
        self.env["MALI_FARM_RUNAWAY_SECS"] = secs
        self.attach("dev-a")
        os.makedirs(self.d + "/state/fake-runaway", exist_ok=True)
        if pkg_cpu:
            open(self.d + "/state/fake-runaway/dev-a", "w").write(pkg_cpu)

    def stopped(self):
        p = self.d + "/state/fake-runaway/dev-a.stopped"
        return open(p).read().split() if os.path.exists(p) else []

    def test_runaway_app_on_an_idle_phone_is_force_stopped_and_logged(self):
        self.runaway("com.example.MediaDeck 128")
        self.farmd()
        self.assertEqual(self.stopped(), ["com.example.MediaDeck"])
        self.assertFalse(os.path.exists(self.d + "/state/fake-runaway/dev-a"))
        line = open(self.d + "/state/alerts.log").read()
        self.assertIn("RUNAWAY-APP dev-a: com.example.MediaDeck at 128% CPU", line)
        self.assertIn("force-stopped", line)

    def test_runaway_guard_waits_for_the_time_threshold_and_ignores_cool_apps(self):
        self.runaway("com.example.MediaDeck 128", secs="3600")
        self.farmd()
        self.assertEqual(self.stopped(), [])                                   # above the CPU limit, but not for long enough yet
        self.runaway("com.example.MediaDeck 12")
        self.farmd()
        self.assertEqual(self.stopped(), [])                                   # a quiet app is left alone
        self.assertFalse(os.path.exists(self.d + "/state/alerts.log") and "RUNAWAY" in open(self.d + "/state/alerts.log").read())

    def test_runaway_guard_never_touches_a_phone_with_a_hold_or_a_job(self):
        self.runaway("com.example.MediaDeck 128")
        h = H.new_hold("dev-a", 60)
        self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")
        self.assertIsNotNone(self.hold_next_for_test("dev-a"))
        self.farmd()                                                           # the queued hold/job run on the phone: it is not idle
        self.assertEqual(self.stopped(), [])

    def git_rev(self, r):
        return subprocess.run(["git", "-C", HERE, "rev-parse", r], capture_output=True, text=True).stdout.strip()

    def test_submit_in_flight_is_idempotent_and_rerun_forces(self):
        rc, j1, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11")
        rc, j2, err = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11")
        self.assertEqual(j1, j2)
        self.assertIn("already pending", err)
        self.assertNotEqual(self.farm("submit", "HEAD", "--suite", "daily", "--need", "v13")[1], j1)        # other class: a new job
        self.assertNotEqual(self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11", "--rerun")[1], j1)

    def test_single_case_diagnostic_goes_before_full_cases(self):
        _, one, _ = self.farm("submit", "HEAD", "--suite", "cases", "--case", "dEQP-VK.a.b", "--need", "v11")
        _, many, _ = self.farm("submit", "HEAD", "--suite", "cases", "--case", "dEQP-VK.a.b", "--case", "dEQP-VK.a.c", "--need", "v11")
        _, forced, _ = self.farm("submit", "HEAD", "--suite", "cases", "--case", "dEQP-VK.a.z", "--need", "v11", "--prio", "30")
        self.assertEqual((L.job_prio(L.find_job(one)), L.job_prio(L.find_job(many)), L.job_prio(L.find_job(forced))), (L.SINGLE_CASE_PRIO, 20, 30))
        self.assertLess(L.SINGLE_CASE_PRIO, 10)                                  # also before bench/menu

    def test_batch_passes_as_one_job(self):
        rc, jid, err = self.farm("submit", "--batch", "HEAD,HEAD~1", "--need", "v11", "--allow-stale")
        self.assertEqual(rc, 0, err)
        j = L.find_job(jid)
        self.assertEqual(j["batch"]["refs"], ["HEAD", "HEAD~1"])
        self.assertTrue(j["ref"].startswith("batch:"))
        self.attach("dev-a")
        self.farmd()
        self.assertEqual(L.job_verdict(L.find_job(jid)), "pass")
        self.assertIn("all branches pass together", self.farm("batch", jid)[1])
        self.assertEqual(self.farm("submit", "--batch", "HEAD,HEAD~1", "--need", "v11", "--allow-stale")[1], jid)      # same batch: the same merge commit, reused

    def test_batch_failure_bisects_per_branch(self):
        culprit = self.git_rev("HEAD~1")[:12]
        rc, jid, err = self.farm("submit", "--batch", "HEAD,HEAD~1", "--need", "v11", "--allow-stale", "--env", "FAKE_FAIL_COMMIT=" + culprit)
        self.assertEqual(rc, 0, err)
        self.attach("dev-a")
        self.farmd()
        self.assertEqual(L.job_verdict(L.find_job(jid)), "fail")
        for _ in range(4):                                                       # bisect jobs are created, then run
            self.farmd()
        out = self.farm("batch", jid)[1]
        res = dict((l.split()[0], l.split()[-1]) for l in out.splitlines()[1:] if l.startswith("  H"))
        self.assertEqual(res, {"HEAD": "pass", "HEAD~1": "fail"}, out)

    def test_stack_gate_order_prefix_bisect(self):
        # HEAD~1 is the stack base, HEAD the PR on top: one job covers both; reversed order is rejected
        self.assertNotEqual(self.farm("submit", "--stack", "HEAD,HEAD~1", "--need", "v11", "--allow-stale")[0], 0)
        self.assertNotEqual(self.farm("submit", "--stack", "HEAD~1,HEAD", "--batch", "HEAD,HEAD~1")[0], 0)
        os.environ.pop("MALI_FARM_SWEEP_LIST", None)                          # an earlier sweep test leaves it behind: farmd would sweep forever
        self.env.pop("MALI_FARM_SWEEP_LIST", None)
        base = self.git_rev("HEAD~1")[:12]
        rc, jid, err = self.farm("submit", "--stack", "HEAD~1,HEAD", "--need", "v11", "--allow-stale", "--env", "FAKE_FAIL_COMMIT=" + base)
        self.assertEqual(rc, 0, err)
        j = L.find_job(jid)
        self.assertTrue(j["ref"].startswith("stack:"))
        self.assertTrue(j["batch"]["stack"])
        self.attach("dev-a")
        self.farmd()
        self.assertEqual(L.job_verdict(L.find_job(jid)), "fail")
        for _ in range(4):
            self.farmd()
        out = self.farm("batch", jid)[1]
        self.assertIn("first failing prefix: HEAD~1", out)                       # the base PR already breaks it; the top PR is not blamed alone
        self.assertEqual(L.find_job(jid)["bisect"]["HEAD"], jid)                 # the full stack is the failed job itself: not re-run

    def test_stack_first_failing_prefix_waits_for_earlier_prefixes(self):
        os.environ.pop("MALI_FARM_SWEEP_LIST", None)
        self.env.pop("MALI_FARM_SWEEP_LIST", None)
        base = self.git_rev("HEAD~1")[:12]
        rc, jid, err = self.farm("submit", "--stack", "HEAD~1,HEAD", "--need", "v11", "--allow-stale", "--env", "FAKE_FAIL_COMMIT=" + base)
        self.assertEqual(rc, 0, err)
        self.attach("dev-a")
        self.farmd()
        self.assertEqual(L.job_verdict(L.find_job(jid)), "fail")
        self.farmd()                                                            # next pass starts the bisect; the base prefix job is queued, not run
        j = L.find_job(jid)
        self.assertTrue(j.get("bisect"), "bisect must have started")
        out = self.farm("batch", jid)[1]
        self.assertNotIn("first failing prefix", out)                           # the base prefix has no result yet: the top PR must not be named
        self.assertIn("diagnosis incomplete", out)
        for _ in range(4):
            self.farmd()
        out = self.farm("batch", jid)[1]
        self.assertIn("first failing prefix: HEAD~1", out)                      # base result is in: definitive

    def test_stack_prefixes_are_merged_with_main(self):
        for ref in ("origin/main~1", "origin/main~2"):   # both prefixes must exist; with them present a failing submit is a real regression
            if subprocess.run(["git", "-C", HERE, "rev-parse", "--verify", "-q", ref], capture_output=True).returncode:
                self.skipTest("no %s in this checkout" % ref)
        rc, jid, err = self.farm("submit", "--stack", "origin/main~2,origin/main~1", "--need", "v11", "--merge-with-main", "--allow-stale")
        self.assertEqual(rc, 0, err)
        j = L.find_job(jid)
        for c in j["batch"]["commits"]:
            parents = subprocess.run(["git", "-C", HERE, "rev-list", "--parents", "-n1", c], capture_output=True, text=True).stdout.split()
            self.assertIn(self.git_rev("origin/main"), parents[1:], "every prefix must include origin/main")
        self.assertEqual(j["batch"]["commits"][-1], j["commit"])                # the last prefix is the job commit: auto_bisect does not re-run it

    def test_batch_needs_two_refs(self):
        self.assertNotEqual(self.farm("submit", "--batch", "HEAD")[0], 0)
        self.assertNotEqual(self.farm("submit")[0], 0)

    def test_guardlib_parsing(self):
        import guardlib as G
        ci = "  128% 4242/com.example.MediaDeck: 100% user + 28% kernel\n  3.1% 99/system_server: 2% user\n"
        self.assertEqual(G.cpu_of(ci, "com.example.MediaDeck"), 128.0)
        self.assertIsNone(G.cpu_of(ci, "com.other"))
        act = "topResumedActivity=ActivityRecord{1 u0 com.mi.android.globallauncher/.Home t1}"
        self.assertIsNone(G.candidate(act))                                  # the launcher is never stopped
        self.assertEqual(G.candidate(act.replace("com.mi.android.globallauncher", "com.x")), "com.x")
        hot = {}
        self.assertIsNone(G.verdict(hot, "p", 100, 0, 90, 60))
        self.assertEqual(G.verdict(hot, "p", 100, 61, 90, 60), 61)
        self.assertIsNone(G.verdict(hot, "p", 10, 70, 90, 60))              # a dip resets the clock
        self.assertEqual(hot, {})

    def test_runaway_guard_is_off_unless_the_phone_opts_in(self):
        self.runaway("com.example.MediaDeck 128", guard=False)
        self.farmd()
        self.assertEqual(self.stopped(), [])

    def hold_next_for_test(self, alias):
        return H.queued(H.list_holds(), alias) or None

    def test_pending_resume_and_json(self):
        rc, jid, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11,v13", "--env", "FAKE_REGRESS=2 FAKE_FIX=1")
        self.assertEqual(rc, 0)
        self.attach("dev-a")                                    # only v11 present
        self.farmd()
        self.assertEqual(self.farm("wait", jid, "--timeout", "1")[0], 3)    # v13 still pending -> timeout
        j = L.find_job(jid)
        self.assertEqual(j["parts"]["v11"]["state"], "done")
        self.assertEqual(j["parts"]["v13"]["state"], "pending")
        self.assertEqual(L.job_verdict(j), "pending")
        self.attach("dev-a", "dev-b")                           # phone reappears -> picked up
        self.farmd()
        rc, out, _ = self.farm("wait", jid, "--timeout", "5")
        self.assertEqual((rc, out), (1, "fail"))                # 2 new failures
        res = json.loads(self.farm("results", jid)[1])
        run = res["runs"]["v13"]
        for k in ("job", "commit", "dirty", "so_sha256", "env", "suite", "device", "gpu", "kbase", "battery_temp_c",
                  "counts", "new_failures", "newly_passing", "verdict", "wall_s"):
            self.assertIn(k, run)
        self.assertEqual(len(run["new_failures"]), 2)
        self.assertEqual(len(run["newly_passing"]), 1)
        self.assertEqual(run["device"], "dev-b")
        self.assertEqual(len(run["so_sha256"]), 64)

    def test_wait_several_jobs_any_all_compact(self):
        os.environ.pop("MALI_FARM_SWEEP_LIST", None)                          # an earlier sweep test leaves it behind: farmd would sweep forever
        self.env.pop("MALI_FARM_SWEEP_LIST", None)
        self.attach("dev-a")                                                   # only v11 present: the v13 part stays pending
        _, j1, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")
        _, j2, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v13", "--keep-old")
        self.farmd()
        rc, out, _ = self.farm("wait", j1, j2, "--any", "--timeout", "5")
        self.assertEqual(rc, 0)
        self.assertEqual(len(out.splitlines()), 1)                             # only the finished job
        self.assertIn(" pass v11:pass", out)
        rc, out, _ = self.farm("wait", j1, j2, "--all", "--timeout", "1")
        self.assertEqual(rc, 3)
        self.assertIn("v13:pending", out)
        self.attach("dev-a", "dev-b")
        self.farmd()
        rc, out, _ = self.farm("wait", j1, j2, "--timeout", "5")               # several jobs = --all
        self.assertEqual((rc, len(out.splitlines())), (0, 2))
        self.assertEqual(self.farm("wait", j1)[1], "pass")                     # a single job keeps the one-word output

    def test_clean_pass_and_smoke_fail(self):
        self.attach("dev-a", "dev-b")
        _, j1, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11")
        _, j2, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v13", "--env", "FAKE_FAIL=1")
        self.farmd()
        self.assertEqual(self.farm("wait", j1)[1], "pass")
        self.assertEqual(self.farm("wait", j2)[1], "fail")

    def test_lock_busy_keeps_pending(self):
        self.attach("dev-a")
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")
        _, devs = L.load_devices()
        lk = L.PhoneLock(devs["dev-a"], fake=True)
        self.assertTrue(lk.acquire())                           # an "agent" holds the phone
        self.farmd()
        self.assertEqual(L.find_job(jid)["parts"]["v11"]["state"], "pending")
        lk.release()
        self.farmd()
        self.assertEqual(L.find_job(jid)["parts"]["v11"]["state"], "done")

    def backdate(self, alias, secs):
        """age a device state by `secs` instead of sleeping (a sleep races the loaded CI/full-suite run)"""
        p = self.d + "/state/devices-state.json"
        st = json.load(open(p))
        st[alias]["since"] -= secs
        json.dump(st, open(p, "w"))

    def test_down_alert(self):
        self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v13")
        self.attach()
        self.farmd()                                            # becomes 'missing'
        self.backdate("dev-b", 5)
        self.farmd()                                            # > DOWN_AFTER -> down + one alert line
        self.farmd()
        lines = open(self.d + "/state/alerts.log").read().strip().splitlines()
        self.assertEqual(len(lines), 1)
        self.assertIn("dev-b", lines[0])
        self.assertNotIn("SERIAL", lines[0])
        al = open(self.d + "/state/ALERTS").read().strip().splitlines()          # the owner-facing file, one line with the recovery steps
        self.assertEqual(len(al), 1)
        self.assertIn("DEVICE-DOWN dev-b", al[0])
        self.assertIn("adb tcpip 5555", al[0])
        self.assertIn("adb connect", al[0])
        out = self.farm("status")[1]
        self.assertIn("dev-b is unreachable", out)
        self.assertIn("adb tcpip 5555", out)
        self.assertNotIn("dev-a is unreachable", out)

    def test_compare(self):
        c, nf, npass = L.compare({"a": "Pass", "b": "Pass", "c": "Fail"}, {"a": "Pass", "b": "Crash", "c": "Pass"})
        self.assertEqual((nf, npass), ([["b", "Crash"]], ["c"]))
        c, nf, _ = L.compare({"a": "Pass"}, {}, ["a"])
        self.assertEqual(nf, [["a", "Missing"]])

    def health(self, lock, **h):
        with open(f"{self.d}/state/fake-health-{lock}.json", "w") as f:
            json.dump(h, f)

    def alerts(self):
        p = self.d + "/state/alerts.log"
        return open(p).read().strip().splitlines() if os.path.exists(p) else []

    def test_battery_gate_hysteresis(self):
        self.attach("dev-a")
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")
        self.health("cmf", level=15, temp_c=30.0, thermal=0)
        self.farmd()
        self.assertEqual(L.find_job(jid)["parts"]["v11"]["state"], "pending")
        self.assertIn("charging", self.farm("status")[1])
        self.assertIn("batt 15%", self.farm("status")[1])
        self.health("cmf", level=35, temp_c=30.0, thermal=0)      # above 20 but below 50: still gated
        self.farmd()
        self.assertEqual(L.find_job(jid)["parts"]["v11"]["state"], "pending")
        self.assertEqual(len(self.alerts()), 1)                     # one line per state change, not per tick
        self.health("cmf", level=55, temp_c=30.0, thermal=0)
        self.farmd()
        run = json.loads(self.farm("results", jid)[1])["runs"]["v11"]
        self.assertEqual(run["verdict"], "pass")
        self.assertEqual((run["battery_pct_start"], run["battery_pct_end"], run["temp_c_start"]), (55, 55, 30.0))
        self.assertEqual(len(self.alerts()), 2)
        self.assertIn("charging->ok", self.alerts()[1])

    def test_thermal_gate(self):
        self.attach("dev-b")
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v13")
        for h, pending in (({"temp_c": 43.0, "thermal": 0}, True), ({"temp_c": 40.0, "thermal": 0}, True),
                           ({"temp_c": 30.0, "thermal": 3}, True), ({"temp_c": 30.0, "thermal": 2}, True), ({"temp_c": 37.0, "thermal": 1}, False)):
            self.health("poco", level=80, **h)
            self.farmd()
            self.assertEqual(L.find_job(jid)["parts"]["v13"]["state"] == "pending", pending, h)
        self.assertEqual(len(self.alerts()), 2)                     # ok->cooling, cooling->ok

    def test_next_state_pure(self):
        th = dict(gate.DEFAULTS)
        ok = {"level": 80, "temp_c": 30.0, "thermal": 0}
        self.assertEqual(gate.next_state("ok", dict(ok, level=19), th)[0], "charging")
        self.assertEqual(gate.next_state("ok", dict(ok, level=20), th)[0], "ok")
        self.assertEqual(gate.next_state("charging", dict(ok, level=49), th)[0], "charging")
        self.assertEqual(gate.next_state("charging", dict(ok, level=50), th)[0], "ok")
        self.assertEqual(gate.next_state("charging", dict(ok, level=50, temp_c=44.0), th)[0], "cooling")
        self.assertEqual(gate.next_state("ok", dict(ok, level=None, temp_c=None, thermal=None), th)[0], "ok")
        self.assertEqual(gate.parse("  level: 74\n  temperature: 281\nCapacity level: 3\nThermal Status: 2\n"),
                         {"level": 74, "temp_c": 28.1, "thermal": 2, "powered": None})
        self.assertTrue(gate.parse("  AC powered: false\n  USB powered: true\n")["powered"])
        self.assertFalse(gate.parse("  AC powered: false\n  USB powered: false\n")["powered"])
        rc = dict(th, require_charging=True)
        self.assertEqual(gate.next_state("ok", dict(ok, powered=False), rc)[0], "on_battery")
        self.assertEqual(gate.next_state("on_battery", dict(ok, powered=True), rc)[0], "ok")
        self.assertEqual(gate.next_state("ok", dict(ok), th)[0], "ok")      # not required: power ignored

    def test_usb_only_never_wireless(self):
        calls = []
        L.adb_live = lambda: {"adb-SERIAL-B-x._adb-tls-connect._tcp": "device", "203.0.113.5:5555": "device"}
        L.sh = lambda cmd, timeout, **kw: calls.append(cmd) or (0, "")
        usb = {"serial": "SERIAL-B", "transport": "usb", "wireless": "203.0.113.5:5555"}
        self.assertIsNone(L.resolve(usb, connect=True))
        self.assertEqual(calls, [])                                 # no `adb connect`
        both = {"serial": "SERIAL-B", "wireless": "203.0.113.5:5555"}
        self.assertEqual(L.resolve(both), "adb-SERIAL-B-x._adb-tls-connect._tcp")
        L.adb_live = lambda: {"SERIAL-B": "device", "adb-SERIAL-B-x._adb-tls-connect._tcp": "device"}
        self.assertEqual(L.resolve(both), "SERIAL-B")               # USB preferred

    def test_two_wireless_routes_and_wireless_only(self):
        calls = []
        L.sh = lambda cmd, timeout, **kw: calls.append(cmd) or (0, "")
        L.adb_live = lambda: {"198.51.100.7:5555": "device"}
        cmf = {"serial": "S1", "wireless": ["203.0.113.9:5555", "198.51.100.7:5555"]}
        self.assertEqual(L.resolve(cmf), "198.51.100.7:5555")              # second route already listed
        L.adb_live = lambda: {"S1": "device", "203.0.113.1:5555": "device"}
        x4 = {"serial": "S1", "transport": "wireless", "address": "203.0.113.1:5555"}
        self.assertEqual(L.resolve(x4), "203.0.113.1:5555")                    # wireless-only ignores USB
        L.adb_live = lambda: {}
        self.assertIsNone(L.resolve(cmf, connect=True))
        self.assertEqual([c[-1] for c in calls if "connect" in c], ["203.0.113.9:5555", "198.51.100.7:5555"])

    def test_device_loss_returns_to_pending(self):
        import time
        t0 = time.time()
        rc, out = L.run_watched(["sleep", "60"], 100, lambda: False, grace=1, poll=0.5)
        self.assertEqual(rc, 125)
        self.assertLess(time.time() - t0, 10)
        rc, out = L.run_watched(["echo", "hi"], 100, lambda: True)
        self.assertEqual((rc, out.strip()), (0, "hi"))

    def health(self, lock, **kw):
        L.write_json(self.d + "/state/fake-health-" + lock + ".json", kw)

    def test_on_battery_pending_then_charger_runs(self):
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v13")
        self.attach("dev-b")
        self.health("poco", powered=False)
        self.farmd()
        self.assertEqual(L.find_job(jid)["parts"]["v13"]["state"], "pending")     # on battery: nothing runs
        self.assertIn("on battery: paused", self.farm("status")[1])
        self.health("poco", powered=True)
        self.farmd()
        self.assertEqual(L.find_job(jid)["parts"]["v13"]["state"], "done")

    def test_unplug_mid_job_back_to_pending_lock_released(self):
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v13", "--env", "FAKE_UNPLUG=1")
        self.attach("dev-b")
        self.farmd()
        p = L.find_job(jid)["parts"]["v13"]
        self.assertEqual((p["state"], p["attempts"]), ("pending", 0))               # clean stop, no retry counted
        lk = L.PhoneLock({"alias": "dev-b", "lock": "poco"}, True)
        self.assertTrue(lk.acquire())                                               # lock was released
        lk.release()
        self.assertIn("on battery: paused", self.farm("status")[1])

    def test_disabled_untouched_enable_runs(self):
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")
        self.attach("dev-a")
        self.assertEqual(self.farm("disable", "dev-a")[0], 0)
        self.assertTrue(os.path.exists(self.d + "/state/disabled/dev-a"))
        self.farmd()
        self.assertEqual(L.find_job(jid)["parts"]["v11"]["state"], "pending")
        self.assertIn("disabled by owner", self.farm("status")[1])
        self.assertEqual(self.farm("enable", "dev-a")[1], "dev-a: enabled")
        self.farmd()
        self.assertEqual(L.find_job(jid)["parts"]["v11"]["state"], "done")
        os.makedirs(self.d + "/state/disabled", exist_ok=True)                      # a plain touch (by lock name) works too
        open(self.d + "/state/disabled/cmf", "w").close()
        _, j2, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11", "--rerun")
        self.farmd()
        self.assertEqual(L.find_job(j2)["parts"]["v11"]["state"], "pending")

    def test_store_route_idempotent(self):
        p = self.d + "/devices.toml"
        self.assertTrue(L.store_route("dev-b", "198.51.100.8:5555", p))
        self.assertFalse(L.store_route("dev-b", "198.51.100.8:5555", p))
        self.assertTrue(L.store_route("dev-b", "198.51.100.9:5555", p))
        _, devs = L.load_devices()
        self.assertEqual(L.wireless_routes(devs["dev-b"]), ["198.51.100.9:5555"])    # follows the CURRENT ip: replaced, not appended
        self.assertEqual(L.wireless_routes(devs["dev-a"]), [])
        open(p, "a").write('\n[[device]]\nalias = "dev-c"\nclass = "v10"\nserial = "SERIAL-C"\nlock = "c"\nbaseline = "c"\naddress = "198.51.100.20:5555"   # keep\nwireless = ["203.0.113.4:7777", "198.51.100.21:5555"]\n')
        self.assertTrue(L.store_route("dev-c", "198.51.100.22:5555", p))
        _, devs = L.load_devices()
        self.assertEqual(L.wireless_routes(devs["dev-c"]), ["203.0.113.4:7777", "198.51.100.22:5555"])
        self.assertIn("# keep", open(p).read())
        self.assertFalse(L.store_route("dev-c", "198.51.100.22:5555", p))

    def test_unreachable_stored_route_is_a_soft_miss(self):
        calls = []
        L._ROUTE_MISS.clear()
        L.sh = lambda cmd, timeout, **kw: calls.append(cmd) or (0, "failed to connect to 203.0.113.50:5555")
        L.adb_live = lambda: {}
        dev = {"serial": "S9", "wireless": ["203.0.113.50:5555"]}
        self.assertIsNone(L.resolve(dev, connect=True))                  # no exception
        self.assertIsNone(L.resolve(dev, connect=True))
        self.assertEqual(len(calls), 1)                                  # second attempt skipped (miss cached)
        L._ROUTE_MISS.clear()

    def test_invalid_run_is_requeued_not_pass_or_fail(self):
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11", "--env", "FAKE_SO_SWAP=1")
        self.attach("dev-a")
        self.farmd()
        p = L.find_job(jid)["parts"]["v11"]
        self.assertEqual((p["state"], p["attempts"]), ("pending", 1))
        self.assertEqual(L.job_verdict(L.find_job(jid)), "pending")
        self.assertTrue(os.path.exists(self.d + f"/state/runs/{jid}-v11.invalid-1.json"))
        self.assertFalse(os.path.exists(self.d + f"/state/runs/{jid}-v11.json"))
        self.farmd()
        self.assertEqual(L.find_job(jid)["parts"]["v11"]["verdict"], "pass")

    def test_ns_to_fail_reported(self):
        base = {"a": "Pass", "b": "NotSupported", "c": "NotSupported", "d": "Fail"}
        cur = {"a": "Pass", "b": "Fail", "c": "NotSupported", "d": "Fail"}
        self.assertEqual(L.ns_to_fail(base, cur), [["b", "Fail"]])
        self.assertEqual(L.compare(base, cur)[1], [])                    # new_failures alone would miss it
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11", "--env", "FAKE_NS2F=3")
        self.attach("dev-a")
        self.farmd()
        run = json.loads(self.farm("results", jid)[1])["runs"]["v11"]
        self.assertEqual(len(run["ns_to_fail"]), 3)
        self.assertIn("3 NotSupported->Fail", run["notes"])

    def test_identified_by_serial_on_any_transport(self):
        L._ROUTE_MISS.clear(); L._SERIALNO.clear()
        L.adb_live = lambda: {"203.0.113.77:5555": "device", "203.0.113.78:5555": "device"}
        L.sh = lambda cmd, timeout, **kw: (0, "OTHER\n" if "203.0.113.78:5555" in cmd else "S9\n")
        dev = {"serial": "S9", "wireless": ["203.0.113.99:5555"]}        # stored route is stale: the phone has another address now
        self.assertEqual(L.resolve(dev), "203.0.113.77:5555")
        dev2 = {"serial": "NOPE", "wireless": ["203.0.113.99:5555"]}     # nobody answers with that serial
        L._SERIALNO.clear()
        self.assertIsNone(L.resolve(dev2))
        L._SERIALNO.clear()

    def test_config_reloaded_without_restart(self):
        import importlib.machinery, importlib.util
        ld = importlib.machinery.SourceFileLoader("farmd_mod", HERE + "/farmd")
        m = importlib.util.module_from_spec(importlib.util.spec_from_loader("farmd_mod", ld))
        ld.exec_module(m)
        f = m.Farm(True, True, 1)
        self.assertEqual(L.wireless_routes(f.devs["dev-b"]), [])
        self.assertEqual(f.phone_env(f.devs["dev-a"])["QSAFE_HELD"], "cmf")
        e = f.phone_env(f.devs["dev-a"])                          # quick.py resolves the lock name through <LOCK>_SERIAL
        self.assertEqual(e["CMF_SERIAL"], e["ANDROID_SERIAL"])
        self.assertTrue(e["CMF_SERIAL"])
        p = self.d + "/devices.toml"
        txt = open(p).read()
        open(p, "w").write(txt.replace('baseline = "poco"\n', 'baseline = "poco"\nwireless = ["198.51.100.5:5555"]\n') + "\n")
        os.utime(p, (f.cfg_mtime + 5, f.cfg_mtime + 5))
        f.reload_devices()
        self.assertEqual(L.wireless_routes(f.devs["dev-b"]), ["198.51.100.5:5555"])

    def test_missing_so_is_an_error_not_an_invalid_driver(self):
        import importlib.machinery, importlib.util
        ld = importlib.machinery.SourceFileLoader("farmd_mod3", HERE + "/farmd")
        m = importlib.util.module_from_spec(importlib.util.spec_from_loader("farmd_mod3", ld))
        ld.exec_module(m)
        f = m.Farm(True, True, 1)
        job = {"so_sha256": "6afe5234e9c5" + "0" * 52}
        r = f.so_gone_result(job, None, 1, "FAIL session not READY")
        self.assertEqual(r["verdict"], "error")
        self.assertIn("session not READY", r["notes"])
        self.assertEqual(f.so_gone_result(job, "b" * 64, 1, "")["verdict"], "invalid")       # a foreign .so is still invalid
        self.assertEqual(f.so_gone_result(job, None, 76, "")["verdict"], "invalid")          # changed mid-run: unchanged

    def test_transient_adb_read_failure_is_invalid_not_error(self):
        import importlib.machinery, importlib.util
        ld = importlib.machinery.SourceFileLoader("farmd_mod4", HERE + "/farmd")
        m = importlib.util.module_from_spec(importlib.util.spec_from_loader("farmd_mod4", ld))
        ld.exec_module(m)
        f = m.Farm(True, True, 1)
        job = {"so_sha256": "6afe5234e9c5" + "0" * 52}
        for rc in (0, 1):
            self.assertEqual(f.so_gone_result(job, m.ADB_READ_FAILED, rc, "")["verdict"], "invalid")
        f.fake = False
        for rc_o, want in (((1, "error: device offline"), m.ADB_READ_FAILED), ((255, ""), m.ADB_READ_FAILED), ((0, "NOFILE\n"), None), ((0, "b" * 64 + "  x\n"), "b" * 64)):
            f.adb = lambda *a, _r=rc_o, **k: _r
            self.assertEqual(f.verify_so(job, {"alias": "x"}, "/p")[1], want)

    def test_bench_cube_ours_plausibility_follows_the_bridge(self):
        def row(drv, gpu, ph="poco-x8"):
            return {"phone": ph, "driver": drv, "bench": "cube", "variant": "cube-720p", "commit": "abc", "version": "v", "presented": {}, "offscreen": {"gpu_ms": gpu}}
        rows = [row("bridge", 1.32), row("ours", 1.50), row("ours", 0.4), row("bridge", 11.3, "cmf"), row("ours", 1.9, "cmf")]
        got = [BL.publishable(r, rows) for r in rows]
        self.assertEqual(got, [True, True, False, True, False])         # X8: 1.50 ms is valid (bridge 1.32), 0.4 is not; CMF: 1.9 ms << 11.3/2
        self.assertFalse(BL.publishable(row("ours", 1.9)))              # no bridge row to compare with: the old 2 ms floor
        self.assertTrue(BL.publishable(row("ours", 2.5)))
        self.assertEqual(len(BL.merge_rows([], rows)), 3)

    def test_merge_rows_uses_already_published_bridge_as_peer(self):
        def row(drv, gpu):
            return {"phone": "poco-x8", "driver": drv, "bench": "cube", "variant": "cube-720p", "commit": "abc", "version": "v", "presented": {}, "offscreen": {"gpu_ms": gpu}}
        # ours-only job: the bridge row of the same commit is already in the file; 1.4 ms is valid (bridge 1.32), not under the 2 ms fallback floor
        merged = BL.merge_rows([row("bridge", 1.32)], [row("ours", 1.40)])
        self.assertEqual([r["driver"] for r in merged], ["bridge", "ours"])
        self.assertEqual(len(BL.merge_rows([row("bridge", 1.32)], [row("ours", 0.4)])), 1)
        # a new bridge row replaces the old one: the stale 1.32 ms must not vouch for an ours row that is implausible against the new 10 ms
        self.assertEqual([r["driver"] for r in BL.merge_rows([row("bridge", 1.32)], [row("bridge", 10.0), row("ours", 2.5)])], ["bridge"])    # still rejects the invalid-winner symptom

    def test_bench_owner_pending_is_gated_not_invalid(self):
        """bench2 stopping with 'PENDING: focus rule violated' pushed nothing: the .so left on the phone by an earlier job must not make the job
        INVALID (2026-11-18: three INVALIDs in 21 s used up the v13 attempts). A finished chunk with a foreign .so is still INVALID."""
        import importlib.machinery, importlib.util
        ld = importlib.machinery.SourceFileLoader("farmd_mod2", HERE + "/farmd")
        m = importlib.util.module_from_spec(importlib.util.spec_from_loader("farmd_mod2", ld))
        ld.exec_module(m)
        f = m.Farm(True, True, 1)
        f.fake = False
        f.bench_prepare = lambda job: "/nonexistent/mdbench"
        f.tree = lambda jid: self.d + "/tree"
        f.best_file = lambda job: self.d + "/best.json"
        f.keep_log = lambda *a: None
        f.shell_route = lambda dev: False
        f.alive = lambda dev: True
        f.phone_env = lambda dev, ex=None: {}
        f.verify_so = lambda job, dev, path, key="so_sha256": (False, "7ce07a55b210")           # the previous job's driver is on the phone
        job = {"id": "j1", "ref": "main", "commit": "0" * 40, "so_sha256": "d4c7f0ef46f2", "env": "", "suite": "bench"}
        dev = {"alias": "dev-b", "class": "v13", "lock": "poco"}
        orig = m.L.run_watched
        self.addCleanup(setattr, m.L, "run_watched", orig)                                      # farmlib is shared by every test
        for out, want in (("dev-b: PENDING: focus rule violated (mCurrentFocus=Window{x u0 com.instagram.android/.Main}); the owner is using the phone", "gated"),
                          ("dev-b: stop: battery 19 %", "gated"),
                          ("dev-b: CHUNK DONE (time budget 230s); run again", "gated"),
                          ("dev-b: all done", "invalid")):
            m.L.run_watched = lambda cmd, tmo, alive, env=None, o=out, **k: (0, o)
            res = f.suite_bench(job, dev)
            if want == "gated":
                self.assertEqual(res, {"gated": True}, out)
            else:
                self.assertEqual(res["verdict"], "invalid", out)
                self.assertIn("7ce07a55b210", res["notes"])

    def test_bench_ui_blocked_is_invalid_and_requeued_once(self):
        """a foreground permission dialog (bench2 prints UI-BLOCKED) or rc=0 / 0 rows / empty log is INVALID with max_attempts 2, not `error`"""
        import importlib.machinery, importlib.util
        ld = importlib.machinery.SourceFileLoader("farmd_mod3", HERE + "/farmd")
        m = importlib.util.module_from_spec(importlib.util.spec_from_loader("farmd_mod3", ld))
        ld.exec_module(m)
        f = m.Farm(True, True, 1)
        f.fake = False
        f.bench_prepare = lambda job: "/nonexistent/mdbench"
        f.tree = lambda jid: self.d + "/tree"
        f.best_file = lambda job: self.d + "/best.json"
        f.keep_log = lambda *a: None
        f.shell_route = lambda dev: False
        f.alive = lambda dev: True
        f.phone_env = lambda dev, ex=None: {}
        f.verify_so = lambda job, dev, path, key="so_sha256": (False, "7ce07a55b210")        # empty run + a stale .so on the phone: still UI-BLOCKED, 2 attempts
        job = {"id": "j1", "ref": "main", "commit": "0" * 40, "so_sha256": "d4c7f0ef46f2", "env": "", "suite": "bench"}
        dev = {"alias": "dev-b", "class": "v10", "lock": "x4"}
        orig = m.L.run_watched
        self.addCleanup(setattr, m.L, "run_watched", orig)
        for out in ("dev-b: UI-BLOCKED: foreground dialog mCurrentFocus=Window{x u0 com.google.android.permissioncontroller/.GrantPermissionsActivity}", ""):
            m.L.run_watched = lambda cmd, tmo, alive, env=None, o=out, **k: (0, o)
            res = f.suite_bench(job, dev)
            self.assertEqual((res["verdict"], res["max_attempts"]), ("invalid", 2), out)
            self.assertIn("UI-BLOCKED", res["notes"])

    def test_bench_empty_log_reports_pending_json_error(self):
        """rc=0, 0 rows, empty log but pending.json has bench2's error (guest session did not start): the alert shows it, not UI-BLOCKED?"""
        import importlib.machinery, importlib.util
        ld = importlib.machinery.SourceFileLoader("farmd_mod4", HERE + "/farmd")
        m = importlib.util.module_from_spec(importlib.util.spec_from_loader("farmd_mod4", ld))
        ld.exec_module(m)
        f = m.Farm(True, True, 1)
        f.fake = False
        f.bench_prepare = lambda job: "/nonexistent/mdbench"
        f.tree = lambda jid: self.d + "/tree"
        f.best_file = lambda job: self.d + "/best.json"
        f.keep_log = lambda *a: None
        f.shell_route = lambda dev: False
        f.alive = lambda dev: True
        f.phone_env = lambda dev, ex=None: {}
        f.verify_so = lambda job, dev, path, key="so_sha256": (True, "x")
        job = {"id": "j2", "ref": "main", "commit": "0" * 40, "so_sha256": "d4c7f0ef46f2", "env": "", "suite": "bench"}
        dev = {"alias": "dev-b", "class": "v10", "lock": "x4"}
        orig = m.L.run_watched
        self.addCleanup(setattr, m.L, "run_watched", orig)
        for printed in ("", "bench2: guest session failed, see pending.json"):          # bench2 may print a diagnostic and still exit 0 with no rows
            def rw2(cmd, tmo, alive, env=None, printed=printed, **k):
                out = cmd[cmd.index("--out") + 1]
                os.makedirs(out, exist_ok=True)
                json.dump({"dev-b": "guest session did not start: rc=1: boom"}, open(out + "/pending.json", "w"))
                return 0, printed
            m.L.run_watched = rw2
            res = f.suite_bench(job, dev)
            self.assertEqual((res["verdict"], res.get("max_attempts")), ("invalid", 2), printed)
            self.assertIn("guest session did not start: rc=1: boom", res["notes"])
            self.assertNotIn("UI-BLOCKED", res["notes"])

    def test_sweep_power_rule_pure(self):
        pb = S.power_blocked
        ok = {"level": 60, "temp_c": 30.0, "powered": True}
        self.assertEqual(pb(ok, {}), "")
        self.assertIn("on battery", pb(dict(ok, powered=False), {}))                         # default: charger only
        self.assertIn("on battery", pb(dict(ok, powered=None), {}))                          # unreadable = battery
        self.assertIn("36", pb(dict(ok, temp_c=36.5), {}))                                   # too warm, even on a charger
        self.assertIn("unreadable", pb(dict(ok, temp_c=None), {}))
        opt = {"sweep_on_battery": True}
        self.assertIn("needs >= 80", pb(dict(ok, powered=False, level=79), opt))
        self.assertEqual(pb(dict(ok, powered=False, level=80), opt), "")
        self.assertIn("needs >= 90", pb(dict(ok, powered=False, level=85), dict(opt, sweep_batt_min=90)))
        self.assertEqual(pb(dict(ok, temp_c=37.0), {"sweep_temp_max": 40}), "")

    def test_sweep_never_runs_on_battery_unless_the_phone_opts_in(self):
        self.sweep_env(["dEQP-VK.api.a.one", "dEQP-VK.api.a.two", "dEQP-VK.api.a.three"], 2)
        self.attach("dev-a")
        hp = self.d + "/state/fake-health-cmf.json"
        L.write_json(hp, {"powered": False, "level": 90})                                   # unplugged, 90 %
        self.farmd()
        self.assertEqual(S.load("v11")["pos"], 0)
        self.assertFalse(os.path.exists(self.d + "/state/sweep/v11-chunks.jsonl"))
        L.write_json(hp, {"powered": True, "temp_c": 37.0})                                 # charger but warm
        self.farmd()
        self.assertEqual(S.load("v11")["pos"], 0)
        L.write_json(hp, {"powered": True})                                                 # charger, cool: sweeps
        self.farmd()
        self.assertEqual(S.load("v11")["pos"], 3)

    def test_gated_menu_job_does_not_cool_the_phone(self):
        """a menu job gated by "owner is using the phone" is deferred by itself; a smoke job queued behind it still runs on the same phone
        (2026-11-18: a 900 s phone-wide cooldown idled the X8 for 16 min with 6 jobs pending)"""
        self.nosweep()
        self.fake_menubench("ok")
        self.attach("dev-a")
        os.makedirs(self.d + "/state/fake-menu", exist_ok=True)
        open(self.d + "/state/fake-menu/dev-a.inuse", "w").write("screen on, launcher foreground")
        _, menu, _ = self.farm("submit", "HEAD", "--suite", "menu", "--need", "v11", "--prio", "0")
        _, smoke, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11", "--prio", "5", "--allow-stale")
        self.env["MALI_FARM_FAKE_DEFER"] = "3600"
        for _ in range(3):
            self.farmd()
        self.assertEqual(L.find_job(menu)["parts"]["v11"]["state"], "pending")        # still waiting for the owner
        self.assertEqual(L.find_job(smoke)["parts"]["v11"]["state"], "done")          # the phone was not blocked

    def test_stale_menu_job_expires(self):
        self.nosweep()
        self.attach("dev-a")
        _, menu, _ = self.farm("submit", "HEAD", "--suite", "menu", "--need", "v11")
        j = L.find_job(menu)
        j["created"] = "2020-01-01T00:00:00+01:00"
        L.write_json(L.job_path(menu), j)
        self.farmd()
        self.assertEqual(L.find_job(menu)["parts"]["v11"]["state"], "cancelled")

    def test_sweep_leaves_a_guest_phone_alone_while_the_owner_uses_it(self):
        """the idle sweep starts the app's guest session, which takes the screen: it waits (no lock hold, no recorded case) while the owner is in
        another app, and walks on once the phone is free"""
        self.sweep_env(["dEQP-VK.api.a.one", "dEQP-VK.api.a.two", "dEQP-VK.api.a.three"], 2)
        self.attach("dev-a")
        os.makedirs(self.d + "/state/fake-menu")
        open(self.d + "/state/fake-menu/dev-a.inuse", "w").write("screen on, foreground com.instagram.android")
        self.farmd()
        self.assertEqual(S.load("v11")["pos"], 0)
        self.assertFalse(os.path.exists(self.d + "/state/sweep/v11-chunks.jsonl"))
        os.unlink(self.d + "/state/fake-menu/dev-a.inuse")
        self.farmd()
        self.assertEqual(S.load("v11")["pos"], 3)

    def test_verdict_uses_main_baseline_too(self):
        # branch baseline is stale for 2 cases main passes; they fail in the run -> must be regressions, not silently "pass"
        self.attach("dev-a")
        rc, jid, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11", "--env", "FAKE_STALE_BRANCH=2")
        self.farmd()
        run = json.loads(self.farm("results", jid)[1])["runs"]["v11"]
        self.assertEqual(run["verdict"], "fail")
        self.assertEqual(len(run["new_failures"]), 2)
        self.assertIn("baseline branch ", run["notes"])
        self.assertIn(" + main ", run["notes"])
        self.assertRegex(run["notes"], r"(?<!\d)[23] cases Pass only on main")                 # 3 while local main still has the pre-refresh baseline

    def test_bless(self):
        repo = os.path.dirname(os.path.dirname(HERE))
        self.attach("dev-a", "dev-b")
        _, bad, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11,v13", "--allow-stale")      # not main's tip -> refused (unless HEAD == main)
        _, jid, _ = self.farm("submit", "main", "--suite", "daily", "--need", "v11,v13", "--allow-stale")
        _, smoke, _ = self.farm("submit", "main", "--suite", "smoke", "--need", "v11", "--allow-stale")
        for _ in range(3):
            self.farmd()
        co = self.d + "/co"
        os.makedirs(co + "/vk/deqp")
        _, devs = L.load_devices()
        for al in ("dev-a", "dev-b"):                                    # what farmd keeps of each daily run
            src = open(os.path.join(repo, "vk/deqp/ours-daily-sample-%s.tsv" % devs[al]["baseline"])).read()
            open(os.path.join(self.d, "state/logs", f"{jid}-{al}-daily.tsv"), "w").write(src.replace("\tPass\t", "\tFail\t", 1))
        rc, _, err = self.farm("bless", smoke, "--checkout", co)
        self.assertEqual(rc, 2)
        self.assertIn("not the full daily", err)
        rc, _, err = self.farm("bless", jid, "--checkout", co)
        self.assertEqual(rc, 0, err)
        for b in ("cmf", "poco"):
            p = f"{co}/vk/deqp/ours-daily-sample-{b}.tsv"
            if os.path.exists(p):
                self.assertEqual(len(open(p).read().splitlines()), 1381)
        j = L.find_job(jid)
        j["parts"]["v13"]["verdict"] = "fail"                             # a failing part -> refused
        L.write_json(L.job_path(jid), j)
        rc, _, err = self.farm("bless", jid, "--checkout", co)
        self.assertEqual(rc, 2)
        self.assertIn("verdict is", err)

    def mkjob(self, ref, commit, suite, parts, tsvs=None):
        j = L.new_job(ref, commit, False, suite, list(parts), "", "r")
        for cls, (alias, verdict) in parts.items():
            j["parts"][cls].update(state="done", verdict=verdict, device=alias)
            L.write_json(L.run_path(j["id"], cls), {"verdict": verdict, "device": alias, "counts": {"Pass": 2, "Fail": 1, "NotSupported": 1}})
            if tsvs:
                open(os.path.join(L.state_dir(), "logs", f"{j['id']}-{alias}-daily.tsv"), "w").write("".join(f"{c}\t{s}\t\n" for c, s in tsvs.items()))
        L.write_json(L.job_path(j["id"]), j)
        return j

    def test_cheap_commands(self):
        m = self.mkjob("main", "a" * 40, "daily", {"v11": ("dev-a", "pass")}, {"dEQP-VK.x.one": "Pass", "dEQP-VK.x.two": "Pass", "dEQP-VK.x.three": "Fail"})
        b = self.mkjob("feat/x", "b" * 40, "daily", {"v11": ("dev-a", "fail")}, {"dEQP-VK.x.one": "Pass", "dEQP-VK.x.two": "NotSupported", "dEQP-VK.x.three": "Pass"})
        rc, out, _ = self.farm("diff", m["id"], b["id"], "--class", "v11")
        self.assertEqual(rc, 0)
        self.assertIn("Pass->NotSupported", out)
        self.assertIn("Fail->Pass", out)
        self.assertNotIn("x.one", out)
        self.assertLess(len(out.splitlines()), 40)
        rc, out, _ = self.farm("digest")
        self.assertIn("queue: 2 done", out)
        self.assertIn("vs main aaaaaaa: 1 regressed (1 ->NS): x.two", out)
        self.assertLess(len(out.splitlines()), 41)
        rc, out, _ = self.farm("case", "x.two")
        rows = out.splitlines()
        self.assertEqual(len(rows), 2)
        self.assertTrue(rows[0].split()[1] == "main" and rows[1].endswith("NotSupported"))     # newest last
        self.assertIn("no stored daily", self.farm("case", "nope.nothing")[1])

    def test_cancel(self):
        self.attach("dev-a")
        _, j1, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")
        _, j2, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11", "--rerun")
        out = self.farm("cancel", j1)[1]
        self.assertIn("v11 cancelled", out)
        self.assertEqual(L.job_verdict(L.find_job(j1)), "cancelled")
        self.farmd()
        self.assertEqual(L.find_job(j1)["parts"]["v11"]["state"], "cancelled")      # never run
        self.assertEqual(L.find_job(j2)["parts"]["v11"]["state"], "done")
        self.assertIn("v11 pass (untouched)", self.farm("cancel", j2)[1])           # finished parts are left alone
        # a running part needs --running; the flag stops it cleanly, the lock is released, the part ends cancelled
        _, j3, _ = self.farm("submit", "HEAD", "--suite", "cases", "--case", "dEQP-VK.a.b", "--need", "v11", "--env", "FAKE_SLOW=1")
        j = L.find_job(j3)
        j["parts"]["v11"]["state"] = "running"
        L.write_json(L.job_path(j3), j)
        self.assertIn("RUNNING (use --running)", self.farm("cancel", j3)[1])
        j["parts"]["v11"]["state"] = "pending"
        L.write_json(L.job_path(j3), j)
        os.makedirs(self.d + "/state/cancel", exist_ok=True)
        open(self.d + "/state/cancel/" + j3, "w").close()
        self.farmd()
        self.assertEqual(L.find_job(j3)["parts"]["v11"]["state"], "cancelled")
        lk = L.PhoneLock({"alias": "dev-a", "lock": "cmf"}, True)
        self.assertTrue(lk.acquire())
        lk.release()

    def test_cases_suite(self):
        self.attach("dev-a")
        rc, jid, err = self.farm("submit", "HEAD", "--suite", "cases", "--case", "dEQP-VK.a.b", "--case", "dEQP-VK.a.c", "--repeat", "3", "--need", "v11")
        self.assertEqual(rc, 0, err)
        self.assertNotEqual(self.farm("submit", "HEAD", "--suite", "cases")[0], 0)      # needs a case
        self.farmd()
        run = json.loads(self.farm("results", jid, "--json")[1])["runs"]["v11"]
        self.assertEqual(run["case_counts"]["dEQP-VK.a.b"], {"Pass": 3})
        self.assertEqual(run["verdict"], "pass")
        txt = self.farm("results", jid)[1]
        self.assertIn("3 Pass of 3 (100% pass)", txt)
        _, j2, _ = self.farm("submit", "HEAD", "--suite", "cases", "--case", "dEQP-VK.a.b", "--repeat", "2", "--need", "v11", "--env", "FAKE_CASE_NS=1")
        self.farmd()
        self.assertIn("2 NotSupported of 2 (0% pass)", self.farm("results", j2)[1])

    # ---- flaky reruns
    def main_pass_cases(self, n):
        repo = os.path.dirname(os.path.dirname(HERE))
        out = subprocess.run(["git", "-C", repo, "show", "main:vk/deqp/ours-daily-sample-cmf.tsv"], capture_output=True, text=True).stdout
        return [l.split("\t")[0] for l in out.splitlines() if l.split("\t")[1:2] == ["Pass"]][:n]

    def test_daily_flaky_rerun(self):
        self.attach("dev-a")
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11", "--env", "FAKE_REGRESS=3 FAKE_FLAKY=2 FAKE_FLAKY_PASS_ON=2")
        self.farmd()
        run = json.loads(self.farm("results", jid)[1])["runs"]["v11"]
        self.assertEqual(run["verdict"], "fail")
        self.assertEqual(len(run["new_failures"]), 1)                       # 3 failed, 2 passed in a rerun
        self.assertEqual(len(run["flaky"]), 2)
        self.assertEqual({(f["fails"], f["runs"]) for f in run["flaky"]}, {(2, 3)})        # failed run 1 + rerun 1, passed rerun 2
        self.assertIn("2 flaky", run["notes"])
        self.assertEqual(len(L.load_flaky_db()), 2)
        self.assertEqual(sum(1 for (c, g) in L.load_flaky_db() if g == "v11"), 2)
        self.assertIn("(+1 new, 2 flaky)", self.farm("digest")[1])
        self.assertIn("(+1 new, 2 flaky)", self.farm("status")[1])
        out = self.farm("flaky")[1]
        self.assertIn("2/3", out)
        self.assertIn("v11", out)
        self.farmd()
        _, j2, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11", "--env", "FAKE_REGRESS=3 FAKE_FLAKY=2 FAKE_FLAKY_PASS_ON=2")
        self.farmd()
        self.assertEqual({v[:2] and (v[0], v[1]) for v in L.load_flaky_db().values()}, {(4, 6)})     # counts accumulate

    def test_daily_crash_burst_is_invalid_and_never_reaches_the_flaky_list(self):
        self.attach("dev-a")
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11", "--env", "FAKE_CRASH=120 FAKE_FLAKY=120 FAKE_REGRESS=120")
        self.farmd()
        inv = json.load(open(self.d + f"/state/runs/{jid}-v11.invalid-1.json"))          # invalid: re-queued, never a pass/fail
        self.assertEqual(inv["verdict"], "invalid")
        self.assertIn("crashed or have no result", inv["notes"])
        self.assertEqual(L.load_flaky_db(), {})                              # no reruns were made, nothing recorded

    def test_daily_few_crashes_are_not_invalid(self):
        self.attach("dev-a")
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11", "--env", "FAKE_CRASH=5")
        self.farmd()
        run = json.loads(self.farm("results", jid)[1])["runs"]["v11"]
        self.assertNotEqual(run["verdict"], "invalid", run["notes"])
        self.assertGreaterEqual(run["counts"].get("Crash", 0), 1)

    def test_lost_cases_unit(self):
        base = {"a": "Pass", "b": "NotSupported", "c": "Crash", "d": "Fail", "e": "Pass"}
        cur = {"a": "Crash", "b": "Crash", "c": "Crash", "d": "Fail"}
        self.assertEqual(L.lost_cases(base, cur, sorted(base)), ["a", "b", "e"])      # c crashed in the baseline already; e has no result

    def test_flaky_burst_over_the_cap_is_reported_but_not_recorded(self):
        self.attach("dev-a")
        n = L.FLAKY_RECORD_MAX + 5
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11", "--env", f"FAKE_REGRESS={n} FAKE_FLAKY={n}")
        self.farmd()
        run = json.loads(self.farm("results", jid)[1])["runs"]["v11"]
        self.assertEqual(len(run["flaky"]), n)
        self.assertIn("NOT added to the flaky list", run["notes"])
        self.assertEqual(L.load_flaky_db(), {})

    def test_daily_flaky_that_never_passes_stays_new(self):
        self.attach("dev-a")
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11", "--env", "FAKE_REGRESS=2 FAKE_FLAKY=1 FAKE_FLAKY_PASS_ON=5")
        self.farmd()
        run = json.loads(self.farm("results", jid)[1])["runs"]["v11"]
        self.assertEqual((len(run["new_failures"]), len(run["flaky"])), (2, 0))      # passes only in rerun 5: only 3 reruns are made
        self.assertIn("(+2 new, 0 flaky)", self.farm("digest")[1])
        self.assertEqual(L.load_flaky_db(), {})
        _, j2, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11")
        self.farmd()
        self.assertIn("(+0 new, 0 flaky)", self.farm("digest")[1])
        self.assertEqual(self.farm("flaky")[1], "flaky list is empty")

    def test_cases_suite_flaky_rerun(self):
        self.attach("dev-a")
        cs = self.main_pass_cases(2)
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "cases", *sum([["--case", c] for c in cs], []), "--need", "v11", "--env", "FAKE_FLAKY_CASES=1")
        self.farmd()
        run = json.loads(self.farm("results", jid, "--json")[1])["runs"]["v11"]
        self.assertEqual(run["verdict"], "pass")
        self.assertEqual(run["new_failures"], [])
        self.assertEqual(len(run["flaky"]), 2)
        self.assertEqual(run["case_counts"][cs[0]], {"Fail": 1, "Pass": 1})
        self.assertEqual(len(L.load_flaky_db()), 2)

    def test_flip_scan_and_seed(self):
        def fake_run(jid, ref, commit, so, started, tsv, cls="v11", dirty=False, verdict="fail"):
            j = {"id": jid, "ref": ref, "commit": commit, "suite": "daily", "parts": {cls: {"state": "done", "verdict": verdict, "device": "dev-a"}}, "seq": 1}
            L.write_json(L.job_path(jid), j)
            L.write_json(L.run_path(jid, cls), {"job": jid, "suite": "daily", "class": cls, "device": "dev-a", "commit": commit, "so_sha256": so, "env": "",
                                               "dirty": dirty, "verdict": verdict, "started": started})
            open(os.path.join(L.state_dir(), "logs", f"{jid}-dev-a-daily.tsv"), "w").write("".join(f"{c}\t{s}\t\n" for c, s in tsv.items()))
        t = lambda h: time.strftime("%Y-%m-%dT", time.localtime()) + f"{h:02d}:00:00+00:00"
        old = time.strftime("%Y-%m-%dT00:00:00+00:00", time.localtime(time.time() - 10 * 86400))
        fake_run("20260101-000001-aaaa", "br1", "a" * 40, "s1", t(1), {"x.flip": "Pass", "x.steady": "Pass", "x.dead": "Fail", "x.ns": "NotSupported"})
        fake_run("20260101-000002-aaaa", "br1", "a" * 40, "s1", t(2), {"x.flip": "Fail", "x.steady": "Pass", "x.dead": "Fail", "x.ns": "Pass"})
        fake_run("20260101-000003-bbbb", "br2", "b" * 40, "s2", t(3), {"x.dirty": "Pass"}, dirty=True)           # dirty tree, other binary
        fake_run("20260101-000004-bbbb", "br2", "b" * 40, "s2", t(4), {"x.dirty": "Fail"}, dirty=True)           # same binary: counts
        fake_run("20260101-000005-m1", "main", "c" * 40, "s3", t(5), {"x.fix": "Fail", "x.noise": "Pass"})
        fake_run("20260101-000006-m2", "main", "d" * 40, "s4", t(6), {"x.fix": "Pass", "x.noise": "Fail"})       # main: one change = a fix / regression
        fake_run("20260101-000007-m3", "main", "e" * 40, "s5", t(7), {"x.fix": "Pass", "x.noise": "Pass"})       # noise: Pass -> Fail -> Pass
        fake_run("20260101-000008-old", "br3", "f" * 40, "s6", old, {"x.old": "Pass"})
        fake_run("20260101-000009-old", "br3", "f" * 40, "s6", old, {"x.old": "Fail"})
        got = L.flip_scan(3)
        self.assertEqual(sorted(c for c, _ in got), ["x.dirty", "x.flip", "x.noise"])
        self.assertEqual(got[("x.flip", "v11")][:2], [1, 2])
        rc, out, _ = self.farm("flaky", "--seed")
        self.assertEqual(rc, 0)
        self.assertIn("v11: 3 flipped case(s)", out)
        self.farm("flaky", "--seed")                                   # idempotent
        db = L.load_flaky_db()
        self.assertEqual(len(db), 3)
        self.assertEqual(db[("x.flip", "v11")][:2], [1, 2])

    # ---- bench suite, auto triggers, priorities
    def test_bench_pure_trigger_and_idle_logic(self):
        self.assertTrue(BL.touches_driver(["README.md", "vk/src/mali_cmd.c"]))
        self.assertTrue(BL.touches_driver(["libkbase/src/ctx.c"]))
        self.assertFalse(BL.touches_driver(["tools/farm/farmd", "vk/tests/t_x.c", "vk/deqp/x.tsv", "bench/RESULTS.md"]))
        self.assertEqual(BL.plan_trigger(None, "b", [], ["v11"]), ("b", []))                       # first sight: record only, no storm
        self.assertEqual(BL.plan_trigger("a", "a", ["vk/src/x.c"], ["v11"]), ("a", []))
        self.assertEqual(BL.plan_trigger("a", "b", ["docs/x.md"], ["v11", "v13"]), ("b", []))     # moved but irrelevant
        self.assertEqual(BL.plan_trigger("a", "b", ["vk/src/x.c"], ["v13", "v11"]), ("b", ["v11", "v13"]))
        devs = {"cmf": {"class": "v11"}, "poco-x8": {"class": "v13"}, "poco-x4": {"class": "v10", "route": "shell"}, "optout": {"class": "v11", "bench": False}}
        st = {a: {"status": "up"} for a in devs}
        st["poco-x8"]["status"] = "absent"
        self.assertEqual(BL.available(devs, st, set()), ["cmf"])                                    # shell route, opt-out and absent excluded
        self.assertEqual(BL.available(devs, st, {"cmf"}), [])                                        # owner-disabled
        rows = []
        idle = {"cmf": 0}
        args = dict(devs=devs, dstate=st, rows=rows, jobs=[], tip="T", disabled=set())
        self.assertEqual(BL.idle_candidates(899, idle, **args), [])                                  # not idle long enough
        self.assertEqual(BL.idle_candidates(901, idle, **args), ["cmf"])
        self.assertEqual(BL.idle_candidates(901, idle, running={"cmf"}, **args), [])
        self.assertEqual(BL.idle_candidates(901, idle, **dict(args, dstate={"cmf": {"status": "cooling"}})), [])   # health gate not ok
        self.assertEqual(BL.idle_candidates(901, idle, **dict(args, rows=[{"phone": "cmf", "commit_full": "T"}])), [])  # already has a row for main
        self.assertEqual(len(BL.idle_candidates(901, idle, **dict(args, rows=[{"phone": "cmf", "commit_full": "OLD"}]))), 1)
        job = {"suite": "bench", "commit": "T", "auto": True, "parts": {"v11": {"state": "pending"}}}
        self.assertEqual(BL.idle_candidates(901, idle, **dict(args, jobs=[job])), [])                # already queued
        job["parts"]["v11"]["state"] = "cancelled"
        self.assertEqual(BL.idle_candidates(901, idle, **dict(args, jobs=[job])), ["cmf"])
        both = {"cmf": 100, "optout2": 0}
        devs2 = {"cmf": {"class": "v11"}, "b2": {"class": "v11"}}
        st2 = {"cmf": {"status": "up"}, "b2": {"status": "up"}}
        self.assertEqual(BL.idle_candidates(2000, {"cmf": 500, "b2": 100}, devs2, st2, [], [], "T", set()), ["b2", "cmf"])   # longest idle = oldest missing first

    def test_bench_priority_order(self):
        jobs = [{"id": "auto1", "auto": True, "seq": 1}, {"id": "ex2", "seq": 2}, {"id": "ex3", "seq": 3}, {"id": "auto0", "auto": True, "seq": 0}]
        self.assertEqual([j["id"] for j in BL.order_jobs(jobs)], ["ex2", "ex3", "auto0", "auto1"])   # explicit > auto, each FIFO
        self.assertTrue(BL.pin_ok({}, "cmf") and BL.pin_ok({"pin": "cmf"}, "cmf") and not BL.pin_ok({"pin": "cmf"}, "poco-x8"))

    def test_bench_digest_lines(self):
        def row(ph, c, drv, var, cpu, gpu, ref="main"):
            return {"phone": ph, "commit_full": c, "ref": ref, "date": "2026-11-17", "driver": drv, "variant": var, "offscreen": {"cpu_ms": cpu, "gpu_ms": gpu}}
        rows = [row("cmf", "a" * 40, "ours", "triangle-3k", 9.0, 0.0), row("cmf", "b" * 40, "ours", "triangle-3k", 1.91, 0.0), row("cmf", "b" * 40, "bridge", "triangle-3k", 3.2, 0.0),
                row("cmf", "b" * 40, "ours", "cube-720p", 0.4, 12.34), row("cmf", "b" * 40, "bridge", "cube-720p", 0.5, 11.0)]
        out = BL.digest_lines(rows)
        self.assertEqual(len(out), 1)
        self.assertIn("bbbbbbb", out[0])                                                              # newest commit only
        self.assertIn("tri CPU ours 1.91 / bridge 3.20 ms", out[0])
        self.assertIn("cube GPU ours 12.34 / bridge 11.00 ms", out[0])

    def test_bench_regressions_flag_over_10_percent(self):
        def row(c, fps, cpu, drv="ours", var="triangle-3k"):
            return {"phone": "p", "commit_full": c * 40, "driver": drv, "variant": var, "offscreen": {"fps": fps, "cpu_ms": cpu}}
        rows = [row("a", 100, 2.0), row("b", 85, 2.5), row("a", 50, 1.0, "bridge"), row("b", 48, 1.05, "bridge"),
                row("a", 100, 2.0, var="cube-720p"), row("b", 95, 2.1, var="cube-720p"), row("c", 90, 2.0, var="solo")]
        out = BL.regressions(rows)
        self.assertEqual(len(out), 2)                                                   # ours triangle: fps -15% and cpu +25%
        self.assertTrue(all("ours triangle-3k" in o for o in out))
        self.assertTrue(any("fps 100 -> 85" in o for o in out) and any("cpu_ms 2 -> 2.5" in o for o in out))
        self.assertEqual(BL.regressions(rows[:1] + [row("a", 1, 9.0)]), [])             # same commit re-run is not a regression pair

    def test_bench_regressions_incomplete_rerun_and_exact_boundary(self):
        def row(c, fps, cpu, off=True):
            return {"phone": "p", "commit_full": c * 40, "driver": "ours", "variant": "v", "offscreen": {"fps": fps, "cpu_ms": cpu} if off else {}}
        rows = [row("a", 100, 2.0), row("b", 85, 2.5), row("b", 0, 0, off=False)]
        self.assertEqual(len(BL.regressions(rows)), 2)                                  # incomplete rerun keeps the valid A..B pair
        self.assertEqual(BL.regressions([row("a", 100, 2.0), row("b", 90, 2.2)]), [])   # exactly 10% on both metrics (2.0 -> 2.2 is 10.000000000000009)
        self.assertEqual(len(BL.regressions([row("a", 100, 2.0), row("b", 89, 2.21)])), 2)
        part = [row("a", 100, 2.0), {"phone": "p", "commit_full": "b" * 40, "driver": "ours", "variant": "v", "offscreen": {"cpu_ms": 2.5}}]
        self.assertEqual(len(BL.regressions(part)), 1)                                  # missing fps skips only that metric

    def nosweep(self):
        os.environ.pop("MALI_FARM_SWEEP_LIST", None)          # an earlier sweep test leaves it in os.environ: a sweep would loop in these passes
        self.env.pop("MALI_FARM_SWEEP_LIST", None)

    def test_bench_suite_idle_trigger_runs_once_and_digest(self):
        self.nosweep()
        self.attach("dev-a")
        self.env.update(MALI_FARM_AUTO_BENCH="1", MALI_FARM_BENCH_IDLE="0")
        self.farmd()
        jobs = [j for j in L.list_jobs() if j["suite"] == "bench"]
        self.assertEqual(len(jobs), 1)
        self.assertTrue(jobs[0].get("auto"))
        self.assertEqual(jobs[0]["pin"], "dev-a")
        self.assertEqual(jobs[0]["parts"]["v11"]["state"], "done")
        self.assertEqual(jobs[0]["parts"]["v11"]["verdict"], "pass")
        rows = BL.load_rows(self.d + "/state")
        self.assertEqual(len(rows), 4)
        self.assertTrue(all(r["phone"] == "dev-a" and r["ref"] == "main" and len(r["commit_full"]) == 40 for r in rows))
        self.assertIn("bench dev-a", self.farm("digest")[1])
        self.assertIn("tri CPU ours 1.90 / bridge 2.47 ms", self.farm("digest")[1])
        self.farmd()                                                                                   # a row for main exists: nothing more is queued
        self.assertEqual(len([j for j in L.list_jobs() if j["suite"] == "bench"]), 1)

    def test_bench_suite_explicit_submit(self):
        self.nosweep()
        self.attach("dev-a")
        rc, jid, err = self.farm("submit", "HEAD", "--suite", "bench", "--need", "v11")
        self.assertEqual(rc, 0, err)
        self.farmd()
        self.assertEqual(self.farm("wait", jid)[1], "pass")
        self.assertFalse(L.find_job(jid).get("auto"))
        rows = BL.load_rows(self.d + "/state")
        self.assertEqual({r["job"] for r in rows}, {jid})
        self.assertEqual(L.find_job(jid)["build"], "ok")

    def test_bench_main_commit_touching_driver_queues_every_available_phone(self):
        self.nosweep()
        self.attach("dev-a", "dev-b")
        root = subprocess.run(["git", "-C", HERE, "rev-list", "--max-parents=0", "main"], capture_output=True, text=True).stdout.split()[-1]
        os.makedirs(self.d + "/state/bench", exist_ok=True)
        L.write_json(self.d + "/state/bench/auto.json", {"last_main": root})                          # main moved a long way since: touches vk/src
        self.env.update(MALI_FARM_AUTO_BENCH="1", MALI_FARM_BENCH_IDLE="999999")
        self.health("poco", level=80, temp_c=30.0, thermal=0, powered=True)
        self.farmd()
        jobs = [j for j in L.list_jobs() if j["suite"] == "bench"]
        self.assertEqual(len(jobs), 1)
        self.assertEqual(sorted(jobs[0]["parts"]), ["v11", "v13"])
        self.assertTrue(jobs[0]["auto"] and "pin" not in jobs[0])
        self.assertEqual(jobs[0]["commit"], subprocess.run(["git", "-C", HERE, "rev-parse", "main"], capture_output=True, text=True).stdout.strip())
        self.assertEqual({p["state"] for p in jobs[0]["parts"].values()}, {"done"})
        self.farmd()                                                                                   # same main: no second job
        self.assertEqual(len([j for j in L.list_jobs() if j["suite"] == "bench"]), 1)

    def test_explicit_job_runs_before_queued_auto_bench(self):
        self.nosweep()
        self.attach("dev-a")
        auto = L.new_job("main", "0" * 40, False, "bench", ["v11"], "", "t", auto=True)               # queued first (older), but auto
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")
        self.farmd()                                                                                   # one phone, one job per pass
        self.assertEqual(L.find_job(jid)["parts"]["v11"]["state"], "done")
        self.assertEqual(L.find_job(auto["id"])["parts"]["v11"]["state"], "pending")
        self.farmd()
        self.assertEqual(L.find_job(auto["id"])["parts"]["v11"]["state"], "done")

    def test_queue_priority_defaults_and_order(self):
        self.assertEqual([L.SUITE_PRIO[x] for x in ("smoke", "daily", "bench", "menu", "cases", "sweep")], [0, 0, 10, 10, 20, 30])
        cs = L.new_job("b", "1" * 40, False, "cases", ["v11"], {}, "t", cases=["x"])                  # exploratory, oldest
        bn = L.new_job("main", "2" * 40, False, "bench", ["v11"], {}, "t", auto=True)
        sm = L.new_job("b", "3" * 40, False, "smoke", ["v11"], {}, "t")
        dl = L.new_job("b", "4" * 40, False, "daily", ["v11"], {}, "t")
        self.assertEqual([j["id"] for j in BL.order_jobs(L.list_jobs())], [sm["id"], dl["id"], bn["id"], cs["id"]])   # gates, bench, cases; FIFO inside
        old = {"id": "old", "suite": "smoke", "seq": 1}                                                # stored before the field existed: suite default
        self.assertEqual(L.job_prio(old), 0)
        self.assertEqual(L.job_prio({"suite": "cases", "prio": 5}), 5)

    def test_farm_prio_and_submit_prio(self):
        _, a, _ = self.farm("submit", "HEAD", "--suite", "cases", "--need", "v11", "--case", "dEQP-VK.api.x.y")
        _, b, _ = self.farm("submit", "HEAD", "--suite", "cases", "--need", "v11", "--case", "dEQP-VK.api.x.z", "--prio", "3")
        self.assertEqual((L.find_job(a)["prio"], L.find_job(b)["prio"]), (L.SINGLE_CASE_PRIO, 3))
        rc, out, _ = self.farm("prio", a, "1")
        self.assertEqual(rc, 0)
        self.assertIn("5 -> 1", out)
        self.assertEqual([j["id"] for j in BL.order_jobs(L.list_jobs())], [a, b])
        self.assertEqual(self.farm("prio", "nosuchjob", "1")[0], 1)
        self.nosweep()
        self.attach("dev-a")
        self.farmd()                                                                                   # the promoted job runs first
        self.assertEqual((L.find_job(a)["parts"]["v11"]["state"], L.find_job(b)["parts"]["v11"]["state"]), ("done", "pending"))

    def test_supersede_old_gate_jobs(self):
        _, old, _ = self.farm("submit", "HEAD~1", "--suite", "smoke", "--need", "v11,v13", "--allow-stale")
        _, other, _ = self.farm("submit", "HEAD~1", "--suite", "daily", "--need", "v11", "--allow-stale")              # another suite: stays
        _, keep, _ = self.farm("submit", "HEAD~2", "--suite", "smoke", "--need", "v11", "--keep-old", "--allow-stale")
        j = L.find_job(old)
        j["parts"]["v13"]["state"] = "running"                                                          # a running part is never touched
        L.write_json(L.job_path(old), j)
        rc, new, err = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11,v13", "--allow-stale")
        self.assertEqual(rc, 0)
        self.assertIn("supersede", err)
        self.assertEqual({c: p["state"] for c, p in L.find_job(old)["parts"].items()}, {"v11": "cancelled", "v13": "running"})
        self.assertEqual(L.find_job(other)["parts"]["v11"]["state"], "pending")
        self.assertEqual(L.find_job(keep)["parts"]["v11"]["state"], "cancelled")                        # older and pending: superseded too (kept only from being cancelled BY ITS OWN submit)
        self.assertIn(old, open(self.d + "/state/logs/farm-queue.log").read())
        _, k2, _ = self.farm("submit", "HEAD~1", "--suite", "smoke", "--need", "v11", "--keep-old", "--allow-stale")
        _, new2, err2 = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11", "--keep-old", "--allow-stale")   # --keep-old on the NEW job: cancels nothing
        self.assertNotIn("supersede", err2)
        self.assertEqual(L.find_job(k2)["parts"]["v11"]["state"], "pending")

    def test_supersede_unit_rules(self):
        base = dict(suite="smoke", repo="r", env={}, only="", seq=1)
        mk = lambda i, c, **kw: dict(base, id=i, commit=c, parts={"v11": {"state": "pending"}, "v13": {"state": "pending"}}, **kw)
        old, bench, cases, env, only, other_repo, newer = (mk("o", "a"), mk("b", "a", suite="bench"), mk("c", "a", suite="cases"), mk("e", "a", env={"X": "1"}),
                                                         mk("n", "a", only="draw"), mk("r", "a", repo="r2"), mk("w", "a", seq=9))
        L.os.makedirs(L.state_dir() + "/jobs", exist_ok=True)
        new = dict(mk("new", "z", seq=5), parts={"v11": {"state": "pending"}})
        got = self._sup(new, [old, bench, cases, env, only, other_repo, newer], lambda o, n: True)
        self.assertEqual(got, [("o", "a", ["v11"])])                                                    # only the class the new job covers
        self.assertEqual(old["parts"]["v13"]["state"], "pending")
        self.assertEqual(self._sup(dict(new, suite="cases"), [mk("o2", "a", suite="cases")], lambda o, n: True), [])
        self.assertEqual(self._sup(new, [mk("o3", "a")], lambda o, n: False), [])           # not an ancestor: another branch

    def test_supersede_all_suites_same_ref(self):
        base = dict(suite="smoke", repo="r", env={}, only="", seq=1, ref="lane/x")
        mk = lambda i, **kw: dict(dict(base, id=i, commit="a", parts={"v13": {"state": "pending"}}), **kw)
        new = mk("new", commit="z", seq=5)
        cases, bench, daily, other_ref, head, sweep, auto = (mk("c", suite="cases"), mk("b", suite="bench"), mk("d", suite="daily"),
            mk("o", suite="cases", ref="lane/y"), mk("h", suite="cases", ref="HEAD"), mk("s", suite="sweep"), mk("a", suite="bench", auto=True))
        L.os.makedirs(L.state_dir() + "/jobs", exist_ok=True)
        got = self._sup(new, [cases, bench, daily, other_ref, head, sweep, auto], lambda o, n: True)
        self.assertEqual(sorted(g[0] for g in got), ["b", "c", "d"])
        self.assertEqual(other_ref["parts"]["v13"]["state"], "pending")
        self.assertEqual(self._sup(dict(new, ref="HEAD"), [mk("c2", suite="cases", ref="HEAD")], lambda o, n: True), [])

    def test_supersede_sibling_needs_other_suite_and_stable_ref(self):
        base = dict(suite="smoke", repo="r", env={}, only="", seq=1, ref="lane/x")
        mk = lambda i, **kw: dict(dict(base, id=i, commit="a", parts={"v13": {"state": "pending"}}), **kw)
        L.os.makedirs(L.state_dir() + "/jobs", exist_ok=True)
        new = mk("new", commit="z", seq=5)
        run = lambda n, js: self._sup(n, js, lambda o, m: True)
        self.assertEqual(run(new, [mk("o1", only="draw"), mk("o2", env={"X": "1"})]), [])               # same suite, other only/env: kept
        for r in ("HEAD~1", "main^", "@{u}", "abc1234", "HEAD^2", "FETCH_HEAD", "ORIG_HEAD", "MERGE_HEAD"):
            self.assertEqual(run(mk("n2", commit="z", seq=5, ref=r), [mk("c", suite="cases", ref=r)]), [], r)
        self.assertEqual([g[0] for g in run(new, [mk("c3", suite="cases")])], ["c3"])

    def test_supersede_skips_job_it_cannot_refresh(self):
        """the re-read before the write fails (job file gone or corrupt): skip the job, never write the stale snapshot back"""
        base = dict(suite="smoke", repo="r", env={}, only="", seq=1)
        mk = lambda i: dict(base, id=i, commit="a", parts={"v11": {"state": "pending"}})
        L.os.makedirs(L.state_dir() + "/jobs", exist_ok=True)
        gone, bad = mk("gone"), mk("bad")
        open(L.job_path("bad"), "w").write("{not json")
        new = dict(mk("new"), commit="z", seq=5)
        self.assertEqual(L.supersede_old_gates(new, [gone, bad], lambda o, n: True), [])
        self.assertFalse(L.os.path.exists(L.job_path("gone")))                                          # nothing written back
        self.assertEqual(open(L.job_path("bad")).read(), "{not json")

    def _sup(self, new, jobs, anc):
        for j in jobs:                                                                                  # supersede re-reads each job from disk
            L.write_json(L.job_path(j["id"]), j)
        return L.supersede_old_gates(new, jobs, anc)

    def test_digest_queue_positions(self):
        a = L.new_job("b", "1" * 40, False, "cases", ["v11", "v13"], {}, "t", cases=["x"])
        g = L.new_job("b", "3" * 40, False, "smoke", ["v11"], {}, "t")
        by, pos = L.queue_view(L.list_jobs(), BL.order_jobs)
        self.assertEqual(by, {"v11": (0, 2), "v13": (0, 1)})
        self.assertEqual((pos[(g["id"], "v11")], pos[(a["id"], "v11")], pos[(a["id"], "v13")]), (1, 2, 1))
        out = self.farm("digest")[1]
        self.assertIn("queue by class: v11 2 pending, 0 running; v13 1 pending, 0 running", out)
        self.assertIn(f"pending {g['id']} smoke p0", out)

    def test_valrun_never_reads_another_runs_result(self):
        """qdrv fails before writing a result ("session not READY"): valrun must report NO RESULT (rc 74), not cat the newest valguest jsonl of
        another phone/job (that made a perf/draw-path smoke fail with a foreign job's work dir in the failing line)"""
        home = self.d + "/home"
        tree = self.d + "/tree"
        qdir = home + "/Projects/mali-gaming/runs/quick"
        for dpath in (qdir, home + "/Projects/mali-gaming/scripts/quick", home + "/.cache/mali-tmp", tree + "/vk/tests", tree + "/vk/out/a64-glibc", tree + "/tools", self.d + "/bin"):
            os.makedirs(dpath, exist_ok=True)
        shutil.copy(HERE + "/../../vk/tests/valrun.sh", tree + "/vk/tests/valrun.sh")
        for n in ("libvulkan_mali.so", "t_vk_draw", "t_vk_compute", "t_vk_meta"):
            open(tree + "/vk/out/a64-glibc/" + n, "w").write("x")
        def script(path, body):
            open(path, "w").write("#!/bin/bash\n" + body)
            os.chmod(path, 0o755)
        script(self.d + "/bin/adb", "exit 0\n")
        script(tree + "/tools/dev-serial", "echo SERIAL\n")
        stale = qdir + "/20990101-000000-cmf-drv-valguest.sh.jsonl"         # another phone's older result, newest file of the directory
        open(stale, "w").write('{"line":"FOREIGN work/other-job/vk/tests/t_vk_draw.c:1059"}\n')
        qdrv = home + "/Projects/mali-gaming/scripts/quick/qdrv"
        script(qdrv, "echo 'FAIL session not READY; fix: scripts/quick/qrestore poco'\n")
        env = dict(os.environ, HOME=home, PATH=self.d + "/bin:" + os.environ["PATH"], QSAFE_LOCK_OVERRIDE="1")
        run = lambda: subprocess.run(["bash", tree + "/vk/tests/valrun.sh", "A=1", "20", ""], capture_output=True, text=True, env=env, timeout=60, cwd=tree)
        r = run()
        self.assertEqual(r.returncode, 74, r.stdout + r.stderr)
        self.assertNotIn("FOREIGN", r.stdout + r.stderr)
        self.assertIn("NO RESULT", r.stderr)
        self.assertIn("session not READY", r.stderr)                        # the qdrv line is kept for the job notes
        script(qdrv, "echo ok; echo '{\"line\":\"OTHER-PHONE\"}' >" + qdir + "/29990101-000000-poco-x4-drv-valguest.sh.jsonl; "
                     "echo '{\"line\":\"MINE\"}' >" + qdir + "/29990101-000001-poco-drv-valguest.sh.jsonl\n")
        r = run()                                                            # a fresh result of THIS phone only (poco is not poco-x4)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("MINE", r.stdout)
        self.assertNotIn("OTHER-PHONE", r.stdout)

    def test_valrun_serial_env_and_symlinked_runs_dir(self):
        """qdrv resolves scripts/quick (a symlink to MediaDeck) and writes runs/quick THERE; quick.py reads CMF_SERIAL only: valrun must export it and look in that dir"""
        home = self.d + "/home"
        tree = self.d + "/tree"
        md = self.d + "/MediaDeck"
        for dpath in (md + "/scripts/quick", md + "/runs/quick", home + "/Projects/mali-gaming/scripts", home + "/.cache/mali-tmp", tree + "/vk/tests", tree + "/vk/out/a64-glibc", tree + "/tools", self.d + "/bin"):
            os.makedirs(dpath, exist_ok=True)
        os.symlink(md + "/scripts/quick", home + "/Projects/mali-gaming/scripts/quick")
        shutil.copy(HERE + "/../../vk/tests/valrun.sh", tree + "/vk/tests/valrun.sh")
        for n in ("libvulkan_mali.so", "t_vk_draw", "t_vk_compute", "t_vk_meta"):
            open(tree + "/vk/out/a64-glibc/" + n, "w").write("x")
        def script(path, body):
            open(path, "w").write("#!/bin/bash\n" + body)
            os.chmod(path, 0o755)
        script(self.d + "/bin/adb", "exit 0\n")
        script(tree + "/tools/dev-serial", "echo SERIAL-CMF\n")
        script(md + "/scripts/quick/qdrv", "echo \"serial=$CMF_SERIAL\"; echo '{\"line\":\"MINE\"}' >" + md + "/runs/quick/29990101-000001-cmf-drv-valguest.sh.jsonl\n")
        env = dict(os.environ, HOME=home, PATH=self.d + "/bin:" + os.environ["PATH"], QSAFE_LOCK_OVERRIDE="1", PHONE="cmf")
        env.pop("CMF_SERIAL", None)
        env.pop("QUICK_RUNS_DIR", None)
        r = subprocess.run(["bash", tree + "/vk/tests/valrun.sh", "A=1", "20", ""], capture_output=True, text=True, env=env, timeout=60, cwd=tree)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("serial=SERIAL-CMF", r.stderr)
        self.assertIn("MINE", r.stdout)
        os.symlink(md + "/runs/quick", self.d + "/link-runs")                # a symlinked QUICK_RUNS_DIR override must be followed
        r = subprocess.run(["bash", tree + "/vk/tests/valrun.sh", "A=1", "20", ""], capture_output=True, text=True,
                           env=dict(env, QUICK_RUNS_DIR=self.d + "/link-runs"), timeout=60, cwd=tree)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("MINE", r.stdout)

    def test_auto_bench_beats_sweep(self):
        self.sweep_env(["dEQP-VK.api.a.one", "dEQP-VK.api.a.two", "dEQP-VK.api.a.three"], 2)
        self.attach("dev-a")
        auto = L.new_job("main", "0" * 40, False, "bench", ["v11"], "", "t", auto=True)
        self.farmd()
        self.assertEqual(L.find_job(auto["id"])["parts"]["v11"]["state"], "done")
        self.assertEqual(S.load("v11")["pos"], 0)                                                      # the sweep did not run over it
        self.farmd()
        self.assertEqual(S.load("v11")["pos"], 3)                                                      # queue empty: sweep resumes

    # ---- idle sweep (fake phones; list = MALI_FARM_SWEEP_LIST, 3 cases per chunk)
    def sweep_env(self, cases, chunks, **extra):
        lst = self.d + "/mustpass.txt"
        open(lst, "w").write("\n".join(cases) + "\n")
        self.env.update(MALI_FARM_SWEEP_LIST=lst, MALI_FARM_SWEEP_CHUNK="3", MALI_FARM_SWEEP_MAX_CHUNKS=str(chunks), **extra)
        os.environ["MALI_FARM_SWEEP_LIST"] = lst

    def test_sweep_walk_resume_crash_skip(self):
        names = ["dEQP-VK.api.a.one", "dEQP-VK.api.a.CRASHX", "dEQP-VK.api.b.three", "dEQP-VK.api.b.HANGX", "dEQP-VK.pipe.c.NSX",
                 "dEQP-VK.pipe.c.FAILX", "dEQP-VK.pipe.c.many_things"]
        self.sweep_env(names, 1)
        self.attach("dev-a")
        self.farmd()
        self.assertEqual(S.load("v11")["pos"], 3)                              # one chunk, then the session ends
        self.farmd()                                                           # resumes where it stopped (no restart)
        st = S.load("v11")
        self.assertEqual(st["pos"], 6)
        self.assertEqual(st["chunks"], 2)
        self.env["MALI_FARM_SWEEP_MAX_CHUNKS"] = "5"
        self.farmd()                                                           # crash/hang/fail cases never stalled it: the pass completed
        passes = S.load("v11")["passes"]
        self.assertTrue(passes and passes[-1]["pos"] == 7)
        self.assertEqual(passes[-1]["counts"], {"P": 2, "C": 1, "T": 1, "N": 1, "F": 1, "K": 1})
        # per-chunk records carry the commit, compact status strings
        recs = [json.loads(l) for l in open(self.d + "/state/sweep/v11-chunks.jsonl")]
        self.assertTrue(all(r["commit"] and r["z"] for r in recs))
        self.assertEqual(S.load("v11")["pos"] < 7 and S.load("v11")["commit"] is not None, True)
        out = self.farm("sweep", "status")[1]
        self.assertIn("v11:", out)
        self.assertLess(len(out.splitlines()), 15)
        fl = self.farm("sweep", "failures", "--class", "v11", "--group", "api.b")[1]
        self.assertNotIn("api.a", fl)

    def test_sweep_failures_listing(self):
        self.sweep_env(["dEQP-VK.api.a.CRASHX", "dEQP-VK.api.a.ok", "dEQP-VK.api.b.FAILX"], 1)
        self.attach("dev-a")
        self.farmd()
        out = self.farm("sweep", "failures", "--class", "v11")[1]
        self.assertIn("Crash    dEQP-VK.api.a.CRASHX", out)
        self.assertIn("Fail     dEQP-VK.api.b.FAILX", out)
        self.assertNotIn("a.ok", out)

    def test_sweep_pauses_on_health_gate_and_never_runs_over_a_queued_job(self):
        self.sweep_env(["dEQP-VK.api.a.one", "dEQP-VK.api.a.two", "dEQP-VK.api.a.three"], 2)
        self.attach("dev-a")
        L.write_json(self.d + "/state/fake-health-cmf.json", {"temp_c": 45.0})              # over the 43 C stop: cooling
        self.farmd()
        self.assertEqual(S.load("v11")["pos"], 0)
        self.assertFalse(os.path.exists(self.d + "/state/sweep/v11-chunks.jsonl"))
        os.unlink(self.d + "/state/fake-health-cmf.json")
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")        # a queued real job: it runs, the sweep does not start first
        self.farmd()
        self.assertEqual(L.find_job(jid)["parts"]["v11"]["state"], "done")
        self.assertEqual(S.load("v11")["pos"], 0)
        self.farmd()                                                                       # queue empty -> now the phone sweeps
        self.assertEqual(S.load("v11")["pos"], 3)

    def test_sweep_yields_to_a_submitted_job_mid_chunk(self):
        self.sweep_env([f"dEQP-VK.api.a.c{i}" for i in range(9)], 50, FAKE_SWEEP_SLOW="20")
        self.attach("dev-a")
        p = subprocess.Popen([sys.executable, HERE + "/farmd", "--fake", "--poll", "0.3"], env=self.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            time.sleep(3)                                                                  # sweeping: a slow chunk is in flight
            _, jid, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")
            t0 = time.time()
            self.assertEqual(self.farm("wait", jid, "--timeout", "30")[0], 0)
            self.assertLess(time.time() - t0, 20)                                          # did not wait for the 20 s chunk
            self.assertEqual(L.find_job(jid)["parts"]["v11"]["verdict"], "pass")
            self.assertEqual(S.load("v11")["pos"], 0)                                      # preempted chunk recorded nothing
        finally:
            p.terminate()
            p.wait(10)

    def test_util_and_prebuild_cache(self):
        self.attach("dev-a")
        _, j1, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")
        _, j2, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11")
        self.farmd()
        self.farmd()
        self.assertTrue(L.find_job(j1).get("so_sha256") == L.find_job(j2).get("so_sha256"))      # same commit: one build
        out = self.farm("util", "--since", "1d")[1]
        self.assertLess(len(out.splitlines()), 15)
        self.assertIn("dev-a", out)
        self.assertIn("busy", out)

    def test_smoke_log_and_only(self):
        self.attach("dev-a")
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11", "--only", "triangle,buf")
        self.assertEqual(L.find_job(jid)["only"], "triangle,buf")
        self.assertNotEqual(self.farm("submit", "HEAD", "--suite", "daily", "--only", "x")[0], 0)
        self.farmd()
        rc, out, _ = self.farm("log", jid, "--class", "v11")
        self.assertEqual(rc, 0)
        self.assertIn("only=triangle,buf", out)
        self.assertEqual(len(self.farm("log", jid, "--tail", "5")[1].splitlines()), 6)
        self.assertIn("fake line 250 only", self.farm("log", jid, "--grep", "line 250 only", "--tail", "3")[1])

    def test_build_cache_covers_t_binaries(self):
        """a daily's cached .so must come with its t_* binaries: a smoke job of the same commit finds them (or rebuilds), never 0/0 tests"""
        import importlib.machinery, importlib.util, threading, shutil
        ld = importlib.machinery.SourceFileLoader("farmd_mod", HERE + "/farmd")
        spec = importlib.util.spec_from_loader("farmd_mod", ld)
        fd = importlib.util.module_from_spec(spec)
        ld.exec_module(fd)
        f = object.__new__(fd.Farm)
        f.fake, f.devs, f.build_locks, f.bl, f.jl = False, {}, {}, threading.Lock(), threading.Lock()
        f.repo = f.main_tree = self.d + "/repo"
        f.missing_deps = lambda suite: ""
        f.borrow_deqp = lambda tree: None
        os.makedirs(self.d + "/state/artifacts", exist_ok=True)
        os.makedirs(self.d + "/state/work", exist_ok=True)
        os.makedirs(self.d + "/state/logs", exist_ok=True)
        builds = []

        def fake_sh(cmd, timeout=0, env=None, **kw):             # stands in for vk/build.sh: writes the .so and t_* next to it
            tree = os.path.dirname(os.path.dirname(cmd[-1]))
            out = os.path.join(tree, "vk", "out", "a64-glibc")
            os.makedirs(out, exist_ok=True)
            open(out + "/libvulkan_mali.so", "w").write("so-built")
            for t in ("t_vk_draw", "t_vk_compute"):
                open(out + "/" + t, "w").write("bin")
                os.chmod(out + "/" + t, 0o755)
            builds.append(tree)
            return 0, ""
        orig_sh, fd.L.sh = fd.L.sh, fake_sh
        try:
            _, j1, _ = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11")
            _, j2, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")
            for j in (j1, j2):
                os.makedirs(f.tree(j) + "/vk", exist_ok=True)       # worktree already "created"
                open(f.tree(j) + "/vk/build.sh", "w").write("")
            sha, _ = f.build(L.find_job(j1))
            self.assertEqual(len(builds), 1)
            sha2, _ = f.build(L.find_job(j2))
            self.assertEqual(sha, sha2)
            self.assertEqual(len(builds), 1, "cache hit expected")
            out2 = f.tree(j2) + "/vk/out/a64-glibc"
            self.assertTrue(os.access(out2 + "/t_vk_draw", os.X_OK) and os.access(out2 + "/t_vk_compute", os.X_OK))
            self.assertEqual(L.find_job(j2).get("build_cached"), j1)
            # archive from before the fix (no t_* stored): the smoke job must rebuild, not run with 0 tests
            shutil.rmtree(f.tests_dir(sha))
            _, j3, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11", "--rerun")
            os.makedirs(f.tree(j3) + "/vk", exist_ok=True)
            open(f.tree(j3) + "/vk/build.sh", "w").write("")
            f.build(L.find_job(j3))
            self.assertEqual(len(builds), 2, "missing t_* archive must trigger a rebuild")
            self.assertTrue(os.access(f.tree(j3) + "/vk/out/a64-glibc/t_vk_draw", os.X_OK))
        finally:
            fd.L.sh = orig_sh

    def test_env_validation(self):
        self.assertEqual(self.farm("submit", "HEAD", "--env", "A=b;rm")[0] != 0, True)


    # ---------- Steam menu suite (fake menubench, no phone)
    def fake_menubench(self, mode="ok", vc=22):
        root = self.d + "/MediaDeck"
        os.makedirs(root + "/scripts/quick", exist_ok=True)
        mb = root + "/scripts/quick/menubench"
        open(mb, "w").write(FAKE_MENUBENCH)
        os.chmod(mb, 0o755)
        open(root + "/mode", "w").write(mode)
        open(root + "/vc", "w").write(str(vc))
        self.env["MALI_FARM_MENUBENCH"] = mb
        return root

    def menu_rows(self):
        return ML.load_rows(ML.menu_path(self.d + "/state"))

    def test_menu_explicit_submit_rows_per_version_and_profile_and_digest(self):
        self.nosweep()
        root = self.fake_menubench("chunks")
        self.attach("dev-a")
        rc, jid, err = self.farm("submit", "HEAD", "--suite", "menu", "--need", "v11")
        self.assertEqual(rc, 0, err)
        self.farmd()
        self.assertEqual(self.farm("wait", jid)[1], "pass")
        j = L.find_job(jid)
        self.assertFalse(j.get("auto"))
        rows = self.menu_rows()
        self.assertEqual(sorted((r["phone"], r["version_code"], r["profile"]) for r in rows), [("dev-a", 22, "default"), ("dev-a", 22, "uncapped")])
        d = [r for r in rows if r["profile"] == "default"][0]
        self.assertEqual((d["valid"], d["selfcheck_ok"], d["reps_ok"], d["avg_fps"], d["median_fps"], d["low1_fps"], d["p99_ms"], d["panel_hz"]), (True, True, 3, 120.0, None, 60.0, 22.0, 60))
        calls = open(root + "/calls").read().splitlines()
        self.assertEqual(len(calls), 2)                                                                  # exit 75 once: resumed with the same command
        self.assertIn("dev-a --profile both --reps 3 --lockwait 30 held=-", calls[0])                    # menubench locks itself: farmd must not claim the lock
        out = self.farm("digest")[1]
        self.assertIn("menu  dev-a    vc 22: default avg 120.0", out)
        self.assertIn("uncapped avg 182.0 med 181.0", out)
        self.assertIn("@120 Hz", out)

    def test_menu_invalid_row_never_replaces_valid(self):
        self.nosweep()
        root = self.fake_menubench("ok")
        self.attach("dev-a")
        self.farm("submit", "HEAD", "--suite", "menu", "--need", "v11")
        self.farmd()
        good = {r["profile"]: r for r in self.menu_rows()}
        self.assertTrue(good["uncapped"]["valid"])
        time.sleep(1.1)                                                                                  # menubench stamps whole seconds: keep the jobs' rows apart
        open(root + "/mode", "w").write("selfcheck")                                                    # same version again: uncapped self-check fails
        rc, jid, _ = self.farm("submit", "HEAD", "--suite", "menu", "--need", "v11", "--rerun")
        self.farmd()
        self.assertEqual(self.farm("wait", jid)[1], "fail")                                              # the job says so ...
        now = {r["profile"]: r for r in self.menu_rows()}
        self.assertEqual(now["uncapped"]["avg_fps"], good["uncapped"]["avg_fps"])                        # ... but the valid row stays
        self.assertTrue(now["uncapped"]["valid"])
        self.assertEqual(now["default"]["job"], jid)                                                     # the valid default row was refreshed
        time.sleep(1.1)
        open(root + "/vc", "w").write("23")                                                              # a new version with no valid row: stored as invalid
        self.farm("submit", "HEAD", "--suite", "menu", "--need", "v11", "--rerun")
        self.farmd()
        inv = [r for r in self.menu_rows() if r["version_code"] == 23 and r["profile"] == "uncapped"][0]
        self.assertFalse(inv["valid"])
        self.assertFalse(inv["selfcheck_ok"])
        self.assertIn("no gpu-process", inv["invalid_reason"])
        self.assertIn("INVALID", self.farm("digest")[1])

    def test_menu_skipped_while_owner_uses_phone_then_runs(self):
        self.nosweep()
        root = self.fake_menubench("ok")
        self.attach("dev-a")
        os.makedirs(self.d + "/state/fake-menu")
        open(self.d + "/state/fake-menu/dev-a.inuse", "w").write("screen on, foreground com.android.chrome")
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "menu", "--need", "v11")
        self.farmd()
        self.assertEqual(L.find_job(jid)["parts"]["v11"]["state"], "pending")
        self.assertFalse(os.path.exists(root + "/calls"))                                                # menubench never started
        os.unlink(self.d + "/state/fake-menu/dev-a.inuse")
        self.farmd()
        self.assertEqual(L.find_job(jid)["parts"]["v11"]["state"], "done")

    def test_menu_busy_lock_keeps_pending(self):
        self.nosweep()
        self.fake_menubench("busy")
        self.attach("dev-a")
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "menu", "--need", "v11")
        self.farmd()
        self.assertEqual(L.find_job(jid)["parts"]["v11"]["state"], "pending")
        self.assertEqual(self.menu_rows(), [])

    def test_menu_auto_trigger_once_per_version_and_priority(self):
        self.nosweep()
        root = self.fake_menubench("ok")
        self.attach("dev-a")
        self.env.update(MALI_FARM_AUTO_MENU="1", MALI_FARM_BENCH_IDLE="0")
        self.farmd()
        menus = [j for j in L.list_jobs() if j["suite"] == "menu"]
        self.assertEqual(len(menus), 1)
        self.assertTrue(menus[0]["auto"] and menus[0]["pin"] == "dev-a")
        self.assertEqual(menus[0]["parts"]["v11"]["state"], "done")
        self.assertEqual(len(self.menu_rows()), 2)
        self.farmd()                                                                                     # same version: not again
        self.assertEqual(len([j for j in L.list_jobs() if j["suite"] == "menu"]), 1)
        open(self.d + "/state/fake-menu/dev-a.vc", "w").write("23") if os.path.isdir(self.d + "/state/fake-menu") else (os.makedirs(self.d + "/state/fake-menu"), open(self.d + "/state/fake-menu/dev-a.vc", "w").write("23"))
        open(root + "/vc", "w").write("23")
        self.farmd()                                                                                     # new installed version: one more run
        menus = [j for j in L.list_jobs() if j["suite"] == "menu"]
        self.assertEqual(len(menus), 2)
        self.assertEqual({r["version_code"] for r in self.menu_rows()}, {22, 23})
        self.farmd()
        self.assertEqual(len([j for j in L.list_jobs() if j["suite"] == "menu"]), 2)

    def test_menu_auto_trigger_not_while_owner_uses_phone(self):
        self.nosweep()
        self.fake_menubench("ok")
        self.attach("dev-a")
        os.makedirs(self.d + "/state/fake-menu")
        open(self.d + "/state/fake-menu/dev-a.inuse", "w").write("x")
        self.env.update(MALI_FARM_AUTO_MENU="1", MALI_FARM_BENCH_IDLE="0")
        self.farmd()
        self.assertEqual([j for j in L.list_jobs() if j["suite"] == "menu"], [])

    def test_explicit_job_runs_before_auto_menu(self):
        self.nosweep()
        self.fake_menubench("ok")
        L.new_job("main", "0" * 40, False, "menu", ["v11"], "", "x", auto=True, pin="dev-a")
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")
        order = [j["suite"] for j in BL.order_jobs(L.list_jobs())]
        self.assertEqual(order, ["smoke", "menu"])

    def test_menu_without_menubench_configured_is_an_error_with_a_hint(self):
        self.nosweep()
        self.env["MALI_FARM_MENUBENCH"] = ""
        self.attach("dev-a")
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "menu", "--need", "v11")
        self.farmd()
        self.assertEqual(self.farm("wait", jid)[1], "error")


    def test_gate_start_limit_hysteresis(self):
        th = dict(gate.DEFAULTS, temp_stop=50.0, temp_resume=38.0, temp_start=40.0)
        ok = {"level": 80, "temp_c": 45.0, "thermal": 0}
        self.assertEqual(gate.next_state("ok", ok, th)[0], "ok")                       # a running job is only stopped at temp_stop
        self.assertEqual(gate.next_state("ok", ok, th, starting=True)[0], "cooling")   # a new one does not start at 45 C
        self.assertEqual(gate.next_state("cooling", dict(ok, temp_c=39.0), th, starting=True)[0], "cooling")
        self.assertEqual(gate.next_state("cooling", dict(ok, temp_c=38.0), th, starting=True)[0], "ok")
        self.assertEqual(gate.next_state("ok", dict(ok, temp_c=50.0), th)[0], "cooling")

    def test_submit_idempotent_and_rerun(self):
        self.fixture_repo()                                     # clean checkout: a dirty developer tree would never be reused
        self.attach("dev-a")
        rc, jid, err = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")
        self.assertEqual(rc, 0, err)
        self.assertTrue(jid, err)
        self.farmd()
        self.assertEqual(L.job_verdict(L.find_job(jid)), "pass")
        rc, again, err = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11")
        self.assertEqual((rc, again), (0, jid))
        self.assertIn("already passed", err)
        for extra in (["--env", "FOO=1"], ["--rerun"]):                                  # other inputs / forced: a new job
            rc, other, err = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11", *extra)
            self.assertEqual(rc, 0, err)
            self.assertTrue(other, err)
            self.assertNotEqual(other, jid)
        rc, _, err = self.farm("submit", "HEAD", "--suite", "smoke", "--need", ",")
        self.assertNotEqual(rc, 0)
        self.assertIn("selects no class", err)
        self.attach("dev-a")
        rc, d1, err = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11")        # daily is never reused (judged against main's moving baseline)
        for _ in range(3):                                      # earlier jobs of this test run first
            self.farmd()
        self.assertEqual((rc, bool(d1)), (0, True), err)
        self.assertEqual(L.find_job(d1)["parts"]["v11"]["state"], "done")
        self.assertEqual(L.job_verdict(L.find_job(d1)), "pass")     # a real passed daily job, so reuse would be possible
        rc, d2, err = self.farm("submit", "HEAD", "--suite", "daily", "--need", "v11")
        self.assertEqual(rc, 0, err)
        self.assertTrue(d2, err)
        self.assertNotEqual(d2, d1)

    def test_find_passed_unit(self):
        j = {"id": "a", "commit": "c1", "suite": "daily", "env": "", "dirty": False, "parts": {"v11": {"state": "done", "verdict": "pass"}, "v13": {"state": "done", "verdict": "fail"}}}
        self.assertIsNone(L.find_passed([j], "c1", "daily", ["v11"], ""))               # the job also asked v13: a different request
        j = dict(j, parts={"v11": {"state": "done", "verdict": "pass"}})
        self.assertEqual(L.find_passed([j], "c1", "daily", ["v11"], "")["id"], "a")
        self.assertIsNone(L.find_passed([j], "c1", "daily", [], ""))
        self.assertIsNone(L.find_passed([j], "c1", "daily", ["v11", "v13"], ""))
        self.assertIsNone(L.find_passed([j], "c2", "daily", ["v11"], ""))
        self.assertIsNone(L.find_passed([dict(j, dirty=True)], "c1", "daily", ["v11"], ""))

    def test_submit_stale_base_and_merge_with_main(self):
        g = self.fixture_repo(12)                               # known ancestry: c0..c11, origin/main at the tip
        g("update-ref", "refs/remotes/origin/main", g("rev-parse", "HEAD"))
        for i in range(12, 22):
            g("commit", "-q", "--allow-empty", "-m", f"c{i}")
        g("update-ref", "refs/remotes/origin/main", "HEAD")
        g("checkout", "-q", "-b", "old", "HEAD~9")              # REF 9 commits behind origin/main (limit L.STALE_COMMITS)
        rc, _, err = self.farm("submit", "old", "--suite", "smoke", "--need", "v11")
        self.assertNotEqual(rc, 0)
        self.assertIn("behind origin/main", err)
        self.assertEqual(self.farm("submit", "old", "--suite", "smoke", "--need", "v11", "--allow-stale")[0], 0)
        rc, jid, err = self.farm("submit", "old", "--suite", "smoke", "--need", "v11", "--merge-with-main")
        self.assertEqual(rc, 0, err)
        j = L.find_job(jid)
        self.assertTrue(j["ref"].endswith("+main"))
        parents = g("rev-list", "--parents", "-n1", j["commit"]).split()
        self.assertEqual(len(parents), 3)                                               # a merge commit of REF and origin/main


class SweepBackoffUnit(unittest.TestCase):
    def test_backoff_1_to_30_min_and_alert_after_5(self):
        import importlib.machinery, importlib.util, types
        ld = importlib.machinery.SourceFileLoader("farmd_mod", os.path.join(os.path.dirname(os.path.abspath(__file__)), "farmd"))
        m = importlib.util.module_from_spec(importlib.util.spec_from_loader("farmd_mod", ld)); ld.exec_module(m)
        alerts = []
        m.alert = alerts.append; m.log = lambda *_: None
        f = types.SimpleNamespace(sweep_bad={}, cool={})
        waits = [m.Farm.sweep_backoff(f, "x", rc, False) for rc in (75, 125, 75, 125, 75, 75, 75, 75)]
        self.assertEqual(waits, [60, 120, 240, 480, 960, 1800, 1800, 1800])
        self.assertEqual(len(alerts), 1); self.assertIn("SWEEP-CHURN", alerts[0])
        self.assertEqual(m.Farm.sweep_backoff(f, "x", 0, False), 0)                 # healthy chunk resets
        self.assertEqual(m.Farm.sweep_backoff(f, "x", 0, True), 60)                 # rc 0 but session died fast, no cases


class StaleLockUnit(unittest.TestCase):
    def test_adb_server_holding_lock_is_recovered_after_10min(self):
        import farmlib as FL, tempfile, subprocess, fcntl
        d = tempfile.mkdtemp()
        path = d + "/qsafe-x.lock"
        dev = {"alias": "x", "lock": "x"}
        lk = FL.PhoneLock(dev); lk.path = path
        got = []
        FL.PhoneLock.on_stale = staticmethod(got.append)
        FL.PhoneLock._busy_since = {}
        # a child named "adb" that inherited the lock fd (what the leak looked like)
        fake = d + "/adb"
        os.symlink("/bin/sleep", fake)
        fd = open(path, "a"); fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        child = subprocess.Popen([fake, "30"], pass_fds=(fd.fileno(),))
        fd.close()
        try:
            self.assertEqual([c for _, c in FL.lock_holders(path)], ["adb"])
            t0 = time.time()
            self.assertFalse(lk.acquire(now=t0))                       # busy, clock starts
            self.assertFalse(lk.acquire(now=t0 + 300))                 # < 10 min: still busy, no alert
            self.assertEqual(got, [])
            self.assertTrue(lk.acquire(now=t0 + 700))                  # > 10 min, holder is adb only: recovered
            self.assertEqual(len(got), 1); self.assertIn("STALE-LOCK", got[0])
            lk.release()
        finally:
            child.kill(); child.wait()

    def test_python_children_do_not_inherit_the_lock(self):
        import farmlib as FL, tempfile, subprocess, fcntl
        d = tempfile.mkdtemp()
        lk = FL.PhoneLock({"alias": "y", "lock": "y"}); lk.path = d + "/l.lock"
        self.assertTrue(lk.acquire())
        r = subprocess.run(["ls", "/proc/self/fd"], capture_output=True, text=True)
        self.assertFalse(any(os.path.realpath(f"/proc/self/fd/{x}") == lk.path for x in []))
        p = subprocess.run(["sh", "-c", "ls -l /proc/self/fd"], capture_output=True, text=True).stdout
        self.assertNotIn("l.lock", p)
        lk.release()


class PublishUnit(unittest.TestCase):
    """bench publishing into a temp origin: own clone, no collision with ff-merging workers, results-only commits never trigger a bench"""
    def git(self, d, *a):
        r = subprocess.run(["git", "-C", d, "-c", "user.name=t", "-c", "user.email=t@t", *a], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.strip()

    def row(self, phone, drv, commit, variant="triangle-3k", bench="triangle", gpu=3.0, ver=None):
        return {"date": "2026-11-18", "bench": bench, "variant": variant, "phone": phone, "class": "v11", "driver": drv, "version": ver or (commit if drv == "ours" else "malideck p22"),
                "commit": commit[:9], "commit_full": commit, "job": "j", "ref": "main", "best_settings": "x",
                "presented": {"fps": 60, "low1": 50, "over50": 0, "cpu_ms": 1, "gpu_ms": gpu}, "offscreen": {"fps": 90, "cpu_ms": 1, "gpu_ms": gpu}}

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.origin = os.path.join(self.tmp, "origin.git")
        subprocess.run(["git", "init", "-q", "--bare", "-b", "main", self.origin], check=True)
        self.work = os.path.join(self.tmp, "work")
        subprocess.run(["git", "clone", "-q", self.origin, self.work], check=True, capture_output=True)
        os.makedirs(os.path.join(self.work, "bench"))
        open(os.path.join(self.work, "bench", "results.jsonl"), "w").close()
        open(os.path.join(self.work, "README"), "w").write("x")
        self.git(self.work, "add", "-A")
        self.git(self.work, "commit", "-qm", "seed")
        self.git(self.work, "push", "-q", "origin", "HEAD:main")
        self.state = os.path.join(self.tmp, "state")
        os.makedirs(os.path.join(self.state, "bench"))
        self.rep = [sys.executable, "-c", "import json;n=len(open('bench/results.jsonl').read().split());open('bench/RESULTS.md','w').write('table %d' % n)"]

    def put(self, *rows):
        with open(BL.results_path(self.state), "a") as f:
            f.write("".join(json.dumps(r) + "\n" for r in rows))

    def pub(self, **k):
        return BL.publish(self.state, self.origin, report_cmd=self.rep, log=lambda m: None, **k)

    def test_publish_commit_message_rows_and_idempotence(self):
        c1 = "a" * 40
        self.put(self.row("cmf", "ours", c1), self.row("cmf", "bridge", c1), self.row("cmf", "ours", c1, "cube-720p", "cube", gpu=0.2))    # last one is implausible
        r = self.pub()
        self.assertEqual((r["status"], r["n"]), ("pushed", 2), r)
        self.git(self.work, "pull", "-q", "origin", "main")
        self.assertEqual(self.git(self.work, "log", "-1", "--format=%s"), "bench: cmf rows for " + c1[:9])
        self.assertEqual(len(open(os.path.join(self.work, "bench", "results.jsonl")).read().splitlines()), 2)
        self.assertTrue(os.path.exists(os.path.join(self.work, "bench", "RESULTS.md")))
        self.assertEqual(self.pub()["status"], "nothing")                                            # idempotent: no empty commit
        c2 = "b" * 40                                                                                # a newer version keeps the older rows (one block per version)
        self.put(self.row("cmf", "ours", c2), self.row("poco-x4", "ours", c2))
        self.assertEqual(self.pub()["status"], "pushed")
        self.git(self.work, "pull", "-q", "origin", "main")
        keys = {(r["phone"], r["driver"], r["commit"]) for r in map(json.loads, open(os.path.join(self.work, "bench", "results.jsonl")))}
        self.assertEqual(len(keys), 4)
        self.assertIn(("cmf", "ours", c1[:9]), keys)
        self.assertEqual(self.git(self.work, "log", "-1", "--format=%s"), "bench: cmf,poco-x4 rows for " + c2[:9])

    def test_publish_regenerates_charts_and_a_failing_plot_never_blocks(self):
        gen = [sys.executable, "-c", "import os;os.makedirs('bench/charts',exist_ok=True);open('bench/charts/x.svg','w').write('svg');open('README.md','a').write('perf')"]
        self.put(self.row("cmf", "ours", "d" * 40))
        self.assertEqual(self.pub(perf_cmd=gen)["status"], "pushed")
        self.git(self.work, "pull", "-q", "origin", "main")
        self.assertTrue(os.path.exists(os.path.join(self.work, "bench", "charts", "x.svg")))      # same commit as the rows
        self.assertEqual(self.git(self.work, "show", "--stat", "--format=", "HEAD").count("\n"), 4)
        self.put(self.row("cmf", "ours", "e" * 40))
        for bad in ([sys.executable, "-c", "raise SystemExit(3)"], ["/nonexistent/plotter"]):
            self.put(self.row("cmf", "ours", "f" * 39 + str(len(bad))))
            self.assertEqual(self.pub(perf_cmd=bad)["status"], "pushed")
        half = [sys.executable, "-c", "import os;os.makedirs('bench/charts',exist_ok=True);open('bench/charts/half.svg','w').write('x');raise SystemExit(1)"]
        self.put(self.row("cmf", "ours", "9" * 40))
        self.assertEqual(self.pub(perf_cmd=half)["status"], "pushed")                            # a half-written report is dropped, not committed
        self.git(self.work, "pull", "-q", "origin", "main")
        self.assertFalse(os.path.exists(os.path.join(self.work, "bench", "charts", "half.svg")))

    def test_rejected_push_is_retried_and_other_checkouts_untouched(self):
        self.put(self.row("cmf", "ours", "c" * 40))
        # a worker ff-merges into origin right between our fetch and our push (pre-push hook of the farm's clone)
        clone = os.path.join(self.state, "bench", "publish-clone")
        subprocess.run(["git", "clone", "-q", self.origin, clone], check=True, capture_output=True)
        flag = os.path.join(self.tmp, "flag")
        hook = os.path.join(clone, ".git", "hooks", "pre-push")
        open(hook, "w").write(f"#!/bin/sh\n[ -e {flag} ] && exit 0\ntouch {flag}\ncd {self.work} && echo more >> README && git -c user.name=t -c user.email=t@t commit -qam worker && git push -q origin HEAD:main\nexit 0\n")
        os.chmod(hook, 0o755)
        before = open(os.path.join(self.work, "README")).read()
        r = self.pub()
        self.assertEqual(r["status"], "pushed", r)
        self.assertTrue(os.path.exists(flag))                                                        # the first push really was rejected
        subs = self.git(self.origin, "log", "--format=%s", "main").splitlines()
        self.assertEqual(subs[0], "bench: cmf rows for " + "c" * 9)
        self.assertIn("worker", subs)                                                                # the worker's commit survived (rebased onto, never overwritten)
        self.assertNotEqual(before, "")
        self.assertEqual(self.git(self.work, "status", "--porcelain"), "")                          # the workers' checkout was never touched

    def test_results_only_commit_does_not_trigger_a_bench(self):
        self.assertEqual(BL.plan_trigger("a", "b", ["bench/results.jsonl", "bench/RESULTS.md", "bench/best.json"], ["v11", "v13"]), ("b", []))
        self.assertFalse(BL.touches_driver(["bench/results.jsonl", "bench/RESULTS.md"]))

    def test_stale_best_rules(self):
        import datetime, importlib.machinery, importlib.util
        ld = importlib.machinery.SourceFileLoader("bench2mod", os.path.join(os.path.dirname(HERE), "bench2"))
        m = importlib.util.module_from_spec(importlib.util.spec_from_loader("bench2mod", ld))
        ld.exec_module(m)
        d = datetime.date(2026, 11, 20)
        fresh = {"cmf": {"ours": {"cube-720p": {"date": "2026-11-18", "sig": m.sweep_sig("cube-720p", "ours")}}}}
        self.assertIsNone(m.best_stale(fresh, "cmf", "ours", "cube-720p", d))
        self.assertIn("days old", m.best_stale(fresh, "cmf", "ours", "cube-720p", datetime.date(2026, 11, 26)))     # > 7 days
        self.assertIn("no stored best", m.best_stale(fresh, "cmf", "ours", "triangle-3k", d))
        fresh["cmf"]["ours"]["cube-720p"]["sig"] = "000000000000"                                      # a new knob/candidate changes the fingerprint
        self.assertIn("new knob", m.best_stale(fresh, "cmf", "ours", "cube-720p", d))
        old = m.OURS_ENV[:]
        m.OURS_ENV.append(["MALI_NEW_KNOB=1"])
        self.assertNotEqual(m.sweep_sig("cube-720p", "ours"), "x")
        sig_new = m.sweep_sig("cube-720p", "ours")
        m.OURS_ENV[:] = old
        self.assertNotEqual(sig_new, m.sweep_sig("cube-720p", "ours"))
        cfg = m.plan("cube-720p", "ours", "final", {"pres": {"env": ["MALI_RING=1"], "mode": "mailbox"}, "off": {"env": [], "off": True}})
        self.assertEqual(cfg[0]["env"], [])                                                            # cube never replays a non-base env for ours


class GateTemp(unittest.TestCase):
    """Poco X4: dumpsys battery 35.2 C, thermalservice 'Cached temperatures' BATTERY frozen at 33.1 (stale), HAL current BATTERY 35.2 / SKIN 37.6"""
    DUMP = ("  level: 97\n  temperature: 352\nThermal Status: 0\nCached temperatures:\n"
            "\tTemperature{mValue=33.1, mType=2, mName=BATTERY, mStatus=0}\n\tTemperature{mValue=38.975, mType=3, mName=SKIN, mStatus=0}\n"
            "Current temperatures from HAL:\n\tTemperature{mValue=35.2, mType=2, mName=BATTERY, mStatus=0}\n"
            "\tTemperature{mValue=%s, mType=3, mName=SKIN, mStatus=0}\n")

    def test_highest_of_battery_and_skin(self):
        self.assertEqual(gate.parse(self.DUMP % "37.6")["temp_c"], 37.6)
        self.assertEqual(gate.parse(self.DUMP % "43.4")["temp_c"], 43.4)       # skin hotter than the battery: it gates
        th = dict(gate.DEFAULTS)
        self.assertEqual(gate.next_state("ok", gate.parse(self.DUMP % "43.0"), th)[0], "cooling")
        self.assertEqual(gate.next_state("ok", gate.parse(self.DUMP % "42.9"), th)[0], "ok")        # worked right up to 43

    def test_stale_cache_is_ignored(self):
        t = "  temperature: 300\nCached temperatures:\n\tTemperature{mValue=47.7, mType=2, mName=BATTERY, mStatus=0}\n"
        self.assertEqual(gate.parse(t)["temp_c"], 30.0)
        self.assertEqual(gate.parse("  temperature: 470\nCurrent temperatures from HAL:\n\tTemperature{mValue=33.1, mType=2, mName=BATTERY, mStatus=0}\n")["temp_c"], 47.0)

    def test_die_sensors_not_used(self):
        t = "  temperature: 350\nCurrent temperatures from HAL:\n\tTemperature{mValue=70.0, mType=1, mName=GPU, mStatus=0}\n"
        self.assertEqual(gate.parse(t)["temp_c"], 35.0)

    def test_device_address(self):
        d = tempfile.mkdtemp()
        open(d + "/d.toml", "w").write('[[device]]\nalias = "poco-x4"\nserial = "SER1"\nlock = "poco-x4"\naddress = "192.0.2.5:5555"\n[[device]]\nalias = "cmf"\nserial = "SER2"\n')
        os.environ["MALI_FARM_DEVICES"] = d + "/d.toml"
        try:
            self.assertEqual(gate.device_address("poco-x4"), "192.0.2.5:5555")
            self.assertEqual(gate.device_address("SER1"), "192.0.2.5:5555")
            self.assertIsNone(gate.device_address("cmf"))
        finally:
            del os.environ["MALI_FARM_DEVICES"]


class MenuUnit(unittest.TestCase):
    PW_ON, PW_OFF = "  mWakefulness=Awake\n", "  mWakefulness=Asleep\n"
    FG = "  topResumedActivity=ActivityRecord{1a2b u0 %s/.MainActivity t42}\n"

    def test_owner_in_use_detection(self):
        pkg = "com.ludashi.benchmark"
        self.assertEqual(ML.owner_in_use(self.PW_OFF, self.FG % "com.android.chrome", pkg)[0], False)         # screen off: free whatever is resumed
        self.assertEqual(ML.owner_in_use(self.PW_ON, self.FG % pkg, pkg)[0], False)                            # MediaDeck in front: ours
        use, why = ML.owner_in_use(self.PW_ON, self.FG % "com.android.chrome", pkg)
        self.assertTrue(use)
        self.assertIn("com.android.chrome", why)
        self.assertTrue(ML.owner_in_use(self.PW_ON, self.FG % "com.nothing.launcher", pkg)[0])                 # home screen = owner
        self.assertTrue(ML.owner_in_use("", self.FG % pkg, pkg)[0])                                             # unreadable: never run blind
        self.assertTrue(ML.owner_in_use(self.PW_ON, "", pkg)[0])
        self.assertEqual(ML.foreground_pkg("  mResumedActivity: ActivityRecord{9 u0 org.x.app/org.x.app.Main t1}"), "org.x.app")

    def test_foreground_launch_opt_in(self):
        pkg = "com.ludashi.benchmark"
        home = self.FG % "com.mi.android.globallauncher"
        self.assertTrue(ML.owner_in_use(self.PW_ON, home, pkg)[0])                                             # lent phone: launcher = owner
        use, why = ML.owner_in_use(self.PW_ON, home, pkg, launch=True)
        self.assertFalse(use)
        self.assertTrue(why.startswith(ML.LAUNCHER_ONLY))
        self.assertTrue(ML.owner_in_use(self.PW_ON, self.FG % "com.android.chrome", pkg, launch=True)[0])      # another app is still the owner
        self.assertTrue(ML.owner_in_use("", home, pkg, launch=True)[0])                                        # unreadable: never run blind
        self.assertTrue(ML.owner_in_use(self.PW_ON, self.FG % "com.example.home", pkg, launch=True)[0])        # an app named *.home is not the launcher
        self.assertFalse(ML.owner_in_use(self.PW_ON, self.FG % "com.miui.home", pkg, launch=True)[0])

    def test_guest_flag_and_start_limit(self):
        devs = {"x4": {"class": "v10", "route": "shell", "guest": True}, "old": {"class": "v10", "route": "shell"}}
        st = {a: {"status": "up"} for a in devs}
        self.assertEqual(BL.available(devs, st, set()), ["x4"])
        import importlib.machinery, importlib.util
        ld = importlib.machinery.SourceFileLoader("bench2", os.path.join(os.path.dirname(HERE), "bench2"))
        sp = importlib.util.spec_from_loader("bench2", ld)
        b2 = importlib.util.module_from_spec(sp)
        ld.exec_module(b2)
        self.assertEqual(b2.start_blocked({"temp_c": 40.5}, {"temp_start": 40.0}) != "", True)
        self.assertEqual(b2.start_blocked({"temp_c": 39.0}, {"temp_start": 40.0}), "")
        self.assertEqual(b2.start_blocked({"temp_c": 45.0}, {"temp_start": None}), "")

    def test_trigger_rule(self):
        self.assertTrue(ML.should_queue(23, 22, False))
        self.assertTrue(ML.should_queue(22, None, False))
        self.assertFalse(ML.should_queue(22, 22, False))
        self.assertFalse(ML.should_queue(None, 22, False))
        self.assertFalse(ML.should_queue(23, 22, True))

    def rec(self, p, vc=22, ok=True, ts="2026-11-18T10:00:00+01:00", fps=100.0, chk=()):
        return {"ts": ts, "alias": "cmf", "profile": p, "version_code": vc, "app_version": "v%d" % vc, "ok": ok and not chk, "selfcheck": list(chk), "status": "ok",
                "panel_hz": 120, "render": {"avg_fps": fps, "median_fps": fps, "low1_fps": 40.0, "p99_ms": 20.0}}

    def test_aggregate_medians_validity_and_since(self):
        recs = [self.rec("default", fps=100), self.rec("default", fps=120), self.rec("default", fps=300), self.rec("default", chk=["x"]),
                self.rec("uncapped", chk=["no gpu"]), self.rec("uncapped", chk=["no gpu"]), self.rec("default", vc=21, ts="2026-11-17T10:00:00+01:00")]
        rows = ML.aggregate(recs, "cmf", "v11", "J", since="2026-11-18T09:00:00+01:00")
        self.assertEqual([(r["version_code"], r["profile"], r["valid"]) for r in rows], [(22, "default", True), (22, "uncapped", False)])
        d = rows[0]
        self.assertEqual((d["avg_fps"], d["reps_ok"], d["reps_failed"], d["selfcheck_ok"]), (120.0, 3, 1, False))
        self.assertEqual(rows[1]["avg_fps"], None)
        self.assertEqual(rows[1]["invalid_reason"], "no gpu")
        self.assertEqual(len(ML.aggregate(recs, "cmf", "v11", "J")), 3)                                       # no since: the old version is included
        self.assertEqual(ML.aggregate(recs, "other", "v11", "J"), [])

    def test_merge_rows_invalid_keeps_valid(self):
        ok = {"phone": "cmf", "version_code": 22, "profile": "default", "valid": True, "avg_fps": 1}
        bad = dict(ok, valid=False, avg_fps=None)
        self.assertEqual(ML.merge_rows([ok], [bad]), [ok])
        self.assertEqual(ML.merge_rows([bad], [ok]), [ok])
        self.assertEqual(ML.merge_rows([ok], [dict(ok, avg_fps=2)])[0]["avg_fps"], 2)
        self.assertEqual(len(ML.merge_rows([ok], [dict(bad, version_code=23)])), 2)

    def test_results_md_section_newest_version_first(self):
        def r(ph, vc, p, d):
            return {"bench": "menu", "phone": ph, "class": "v11", "version_code": vc, "app_version": "1.0.0-p%d" % vc, "profile": p, "valid": True, "selfcheck_ok": True,
                    "avg_fps": 99.0, "median_fps": 98.0, "low1_fps": 50.0, "p99_ms": 12.345, "panel_hz": 120, "reps_ok": 3, "reps_failed": 0, "date": d}
        rows = [r("cmf", 22, "default", "2026-11-17"), r("cmf", 22, "uncapped", "2026-11-17"), r("cmf", 23, "default", "2026-11-18"), dict(r("poco-x8", 23, "uncapped", "2026-11-18"), valid=False, selfcheck_ok=False, invalid_reason="no gpu")]
        md = "\n".join(ML.report_lines(rows))
        self.assertIn("## Steam menu", md)
        self.assertLess(md.index("### MediaDeck vc 23"), md.index("### MediaDeck vc 22"))
        self.assertIn("| cmf | v11 | default | 99.0 | 98.0 | 50.0 | 12.35 | 120 | 3/0 | passed | 2026-11-18 |", md)
        self.assertIn("INVALID: no gpu", md)
        self.assertIn("No menu run yet", "\n".join(ML.report_lines([])))

    def test_bench2_report_has_steam_menu_section(self):
        import importlib.machinery, importlib.util
        tmp = tempfile.mkdtemp()
        ld = importlib.machinery.SourceFileLoader("bench2menu", os.path.join(os.path.dirname(HERE), "bench2"))
        m = importlib.util.module_from_spec(importlib.util.spec_from_loader("bench2menu", ld))
        ld.exec_module(m)
        m.JSONL, m.MD, m.PENDING, m.MENU = tmp + "/r.jsonl", tmp + "/R.md", tmp + "/p.json", tmp + "/menu.jsonl"
        with open(m.MENU, "w") as f:
            f.write(json.dumps({"bench": "menu", "phone": "cmf", "class": "v11", "version_code": 22, "app_version": "p22", "profile": "default", "valid": True, "avg_fps": 99.0, "date": "2026-11-18"}) + "\n")
        m.report()
        md = open(m.MD).read()
        self.assertIn("## triangle (CPU-heavy)", md)
        self.assertIn("## Steam menu", md)
        self.assertLess(md.index("## cube (GPU-heavy)"), md.index("## Steam menu"))

    def test_publish_menu_rows_with_their_own_block_and_message(self):
        tmp = tempfile.mkdtemp()
        origin = tmp + "/origin.git"
        subprocess.run(["git", "init", "-q", "--bare", "-b", "main", origin], check=True)
        work = tmp + "/work"
        subprocess.run(["git", "clone", "-q", origin, work], check=True, capture_output=True)
        g = lambda *a: subprocess.run(["git", "-C", work, "-c", "user.name=t", "-c", "user.email=t@t", *a], check=True, capture_output=True, text=True).stdout.strip()
        open(work + "/README", "w").write("x")
        g("add", "-A"); g("commit", "-qm", "seed"); g("push", "-q", "origin", "HEAD:main")
        state = tmp + "/state"
        row = {"bench": "menu", "phone": "cmf", "class": "v11", "version_code": 22, "app_version": "p22", "profile": "default", "valid": True, "avg_fps": 99.0, "date": "2026-11-18"}
        ML.append_rows(state, [row, dict(row, profile="uncapped")])
        rep = [sys.executable, "-c", "import json;n=len(open('bench/menu.jsonl').read().split('\\n'));open('bench/RESULTS.md','w').write('t %d' % n)"]
        r = BL.publish(state, origin, report_cmd=rep, log=lambda m: None)
        self.assertEqual((r["status"], r["n"]), ("pushed", 2), r)
        g("pull", "-q", "origin", "main")
        self.assertEqual(g("log", "-1", "--format=%s"), "bench: Steam menu rows cmf for MediaDeck vc22")
        self.assertEqual(len(ML.load_rows(work + "/bench/menu.jsonl")), 2)
        self.assertFalse(os.path.exists(work + "/bench/results.jsonl"))
        self.assertEqual(BL.publish(state, origin, report_cmd=rep, log=lambda m: None)["status"], "nothing")
        ML.append_rows(state, [dict(row, valid=False, avg_fps=None)])                                          # an invalid re-run: nothing new to publish
        self.assertEqual(BL.publish(state, origin, report_cmd=rep, log=lambda m: None)["status"], "nothing")


class SmokeRoute(unittest.TestCase):
    """guest smoke -> adb-shell fallback when the app session cannot be READY (locked phone): route choice, log header, harness"""
    def setUp(self):
        self.adb_rc = 0
        self.orig_rw = L.run_watched                                   # farmd_sr shares this farmlib: restore after each test

    def tearDown(self):
        L.run_watched = self.orig_rw

    def farm(self, fallback=True, dumpsys="mWakefulness=Dozing\nisKeyguardShowing=true"):
        import importlib.machinery, importlib.util, threading
        ld = importlib.machinery.SourceFileLoader("farmd_sr", HERE + "/farmd")
        spec = importlib.util.spec_from_loader("farmd_sr", ld)
        self.fd = importlib.util.module_from_spec(spec)
        ld.exec_module(self.fd)
        f = object.__new__(self.fd.Farm)
        self.dev = {"alias": "poco-x8", "class": "v13", "serial": "SER8", "lock": "poco"}
        if fallback:
            self.dev["smoke_fallback"] = "shell"
        f.fake, f.devs, f.tid = False, {"poco-x8": self.dev}, {}
        self.calls = []
        f.tree = lambda jid: "/nonexistent-tree"
        f.harness = lambda tree: self.calls.append("harness")
        f.alive = lambda dev: True
        f.adb = lambda dev, *a, **k: (self.adb_rc, dumpsys) if "dumpsys" in a[-1] else (0, "")
        f.verify_so = lambda job, dev, path, key="so_sha256": (True, job.get(key))
        f.run_shellroute = lambda job, dev, mode, rid, extra, tmo, nodim=False: (self.calls.append(("shell", nodim)) or (0, SMOKE_OUT))
        f.keep_log = lambda job, dev, part, out, rc: setattr(self, "kept", out)
        return f

    JOB = {"id": "J1", "env": "", "suite": "smoke", "so_sha256": "g" * 64, "so_sha256_bionic": "b" * 64, "parts": {"v13": {}}}

    def test_locked_phone_uses_shell_route_and_logs_it(self):
        f = self.farm()
        self.fd.L.run_watched = lambda *a, **k: self.fail("guest route must not be tried on a locked phone")
        r = f.suite_smoke(dict(self.JOB), self.dev)
        self.assertEqual(r["verdict"], "pass", r)
        self.assertEqual(self.calls, [("shell", True)])                  # shell route, screen untouched
        first = self.kept.splitlines()[0]
        self.assertIn("route=shell-fallback", first)
        self.assertIn("serial=SER8", first)
        self.assertIn("so_sha256=" + "b" * 64, first)                      # the bionic build that actually ran
        self.assertIn("Dozing", first)
        self.assertIn("skipped=", first)
        self.assertIn("route=shell-fallback", r["notes"])
        self.assertIn("gamescope", r["notes"])

    def test_dozing_with_nonzero_grep_rc_still_counts_as_locked(self):
        """the keyguard grep finds nothing on this phone (rc 1) while mWakefulness=Dozing was printed: must still skip the guest route"""
        f = self.farm(dumpsys="mWakefulness=Dozing\n")
        self.adb_rc = 1
        self.fd.L.run_watched = lambda *a, **k: self.fail("guest route must not be tried")
        self.assertEqual(f.suite_smoke(dict(self.JOB), self.dev)["verdict"], "pass")
        self.assertIn("Dozing", self.kept.splitlines()[0])

    def test_session_not_ready_with_awake_screen_falls_back_after_guest(self):
        f = self.farm(dumpsys="mWakefulness=Awake")
        self.fd.L.run_watched = lambda *a, **k: (74, "NO RESULT: qdrv wrote no poco valguest result\nDRV poco FAIL session not READY")
        r = f.suite_smoke(dict(self.JOB), self.dev)
        self.assertEqual(r["verdict"], "pass", r)
        self.assertIn("route=shell-fallback", self.kept.splitlines()[0])
        self.assertIn("session not READY", self.kept.splitlines()[0])

    def test_without_fallback_a_missing_session_is_an_error_not_a_pass(self):
        f = self.farm(fallback=False)
        self.fd.L.run_watched = lambda *a, **k: (74, "NO RESULT: qdrv wrote no poco valguest result")
        r = f.suite_smoke(dict(self.JOB), self.dev)
        self.assertEqual(r["verdict"], "error", r)
        self.assertEqual([c for c in self.calls if c != "harness"], [])

    def test_awake_session_ready_uses_guest_and_logs_it(self):
        f = self.farm(dumpsys="mWakefulness=Awake")
        self.fd.L.run_watched = lambda *a, **k: (0, SMOKE_OUT)
        r = f.suite_smoke(dict(self.JOB), self.dev)
        self.assertEqual(r["verdict"], "pass", r)
        self.assertIn("route=guest", self.kept.splitlines()[0])
        self.assertIn("so_sha256=" + "g" * 64, self.kept.splitlines()[0])
        self.assertNotIn(("shell", True), self.calls)

    def test_shell_route_failing_test_is_a_fail(self):
        f = self.farm()
        bad = SMOKE_OUT.replace('"test":"t_vk_draw","rc":0', '"test":"t_vk_draw","rc":1').replace('"ok":true', '"ok":false')
        f.run_shellroute = lambda job, dev, mode, rid, extra, tmo, nodim=False: (0, bad)
        self.assertEqual(f.suite_smoke(dict(self.JOB), self.dev)["verdict"], "fail")

    def test_job_built_before_the_fallback_gets_its_bionic_build_on_demand(self):
        f = self.farm()
        f.tree = lambda jid: tempfile.gettempdir()
        built = []
        f.build_bionic = lambda job, tree: (built.append(job["id"]) or "c" * 64, "")
        seen = []
        f.run_shellroute = lambda job, dev, mode, rid, extra, tmo, nodim=False: (seen.append(job["so_sha256_bionic"]) or (0, SMOKE_OUT))
        job = dict(self.JOB)
        del job["so_sha256_bionic"]
        r = f.suite_smoke(job, self.dev)
        self.assertEqual((r["verdict"], built, seen), ("pass", ["J1"], ["c" * 64]))
        self.assertIn("so_sha256=" + "c" * 64, self.kept.splitlines()[0])

    def test_pure_helpers(self):
        import smokeroute as SR
        self.assertEqual(SR.blocked_reason("mWakefulness=Awake\nisKeyguardShowing=false"), "")
        self.assertIn("Dozing", SR.blocked_reason("mWakefulness=Dozing"))
        self.assertEqual(SR.blocked_reason("mWakefulness=Awake\nisKeyguardShowing=true"), "keyguard showing")
        self.assertEqual(SR.blocked_reason(""), "")
        self.assertTrue(SR.guest_unusable(74, "") and SR.guest_unusable(1, "DRV poco FAIL session not READY") and not SR.guest_unusable(0, "{}"))

    def test_harness_replaces_valrun_without_the_this_run_fix(self):
        """a job tree whose valrun.sh has the SO hook but not 4f24ee4's -newer fix read another phone's result (v13 'pass' with G615 lines)"""
        f = self.farm()
        f.harness = self.fd.Farm.harness.__get__(f)
        tree = tempfile.mkdtemp()
        os.makedirs(tree + "/vk/tests")
        open(tree + "/vk/tests/valrun.sh", "w").write("VAL_SO_SHA256 old: ls -t | head -1\n")
        f.harness(tree)
        self.assertIn('-newer "$M"', open(tree + "/vk/tests/valrun.sh").read())


class HoldTests(unittest.TestCase):
    """farm hold: server-side FIFO queue of agent holds, fair with jobs, lease + heartbeat, same flock underneath"""
    setUp, attach, farm = T.setUp, T.attach, T.farm

    def daemon(self):
        self.env.update(MALI_FARM_HB_STALE="3", MALI_FARM_HB_EVERY="0.3")
        os.environ.pop("MALI_FARM_SWEEP_LIST", None)
        self.env.pop("MALI_FARM_SWEEP_LIST", None)
        os.makedirs(self.d + "/state/sweep", exist_ok=True)
        open(self.d + "/state/sweep/off", "w").close()                       # the fake idle sweep would spin on the phone
        self.p = subprocess.Popen([sys.executable, HERE + "/farmd", "--fake", "--poll", "0.3"], env=self.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.addCleanup(lambda: (self.p.terminate(), self.p.wait(10)))

    def hold(self, *a, timeout=60):
        r = subprocess.run([sys.executable, HERE + "/farm", "hold", *a], capture_output=True, text=True, env=self.env, timeout=timeout)
        return r.returncode, r.stdout.strip(), r.stderr

    def test_hold_runs_command_under_the_flock_env_and_restores(self):
        self.attach("dev-a")
        self.daemon()
        rc, out, err = self.hold("dev-a", "--max", "1", "--", "sh", "-c", "echo $QSAFE_HELD $ANDROID_SERIAL $FARM_HOLD_DEVICE; exit 7")
        self.assertEqual(rc, 7, err)                                       # the command's own exit code
        self.assertEqual(out, "cmf SERIAL-A dev-a")
        self.assertIn("restored clean", err)
        h = H.list_holds()[-1]
        self.assertEqual((h["state"], h["reason"], h["clean"]), ("done", "released", True))
        lk = L.PhoneLock(self.devs()["dev-a"], fake=True)
        t0 = time.time()
        while not lk.acquire() and time.time() - t0 < 10:                              # farmd unlocks right after it writes `done`
            time.sleep(0.1)
        self.assertIsNotNone(lk.fd)                                                    # the lock is free again
        lk.release()

    def devs(self):
        return L.load_devices()[1]

    def test_hold_blocks_the_flock_for_old_scripts_and_wins_over_a_later_priority_job(self):
        self.attach("dev-a")
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11", "--prio", "5")
        h = H.new_hold("dev-a", 60, prio=0)                                  # both queued before the daemon exists: the order is the scheduler's alone
        self.daemon()
        t0 = time.time()
        while time.time() - t0 < 20 and L.read_json(H.path(h["id"]))["state"] != "active":
            H.touch(h["id"], "hb")
            time.sleep(0.2)
        self.assertEqual(L.read_json(H.path(h["id"]))["state"], "active")
        lk = L.PhoneLock(self.devs()["dev-a"], fake=True)
        self.assertFalse(lk.acquire())                                       # an old flock user sees the phone busy
        for _ in range(8):                                                   # the job stays pending the whole time the hold owns the phone
            H.touch(h["id"], "hb")
            self.assertEqual(L.job_verdict(L.find_job(jid)), "pending")
            time.sleep(0.25)
        H.touch(h["id"], "rel")
        self.assertEqual(self.farm("wait", jid, "--timeout", "30")[1], "pass")        # then the job runs
        self.assertEqual(L.read_json(H.path(h["id"]))["state"], "done")

    def test_job_with_better_priority_goes_before_a_queued_hold(self):
        self.attach("dev-a")
        h = H.new_hold("dev-a", 60, prio=10)
        _, jid, _ = self.farm("submit", "HEAD", "--suite", "smoke", "--need", "v11", "--prio", "0")
        self.assertLess(H.key({"prio": 0, "seq": 2}), H.key(h))
        self.assertLess(H.key({"prio": 0, "seq": 1}), H.key({"prio": 0, "seq": 2}))     # FIFO at equal priority
        self.assertLess(H.key({"prio": 0, "seq": 5}), H.key({"prio": 0, "auto": True, "seq": 1}))
        self.daemon()
        t0 = time.time()
        while time.time() - t0 < 30 and L.job_verdict(L.find_job(jid)) == "pending":
            H.touch(h["id"], "hb")                                                    # a live caller waiting in the queue
            time.sleep(0.2)
        self.assertEqual(L.job_verdict(L.find_job(jid)), "pass")
        t0 = time.time()
        while time.time() - t0 < 20 and L.read_json(H.path(h["id"]))["state"] != "active":
            H.touch(h["id"], "hb")
            time.sleep(0.2)
        self.assertEqual(L.read_json(H.path(h["id"]))["state"], "active")             # the hold got its turn after the job
        H.touch(h["id"], "rel")

    def test_lease_dies_with_its_watched_pid_and_max_time_expires(self):
        self.attach("dev-a")
        self.daemon()
        sleeper = subprocess.Popen(["sleep", "2"])
        rc, out, err = self.hold("dev-a", "--max", "1", "--lease", "--watch-pid", str(sleeper.pid))
        self.assertEqual(rc, 0, err)
        info = json.loads(out)
        self.assertEqual(L.read_json(H.path(info["hold"]))["state"], "active")
        sleeper.wait()                                                       # reaped: no zombie keeping /proc/<pid> alive
        t0 = time.time()
        while time.time() - t0 < 30 and L.read_json(H.path(info["hold"]))["state"] != "done":
            time.sleep(0.3)
        h = L.read_json(H.path(info["hold"]))
        self.assertEqual((h["state"], h.get("reason")), ("done", "caller lost (no heartbeat)"), h)
        # max time: a 1 s hold with a command that sleeps longer is killed (rc 124)
        rc, out, err = self.hold("dev-a", "--max", "0.02", "--", "sleep", "30")
        self.assertEqual(rc, 124, err)

    def test_dead_caller_while_queued_and_release_cancel(self):
        self.attach("dev-a")
        h = H.new_hold("dev-a", 60)
        os.utime(H.path(h["id"], "hb"), (0, 0))                             # a caller that died in the queue
        self.daemon()
        t0 = time.time()
        while time.time() - t0 < 20 and L.read_json(H.path(h["id"]))["state"] != "done":
            time.sleep(0.2)
        self.assertIn("caller lost", L.read_json(H.path(h["id"]))["reason"])

    def test_end_reason(self):
        h = {"deadline": 100}
        self.assertEqual(H.end_reason(h, 1, False, now=50), "")
        self.assertEqual(H.end_reason(h, 1, True, now=50), "released")
        self.assertEqual(H.end_reason(h, 1, False, now=101), "max time reached")
        self.assertIn("caller lost", H.end_reason(h, H.HB_STALE + 1, False, now=50))
        self.assertEqual(H.end_reason(h, 1, False, now=50, disabled=True), "disabled by owner")


class Bench2Rank(unittest.TestCase):
    """bench2's presented-config winner: by fps (a FIFO run flat at the display cap must not beat mailbox 450 fps on 1% low), and stored best entries
    that were not swept under the current ranking/candidate list are re-swept"""

    @classmethod
    def setUpClass(cls):
        import importlib.machinery, importlib.util
        ld = importlib.machinery.SourceFileLoader("bench2_mod", os.path.join(HERE, "..", "bench2"))
        cls.b = importlib.util.module_from_spec(importlib.util.spec_from_loader("bench2_mod", ld))
        ld.exec_module(cls.b)

    def test_cube_winner_is_fastest_not_steadiest(self):
        pres = {"pres:fifo/4img/fif2": {"fps": 120.4, "low1": 107.0, "cpu_ms": 1.1},
                "pres:mailbox/2img/fif2": {"fps": 457.0, "low1": 90.0, "cpu_ms": 0.4}}
        self.assertEqual(min(pres, key=self.b.pres_rank(pres, "cube")), "pres:mailbox/2img/fif2")

    def test_cube_tie_goes_to_better_low1(self):
        pres = {"a": {"fps": 100.0, "low1": 50.0, "cpu_ms": 1}, "b": {"fps": 100.0, "low1": 80.0, "cpu_ms": 1}}
        self.assertEqual(min(pres, key=self.b.pres_rank(pres, "cube")), "b")

    def test_triangle_winner_by_fps_then_cpu(self):
        pres = {"a": {"fps": 600.0, "low1": 1, "cpu_ms": 0.9}, "b": {"fps": 600.0, "low1": 1, "cpu_ms": 0.5}, "c": {"fps": 500.0, "low1": 9, "cpu_ms": 0.1}}
        self.assertEqual(min(pres, key=self.b.pres_rank(pres, "triangle")), "b")

    def test_env_penalty_still_applies(self):
        pres = {"a": {"fps": 100.0, "low1": 1, "cpu_ms": 1}, "b": {"fps": 103.0, "low1": 1, "cpu_ms": 1}}
        pen = lambda k: 1.05 if k == "b" else 1.0
        self.assertEqual(min(pres, key=self.b.pres_rank(pres, "cube", pen)), "a")

    def test_readmission_keeps_the_phone_lock(self):
        """the recursive re-sweep calls admit() again on the same Phone: the first admission's flock must survive (QSAFE_HELD is then set by
        ourselves, which must not be mistaken for an ancestor's lock and drop ours)"""
        b = self.b
        dev = {"alias": "poco-x8", "class": "x8", "lock": "unit-test-lock"}
        ph = b.Phone(dev, {"quick_root": "/nonexistent"})
        got = []
        ph.acquire_wait = lambda secs: (setattr(ph.lock, "fd", object()), got.append(ph.lock), True)[-1]
        ph.health = lambda: None
        saved = (b.L.resolve, b.L.disabled_dir, b.G.disabled, os.environ.pop("QSAFE_HELD", None))
        b.L.resolve, b.L.disabled_dir, b.G.disabled = (lambda *a, **k: "tid"), (lambda: None), (lambda *a: False)
        try:
            self.assertIsNone(ph.admit())
            first = ph.lock
            self.assertIsNotNone(first)
            self.assertIsNone(ph.admit())
            self.assertIs(ph.lock, first)            # not replaced / not dropped
            self.assertIsNotNone(first.fd)
            self.assertEqual(len(got), 1)            # and not re-acquired
        finally:
            b.L.resolve, b.L.disabled_dir, b.G.disabled = saved[:3]
            os.environ.pop("QSAFE_HELD", None)
            if saved[3] is not None:
                os.environ["QSAFE_HELD"] = saved[3]

    def test_stored_best_must_carry_the_current_sig(self):
        v, d = "cube-native", "ours"
        today = __import__("datetime").date(2026, 11, 18)
        mk = lambda **k: {"poco-x8": {d: {v: {"date": "2026-11-18", **k}}}}
        self.assertIsNone(self.b.best_stale(mk(sig=self.b.sweep_sig(v, d)), "poco-x8", d, v, today))
        self.assertTrue(self.b.best_stale(mk(), "poco-x8", d, v, today))                 # no sig: unvouched (this is how a fifo winner survived)
        self.assertTrue(self.b.best_stale(mk(sig="old-ranking"), "poco-x8", d, v, today))

    def test_rank_version_changes_the_sig(self):
        s0 = self.b.sweep_sig("cube-native", "ours")
        self.b.RANK_VER += 1
        try:
            self.assertNotEqual(s0, self.b.sweep_sig("cube-native", "ours"))
        finally:
            self.b.RANK_VER -= 1

    def test_draw_count_variants_are_in_the_default_set(self):
        for v in ("triangle-3k", "triangle-5k", "triangle-10k", "triangle-20k"):
            self.assertIn(v, self.b.WORK)
            self.assertIn(f"--n {int(v.split('-')[1][:-1]) * 1000} ", self.b.WORK[v][1] + " ")


SMOKE_OUT = "\n".join(['{"test":"t_vk_draw","rc":0,"fail_lines":0,"pass_lines":3}', '{"test":"t_vk_compute","rc":0,"fail_lines":0,"pass_lines":1}',
                      '{"test":"t_vk_meta","rc":0,"fail_lines":0,"pass_lines":1}', '{"test":"summary","ok":true}'])


class ReadyTests(unittest.TestCase):
    """farm ready: decision logic with fixtures (no gh, no git)"""
    HEAD, MERGE, OLD = "h" * 40, "m" * 40, "o" * 40
    ANC = {(HEAD, MERGE), (OLD, HEAD)}                       # pairs (ancestor, descendant)
    anc = staticmethod(lambda a, b: (a, b) in ReadyTests.ANC)

    def job(self, commit, verdict, state="done", jid="20261119-000000-abcd1234"):
        return {"id": jid, "suite": "smoke", "commit": commit, "parts": {"v13": {"state": state, "verdict": verdict}}}

    def pr(self, **kw):
        d = {"number": 7, "headRefOid": self.HEAD, "mergeable": "MERGEABLE", "isDraft": False,
             "statusCheckRollup": [{"status": "COMPLETED", "conclusion": "SUCCESS"}]}
        d.update(kw)
        return d

    def row(self, jobs, threads=0, links=(), **kw):
        import ready as R
        return R.make_row(self.pr(**kw), jobs, self.anc, L.job_verdict, threads, list(links))

    def test_ready_exact_and_merge_commit(self):
        self.assertEqual(self.row([self.job(self.HEAD, "pass")])["verdict"], "READY")
        self.assertEqual(self.row([self.job(self.MERGE, "pass")])["verdict"], "READY")      # --merge-with-main job: head is its parent

    def test_stale_none_fail_pending(self):
        self.assertEqual(self.row([self.job(self.OLD, "pass")])["verdict"], "smoke is for an older head")
        self.assertEqual(self.row([])["verdict"], "no smoke for head")
        self.assertEqual(self.row([self.job(self.HEAD, "fail")])["verdict"], "smoke FAIL")
        self.assertEqual(self.row([self.job(self.HEAD, None, state="running")])["verdict"], "smoke pending")

    def test_newest_covering_job_wins(self):
        jobs = [self.job(self.HEAD, "fail", jid="20261119-000001-a"), self.job(self.HEAD, "pass", jid="20261119-000002-b")]
        self.assertEqual(self.row(jobs)["smoke"], "pass")
        self.assertEqual(self.row(list(reversed(jobs)))["smoke"], "FAIL")

    def test_other_columns_block(self):
        ok = [self.job(self.HEAD, "pass")]
        self.assertIn("checks FAIL", self.row(ok, statusCheckRollup=[{"status": "COMPLETED", "conclusion": "FAILURE"}])["verdict"])
        self.assertIn("checks pending", self.row(ok, statusCheckRollup=[{"status": "IN_PROGRESS", "conclusion": ""}])["verdict"])
        self.assertIn("2 unresolved", self.row(ok, threads=2)["verdict"])
        self.assertIn("symlink", self.row(ok, links=["third_party"])["verdict"])
        self.assertIn("CONFLICTING", self.row(ok, mergeable="CONFLICTING")["verdict"])
        self.assertEqual(self.row(ok, isDraft=True)["verdict"], "draft")

    def test_partial_only_unknown_and_cancelled(self):
        import ready as R
        only = dict(self.job(self.HEAD, "pass"), only=["t_vk_draw"])
        self.assertEqual(self.row([only])["verdict"], "no smoke for head")                  # --only job is not the full gate
        canc = self.job(self.HEAD, "pass")
        canc["parts"]["v11"] = {"state": "cancelled"}
        self.assertEqual(self.row([canc])["verdict"], "smoke job has cancelled part(s)")
        unk = lambda a, b: None                                                              # git could not tell (pruned commit)
        r = R.make_row(self.pr(), [self.job(self.MERGE, "pass")], unk, L.job_verdict, 0, [])
        self.assertEqual(r["verdict"], "smoke ancestry unknown (commit not resolvable)")
        self.assertEqual(R.make_row(self.pr(), [self.job(self.HEAD, "pass")], unk, L.job_verdict, 0, [])["verdict"], "READY")   # exact head needs no git

    def test_symlink_by_tree_mode(self):
        import ready as R
        trees = {"vk/tests": {"a.c": "100644", "lnk": "120000"}, "": {"x": "120000"}}
        self.assertEqual(R.symlink_paths(trees, ["vk/tests/a.c", "vk/tests/lnk", "x"]), ["vk/tests/lnk", "x"])

    def test_bad_files_and_render(self):
        import ready as R
        self.assertEqual(R.bad_files(["vk/deqp/out/x", "vk/src/a.c", "third_party"]), ["vk/deqp/out/x", "third_party"])
        out = R.render([self.row([self.job(self.HEAD, "pass")]), self.row([], links=[])])
        self.assertIn("1 READY of 2", out)


if __name__ == "__main__":
    unittest.main()
