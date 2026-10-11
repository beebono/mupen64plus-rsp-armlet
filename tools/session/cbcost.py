#!/usr/bin/env python3
"""cbcost.py: the cost of the RDP callback (the video plugin's ProcessRdpList, called from inside the RSP for every CMD_END write).

  cbcost.py SESSIONDIR GAME...

Per task of the fit run of session F.<game> (fresh graphics tasks inside the window):
    t_cb = a * calls + b * triangles + c * other commands + d                      (non-negative least squares, per game and pooled)
a is the fixed cost of one call (what coalescing the lists of a task into fewer calls could save), b the cost per triangle the plugin sets up,
c per other RDP command, d per task (waiting at SYNC_FULL for the plugin's worker threads, mostly). The fixed part includes the profiler's own two
clock reads around the call (about 2 x 15-25 ns on these devices).
The potential of coalescing: calls now and calls if every call carried at least 1 KiB of list (or ended a task), times a.
"""
import math
import os
import sys

import numpy as np
from scipy.optimize import nnls

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import profdata as pd
import report_time as rt


def rows_of(sdir, game):
    run = os.path.join(sdir, "F." + game, game + ".fit", "prof")
    keys = pd.read_keys(run)
    out = []
    for r in pd.read_tasks(run):
        if keys[int(r["key"])]["type"] != 1 or r["fresh"] != 1 or r["yield"] != 0 or r["phase"] != 1:
            continue
        out.append(r)
    return out


def fit(rows):
    y = np.array([r["t_cb_ns"] for r in rows])
    X = np.column_stack([[r["n_cb"] for r in rows], [r["rdp_tri"] for r in rows], [r["rdp_cmds"] - r["rdp_tri"] for r in rows], np.ones(len(rows))])
    sc = X.max(axis=0)
    sc[sc == 0] = 1
    c, _ = nnls(X / sc, y)
    c = c / sc
    pred = X @ c
    ss = ((y - y.mean()) ** 2).sum()
    return c, (1 - ((y - pred) ** 2).sum() / ss) if ss > 0 else float("nan"), X, y


BINS = ["<= 8 B", "<= 16", "<= 32", "<= 64", "<= 128", "<= 256", "<= 512", "<= 1024", "> 1024"]


def histogram(sdir, game):
    """callback calls and mean ns by list size, summed over the profiler runs of session A.<game> (windows M0..M1)"""
    d = os.path.join(sdir, "A." + game)
    n = {}
    ns = {}
    runs = 0
    if not os.path.isdir(d):
        return None
    for t in sorted(os.listdir(d)):
        if not t.startswith(game + ".p"):
            continue
        run = os.path.join(d, t)
        prof = os.path.join(run, "prof")
        keys = pd.read_keys(prof)
        S = pd.read_stats(prof)
        fr = pd.read_frames(run)
        for k, ki in keys.items():
            if ki["type"] != 1:
                continue
            w = pd.window(S, "M0", "M%d" % (len(fr) - 1), k)
            for b, v in w.get("cbh_n", {}).items():
                n[b] = n.get(b, 0) + v
            for b, v in w.get("cbh_ns", {}).items():
                ns[b] = ns.get(b, 0) + v
        runs += 1
    return n, ns, runs


