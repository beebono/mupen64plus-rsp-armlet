#!/usr/bin/env python3
"""report_ab.py SESSIONDIR LABEL TAG-PREFIX...   RSP ms/frame and emulation-thread ms/frame of the runs <prefix>.on<N> / <prefix>.off<N> of a session
(window = the last two MARK lines of the shim's log: calls, RSP ms, emulation thread CPU ms, process CPU ms). Native ON against OFF, means and ranges."""
import os, statistics, sys

d, label = sys.argv[1], sys.argv[2]
for pre in sys.argv[3:]:
    res = {}
    for kind in ("on", "off"):
        rows = []
        n = 1
        while True:
            f = os.path.join(d, label, "%s.%s%d" % (pre, kind, n), "rspt")
            if not os.path.exists(f):
                break
            marks = [l.split() for l in open(f) if l.startswith("MARK")]
            fr = open(os.path.join(os.path.dirname(f), "frames")).read().strip().split(",")
            nf = int(fr[-1]) - int(fr[0])
            a, b = marks[0], marks[-1]
            rows.append(((float(b[2]) - float(a[2])) / nf, (float(b[3]) - float(a[3])) / nf))
            n += 1
        res[kind] = rows
    for kind in ("off", "on"):
        r = res[kind]
        if r:
            rs = [x[0] for x in r]; et = [x[1] for x in r]
            print("%-8s %-4s n=%d  RSP %.3f +- %.3f ms/frame   emu thread %.3f +- %.3f ms/frame" % (pre, kind, len(r), statistics.mean(rs), statistics.pstdev(rs), statistics.mean(et), statistics.pstdev(et)))
    if res["on"] and res["off"]:
        dr = statistics.mean(x[0] for x in res["off"]) - statistics.mean(x[0] for x in res["on"])
        de = statistics.mean(x[1] for x in res["off"]) - statistics.mean(x[1] for x in res["on"])
        print("%-8s saving  RSP %.3f ms/frame   emu thread %.3f ms/frame" % (pre, dr, de))
