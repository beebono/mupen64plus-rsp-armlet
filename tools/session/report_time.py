#!/usr/bin/env python3
"""report_time.py: the level-1 timing tables of docs/profiling.md.

  report_time.py SESSIONDIR GAME...        SESSIONDIR = test-output/session/<dev>; GAME = the prefix of the A.<game> session (banjo, dk64, ...)

For each game the runs <game>.p1..pN (armlet-prof, ARMLET_PROF=1) and <game>.b1..bN (armlet, the normal build) of the session A.<game>
are read; a window is the difference of the two marks (snapshots M0, M1 of the profiler, MARK lines of the shim).

 time of one call to DoRspCycles (the shim's number, and the profiler's own):
   gfx task  = rsp-hle's overhead + parallelRSPDoRspCycles; the latter = JIT proper + RDP callback (the video plugin's ProcessRdpList, on the
               emulation thread) + CheckInterrupts + the profiler's own list parse (t_parse, overhead, shown separately)
   JIT proper = time in parallelRSPDoRspCycles - callback - CheckInterrupts - parse (JIT code, helpers, SP DMA, block lookup)
"""
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import profdata as pd

# JIT proper ms/frame of the earlier study (2026-10-09, before fork commit e317ef6): game -> (JIT proper, RDP callback, tasks/frame)
OLD = {"banjo": (5.18, 3.64, 0.45), "dk64": (4.51, 5.46, 0.49), "oot": (2.07, 1.89, 0.33), "mk64r": (3.66, 2.61, 0.51), "pm": (2.80, 3.33, 0.97),
       "sm64": (3.44, 2.37, 0.50), "snap": (0.17, 2.23, 0.50)}
NAMES = {"banjo": "Banjo-Kazooie", "dk64": "Donkey Kong 64", "oot": "Ocarina of Time", "mk64": "Mario Kart 64 (menu)", "mk64r": "Mario Kart 64 (race)", "pm": "Paper Mario", "sm64": "Super Mario 64",
         "snap": "Pokemon Snap"}
FAM = {1: "F3D", 2: "F3DEX", 3: "F3DEX2", 0: "?"}


def ms(x):
    return "%.2f" % x


def prof_run(run):
    """per-frame numbers of the profiler's window of a run"""
    prof = os.path.join(run, "prof")
    keys = pd.read_keys(prof)
    S = pd.read_stats(prof)
    fr = pd.read_frames(run)
    nf = fr[-1] - fr[0]
    m0, m1 = "M0", "M%d" % (len(fr) - 1)
    r = collections = None
    out = dict(frames=nf, gfx=dict(), audio=0.0, other=0.0, total=0.0, tasks=0.0, keys={})
    tot = dict(t_total=0.0, t_fwd=0.0, t_cb=0.0, t_parse=0.0, t_chk=0.0, t_lookup=0.0, t_compile=0.0, n_cb=0.0, tasks=0.0, calls=0.0)
    for k, ki in keys.items():
        d = pd.window(S, m0, m1, k)
        if not d or not d.get("calls"):
            continue
        out["total"] += d["t_total"] / 1e6 / nf
        if ki["type"] == 1:
            for n in tot:
                tot[n] += d.get(n, 0.0)
            out["keys"][k] = dict(ki, d=d)
        elif ki["type"] == 2:
            out["audio"] += d["t_total"] / 1e6 / nf
        else:
            out["other"] += d["t_total"] / 1e6 / nf
    f = lambda n: tot[n] / 1e6 / nf
    out["gfx"] = dict(total=f("t_total"), fwd=f("t_fwd"), cb=f("t_cb"), parse=f("t_parse"), chk=f("t_chk"), lookup=f("t_lookup"), compile=f("t_compile"),
                      hle=f("t_total") - f("t_fwd"), jit=f("t_fwd") - f("t_cb") - f("t_parse") - f("t_chk"), tasks=tot["tasks"] / nf,
                      n_cb=tot["n_cb"] / nf, calls=tot["calls"] / nf)
    return out


def shim_run(run):
    m = pd.read_marks(run)
    fr = pd.read_frames(run)
    if len(m) < len(fr):
        return None
    nf = fr[-1] - fr[0]
    a, b = m[0], m[-1]
    return dict(rsp=(b[1] - a[1]) / nf, emu=(b[2] - a[2]) / nf, proc=(b[3] - a[3]) / nf, calls=b[0] - a[0], frames=nf)


