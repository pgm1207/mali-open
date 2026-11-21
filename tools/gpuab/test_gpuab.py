#!/usr/bin/env python3
import os, sys, unittest
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import subprocess
from gpuablib import rotate, quantile, iqr, parse, summarize, chunk_units

class T(unittest.TestCase):
    def test_rotate(self):
        v = ["a", "b", "c"]
        self.assertEqual([rotate(v, r) for r in range(4)], [["a","b","c"], ["b","c","a"], ["c","a","b"], ["a","b","c"]])
        first = [rotate(v, r)[0] for r in range(6)]; last = [rotate(v, r)[-1] for r in range(6)]
        self.assertEqual({x: first.count(x) for x in v}, {"a": 2, "b": 2, "c": 2})
        self.assertEqual({x: last.count(x) for x in v}, {"a": 2, "b": 2, "c": 2})
        self.assertEqual(v, ["a", "b", "c"])          # input untouched
        self.assertEqual(rotate(["x"], 5), ["x"])
    def test_quantile(self):
        self.assertEqual(quantile([1, 2, 3, 4, 5], .5), 3)
        self.assertAlmostEqual(quantile([1, 2, 3, 4], .25), 1.75)
        self.assertAlmostEqual(iqr([1, 2, 3, 4, 5]), 2)
        self.assertEqual(iqr([7]), 0)
    @staticmethod
    def mk(n, sc, r, t, f, g, end=None):
        return f"RUN {n} {sc} {r} temp={t}\n\"fps\":{f}\nSUM gpu={g} n=10\nEND temp={t if end is None else end}\n"
    LOG = "".join(f"RUN {n} cube {r} temp={t}\n{{\"fps\":{f}\nSUM gpu={g} n=10\nEND temp={t}\n" for n, r, t, f, g in [
        ("ours", 0, 380, 500, 0.60), ("bridge", 0, 380, 450, 0.50),
        ("ours", 1, 390, 500, 0.66), ("bridge", 1, 390, 450, 0.55),
        ("ours", 2, 450, 500, 9.99), ("bridge", 2, 450, 450, 9.99)])
    def test_parse_drops_hot(self):
        res = parse(self.LOG.replace('{"fps"', '"fps"'), 43.0)
        self.assertEqual(res[("cube", "ours")], [(0, 0.60, 500.0), (1, 0.66, 500.0)])   # round 2 at 45 C dropped
    def test_summarize_paired(self):
        res = parse(self.LOG, 43.0); s = summarize(res, ["cube"], ["ours", "bridge"])
        self.assertAlmostEqual(s[("cube", "ours")]["med"], 0.63); self.assertEqual(s[("cube", "ours")]["ratio"], 1.0)
        self.assertAlmostEqual(s[("cube", "bridge")]["ratio"], (0.5 / 0.6 + 0.55 / 0.66) / 2)
        self.assertEqual(s[("cube", "ours")]["n"], 2)
    def test_missing_variant(self):
        self.assertEqual(summarize({}, ["cube"], ["a"]), {})
    def test_hot_round_dropped_for_all_variants(self):
        """ours starts cool at round 1 but bridge (later in the round) is hot: round 1 must vanish for BOTH variants"""
        log = (self.mk("ours", "cube", 0, 380, 500, 0.60) + self.mk("bridge", "cube", 0, 380, 450, 0.50)
               + self.mk("ours", "cube", 1, 420, 500, 0.66) + self.mk("bridge", "cube", 1, 450, 450, 0.55))
        res = parse(log, 43.0)
        self.assertEqual(res[("cube", "ours")], [(0, 0.60, 500.0)])
        self.assertEqual(res[("cube", "bridge")], [(0, 0.50, 450.0)])
    def test_temperature_after_the_run_counts(self):
        res = parse(self.mk("ours", "cube", 0, 420, 500, 0.6, end=440) + self.mk("ours", "cube", 1, 420, 500, 0.7), 43.0)
        self.assertEqual(res[("cube", "ours")], [(1, 0.7, 500.0)])           # started at 42 C, ended at 44 C: dropped
    def test_hot_round_is_per_scene(self):
        res = parse(self.mk("a", "cube", 0, 450, 1, 1.0) + self.mk("a", "alu", 0, 380, 1, 2.0), 43.0)
        self.assertEqual(res, {("alu", "a"): [(0, 2.0, 1.0)]})
    def test_bad_header_clears_the_current_run(self):
        """`RUN bridge cube 0 temp=` (temperature unreadable) must not leave 'ours' as the current run: its fps/SUM are not ours"""
        log = self.mk("ours", "cube", 0, 380, 500, 0.60) + "RUN bridge cube 1 temp=\n\"fps\":450\nSUM gpu=0.5 n=10\nEND temp=380\n" + self.mk("ours", "cube", 2, 380, 500, 0.62)
        res = parse(log, 43.0)
        self.assertEqual(res[("cube", "ours")], [(0, 0.60, 500.0), (2, 0.62, 500.0)])
        self.assertNotIn(("cube", "bridge"), res)
    def test_chunks_stay_under_the_guest_cap(self):
        units = [(r, "cube") for r in range(32)]; per = 2 * (5 + 6)          # default workload: 764 s in one call
        ch = chunk_units(units, per)
        self.assertGreater(len(ch), 1)
        self.assertEqual([u for c in ch for u in c], units)
        for c in ch: self.assertLessEqual(60 + per * len(c), 570)
        self.assertEqual(len(chunk_units(units[:1], 10000)), 1)              # an over-long unit still runs
    def test_duplicate_variant_names_rejected(self):
        p = subprocess.run([sys.executable, "-I", os.path.join(os.path.dirname(os.path.abspath(__file__)), "gpuab.py"), "--variant", "t=bridge", "--variant", "t=icd:x.so"],
                           capture_output=True, text=True, timeout=30)
        self.assertNotEqual(p.returncode, 0); self.assertIn("unique", p.stderr)
if __name__ == "__main__": unittest.main()
