#!/usr/bin/env python3
"""costmodel.py: fits the cost of an RSP instruction class (ns of JIT-proper time per executed instruction) to the measured time of the
graphics tasks, and writes it for ucodemap.py (--cost).

  costmodel.py SESSIONDIR GAME... [--out cost.json] [--groups]

For each game the session F.<game> holds two runs of the same frames: <game>.fit (ARMLET_PROF=2 with the task log: the time of every task,
JIT proper = time in parallelRSPDoRspCycles - RDP callback - CheckInterrupts - parse) and <game>.cnt (the --prof3 build, ARMLET_PROF=3 with
the task log: the dynamic instruction counts per class of every task). Emulation is deterministic (RandomizeInterrupt = False, the pad
script is keyed on controller polls), so task n of one run is task n of the other: the pairs are joined on (key, call number) and accepted
only when the walker's command / vertex / triangle counts of the two logs agree (and the RSP state hash streams of the runs are identical,
checked by compare_hash below).

    t_jit(task) = sum_class c_class * n_class + c_dma * dma_bytes + c_task        (non-negative least squares)

Tasks in which a region was compiled are left out (the cost of compiling is not steady state), and so are resumed tasks.
"""
import argparse
import json
import os
import sys

import numpy as np
from scipy.optimize import nnls

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import profdata as pd

CLS = ["vmul", "vmac", "vaddsub", "vcmp", "vlogic", "vdiv", "vsar", "vload", "vstore", "salu", "sload", "sstore", "branch", "cop0", "cop2mov", "other"]
GROUPS = [("vmul+vmac", ["vmul", "vmac"]), ("vaddsub+vcmp+vlogic+vsar", ["vaddsub", "vcmp", "vlogic", "vsar"]), ("vdiv", ["vdiv"]),
          ("vload+vstore", ["vload", "vstore"]), ("salu+branch", ["salu", "branch", "other"]), ("sload+sstore", ["sload", "sstore"]), ("cop0+cop2mov", ["cop0", "cop2mov"])]


def compare_hash(a, b):
    """the RSP state hash streams of two runs agree over their common prefix"""
    ha = open(os.path.join(a, "rsp.hash")).read().split("\n")[:-1]
    hb = open(os.path.join(b, "rsp.hash")).read().split("\n")[:-1]
    m = min(len(ha), len(hb))
    return ha[:m] == hb[:m], m


def joined(sdir, game):
    d = os.path.join(sdir, "F." + game)
    fit, cnt = os.path.join(d, game + ".fit"), os.path.join(d, game + ".cnt")
    ok, n = compare_hash(fit, cnt)
    tf = {(int(r["key"]), int(r["nth"])): r for r in pd.read_tasks(os.path.join(fit, "prof"))}
    tc = {(int(r["key"]), int(r["nth"])): r for r in pd.read_tasks(os.path.join(cnt, "prof"))}
    keys = pd.read_keys(os.path.join(fit, "prof"))
    rows = []
    mismatch = 0
    for k, a in tf.items():
        b = tc.get(k)
        if not b or keys[k[0]]["type"] != 1:
            continue
        if a["fresh"] != 1 or a["yield"] != 0 or a["phase"] != 1:
            continue
        if a["w_cmds"] is None or (a["w_cmds"], a["w_verts"], a["w_tris"]) != (b["w_cmds"], b["w_verts"], b["w_tris"]):
            mismatch += 1
            continue
        if a["t_compile_ns"] > 0:
            continue
        y = a["t_fwd_ns"] - a["t_cb_ns"] - a["t_parse_ns"] - a["t_chk_ns"]
        rows.append(dict(y=y, cls=[b[c] for c in CLS], dma=a["dma_rd_bytes"] + a["dma_wr_bytes"], enters=a["enters"], dyn=b["dyn"], cb=a["t_cb_ns"], n_cb=a["n_cb"],
                         total=a["t_total_ns"], cmds=a["w_cmds"], verts=a["w_verts"], tris=a["w_tris"], rdp_tri=a["rdp_tri"], clean=bool(a["w_clean"])))
    return rows, dict(hash_equal=ok, hash_calls=n, mismatch=mismatch, tasks=len(rows))


def design(rows, groups):
    cl = np.array([r["cls"] for r in rows], dtype=float)
    if groups:
        cols = [cl[:, [CLS.index(c) for c in cs]].sum(axis=1) for _, cs in GROUPS]
        names = [n for n, _ in GROUPS]
    else:
        cols = [cl[:, i] for i in range(len(CLS)) if i != CLS.index("other")]
        names = [c for c in CLS if c != "other"]
    cols.append(np.array([r["dma"] for r in rows], dtype=float))
    names.append("dma_bytes")
    cols.append(np.ones(len(rows)))
    names.append("task")
    return np.column_stack(cols), names


def prior(rows, names):
    """a uniform cost per instruction (the mean JIT-proper ns per executed instruction) for the classes, 0.1 ns per DMA byte, nothing per task"""
    y = sum(r["y"] for r in rows)
    n = sum(sum(r["cls"]) for r in rows)
    c0 = np.full(len(names), y / n)
    c0[names.index("dma_bytes")] = 0.1
    c0[names.index("task")] = 0.0
    return c0