def mean_sd(v):
    return statistics.mean(v), (statistics.stdev(v) if len(v) > 1 else float("nan"))


def main():
    sdir = sys.argv[1]
    games = sys.argv[2:]
    t1 = ["| game | ucode | tasks/frame | gfx RSP ms/frame | = rsp-hle overhead | + JIT proper | + RDP callback | + CheckInterrupts | profiler parse (overhead) | block lookup + compile (inside JIT proper) | audio HLE ms/frame | JIT proper per task (us) | earlier study: JIT proper / RDP cb | JIT proper change |",
          "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    t2 = ["| game | n | RSP ms/frame normal build | RSP ms/frame profiler (level 1) | delta | delta % of RSP | emu-thread CPU ms/frame normal -> profiler | delta | parse share of the delta |",
          "|---|---|---|---|---|---|---|---|---|"]
    raw = []
    for g in games:
        d = os.path.join(sdir, "A." + g)
        if not os.path.isdir(d):
            continue
        ps, bs = [], []
        for t in sorted(os.listdir(d)):
            if t.startswith(g + ".p"):
                ps.append(t)
            elif t.startswith(g + ".b"):
                bs.append(t)
        P = [prof_run(os.path.join(d, t)) for t in ps]
        SP = [shim_run(os.path.join(d, t)) for t in ps]
        SB = [shim_run(os.path.join(d, t)) for t in bs]
        SP = [x for x in SP if x]
        SB = [x for x in SB if x]
        if not P:
            continue
        keys = P[0]["keys"]
        fam = FAM.get(next(iter(keys.values()))["family"], "?") if keys else "?"
        col = lambda f: mean_sd([f(p) for p in P])
        gfx = lambda n: col(lambda p: p["gfx"][n])
        tasks = gfx("tasks")[0]
        jit = gfx("jit")
        old = OLD.get(g)
        t1.append("| %s | %s | %.2f | %s +- %s | %s | %s +- %s | %s +- %s | %s | %s | %s | %s | %.0f | %s | %s |" % (
            NAMES.get(g, g), fam, tasks, ms(gfx("total")[0]), ms(gfx("total")[1]), ms(gfx("hle")[0]), ms(jit[0]), ms(jit[1]), ms(gfx("cb")[0]), ms(gfx("cb")[1]),
            ms(gfx("chk")[0]), ms(gfx("parse")[0]), ms(gfx("lookup")[0]), ms(col(lambda p: p["audio"])[0]), jit[0] * 1000 / max(tasks, 1e-9),
            ("%.2f / %.2f" % (old[0], old[1])) if old else "-", ("%+.2f (%+.0f %%)" % (jit[0] - old[0], 100 * (jit[0] - old[0]) / old[0])) if old else "-"))
        if SB and SP:
            rp, rb = mean_sd([x["rsp"] for x in SP]), mean_sd([x["rsp"] for x in SB])
            ep, eb = mean_sd([x["emu"] for x in SP]), mean_sd([x["emu"] for x in SB])
            delta = rp[0] - rb[0]
            parse = gfx("parse")[0]
            t2.append("| %s | %d + %d | %.3f +- %.3f | %.3f +- %.3f | %+.3f | %+.1f %% | %.3f -> %.3f | %+.3f | %.0f %% |" % (
                NAMES.get(g, g), len(SP), len(SB), rb[0], rb[1], rp[0], rp[1], delta, 100 * delta / rb[0], eb[0], ep[0], ep[0] - eb[0], 100 * parse / delta if delta > 0 else float("nan")))
        raw.append("%s: profiler runs RSP/emu ms/frame %s; normal runs %s; JIT proper per run %s" % (
            g, ["%.3f/%.3f" % (x["rsp"], x["emu"]) for x in SP], ["%.3f/%.3f" % (x["rsp"], x["emu"]) for x in SB], ["%.3f" % p["gfx"]["jit"] for p in P]))
    print("\n".join(t1))
    print()
    print("\n".join(t2))
    print()
    print("\n".join("<!-- %s -->" % r for r in raw))


if __name__ == "__main__":
    main()
