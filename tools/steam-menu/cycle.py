#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""One iteration of the Steam-menu loop on a phone: apply BL_WEBHELPER_ARGS / env through the
guest hook file, start Steam, wait for the first frame, measure fps (or capture the GPU-process
error), always restore the hook file and stop the session, print one JSON summary line.

Touches a phone. See README.md. Stdout carries exactly one line (the JSON); everything else goes
to stderr.
"""
import argparse
import json
import os
import re
import shlex
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
FPS_TOOL = os.path.join(REPO, "tools", "steam-fps-host", "steam_fps_host.py")
HOOK = "/usr/local/lib/mali-open/wayland.env"
PKG = "com.ludashi.benchmark"
MARK = "# --- steam-menu/cycle.sh overrides (removed on restore) ---"
KEY_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
ERR_RE = re.compile(r"gpu[ _-]?process|exit_code|GPU.*(fail|crash|lost|exit)|viz|ERROR:|vkCreate|VK_ERROR|angle.*(fail|error)",
                    re.I)
SUMMARY_KEYS = ("session_reached", "first_frame", "fps_mean", "p50_ms", "p99_ms", "verdict", "error")


class CycleError(Exception):
    def __init__(self, verdict, msg):
        super().__init__(msg)
        self.verdict, self.msg = verdict, msg


# ---------------------------------------------------------------- parsing / pure helpers

def parse_args(argv):
    ap = argparse.ArgumentParser(prog="cycle.sh", description=__doc__.split("\n\n")[0])
    ap.add_argument("serial")
    ap.add_argument("--args", dest="webhelper_args", default=None,
                    help="value for BL_WEBHELPER_ARGS (one string)")
    ap.add_argument("--env", nargs="+", action="extend", default=[], metavar="K=V",
                    help="env overrides, one or more K=V after the flag; may repeat")
    ap.add_argument("--wait", type=int, default=120, help="seconds to wait for first frame (default 120)")
    ap.add_argument("--seconds", type=int, default=30, help="measurement window (default 30)")
    ap.add_argument("--runs", type=int, default=2, help="measurement windows (default 2)")
    ap.add_argument("--hook-path", default=HOOK, help="guest hook file (default %(default)s)")
    ap.add_argument("--pkg", default=PKG)
    ap.add_argument("--play-xy", default=None, metavar="X,Y", help="fallback tap point for the Play button")
    ap.add_argument("--fps-tool", default=FPS_TOOL)
    ap.add_argument("--dry-run", action="store_true", help="print the commands, touch nothing")
    a = ap.parse_args(argv)
    env = {}
    for item in a.env:
        k, sep, v = item.partition("=")
        if not sep or not KEY_RE.match(k):
            ap.error(f"--env takes K=V with a shell-safe key, got {item!r}")
        if "\n" in v or "\0" in v:
            ap.error(f"--env value for {k} contains a newline")
        env[k] = v
    if a.webhelper_args is not None and ("\n" in a.webhelper_args or "\0" in a.webhelper_args):
        ap.error("--args contains a newline")
    if "BL_WEBHELPER_ARGS" in env:
        ap.error("use --args for BL_WEBHELPER_ARGS")
    if a.play_xy and not re.fullmatch(r"\d+,\d+", a.play_xy):
        ap.error("--play-xy takes X,Y")
    if a.wait < 1 or a.seconds < 1 or a.runs < 1:
        ap.error("--wait, --seconds and --runs must be positive")
    a.env = env
    return a


def overrides(a):
    """Ordered (key, value) pairs to force through the hook."""
    out = []
    if a.webhelper_args is not None:
        out.append(("BL_WEBHELPER_ARGS", a.webhelper_args))
    out.extend(sorted(a.env.items()))
    return out


def render_hook(original, pairs):
    """Original hook text (or '' if absent) + our block, appended so it wins over the original."""
    base = strip_block(original)
    if not base.endswith("\n") and base:
        base += "\n"
    lines = [MARK] + [f"export {k}={shlex.quote(v)}" for k, v in pairs]
    return base + "\n".join(lines) + "\n"


def strip_block(text):
    i = text.find(MARK)
    return text if i < 0 else text[:i]


def parse_bounds(b):
    m = re.fullmatch(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]", b.strip())
    if not m:
        return None
    x1, y1, x2, y2 = map(int, m.groups())
    return (x1 + x2) // 2, (y1 + y2) // 2


def find_play(xml):
    """Centre of the first uiautomator node whose text/content-desc looks like the Play button."""
    for m in re.finditer(r"<node\b[^>]*>", xml):
        n = m.group(0)
        label = " ".join(re.findall(r'\b(?:text|content-desc)="([^"]*)"', n))
        if re.search(r"\bplay\b", label, re.I):
            b = re.search(r'bounds="([^"]*)"', n)
            c = parse_bounds(b.group(1)) if b else None
            if c:
                return c
    return None


def extract_gpu_error(log, limit=3, width=400):
    """Last `limit` lines of the session log that look like a GPU-process error, verbatim."""
    hits = [l.rstrip() for l in log.splitlines() if ERR_RE.search(l)]
    return "\n".join(l[:width] for l in hits[-limit:])


def verify_dump(text, pairs):
    """Parse the guest /proc dump; -> list of failures (empty = every override reached the processes).

    Dump format: `P <pid> <comm>`, `C <cmdline, NUL->space>`, `E <KEY=VALUE>` (environ lines of
    the steam/steamwebhelper processes)."""
    cmds, envs, procs = [], set(), 0
    for l in text.splitlines():
        if l.startswith("P "):
            procs += 1
        elif l.startswith("C "):
            cmds.append(l[2:])
        elif l.startswith("E "):
            envs.add(l[2:])
    if not procs:
        return ["no steam/steamwebhelper process found"]
    bad = []
    for k, v in pairs:
        if k == "BL_WEBHELPER_ARGS":
            toks = v.split()
            web = [c for c in cmds if "steamwebhelper" in c]
            if not web:
                bad.append("no steamwebhelper cmdline")
            elif not any(all(t in c.split() for t in toks) for c in web):
                bad.append(f"BL_WEBHELPER_ARGS tokens {toks} not in any steamwebhelper cmdline")
        elif f"{k}={v}" not in envs:
            bad.append(f"{k}={v} not in /proc/<pid>/environ of steam/steamwebhelper")
    return bad


def proc_script(pairs):
    keys = "|".join(re.escape(k) for k, _ in pairs if k != "BL_WEBHELPER_ARGS")
    egrep = f"grep -aE '^({keys})=' 2>/dev/null" if keys else "true"
    return ("for p in /proc/[0-9]*; do c=$(cat $p/comm 2>/dev/null); "
            "case \"$c\" in steam|steamwebhelper|steamweb*) "
            "echo \"P ${p#/proc/} $c\"; echo \"C $(tr '\\0' ' ' < $p/cmdline 2>/dev/null)\"; "
            f"tr '\\0' '\\n' < $p/environ 2>/dev/null | {egrep} | sed 's/^/E /';; esac; done")


def summarize(**kw):
    d = {k: None for k in SUMMARY_KEYS}
    d.update(session_reached=False, first_frame=False)
    d.update(kw)
    return json.dumps({k: d[k] for k in SUMMARY_KEYS}, separators=(",", ":"))


def combine_runs(runs):
    """fps tool JSON dicts -> (fps_mean, p50_ms, p99_ms, verdict); worst verdict wins."""
    order = ["120-paced", "90-capped", "60-capped", "dropped-frames", "no-frames"]
    ok = [r for r in runs if r.get("verdict") != "no-frames"]
    if not ok:
        return None, None, None, "no-frames"
    mean = lambda k: round(sum(float(r[k]) for r in ok) / len(ok), 2)
    verdict = max((r.get("verdict", "no-frames") for r in runs),
                  key=lambda v: order.index(v) if v in order else len(order))
    return mean("fps"), mean("p50Ms"), round(max(float(r["p99Ms"]) for r in ok), 2), verdict


# ---------------------------------------------------------------- phone side

class Phone:
    def __init__(self, a):
        self.a = a
        self.serial = a.serial
        self.ctl_bin = os.path.join(os.environ.get("QUICK_ROOT", os.path.expanduser("~/Projects/mali-gaming")),
                                    "tools", "droiddeckctl")
        self.env = dict(os.environ, DROIDDECK_PACKAGE=a.pkg)
        self.dry = a.dry_run

    # command builders (also used by --dry-run)
    def ctl_cmd(self, *x):
        return [sys.executable, "-I", self.ctl_bin, "--serial", self.serial, *x]

    def adb_cmd(self, *x):
        return ["adb", "-s", self.serial, *x]

    def run(self, cmd, timeout=150, stdin=None):
        print("+ " + " ".join(shlex.quote(c) for c in cmd).replace("\n", "\\n"), file=sys.stderr)
        if self.dry:
            return 0, ""
        try:
            p = subprocess.run(cmd, capture_output=True, timeout=timeout, env=self.env, input=stdin)
        except subprocess.TimeoutExpired:
            return 124, "timeout"
        return p.returncode, p.stdout.decode(errors="replace") + p.stderr.decode(errors="replace")

    def ctl(self, *x, timeout=150):
        rc, out = self.run(self.ctl_cmd(*x), timeout)
        i = out.find("{")
        try:
            j = json.loads(out[i:]) if i >= 0 else {}
        except ValueError:
            j = {"text": out[-300:]}
        j["_rc"], j["_text"] = rc, out
        return j

    def provider_ok(self):
        j = self.ctl("state", timeout=30)
        return j.get("ok") is True and "session" in j, j

    def guest(self, script, stdin=None, timeout=30):
        x = ["guest", "--timeout", str(timeout), "--"]
        if stdin is not None:
            x = ["guest", "--timeout", str(timeout), "--stdin", stdin, "--"]
        j = self.ctl(*x, "sh", "-c", script, timeout=timeout + 40)
        out = (j.get("stdout") or {}).get("text", "") if isinstance(j.get("stdout"), dict) else ""
        return j.get("exitCode", 1 if j.get("ok") is not True else 0), out, j

    def running(self):
        ok, j = self.provider_ok()
        if not ok:
            return None
        s = j["session"]
        return bool(s.get("running")) or s.get("phase") not in {"IDLE", "FAILED"}

    def stop(self):
        j = self.ctl("stop", "--timeout", "60", timeout=150)
        if j.get("ok") is not True and not self.dry:       # no provider: leave through Android
            self.run(self.adb_cmd("shell", "am", "force-stop", self.a.pkg), 30)

    # start: provider first, Play button as the fallback
    def start(self):
        j = self.ctl("start", "steam", "--wait", "--timeout", str(self.a.wait), timeout=self.a.wait + 60)
        if j.get("ok") is True or self.dry:
            return "ctl"
        tail = (j.get("_text") or "").strip().splitlines()
        print("start: droiddeckctl start failed, falling back to the Play button: "
              + (tail[-1][:160] if tail else ""), file=sys.stderr)
        self.play_button()
        return "tap"

    def play_button(self):
        self.run(self.adb_cmd("shell", "monkey", "-p", self.a.pkg, "-c", "android.intent.category.LAUNCHER", "1"), 30)
        time.sleep(0 if self.dry else 4)
        shot = f"/tmp/cycle-{self.serial}-launch.png"
        self.run(["sh", "-c", f"adb -s {shlex.quote(self.serial)} exec-out screencap -p > {shlex.quote(shot)}"], 30)
        rc, xml = self.run(self.adb_cmd("exec-out", "uiautomator", "dump", "/dev/tty"), 40)
        xy = find_play(xml)
        if not xy and self.a.play_xy:
            xy = tuple(map(int, self.a.play_xy.split(",")))
        if not xy and not self.dry:
            raise CycleError("no-session", f"Play button not found (screenshot {shot}); pass --play-xy X,Y")
        xy = xy or ("X", "Y")
        self.run(self.adb_cmd("shell", "input", "tap", str(xy[0]), str(xy[1])), 15)

    def sf_layer_up(self):
        rc, out = self.run(self.adb_cmd("shell", "dumpsys", "SurfaceFlinger", "--list"), 30)
        return any("SessionActivity" in l for l in out.splitlines())

    def wait_first_frame(self, pairs):
        """-> (session_reached, first_frame, verify_failures, state_json)"""
        deadline = time.time() + self.a.wait
        reached = frame = False
        verified, bad, st = False, [], {}
        while time.time() < deadline:
            ok, j = self.provider_ok()
            if ok:
                s = j["session"]
                st = s
                reached = reached or bool(s.get("running")) or s.get("phase") in {"STARTING_STEAM", "READY"}
                if s.get("phase") == "FAILED" or s.get("failure"):
                    break
                frame = bool(s.get("firstFrame"))
            else:                                   # provider missing: SurfaceFlinger layer is the proxy
                if self.sf_layer_up():
                    reached = frame = True
            if reached and not verified and pairs:
                rc, out, _ = self.guest(proc_script(pairs)) if ok else (1, "", {})
                bad = verify_dump(out, pairs) if rc == 0 else ["guest /proc check unavailable"]
                # the webhelper may not exist yet: keep polling until it does or the first frame is in
                verified = not bad or frame
            if frame and (verified or not pairs):
                break
            time.sleep(3)
        return reached, frame, bad if pairs else [], st

    def session_log(self):
        d = f"/tmp/cycle-{self.serial}-logs"
        self.ctl("logs", "latest", d, timeout=180)
        text = ""
        for root, _, files in os.walk(d) if not self.dry else []:
            for f in files:
                if f.startswith("session") and f.endswith(".log"):
                    text += open(os.path.join(root, f), errors="replace").read()
        return text

    def measure(self):
        runs = []
        for i in range(self.a.runs):
            rc, out = self.run([sys.executable, "-I", self.a.fps_tool, str(self.a.seconds), "-s", self.serial],
                               self.a.seconds + 90)
            i0 = out.find("{")
            try:
                runs.append(json.loads(out[i0:]))
            except ValueError:
                runs.append({"verdict": "no-frames", "_raw": out[-200:]})
        return runs


# ---------------------------------------------------------------- the cycle

def plan(a):
    """Dry-run: the exact commands, in order, without touching anything."""
    ph = Phone(a)
    pairs = overrides(a)
    print("# cycle plan (nothing is executed)", file=sys.stderr)
    ph.stop()
    ph.start()                                       # bootstrap session: the hook can only be written inside the guest
    ph.guest(f"[ -e {a.hook_path} ] && echo EXISTS && cat {a.hook_path}")      # read + keep the original
    ph.guest(f"cat > {a.hook_path}.new && mv {a.hook_path}.new {a.hook_path}",
             stdin=render_hook("<original hook content>\n", pairs))
    ph.stop()
    ph.start()
    ph.guest(proc_script(pairs))
    ph.ctl("state")
    for _ in range(a.runs):
        ph.run([sys.executable, "-I", a.fps_tool, str(a.seconds), "-s", a.serial])
    ph.ctl("logs", "latest", f"/tmp/cycle-{a.serial}-logs")
    print("# restore hook (needs a running session) then stop", file=sys.stderr)
    ph.guest(f"cat > {a.hook_path}.new && mv {a.hook_path}.new {a.hook_path}", stdin="<original hook content>\n")
    ph.stop()
    return 0


def cycle(a):
    ph = Phone(a)
    pairs = overrides(a)
    res = dict(session_reached=False, first_frame=False)
    original, existed, wrote = "", False, False
    keep = f"/tmp/cycle-{a.serial}-wayland.env.orig"
    try:
        if not os.path.exists(a.fps_tool):
            raise CycleError("no-tool", f"{a.fps_tool} missing (steam_fps_host.py is PR #254)")
        # (1) no session running
        if ph.running():
            ph.stop()
        # (2) the hook file can only be written from inside the guest: bootstrap session, write, stop
        if pairs:
            if ph.start() == "tap" and not ph.provider_ok()[0]:
                raise CycleError("no-provider", "no agent provider: cannot write the guest hook file")
            rc, out, j = ph.guest(f"[ -e {a.hook_path} ] && echo EXISTS && cat {a.hook_path}")
            if rc != 0 and "EXISTS" not in out:
                existed = False
            else:
                existed, original = True, out.split("EXISTS\n", 1)[-1]
            open(keep, "w").write(original)
            rc, _, j = ph.guest(f"cat > {a.hook_path}.new && mv {a.hook_path}.new {a.hook_path}",
                                stdin=render_hook(original, pairs))
            if rc != 0:
                raise CycleError("hook-write-failed", j.get("_text", "")[-200:])
            wrote = True
            ph.stop()
        # (3)+(4) start for real and wait for the first frame, verifying the overrides from /proc
        ph.start()
        reached, frame, bad, st = ph.wait_first_frame(pairs)
        res.update(session_reached=reached, first_frame=frame)
        if bad:
            raise CycleError("env-not-applied", "; ".join(bad))
        if not reached:
            raise CycleError("no-session", f"session not up in {a.wait}s: {st.get('failure') or st.get('phase')}")
        if not frame:
            log = ph.session_log()
            raise CycleError("no-first-frame", extract_gpu_error(log) or f"no firstFrame in {a.wait}s (no GPU error in the log)")
        # (5) measure
        fps, p50, p99, verdict = combine_runs(ph.measure())
        res.update(fps_mean=fps, p50_ms=p50, p99_ms=p99, verdict=verdict)
        if verdict == "no-frames":
            res["error"] = "popup up but no frames presented: " + extract_gpu_error(ph.session_log())
    except CycleError as e:
        res.update(verdict=e.verdict, error=e.msg)
    finally:
        # (6) always restore the hook and stop the session
        try:
            if wrote:
                if not ph.running():
                    ph.start()                       # writing needs a live guest
                body = original
                cmd = (f"cat > {a.hook_path}.new && mv {a.hook_path}.new {a.hook_path}" if existed
                       else f"rm -f {a.hook_path}")
                rc, _, j = ph.guest(cmd, stdin=body if existed else None)
                if rc != 0:
                    msg = f"RESTORE FAILED for {a.hook_path}; original saved in {keep}"
                    print(msg, file=sys.stderr)
                    res["error"] = ((res.get("error") or "") + " | " + msg).strip(" |")
        except Exception as e:  # noqa: BLE001 - must still stop the session
            print(f"restore error: {e}; original in {keep}", file=sys.stderr)
            res["error"] = ((res.get("error") or "") + f" | RESTORE FAILED ({e}); original in {keep}").strip(" |")
        try:
            ph.stop()
        except Exception as e:  # noqa: BLE001
            print(f"stop error: {e}", file=sys.stderr)
    if res.get("error"):
        print("error: " + res["error"], file=sys.stderr)
    print(summarize(**res))
    return 0 if res.get("verdict") in ("120-paced", "90-capped", "60-capped", "dropped-frames") else 1


def main(argv=None):
    a = parse_args(argv)
    if a.dry_run:
        rc = plan(a)
        print(summarize(verdict="dry-run"))
        return rc
    for s in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(s, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    try:
        return cycle(a)
    except KeyboardInterrupt:
        print(summarize(verdict="interrupted", error="interrupted"))
        return 130


if __name__ == "__main__":
    sys.exit(main())
