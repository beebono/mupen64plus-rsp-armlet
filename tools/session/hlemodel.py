#!/usr/bin/env python3
"""hlemodel.py: what native (HLE) graphics code would save, per game, with a parametric native cost model.

  hlemodel.py SESSIONDIR UCODEMAP_DIR GAME... [--ghz 1.58] [--json out.json]

Inputs, per game: the level-1 timing runs (session A.<game>: JIT proper, RDP callback ms/frame), the walker's workload per frame of the
fit run (session F.<game>: commands, vertices, triangles submitted / sent to the RDP, matrices), and ucodemap's routine shares
(UCODEMAP_DIR/<game>.map.json, cost-weighted: the fitted ns per instruction class applied to the dynamic counts).

Strategies (ms per frame saved on the emulation thread; the RDP callback, rsp-hle's overhead and audio are the same in all of them):
  now          the JIT runs everything
  top-N        the N routines (ucodemap categories) with the largest net saving are replaced by native code through a PC-entry hook in front of
               the JIT; the rest of the ucode still runs on the JIT. Each native call costs the hook overhead on top of the native code.
  whole DL     a native handler runs the whole display list (every routine native, one call per task)
Native cost parameters are cycles per event (low / mid / high) at --ghz, with NEON assumed for vertex transform and lighting; the events come from
the walker. They are the parameters of the earlier study's model (hle_model.py) re-based on NEON vertex code:

  vertex 70 / 110 / 180      submitted triangle 25 / 40 / 70      triangle sent to the RDP 250 / 400 / 700      matrix command 150 / 300 / 600
  RDP command copied 8 / 12 / 25      state command 8 / 12 / 25      display-list command 10 / 15 / 30      dispatch per command 4 / 6 / 12      task 5k / 10k / 30k
  hook overhead per native routine call 80 / 150 / 300 ns
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import profdata as pd
import report_time as rt

P = {"low": dict(v=70, ts=25, te=250, rdp=8, state=8, dl=10, disp=4, m=150, task=5000, hook=80),
     "mid": dict(v=110, ts=40, te=400, rdp=12, state=12, dl=15, disp=6, m=300, task=10000, hook=150),
     "high": dict(v=180, ts=70, te=700, rdp=25, state=25, dl=30, disp=12, m=600, task=30000, hook=300)}
HANDLERS = ("vertex", "triangle", "matrix", "rdp", "state", "dl")      # what a native routine can replace; dispatch, DMA and the rest stay with the JIT unless the whole DL is native
# which categories each native routine would cost: (category) -> list of (event, weight key)
CATEGORY_EVENTS = {"vertex": "v", "triangle": None, "matrix": "m"}


def workload(sdir, game):
    run = os.path.join(sdir, "F." + game, game + ".fit")
    prof = os.path.join(run, "prof")
    keys = pd.read_keys(prof)
    S = pd.read_stats(prof)
    fr = pd.read_frames(run)
    nf = fr[-1] - fr[0]
    m0, m1 = "M0", "M%d" % (len(fr) - 1)
    tot = dict(cmds=0, verts=0, tris=0, mtx=0, tasks=0, rdp_tri=0, n_cb=0, rdp_bytes=0, rdp_cmds=0, vtxloads=0, rdpcmds=0, dlcmds=0, tricmds=0, mtxcmds=0)
    for k, ki in keys.items():
        if ki["type"] != 1:
            continue
        d = pd.window(S, m0, m1, k)
        w = d.get("walk", {})
        tot["cmds"] += w.get("cmds", 0)
        tot["verts"] += w.get("verts", 0)
        tot["vtxloads"] += w.get("vtxloads", 0)
        tot["tris"] += w.get("tris", 0)
        tot["mtx"] += w.get("mtx", 0)
        tot["rdpcmds"] += w.get("rdpcmds", 0)
        ops = d.get("ops", {})
        fam = ki["family"]
        tot["dlcmds"] = tot.get("dlcmds", 0) + w.get("dl", 0) + w.get("culldl", 0) + w.get("branchz", 0) + ops.get(0xDF if fam == 3 else 0xB8, 0)
        tot["tricmds"] = tot.get("tricmds", 0) + (sum(ops.get(o, 0) for o in (5, 6, 7, 8)) if fam == 3 else sum(ops.get(o, 0) for o in (0xBF, 0xB1, 0xB5)))
        tot["mtxcmds"] = tot.get("mtxcmds", 0) + w.get("mtx", 0) + w.get("pop", 0) + w.get("movemem", 0)
        tot["tasks"] += d.get("tasks", 0)
        tot["rdp_tri"] += d.get("rdp_tri", 0)
        tot["n_cb"] += d.get("n_cb", 0)
        tot["rdp_bytes"] += d.get("rdp_bytes", 0)
        tot["rdp_cmds"] += d.get("rdp_cmds", 0)
    return {k: v / nf for k, v in tot.items()}, nf


def native_ms(w, p, ghz):
    """native code per frame, by routine: (events x cycles) / ghz in ms"""
    c = lambda cyc: cyc / ghz / 1e6
    state_cmds = max(w["cmds"] - w["vtxloads"] - w["tricmds"] - w["mtxcmds"] - w["rdpcmds"] - w["dlcmds"], 0)
    return dict(vertex=c(w["verts"] * p["v"] + w["vtxloads"] * 12), triangle=c(w["tris"] * p["ts"] + w["rdp_tri"] * p["te"]), matrix=c(w["mtxcmds"] * p["m"]),
                rdp=c(w["rdpcmds"] * p["rdp"]), state=c(state_cmds * p["state"]), dl=c(w["dlcmds"] * p["dl"]), dispatch=c(w["cmds"] * p["disp"] + w["tasks"] * p["task"]))


def calls(w, k):
    """native routine calls per frame (each costs the hook overhead when the rest of the ucode stays on the JIT)"""
    state_cmds = max(w["cmds"] - w["vtxloads"] - w["tricmds"] - w["mtxcmds"] - w["rdpcmds"] - w["dlcmds"], 0)
    return dict(vertex=w["vtxloads"], triangle=w["tricmds"], matrix=w["mtxcmds"], rdp=w["rdpcmds"], state=state_cmds, dl=w["dlcmds"]).get(k, 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sdir")
    ap.add_argument("mapdir")
    ap.add_argument("games", nargs="+")
    ap.add_argument("--ghz", type=float, default=1.58)
    ap.add_argument("--json")
    a = ap.parse_args()
    rows = []
    out = {}
    for g in a.games:
        pr = [rt.prof_run(os.path.join(a.sdir, "A." + g, t)) for t in sorted(os.listdir(os.path.join(a.sdir, "A." + g))) if t.startswith(g + ".p")]
        if not pr:
            continue
        jit = sum(p["gfx"]["jit"] for p in pr) / len(pr)
        cb = sum(p["gfx"]["cb"] for p in pr) / len(pr)
        gfx = sum(p["gfx"]["total"] for p in pr) / len(pr)
        w, nf = workload(a.sdir, g)
        um = json.load(open(os.path.join(a.mapdir, g + ".map.json")))
        um = [r for r in um if "categories" in r][0]
        tc = sum(v["cost"] for v in um["categories"].values())
        share_model = {k: v["cost"] / tc for k, v in um["categories"].items()}
        share = dict(share_model)
        pfiles = [f for f in sorted(os.listdir(a.mapdir)) if f.startswith(g + ".perf") and f.endswith(".json")]
        pshare = {}
        for f in pfiles:
            r = json.load(open(os.path.join(a.mapdir, f)))
            for k, v in r.get("perf_share", {}).items():
                pshare[k] = pshare.get(k, 0) + v / len(pfiles)
        if pshare:
            share = pshare                     # time-weighted: the perf samples of the emulation thread, mapped to routines
        ranked = [k for k in sorted(share, key=lambda k: -share[k]) if k in HANDLERS]
        res = dict(jit=jit, cb=cb, gfx=gfx, share=share, share_model=share_model, share_perf=pshare, workload=w, ranked=ranked, strategies={})
        for pn, p in P.items():
            nat = native_ms(w, p, a.ghz)
            strat = {"whole DL": jit - sum(nat.values())}
            net = {k: share.get(k, 0) * jit - nat[k] - calls(w, k) * p["hook"] / 1e6 for k in HANDLERS}   # what replacing one routine saves, with the hook
            order = sorted(net, key=lambda k: -net[k])
            for N in (1, 2, 3):
                strat["top-%d" % N] = sum(max(net[k], 0) for k in order[:N])
            res["strategies"][pn] = strat
        out[g] = res
        rows.append((g, res))
    print("(routine shares: %s)\n" % ("perf samples" if any(r["share_perf"] for _, r in rows) else "instruction counts weighted by the fitted class costs"))
    print("| game | JIT proper now | RDP cb | share: %s | native whole DL (low / mid / high) | saving top-1 (routines) | top-2 | top-3 | whole DL |" % " / ".join(["vertex", "triangle", "matrix", "rdp", "state", "dl", "dispatch"]))
    print("|---|---|---|---|---|---|---|---|---|")
    for g, r in rows:
        s = r["share"]
        nat = {pn: sum(native_ms(r["workload"], p, a.ghz).values()) for pn, p in P.items()}
        sv = r["strategies"]
        print("| %s | %.2f | %.2f | %s | %.2f / %.2f / %.2f | %s | %s | %s | %s |" % (
            rt.NAMES.get(g, g), r["jit"], r["cb"], " / ".join("%.0f%%" % (100 * s.get(k, 0)) for k in ("vertex", "triangle", "matrix", "rdp", "state", "dl", "dispatch")),
            nat["low"], nat["mid"], nat["high"],
            *["%.2f (%.2f..%.2f)" % (sv["mid"]["top-%d" % n], sv["high"]["top-%d" % n], sv["low"]["top-%d" % n]) for n in (1, 2, 3)],
            "%.2f (%.2f..%.2f)" % (sv["mid"]["whole DL"], sv["high"]["whole DL"], sv["low"]["whole DL"])))
    if a.json:
        json.dump(out, open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
