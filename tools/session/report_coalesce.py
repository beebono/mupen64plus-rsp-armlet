#!/usr/bin/env python3
"""report_coalesce.py: the RDP-list coalescing tables of docs/testing.md.

  report_coalesce.py [--labels REGEX] SESSIONDIR SCENE...        SESSIONDIR = test-output/session/<dev>; SCENE = the prefix of the tags (banjo, mk64r, snap, ...)

Every run directory under SESSIONDIR/*/ whose tag is SCENE.CONFIG.N is read (CONFIG: off, on, t<bytes> (on with another threshold), ref, poff, pon (the
profiler build, ARMLET_PROF=1), foff, fon (foir with FOIR_FRAMELOG)). The window of a run is the difference of its first and last mark (VI frames).
  off / on / ref / t*   RSP ms/frame (the shim: time inside DoRspCycles) and emulation-thread CPU ms/frame (CLOCK_THREAD_CPUTIME_ID at the marks)
  poff / pon            callbacks/frame (calls of the video plugin's ProcessRdpList that the profiler counts), their time, graphics RSP ms/frame
  foff / fon / ft<bytes>  lists/frame (foir's own count), frame time p50 / p95 / p99 (swap to swap) in the window
"""
import os
import re
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import profdata as pd
import framelog_read as fl


def mean_sd(v):
    return statistics.mean(v), (statistics.stdev(v) if len(v) > 1 else float("nan"))


def fmt(v, d=3):
    m, s = mean_sd(v)
    return "%.*f +- %.*f" % (d, m, d, s) if len(v) > 1 else "%.*f" % (d, m)


def runs(sdir, labels):
    r = {}
    for lab in sorted(os.listdir(sdir)):
        if not re.match(labels, lab):
            continue
        p = os.path.join(sdir, lab)
        if not os.path.isdir(p):
            continue
        for t in sorted(os.listdir(p)):
            m = re.match(r"([a-z0-9]+)\.([a-z0-9]+)\.(\d+)$", t)
            if m and os.path.isdir(os.path.join(p, t)):
                r.setdefault((m.group(1), m.group(2)), []).append(os.path.join(p, t))
    return r


def shim(run):
    m = pd.read_marks(run)
    fr = pd.read_frames(run)
    if len(m) < len(fr):
        return None
    nf = fr[-1] - fr[0]
    a, b = m[0], m[-1]
    return dict(rsp=(b[1] - a[1]) / nf, emu=(b[2] - a[2]) / nf, proc=(b[3] - a[3]) / nf, calls=b[0] - a[0])


def prof(run):
    pr = os.path.join(run, "prof")
    keys = pd.read_keys(pr)
    S = pd.read_stats(pr)
    fr = pd.read_frames(run)
    nf = fr[-1] - fr[0]
    m0, m1 = "M0", "M%d" % (len(fr) - 1)
    t = dict(n_cb=0.0, t_cb=0.0, t_fwd=0.0, tasks=0.0)
    for k, ki in keys.items():
        if ki["type"] == 1:
            d = pd.window(S, m0, m1, k)
            for n in t:
                t[n] += d.get(n, 0.0)
    return dict(cb=t["n_cb"] / nf, cb_ms=t["t_cb"] / 1e6 / nf, fwd_ms=t["t_fwd"] / 1e6 / nf, tasks=t["tasks"] / nf)


def flog(run):
    fr = pd.read_frames(run)
    recs = fl.read(os.path.join(run, "framelog.bin"))
    recs = [r for r in recs if fr[0] < r["frame"] <= fr[-1]]
    dt = sorted((b["t_leave"] - a["t_leave"]) / 1e6 for a, b in zip(recs, recs[1:]))
    if not dt:
        return None
    n = len(dt)
    pc = lambda p: dt[min(n - 1, int(n * p / 100))]
    return dict(lists=sum(r["lists"] for r in recs) / len(recs), p50=pc(50), p95=pc(95), p99=pc(99), max=dt[-1], over33=100.0 * sum(x > 33.3 for x in dt) / n,
                cpu=sum(r["thr_cpu"] for r in recs) / 1e6 / len(recs), cb=sum(r["cb"] for r in recs) / 1e6 / len(recs))


def main():
    args = sys.argv[1:]
    labels = ".*"
    if args[0] == "--labels":                   # only the sessions whose label matches (e.g. '(t|p|f)\\.' for the main sessions, 'sw\\.|fl\\.' for the threshold sweep)
        labels, args = args[1], args[2:]
    sdir = args[0]
    R = runs(sdir, labels)
    for scene in args[1:]:
        cfgs = sorted({c for (s, c) in R if s == scene})
        print("\n### %s" % scene)
        print("| config | runs | RSP ms/frame | emu thread ms/frame | process ms/frame | RSP calls (window) |")
        print("|---|---|---|---|---|---|")
        for c in cfgs:
            if c in ("off", "on", "ref") or re.match(r"t\d+$", c):
                v = [x for x in (shim(r) for r in R[(scene, c)]) if x]
                if v:
                    print("| %s | %d | %s | %s | %s | %s |" % (c, len(v), fmt([x["rsp"] for x in v]), fmt([x["emu"] for x in v]), fmt([x["proc"] for x in v], 2),
                                                          "/".join(sorted({str(x["calls"]) for x in v}))))
        for c in cfgs:
            if c in ("poff", "pon"):
                v = [prof(r) for r in R[(scene, c)]]
                print("\nprofiler %s (%d runs): callbacks/frame %s, callback ms/frame %s, graphics tasks/frame %s, gfx RSP ms/frame (profiler) %s" % (
                    c, len(v), fmt([x["cb"] for x in v], 1), fmt([x["cb_ms"] for x in v]), fmt([x["tasks"] for x in v], 2), fmt([x["fwd_ms"] for x in v])))
        for c in cfgs:
            if re.match(r"f(off|on|t\d+)$", c):
                v = [x for x in (flog(r) for r in R[(scene, c)]) if x]
                if v:
                    print("\nframelog %s (%d runs): lists/frame %s, frame time p50 %s p95 %s p99 %s max %s ms, >33.3 ms %s %%, emu thread CPU %s ms/frame, ProcessRDPList %s ms/frame" % (
                        c, len(v), fmt([x["lists"] for x in v], 1), fmt([x["p50"] for x in v], 2), fmt([x["p95"] for x in v], 2), fmt([x["p99"] for x in v], 2),
                        fmt([x["max"] for x in v], 1), fmt([x["over33"] for x in v], 1), fmt([x["cpu"] for x in v], 2), fmt([x["cb"] for x in v], 2)))


if __name__ == "__main__":
    main()
