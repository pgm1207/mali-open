#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Phone-free tests for cycle.py: argument parsing, hook rendering, /proc verification,
GPU-error extraction, JSON assembly and the --dry-run plan.  python3 -I tools/steam-menu/test_cycle.py"""
import io
import json
import os
import subprocess
import sys
import unittest
from contextlib import redirect_stderr, redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cycle as C  # noqa: E402

SH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cycle.sh")


class Parse(unittest.TestCase):
    def test_basic(self):
        a = C.parse_args(["SER", "--args", "--ozone-platform=wayland --use-gl=angle", "--env", "A=1", "B=x y", "--env", "C=3"])
        self.assertEqual(a.serial, "SER")
        self.assertEqual(a.env, {"A": "1", "B": "x y", "C": "3"})
        self.assertEqual(C.overrides(a)[0], ("BL_WEBHELPER_ARGS", "--ozone-platform=wayland --use-gl=angle"))
        self.assertEqual((a.wait, a.seconds, a.runs), (120, 30, 2))

    def test_value_with_equals(self):
        self.assertEqual(C.parse_args(["S", "--env", "X=a=b"]).env, {"X": "a=b"})

    def test_rejects(self):
        for bad in (["S", "--env", "novalue"], ["S", "--env", "1A=2"], ["S", "--env", "A B=2"],
                    ["S", "--env", "BL_WEBHELPER_ARGS=x"], ["S", "--args", "a\nb"], ["S", "--wait", "0"],
                    ["S", "--play-xy", "1;2"], []):
            with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit, msg=bad):
                C.parse_args(bad)


class Hook(unittest.TestCase):
    def test_render_appends_and_quotes(self):
        t = C.render_hook("WAYLAND_DISPLAY=x\nBL_WEBHELPER_ARGS=old", [("BL_WEBHELPER_ARGS", "--a='b' $c"), ("K", "v")])
        self.assertTrue(t.startswith("WAYLAND_DISPLAY=x\nBL_WEBHELPER_ARGS=old\n" + C.MARK))
        self.assertIn("export BL_WEBHELPER_ARGS='--a='\"'\"'b'\"'\"' $c'", t)
        self.assertTrue(t.endswith("export K=v\n"))

    def test_roundtrip_strip(self):
        orig = "A=1\n"
        self.assertEqual(C.strip_block(C.render_hook(orig, [("K", "v")])), orig)
        self.assertEqual(C.strip_block(C.render_hook(C.render_hook(orig, [("K", "1")]), [("K", "2")])), orig)

    def test_shell_semantics(self):
        t = C.render_hook("", [("BL_WEBHELPER_ARGS", "--x='y' $HOME z")])
        out = subprocess.run(["sh", "-c", f'{t}\nprintf %s "$BL_WEBHELPER_ARGS"'], capture_output=True, text=True).stdout
        self.assertEqual(out, "--x='y' $HOME z")


class Verify(unittest.TestCase):
    PAIRS = [("BL_WEBHELPER_ARGS", "--use-angle=vulkan --foo"), ("K", "v")]
    GOOD = ("P 10 steam\nC /bin/steam -x\nE K=v\n"
            "P 11 steamwebhelper\nC /opt/steamwebhelper --use-angle=vulkan --foo --bar\nE K=v\n")

    def test_ok(self):
        self.assertEqual(C.verify_dump(self.GOOD, self.PAIRS), [])

    def test_missing_arg(self):
        bad = C.verify_dump(self.GOOD.replace("--foo ", ""), self.PAIRS)
        self.assertEqual(len(bad), 1)
        self.assertIn("BL_WEBHELPER_ARGS", bad[0])

    def test_missing_env(self):
        self.assertTrue(any("K=v" in b for b in C.verify_dump(self.GOOD.replace("E K=v\n", ""), self.PAIRS)))

    def test_no_process(self):
        self.assertEqual(C.verify_dump("", self.PAIRS), ["no steam/steamwebhelper process found"])

    def test_proc_script_runs(self):                         # the generated guest script is valid sh
        subprocess.run(["sh", "-n", "-c", C.proc_script(self.PAIRS)], check=True)


class Misc(unittest.TestCase):
    def test_gpu_error(self):
        log = "ok\nERROR:gpu_process_host.cc GPU process exited unexpectedly: exit_code=8704\nfine\nviz: lost\n"
        self.assertEqual(C.extract_gpu_error(log).splitlines(),
                         ["ERROR:gpu_process_host.cc GPU process exited unexpectedly: exit_code=8704", "viz: lost"])
        self.assertEqual(C.extract_gpu_error("nothing here"), "")

    def test_find_play(self):
        xml = '<node text="Library" bounds="[0,0][100,50]"/><node content-desc="Play Steam" bounds="[10,20][110,60]"/>'
        self.assertEqual(C.find_play(xml), (60, 40))
        self.assertIsNone(C.find_play('<node text="Display" bounds="[0,0][1,1]"/>'))

    def test_summary_shape(self):
        d = json.loads(C.summarize(session_reached=True, first_frame=True, fps_mean=119.8, verdict="120-paced"))
        self.assertEqual(list(d), list(C.SUMMARY_KEYS))
        self.assertIsNone(d["error"])
        self.assertEqual(json.loads(C.summarize(verdict="x"))["session_reached"], False)

    def test_combine(self):
        r = [{"fps": 119, "p50Ms": 8.3, "p99Ms": 9.0, "verdict": "120-paced"},
             {"fps": 61, "p50Ms": 16.6, "p99Ms": 20.0, "verdict": "60-capped"}]
        self.assertEqual(C.combine_runs(r), (90.0, 12.45, 20.0, "60-capped"))
        self.assertEqual(C.combine_runs([{"verdict": "no-frames"}])[3], "no-frames")


class DryRun(unittest.TestCase):
    def test_cli(self):
        p = subprocess.run([SH, "SER", "--args", "--a --b", "--env", "K=v", "--dry-run"], capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        lines = p.stdout.strip().splitlines()
        self.assertEqual(len(lines), 1)                       # stdout is exactly the JSON line
        self.assertEqual(json.loads(lines[0])["verdict"], "dry-run")
        cmds = [l for l in p.stderr.splitlines() if l.startswith("+ ")]
        joined = "\n".join(cmds)
        for want in ("droiddeckctl --serial SER stop", "start steam --wait", "steam_fps_host.py 30 -s SER",
                     "logs latest", "export BL_WEBHELPER_ARGS", "wayland.env", "original hook content"):
            self.assertIn(want, joined)
        self.assertEqual(sum("steam_fps_host.py" in c for c in cmds), 2)
        self.assertEqual(p.stderr.count("steam_fps_host.py 30"), 2)

    def test_bad_args_exit2(self):
        p = subprocess.run([SH, "SER", "--env", "bad", "--dry-run"], capture_output=True, text=True)
        self.assertEqual(p.returncode, 2)
        self.assertEqual(p.stdout, "")


if __name__ == "__main__":
    unittest.main()
