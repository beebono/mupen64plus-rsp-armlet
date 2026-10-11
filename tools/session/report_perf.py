#!/usr/bin/env python3
"""report_perf.py: routine shares of the graphics JIT time from the perf samples, next to the shares from the instruction counts and from the cost model.

  report_perf.py ANALYSISDIR SESSIONDIR GAME...

ANALYSISDIR holds, per game, <game>.perf<n>.json (perfmap.py, perf share and the shares of the cost model it was compared with: the regression of costmodel.py,
the one in the file it was run with) and <game>.map.json (report_routines.py, with whichever cost file it was run with: that is the file the `cost-weighted` rows come from).
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import report_time as rt

CATS = ["vertex", "triangle", "matrix", "dl", "state", "rdp", "dispatch", "dma", "rdp-submit", "task init"]


def main():
    d = sys.argv[1]
    sdir = sys.argv[2]
    games = sys.argv[3:]
    rows = []
    t2 = ["| game | perf runs | emulation-thread samples | JIT code + stubs + helpers, share of the thread (perf) | JIT proper share of the thread (timing) | perf vs the regression (total variation distance) | perf vs plain instruction counts |", "|---|---|---|---|---|---|---|"]
    print("| game | basis | " + " | ".join(CATS) + " |")
    print("|---|---|" + "---|" * len(CATS))
    for g in games:
        pf = sorted(f for f in os.listdir(d) if f.startswith(g + ".perf") and f.endswith(".json"))
        if not pf:
            continue
        R = [json.load(open(os.path.join(d, f))) for f in pf]
        R = [r for r in R if "perf_share" in r]
        if not R:
            continue
        perf = {c: sum(r["perf_share"].get(c, 0) for r in R) / len(R) for c in CATS}
        um = [r for r in json.load(open(os.path.join(d, g + ".map.json"))) if "categories" in r][0]
        tc = sum(v["cost"] for v in um["categories"].values())
        ti = um["total_instr"]
        cw = {c: um["categories"].get(c, {}).get("cost", 0) / tc for c in CATS}
        ic = {c: um["categories"].get(c, {}).get("instr", 0) / ti for c in CATS}
        nm = rt.NAMES.get(g, g)
        print("| %s | instruction counts | %s |" % (nm, " | ".join("%.1f" % (100 * ic[c]) for c in CATS)))
        print("| | cost-weighted (perf-calibrated class costs) | %s |" % " | ".join("%.1f" % (100 * cw[c]) for c in CATS))
        print("| | **perf samples** | %s |" % " | ".join("**%.1f**" % (100 * perf[c]) for c in CATS))
        ad = os.path.join(sdir, "A." + g)
        tshare = "-"
        if os.path.isdir(ad):
            P = [rt.prof_run(os.path.join(ad, t)) for t in sorted(os.listdir(ad)) if t.startswith(g + ".p")]
            B = [rt.shim_run(os.path.join(ad, t)) for t in sorted(os.listdir(ad)) if t.startswith(g + ".b")]
            if P and B:
                tshare = "%.1f %%" % (100 * sum(p["gfx"]["jit"] for p in P) / len(P) / (sum(b["emu"] for b in B) / len(B)))
        t2.append("| %s | %d | %d | %.1f %% | %s | %.3f | %.3f |" % (nm, len(R), sum(r["emu_samples"] for r in R), 100 * sum(r["routine_samples"] / r["emu_samples"] for r in R) / len(R), tshare,
                                                              sum(r["l1_perf_vs_model"] for r in R) / len(R), sum(r["l1_perf_vs_instr"] for r in R) / len(R)))
    print()
    print("\n".join(t2))


if __name__ == "__main__":
    main()
