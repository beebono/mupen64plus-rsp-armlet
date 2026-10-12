#!/usr/bin/env python3
"""Compares equivalence runs (the out/ directory fetched from a device, one sub-directory per TAG).

  compare.py OUTDIR REF NEW [REF NEW ...]        gates 2-4 (and gate 1 when both are ref runs) for each pair
  compare.py --timing OUTDIR TAG...              RSP / CPU ms per frame of 'time' runs (ref against armlet per game)

TAG is GAME.BUILD.REP (see docs/testing.md). A pair is equal when the RSP hash streams agree over their common prefix, the screenshots
taken at the same frames have the same md5 and the audio dumps agree over their common prefix.
"""
import os
import re
import statistics
import sys


def lines(path):
    with open(path) as f:
        return f.read().split("\n")[:-1]  # complete lines only: the last one of a killed run may be cut


def shots(d):
    r = {}
    p = os.path.join(d, "shots.md5")
    if os.path.exists(p):
        for l in open(p):
            h, n = l.split()
            r[n] = h
    return r


def compare(out, a, b):
    da, db = os.path.join(out, a), os.path.join(out, b)
    res = {}
    ha, hb = lines(os.path.join(da, "rsp.hash")), lines(os.path.join(db, "rsp.hash"))
    m = min(len(ha), len(hb))
    first = next((i for i in range(m) if ha[i] != hb[i]), None)
    res["hash"] = (len(ha), len(hb), m, "IDENTICAL" if first is None else "DIFFER@%d" % first)
    pa, pb = os.path.join(da, "rsp.state"), os.path.join(db, "rsp.state")
    if os.path.exists(pa) and os.path.exists(pb):
        xa, xb = lines(pa), lines(pb)
        m = min(len(xa), len(xb))
        first = next((i for i in range(m) if xa[i] != xb[i]), None)
        res["state"] = (len(xa), len(xb), m, "IDENTICAL" if first is None else "DIFFER@%d" % first)
    sa, sb = shots(da), shots(db)
    common = sorted(set(sa) & set(sb))
    bad = [n for n in common if sa[n] != sb[n]]
    res["shots"] = (len(sa), len(sb), len(common), len(bad))
    fa, fb = os.path.join(da, "audio.raw"), os.path.join(db, "audio.raw")
    if os.path.exists(fa) and os.path.exists(fb):
        sza, szb = os.path.getsize(fa), os.path.getsize(fb)
        m = min(sza, szb)
        diff = None
        with open(fa, "rb") as x, open(fb, "rb") as y:
            off = 0
            while off < m:
                n = min(1 << 20, m - off)
                p, q = x.read(n), y.read(n)
                if p != q:
                    diff = off + next(i for i in range(n) if p[i] != q[i])
                    break
                off += n
        res["audio"] = (sza, szb, m, "IDENTICAL" if diff is None else "DIFFER@%d" % diff)
    else:
        res["audio"] = (0, 0, 0, "MISSING")
    ok = res.get("state", (0, 0, 0, "IDENTICAL"))[3] == "IDENTICAL" and res["hash"][3] == "IDENTICAL" and res["shots"][3] == 0 and res["shots"][2] > 0 and res["audio"][3] == "IDENTICAL"
    return ok, res


def status(out, t):
    p = os.path.join(out, t, "status")
    return open(p).read().strip() if os.path.exists(p) else "no status"


def pairs(out, args):
    allok = True
    for a, b in zip(args[::2], args[1::2]):
        ok, r = compare(out, a, b)
        allok &= ok
        st = ("  state(%d/%d common %d %s)" % r["state"]) if "state" in r else ""
        print(("%-5s %s vs %s  hash(%d/%d common %d %s)" + st + "  shots(%d/%d common %d bad %d)  audio(%d/%d common %d %s)  [%s | %s]") % (
            ("PASS" if ok else "FAIL", a, b) + r["hash"] + r["shots"] + r["audio"] + (status(out, a), status(out, b))))
    return allok


def timing(out, tags):
    """RSP ms/frame, emulation-thread CPU ms/frame and process CPU ms/frame at the MARK (frame LAST, same emulated point for every
    build) of the 'time' runs, ref against armlet per game. Parity: |delta RSP| <= max(0.03 ms, 2 sigma of the ref repeats)."""
    g = {}
    for t in tags:
        m = re.match(r"(.+)\.(ref|armlet)\.(\d+)$", t)
        rs, lf = os.path.join(out, t, "rspt"), os.path.join(out, t, "last")
        if not m or not os.path.exists(rs) or not os.path.exists(lf):
            continue
        mark = [l.split() for l in open(rs) if l.startswith("MARK")]
        if not mark:
            continue
        frames = int(open(lf).read())
        calls, rsp, emu, proc = int(mark[0][1]), float(mark[0][2]), float(mark[0][3]), float(mark[0][4])
        g.setdefault(m.group(1), {}).setdefault(m.group(2), []).append((calls, rsp / frames, emu / frames, proc / frames))
    print("| game | frames | calls (ref / armlet) | RSP ms/frame ref | RSP ms/frame armlet | delta | limit | emu-thread ms/frame ref -> armlet | process ms/frame ref -> armlet | parity |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    ok = True
    for game, b in sorted(g.items()):
        if "ref" not in b or "armlet" not in b:
            continue
        r, a = b["ref"], b["armlet"]
        col = lambda v, i: [x[i] for x in v]
        sd = lambda v: statistics.stdev(v) if len(v) > 1 else float("nan")
        rr, aa = col(r, 1), col(a, 1)
        delta = statistics.mean(aa) - statistics.mean(rr)
        limit = max(0.03, 2 * sd(rr))
        er, ea = col(r, 2), col(a, 2)
        edelta = statistics.mean(ea) - statistics.mean(er)
        elimit = max(0.03, 2 * sd(er))
        same_calls = len({x[0] for x in r + a}) == 1
        good = abs(delta) <= limit and abs(edelta) <= elimit and same_calls
        ok &= good
        print("| %s | %s | %s | %.3f +- %.3f (n=%d) | %.3f +- %.3f (n=%d) | %+.3f | %.3f | %.3f -> %.3f (%+.3f, limit %.3f) | %.3f -> %.3f | %s |" % (
            game, open(os.path.join(out, [t for t in tags if t.startswith(game + ".")][0], "last")).read().strip(),
            "%d / %d" % (r[0][0], a[0][0]) if same_calls else "DIFFER", statistics.mean(rr), sd(rr), len(rr), statistics.mean(aa), sd(aa), len(aa),
            delta, limit, statistics.mean(er), statistics.mean(ea), edelta, elimit, statistics.mean(col(r, 3)), statistics.mean(col(a, 3)),
            "PASS" if good else "FAIL"))
        print("<!-- %s ref %s armlet %s -->" % (game, ["%.3f/%.3f" % (x[1], x[2]) for x in r], ["%.3f/%.3f" % (x[1], x[2]) for x in a]))
    return ok


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--timing":
        sys.exit(0 if timing(sys.argv[2], sys.argv[3:]) else 1)
    elif len(sys.argv) >= 4 and len(sys.argv) % 2 == 0:
        sys.exit(0 if pairs(sys.argv[1], sys.argv[2:]) else 1)
    else:
        print(__doc__)
        sys.exit(2)
