#!/usr/bin/env python3
"""Verify the gplasync async-pipeline patchset (0006) is intact and revertible.

patches/0006-dxvk-gplasync-3.1.1-1.patch is upstream gplasync v3.1.1-1
(https://gitlab.com/Ph42oN/dxvk-gplasync), rebased by one context line onto our
pin. It makes a missing pipeline compile on a worker thread and skip the draw
instead of stalling the submit thread, which is what removes the first-draw
pipeline-compile hitch. A dropped hunk would silently restore the stall, and a
changed default would turn the feature on for titles that cannot take it, so
the build refuses to package a tree that is not the one we reviewed.

Usage: check_async_pipeline.py <path to the patched dxvk source tree>
"""
import os
import re
import sys

# Every marker is (file, needle, why). Keep in sync with tools/dxvk/README.md.
MARKERS = [
    ("src/dxvk/dxvk_options.cpp",
     '"dxvk.enableAsync"',
     "config option dxvk.enableAsync is not read"),
    ("src/dxvk/dxvk_options.h",
     "bool enableAsync = true;",
     "DxvkOptions::enableAsync is not declared"),
    ("src/dxvk/dxvk_graphics.cpp",
     'env::getEnvVar("DXVK_ASYNC") != "0"',
     "DXVK_ASYNC=0 escape hatch is missing"),
    ("src/dxvk/dxvk_graphics.cpp",
     "m_workers->compileGraphicsPipeline(this, state, DxvkPipelinePriority::High)",
     "async path does not hand the compile to the worker pool"),
    ("src/dxvk/dxvk_graphics.cpp",
     "std::unique_lock<dxvk::mutex> lock(useAsync ? m_asyncMutex : m_mutex)",
     "async path does not take its own mutex"),
    ("src/dxvk/dxvk_graphics.cpp",
     "if (!m_async && this->canCreateBasePipeline(state))",
     "worker compile does not skip the fast-link bail-out"),
    ("src/dxvk/dxvk_graphics.cpp",
     "m_async = false;",
     "m_async is never cleared after the worker finishes"),
    ("src/dxvk/dxvk_graphics.h",
     "m_asyncMutex",
     "per-pipeline async mutex is missing"),
    ("src/dxvk/dxvk_context.cpp",
     "this->checkAsyncCompilationCompat()",
     "context does not gate async compilation on render-target compatibility"),
    ("src/dxvk/dxvk_context.h",
     "checkAsyncCompilationCompat() const",
     "checkAsyncCompilationCompat() is not declared"),
    ("src/dxvk/dxvk_image.h",
     "getRtBindingAsyncCompilationCompat",
     "image has no async-compilation compatibility flag"),
]


def main(root):
    if not os.path.isdir(root):
        sys.exit(f"async pipelines: {root} is not a directory")
    for path, needle, why in MARKERS:
        full = os.path.join(root, path)
        try:
            text = open(full, encoding="utf-8").read()
        except OSError as e:
            sys.exit(f"async pipelines: cannot read {full}: {e}")
        if needle not in text:
            sys.exit(f"async pipelines: {path}: {why} (missing {needle!r})")

    # The default has to stay opt-out, and the skip-draw return has to stay in
    # the async branch so a synchronous caller still compiles on its own thread.
    options = open(os.path.join(root, "src/dxvk/dxvk_options.cpp"), encoding="utf-8").read()
    if not re.search(r'"dxvk\.enableAsync",\s*true\)', options):
        sys.exit("async pipelines: dxvk.enableAsync must default to true")
    graphics = open(os.path.join(root, "src/dxvk/dxvk_graphics.cpp"), encoding="utf-8").read()
    async_block = re.search(r"if \(useAsync\) \{(.*?)\n        \}", graphics, re.S)
    if not async_block or "return DxvkGraphicsPipelineHandle();" not in async_block.group(1):
        sys.exit("async pipelines: async branch no longer returns an empty handle "
                 "(it would stall the submit thread again)")

    print("async pipelines: gplasync v3.1.1-1 hunks present, opt-out intact")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: check_async_pipeline.py <dxvk source tree>")
    main(sys.argv[1])
