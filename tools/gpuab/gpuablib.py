"""Pure helpers of gpuab.py (host-testable, no adb)."""
import re, statistics as st

def rotate(variants, r):
    """Variant order for round r: rotated so no variant is always first or last."""
    k = r % len(variants)
    return variants[k:] + variants[:k]

def quantile(x, f):
    x = sorted(x); k = (len(x) - 1) * f; lo = int(k); hi = min(lo + 1, len(x) - 1)
    return x[lo] + (x[hi] - x[lo]) * (k - lo)

def iqr(x): return quantile(x, .75) - quantile(x, .25)

def parse(txt, stop_temp):
    """RUN/fps/SUM/END log -> {(scene, variant): [(round, gpu_ms, fps)]}.
    A run counts only when its RUN header is complete (an empty temp= clears the current run, so later fps/SUM lines of it are not
    filed under the previous variant), it has an END line with a temperature, and the temperature at start AND at END is <= stop_temp.
    A scene-round in which ANY run was hotter (or whose temperature could not be read) is dropped for ALL variants, so every variant's
    median/IQR and the paired ratios use the same rounds (a hot round is slower and would bias the variant that happens to lose it)."""
    def norm(t): return t / 10 if t > 100 else t
    runs, hot = [], set(); cur = None; fps = None; gpu = None
    for line in txt.split("\n"):
        m = re.search(r"RUN (\S+) (\S+) (\d+) temp=(\d*)", line)
        if m:
            cur = fps = gpu = None                       # every RUN boundary invalidates the previous run first
            if not m[4]:
                hot.add((m[2], int(m[3])))               # temperature unreadable: the round is not trustworthy
                continue
            t = norm(int(m[4])); cur = (m[1], m[2], int(m[3]), t)
            if t > stop_temp: hot.add((m[2], int(m[3])))
            continue
        if cur is None: continue
        m = re.search(r'"fps":([0-9.]+)', line)
        if m: fps = float(m[1])
        m = re.search(r"SUM gpu=([0-9.]+)", line)
        if m: gpu = float(m[1])
        m = re.search(r"END temp=(\d*)", line)
        if m:
            if not m[1] or norm(int(m[1])) > stop_temp: hot.add((cur[1], cur[2]))
            elif fps is not None and gpu is not None: runs.append((cur[1], cur[0], cur[2], gpu, fps))
            cur = fps = gpu = None
    res = {}
    for sc, v, r, g, f in runs:
        if (sc, r) not in hot: res.setdefault((sc, v), []).append((r, g, f))
    return res

def chunk_units(units, per_unit_s, cap=570, overhead=60):
    """Split `units` (scene-rounds) into lists whose guest call stays within `cap` seconds (droiddeckctl guest allows <= 600 s per call;
    qdeqp.py caps at 570).  A single unit longer than the cap still gets its own chunk."""
    n = max(1, (cap - overhead) // per_unit_s)
    return [units[i:i + n] for i in range(0, len(units), n)]

def summarize(res, scenes, names):
    """-> {(scene, variant): dict(n, med, iqr, fps, ratio)}; ratio = median over rounds of gpu / gpu of names[0] in the same round."""
    out = {}
    for sc in scenes:
        b = {r: g for r, g, f in res.get((sc, names[0]), [])}
        for n in names:
            rows = res.get((sc, n), [])
            if not rows: continue
            g = [x[1] for x in rows]
            pr = [x[1] / b[x[0]] for x in rows if b.get(x[0])]
            out[(sc, n)] = dict(n=len(g), med=st.median(g), iqr=iqr(g), fps=st.median(x[2] for x in rows),
                                ratio=st.median(pr) if pr else float("nan"))
    return out
