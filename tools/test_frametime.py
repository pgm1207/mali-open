#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""host test for tools/frametime row()/buckets()/variants() on a synthetic mdbench result (no phone)"""
import importlib.machinery, importlib.util, os, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ld = importlib.machinery.SourceFileLoader("frametime", os.path.join(HERE, "frametime"))
spec = importlib.util.spec_from_loader("frametime", ld)
FT = importlib.util.module_from_spec(spec)
ld.exec_module(FT)


def res(scene, shadow=1.0, post=0.5, gpu=4.0, p99=9.0, present=0.3):
    pc = lambda v: {"n": 10, "mean": v, "min": v, "p50": v, "p90": v, "p95": v, "p99": v, "max": v}
    return {"mdbench": {"fps": 120.0, "budget_ms": 8.3333, "over_budget": 3, "over_budget_pct": 3.0,
                        "frame_time_ms": {"all": pc(8.3) | {"p99": p99}}, "gpu_ms": pc(gpu),
                        "gpu_pass_ms": {"shadow": pc(shadow), "scene": pc(scene), "post": pc(post)},
                        "cpu_ms": {"record": pc(1.0), "submit": pc(0.2), "present_acquire": pc(present), "fence_wait": pc(2.0)}}}


class T(unittest.TestCase):
    def test_buckets(self):
        rows = {"full": FT.row(res(3.0)), "scissor64": FT.row(res(1.2)), "offscreen": FT.row(res(3.0))}
        b = FT.buckets(rows)
        self.assertAlmostEqual(b["fragment_scene_ms"], 1.8)       # full scene minus scissored scene
        self.assertAlmostEqual(b["tiler_cpu_scene_ms"], 1.2)
        self.assertEqual(b["present_acquire_cpu_ms"], 0.3)
        self.assertEqual(b["frame_p99_ms"], 9.0)

    def test_missing_variant_does_not_invent(self):
        b = FT.buckets({"full": FT.row(res(3.0))})
        self.assertNotIn("fragment_scene_ms", b)
        self.assertNotIn("present_cost_ms", b)

    def test_variants_args(self):
        v = FT.variants("1920x1080", 120)
        self.assertIn("--mega-scissor-px 64", v["scissor64"]["scene"])
        self.assertEqual(v["offscreen"]["wsi"], "--wsi headless")
        self.assertIn("--target-hz 120", v["full"]["scene"])


if __name__ == "__main__":
    unittest.main()