def durations(sdir):
    """the callback duration distribution from session C: per game, for the lists of one non-triangle command (<= 8 B) and for the lists holding a
    triangle, the calls per frame, the median duration bin (a bin is a factor of two) and the share of the callback time spent in calls longer than 64 us
    (the waits for the video plugin's workers: SYNC_FULL)"""
    out = []
    for label in sorted(d for d in os.listdir(sdir) if d.startswith("C.") and os.path.isdir(os.path.join(sdir, d))):
        for t in sorted(os.listdir(os.path.join(sdir, label))):
            run = os.path.join(sdir, label, t)
            prof = os.path.join(run, "prof")
            if not os.path.exists(os.path.join(prof, "stats")):
                continue
            keys = pd.read_keys(prof)
            S = pd.read_stats(prof)
            fr = pd.read_frames(run)
            nf = fr[-1] - fr[0]
            npt = pd.read_info(prof)["ns_per_tick"]
            for k, ki in keys.items():
                if ki["type"] != 1:
                    continue
                w = pd.window(S, "M0", "M%d" % (len(fr) - 1), k)
                if not w.get("cbd"):
                    continue
                row = dict(game=t.split(".")[0], nf=nf, calls=w["n_cb"] / nf, cb_ms=w["t_cb"] / 1e6 / nf)
                for name, bins in (("tiny", [(0, 0)]), ("tri", [(b, 1) for b in range(9)]), ("other", [(b, 0) for b in range(1, 9)])):
                    cnt = {}
                    for b, tf in bins:
                        for j in range(32):
                            c = w["cbd"].get((b * 2 + tf) * 32 + j, 0)
                            if c:
                                cnt[j] = cnt.get(j, 0) + c
                    tot = sum(cnt.values())
                    if not tot:
                        continue
                    acc, med = 0, 0
                    for j in sorted(cnt):
                        acc += cnt[j]
                        if acc >= tot / 2:
                            med = j
                            break
                    lo = (2 ** (med - 1) if med else 0) * npt
                    hi = (2 ** med) * npt
                    big = sum(c * 1.5 * 2 ** (j - 1) * npt for j, c in cnt.items() if j >= 11)    # >= 2^10 ticks = 43 us
                    allt = sum(c * (1.5 * 2 ** (j - 1) if j else 0.5) * npt for j, c in cnt.items())
                    row[name] = (tot / nf, lo, hi, big / allt if allt else 0)
                out.append(row)
    return out


def saving(sdir, games, fixed_lo=0.29, fixed_hi=0.63):
    """what coalescing the lists of a task into calls of at least 1 KiB would save, ms per frame: (calls now - calls then) x the fixed cost of a call
    (the median of a call with one non-triangle command, 0.33-0.67 us, minus the profiler's two clock reads, about 40 ns). calls now from session C,
    calls then from the per-task list sizes of the fit run of session F"""
    cd = {r["game"]: r for r in durations(sdir)}
    out = []
    for g in games:
        r = cd.get(g)
        run = os.path.join(sdir, "F." + g, g + ".fit")
        if not r or not os.path.isdir(run):
            continue
        rows = rows_of(sdir, g)
        nf = pd.read_frames(run)
        nf = nf[-1] - nf[0]
        calls = sum(x["n_cb"] for x in rows) / nf
        co = sum(max(1, math.ceil(x["rdp_bytes"] / 1024)) for x in rows) / nf
        ad = os.path.join(sdir, "A." + g)
        emu = None
        if os.path.isdir(ad):
            B = [rt.shim_run(os.path.join(ad, t)) for t in sorted(os.listdir(ad)) if t.startswith(g + ".b")]
            B = [b for b in B if b]
            if B:
                emu = sum(b["emu"] for b in B) / len(B)
        out.append((g, calls, co, (calls - co) * fixed_lo / 1e3, (calls - co) * fixed_hi / 1e3, r["cb_ms"], emu))
    return out


def main():
    sdir = sys.argv[1]
    d = durations(sdir)
    if d:
        print("callback duration distribution (session C; calls per frame, median duration range in ns, share of the time spent in calls above ~43 us)\n")
        print("| game | callbacks/frame | callback ms/frame | one non-triangle command (<= 8 B): calls/frame, median, long-call share | lists with a triangle | other lists |")
        print("|---|---|---|---|---|---|")
        for r in d:
            cell = lambda n: ("%.0f / %.0f-%.0f ns / %.0f %%" % (r[n][0], r[n][1], r[n][2], 100 * r[n][3])) if n in r else "-"
            print("| %s | %.0f | %.2f | %s | %s | %s |" % (rt.NAMES.get(r["game"], r["game"]), r["calls"], r["cb_ms"], cell("tiny"), cell("tri"), cell("other")))
        print()
    sv = saving(sdir, sys.argv[2:])
    if sv:
        print("coalescing (calls of at least 1 KiB of list): calls per frame now -> then, the saving, and it as a share of the callback and of the emulation thread\n")
        print("| game | calls/frame now | coalesced | saving ms/frame (fixed cost 0.29-0.63 us per call) | share of the callback time | share of the emulation thread |")
        print("|---|---|---|---|---|---|")
        for g, c, co, lo, hi, cbm, emu in sv:
            print("| %s | %.0f | %.0f | %.2f-%.2f | %.0f-%.0f %% | %s |" % (rt.NAMES.get(g, g), c, co, lo, hi, 100 * lo / cbm, 100 * hi / cbm, ("%.1f-%.1f %%" % (100 * lo / emu, 100 * hi / emu)) if emu else "-"))


if __name__ == "__main__":
    main()
