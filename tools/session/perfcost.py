#!/usr/bin/env python3
"""perfcost.py: the cost of an instruction class from the perf samples, checked against the measured time of the tasks.

  perfcost.py SESSIONDIR ANALYSISDIR GAME... [--out cost_perf.json]

perfmap.py (one result file per perf run, ANALYSISDIR/<game>.perf<n>.json) gives, per instruction class, the share of the emulation thread's JIT
samples and the share of the executed instructions: their ratio is the cost of one instruction of that class relative to the average. Pooled over the
runs and scaled so that the total matches the measured JIT proper time, that is a cost in ns per instruction; applied to the dynamic class counts of
every task of the F sessions (costmodel.py's joined rows) it predicts the task times, which are compared with the measurement per game: the
independent test that the sample-based costs describe the JIT, next to the regression of costmodel.py (whose classes are too collinear to fit).
"""
import argparse
import glob
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import costmodel as cm
import report_time as rt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sdir")
    ap.add_argument("adir")
    ap.add_argument("games", nargs="+")
    ap.add_argument("--out")
    a = ap.parse_args()
    samples = {c: 0.0 for c in cm.CLS}
    execs = {c: 0.0 for c in cm.CLS}
    nruns = 0
    for g in a.games:
        for f in sorted(glob.glob(os.path.join(a.adir, g + ".perf*.json"))):
            r = json.load(open(f))
            if "class_samples" not in r:
                continue
            # weight every run equally: samples and executions as shares of that run
            ts, te = sum(r["class_samples"].values()), sum(r["class_exec"].values())
            for c in cm.CLS:
                samples[c] += r["class_samples"].get(c, 0) / ts
                execs[c] += r["class_exec"].get(c, 0) / te
            nruns += 1
    if not nruns:
        print("no perf results")
        return 1
    rel = {c: (samples[c] / execs[c]) if execs[c] > 0.002 * nruns else None for c in cm.CLS}
    print("relative cost of an executed instruction by class, pooled over %d perf runs (1 = the average):" % nruns)
    print("   " + ", ".join("%s %.2f" % (c, v) for c, v in sorted(((c, v) for c, v in rel.items() if v), key=lambda x: -x[1])))
    # joined rows of all games
    allrows, gof = [], []
    for g in a.games:
        try:
            rows, inf = cm.joined(a.sdir, g)
        except Exception as e:
            continue
        allrows += rows
        gof += [g] * len(rows)
    X = np.array([r["cls"] for r in allrows], dtype=float)
    y = np.array([r["y"] for r in allrows], dtype=float)
    w = np.array([rel[c] if rel[c] else 1.0 for c in cm.CLS])
    base = X @ w
    scale = y.sum() / base.sum()
    cost = w * scale
    pred = base * scale
    print("scaled to the measured JIT proper time: %.2f ns per instruction on average" % (y.sum() / X.sum()))
    print("   ns per instruction: " + ", ".join("%s %.1f" % (c, v) for c, v in zip(cm.CLS, cost)))
    ss = ((y - y.mean()) ** 2).sum()
    print("   pooled over %d tasks: R2 %.3f, rmse %.0f us (%.1f %% of the mean task)" % (len(y), 1 - ((y - pred) ** 2).sum() / ss, np.sqrt(((y - pred) ** 2).mean()) / 1e3,
                                                                                      100 * np.sqrt(((y - pred) ** 2).mean()) / y.mean()))
    uni = X.sum(axis=1) * (y.sum() / X.sum())
    print("   (the uniform cost per instruction: R2 %.3f, rmse %.0f us)" % (1 - ((y - uni) ** 2).sum() / ss, np.sqrt(((y - uni) ** 2).mean()) / 1e3))
    print("\n| game | tasks | measured JIT proper per task (us) | predicted, perf-calibrated class costs | error | predicted, one cost for every instruction | error |")
    print("|---|---|---|---|---|---|---|")
    i = 0
    for g in dict.fromkeys(gof):
        n = gof.count(g)
        m, p, u = y[i:i + n].mean(), pred[i:i + n].mean(), uni[i:i + n].mean()
        print("| %s | %d | %.0f | %.0f | %+.1f %% | %.0f | %+.1f %% |" % (rt.NAMES.get(g, g), n, m / 1e3, p / 1e3, 100 * (p / m - 1), u / 1e3, 100 * (u / m - 1)))
        i += n
    if a.out:
        json.dump(dict(relative=rel, ns_per_instr={c: float(v) for c, v in zip(cm.CLS, cost)}, runs=nruns), open(a.out, "w"), indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