def solve(X, y, c0, lam):
    """min |X c - y|^2 + lam * sum_k rms_k^2 (c_k - c0_k)^2, c >= 0 (the penalty is the prediction change a deviation from the prior makes)"""
    rms = np.sqrt((X ** 2).mean(axis=0))
    rms[rms == 0] = 1
    Xs = X / rms
    A = np.vstack([Xs, np.sqrt(lam * len(y)) * np.eye(X.shape[1])])
    b = np.concatenate([y, np.sqrt(lam * len(y)) * c0 * rms])
    c, _ = nnls(A, b)
    return c / rms


def fit(rows, groups, lam=None, game_of=None):
    """ridge-regularised non-negative least squares towards a uniform cost per instruction. lam: None = chosen by leave-one-game-out (or, with one
    game, 4-block) cross-validation over a grid"""
    X, names = design(rows, groups)
    y = np.array([r["y"] for r in rows], dtype=float)
    c0 = prior(rows, names)
    cv = None
    if lam is None:
        grid = [0.0, 0.003, 0.01, 0.03, 0.1, 0.3, 1.0, 3.0, 10.0, 100.0]
        gs = np.array(game_of if game_of else [i * 4 // len(rows) for i in range(len(rows))])
        folds = sorted(set(gs))
        cv = {}
        for l in grid:
            err = 0.0
            for f in folds:
                tr, te = gs != f, gs == f
                c = solve(X[tr], y[tr], prior([r for r, m in zip(rows, tr) if m], names), l)
                err += ((X[te] @ c - y[te]) ** 2).sum()
            cv[l] = float(np.sqrt(err / len(y)))
        lam = min(cv, key=cv.get)
    c = solve(X, y, c0, lam)
    pred = X @ c
    ss = ((y - y.mean()) ** 2).sum()
    r2 = 1 - ((y - pred) ** 2).sum() / ss if ss > 0 else float("nan")
    rmse = float(np.sqrt(((y - pred) ** 2).mean()))
    return dict(names=names, coef=c, r2=r2, rmse=rmse, mean_y=float(y.mean()), n=len(rows), total_err=float((pred.sum() - y.sum()) / y.sum()), lam=lam, cv=cv,
                prior=float(c0[0]), mean_contrib={n: float((X[:, i] * c[i]).mean()) for i, n in enumerate(names)})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sdir")
    ap.add_argument("games", nargs="+")
    ap.add_argument("--out")
    ap.add_argument("--groups", action="store_true", help="fit 7 instruction groups instead of 15 classes")
    a = ap.parse_args()
    allrows = []
    gof = []
    info = {}
    per = {}
    for g in a.games:
        rows, inf = joined(a.sdir, g)
        info[g] = inf
        if len(rows) < 30:
            print("%s: only %d joined tasks (%s), skipped" % (g, len(rows), inf))
            continue
        per[g] = fit(rows, a.groups)
        allrows += rows
        gof += [g] * len(rows)
        f = per[g]
        print("== %s: %d tasks (hash streams %s over %d calls, %d walker mismatches), mean JIT proper %.0f us, in-sample R2 %.3f, rmse %.0f us (%.1f %%), total error %+.2f %%" % (
            g, f["n"], "identical" if inf["hash_equal"] else "DIFFER", inf["hash_calls"], inf["mismatch"], f["mean_y"] / 1e3, f["r2"], f["rmse"] / 1e3, 100 * f["rmse"] / f["mean_y"],
            100 * f["total_err"]))
        print("   ns per instruction: " + ", ".join("%s %.1f" % (n, c) for n, c in zip(f["names"], f["coef"])))
    if len(per) > 1:
        f = fit(allrows, a.groups, game_of=gof)
        print("== pooled over %d games: %d tasks, R2 %.3f, rmse %.0f us (%.1f %% of the mean task), total error %+.2f %%" % (len(per), f["n"], f["r2"], f["rmse"] / 1e3,
                                                                                                                     100 * f["rmse"] / f["mean_y"], 100 * f["total_err"]))
        print("   regularisation %.3g (leave-one-game-out error by lambda: %s); prior %.2f ns per instruction" % (f["lam"], ", ".join("%g: %.0f us" % (k, v / 1e3) for k, v in f["cv"].items()), f["prior"]))
        print("   ns per instruction: " + ", ".join("%s %.2f" % (n, c) for n, c in zip(f["names"], f["coef"])))
        print("   mean contribution per task (us): " + ", ".join("%s %.1f" % (n, v / 1e3) for n, v in f["mean_contrib"].items()))
        # residual of the pooled model per game
        X, names = design(allrows, a.groups)
        y = np.array([r["y"] for r in allrows])
        pred = X @ f["coef"]
        i = 0
        for g in per:
            n = per[g]["n"]
            print("   pooled model on %-6s: predicted %.0f us vs measured %.0f us per task (%+.1f %%)" % (g, pred[i:i + n].mean() / 1e3, y[i:i + n].mean() / 1e3,
                                                                                                      100 * (pred[i:i + n].mean() / y[i:i + n].mean() - 1)))
            i += n
        per["pooled"] = f
    if a.out:
        out = {}
        for k, f in per.items():
            d = dict(zip(f["names"], [float(c) for c in f["coef"]]))
            if a.groups:                       # one entry per class, as ucodemap.py wants it
                for gname, cs in GROUPS:
                    for c in cs:
                        d[c] = d[gname]
            d.update(_r2=f["r2"], _rmse_ns=f["rmse"], _n=f["n"], _lambda=f["lam"])
            out[k] = d
        json.dump(out, open(a.out, "w"), indent=1)


if __name__ == "__main__":
    main()
