#!/usr/bin/env python3
"""report_snap.py: what the emulation thread does in the Pokemon Snap attract-demo zoom shot (no input, VI frames 1100..1340).

  report_snap.py SESSIONDIR

From session A.snap: runs snap.b1.. (the normal module, the shim's emulation-thread CPU time and RSP time per frame) and snap.p1.. (the profiler:
per task type and the split of a graphics task into JIT proper / RDP callback / rsp-hle overhead), and foir's own log of the window (wall ms per frame,
its callback time and how much of it is waiting at SYNC_FULL, the CPU time of its worker threads).
"""
import os
import re
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import profdata as pd
import report_time as rt


def foir_window(run, f0, f1):
    """averages of foir's per-120-frame lines whose end frame lies inside (f0, f1 + 119]"""
    rows = []
    cur = None
    for l in open(os.path.join(run, "foir.log"), errors="replace"):
        m = re.match(r"frame (\d+): ([\d.]+) ms/frame wall, ([\d.]+) ms/frame RDP \(([\d.]+) parse\+setup of which ([\d.]+) waiting at SYNC_FULL", l)
        if m:
            cur = dict(frame=int(m.group(1)), wall=float(m.group(2)), rdp=float(m.group(3)), wait=float(m.group(5)))
            continue
        m = re.match(r"cpu: ([\d.]+) ms/frame in the process \(([\d.]+) on the emulation thread, ([\d.]+) on the other threads\)", l)
        if m and cur:
            cur.update(proc=float(m.group(1)), emu=float(m.group(2)), workers=float(m.group(3)))
            if f0 < cur["frame"] <= f1 + 1:
                rows.append(cur)
            cur = None
    return rows


def main():
    d = os.path.join(sys.argv[1], "A.snap")
    runs = sorted(os.listdir(d))
    P = [rt.prof_run(os.path.join(d, t)) for t in runs if t.startswith("snap.p")]
    B = [rt.shim_run(os.path.join(d, t)) for t in runs if t.startswith("snap.b")]
    SP = [rt.shim_run(os.path.join(d, t)) for t in runs if t.startswith("snap.p")]
    fr = pd.read_frames(os.path.join(d, runs[0]))
    ms = lambda v: statistics.mean(v)
    emu_b = ms([x["emu"] for x in B])
    rsp_b = ms([x["rsp"] for x in B])
    g = lambda n: ms([p["gfx"][n] for p in P])
    audio = ms([p["audio"] for p in P])
    other = ms([p["other"] for p in P])
    print("Pokemon Snap, VI frames %d..%d, no input; %d + %d runs (normal module + profiler), rgdsplus-class numbers are per frame\n" % (fr[0], fr[-1], len(B), len(P)))
    rows = []
    for t in runs:
        if t.startswith("snap.b"):
            rows += foir_window(os.path.join(d, t), fr[0], fr[-1])
    wall = ms([r["wall"] for r in rows]) if rows else float("nan")
    cb_foir = ms([r["rdp"] for r in rows]) if rows else float("nan")
    wait = ms([r["wait"] for r in rows]) if rows else float("nan")
    workers = ms([r["workers"] for r in rows]) if rows else float("nan")
    print("| item | ms/frame | share of the emulation thread |")
    print("|---|---|---|")
    print("| emulation thread CPU (shim, normal module) | %.2f | 100 %% |" % emu_b)
    print("| of which in DoRspCycles (shim) | %.2f | %.0f %% |" % (rsp_b, 100 * rsp_b / emu_b))
    print("| - graphics tasks: JIT proper | %.2f | %.0f %% |" % (g("jit"), 100 * g("jit") / emu_b))
    print("| - graphics tasks: the video plugin's RDP callback (foir: list parse, setup, waiting) | %.2f | %.0f %% |" % (g("cb"), 100 * g("cb") / emu_b))
    print("| - graphics tasks: rsp-hle overhead + CheckInterrupts | %.2f | %.0f %% |" % (g("hle") + g("chk"), 100 * (g("hle") + g("chk")) / emu_b))
    print("| - audio HLE (type 2 tasks) | %.2f | %.0f %% |" % (audio, 100 * audio / emu_b))
    print("| - other tasks and non-tasks | %.2f | %.0f %% |" % (other, 100 * other / emu_b))
    print("| the rest of the emulation thread (core: CPU, devices, VI, and the profiler-free shim) | %.2f | %.0f %% |" % (emu_b - rsp_b, 100 * (emu_b - rsp_b) / emu_b))
    nonfoir = emu_b - g("cb")
    print("\nemulation thread without the video plugin's callback: %.2f ms/frame, of which JIT proper %.2f (%.0f %%), audio HLE %.2f (%.0f %%), the rest %.2f (%.0f %%)" % (
        nonfoir, g("jit"), 100 * g("jit") / nonfoir, audio, 100 * audio / nonfoir, nonfoir - g("jit") - audio, 100 * (nonfoir - g("jit") - audio) / nonfoir))
    print("\nfoir's own log of the window (%d lines of 120 frames): wall %.2f ms/frame, callback %.2f of which waiting at SYNC_FULL %.2f, pixel worker threads %.1f ms CPU/frame (/3 = %.1f)" % (
        len(rows), wall, cb_foir, wait, workers, workers / 3))
    print("tasks per frame %.2f, JIT proper per task %.1f ms, callbacks per frame %.0f" % (g("tasks"), g("jit") / max(g("tasks"), 1e-9), g("n_cb")))


if __name__ == "__main__":
    main()
