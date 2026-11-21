#!/usr/bin/env python3
"""Verify the DXVK compatibility shim relaxes exactly the reviewed features.

The shim (patches/0005-relax-required-features.patch) lets device creation
continue when DXVK lists a feature as required that our Mali driver cannot
provide. A typo, a widened list or a dropped log line would silently hide a
new incompatibility, so the build refuses to package a shim that is not the
one we reviewed.

Usage: check_compat_relaxation.py <path to src/dxvk/dxvk_device_info.cpp>
"""
import re
import sys

# The only features we are allowed to relax, with the reason each is harmless
# for the titles we run. Keep in sync with tools/dxvk/README.md.
RELAXED = {
    "geometryShader": "Mali has no geometry stage; no shipped title creates a GS pipeline",
    "shaderCullDistance": "no shipped title writes SV_CullDistance",
}

FUNCTION = "maliRelaxedRequiredFeature"


def main(path):
    source = open(path, encoding="utf-8").read()

    match = re.search(r"static bool %s\(.*?\n  \}\n" % FUNCTION, source, re.S)
    if not match:
        sys.exit(f"compat shim: {FUNCTION}() not found in {path}")
    body = match.group(0)

    names = re.findall(r'if \(n == "([A-Za-z0-9_]+)"\)', body)
    if sorted(names) != sorted(RELAXED):
        sys.exit("compat shim: relaxed feature set is %s, expected %s"
                 % (sorted(names), sorted(RELAXED)))
    if len(names) != len(set(names)):
        sys.exit(f"compat shim: duplicate entry in {FUNCTION}()")

    for name in RELAXED:
        message = f"mali-open: dxvk compat relaxation: {name} absent, allowed"
        if message not in body:
            sys.exit(f"compat shim: missing log line {message!r}")

    if "if (maliRelaxedRequiredFeature(f.readableName))" not in source:
        sys.exit("compat shim: checkDeviceCompatibility() does not consult the shim")

    # Nothing else may be relaxed: required features either pass or fail hard.
    for name in ("robustBufferAccess", "robustBufferAccess2", "sampleRateShading",
                 "descriptorIndexing", "maxPushConstantsSize"):
        if re.search(r'if \(n == "%s"\)' % name, body):
            sys.exit(f"compat shim: {name} must stay a hard requirement")

    print("compat shim: %d relaxed features, log lines present" % len(RELAXED))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: check_compat_relaxation.py <dxvk_device_info.cpp>")
    main(sys.argv[1])
