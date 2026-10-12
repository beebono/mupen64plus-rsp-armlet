#!/usr/bin/env python3
"""report_f2.py: native (GfxMode=hle) against JIT runs of one window: RSP ms, emulation thread ms and the frame-time distribution per VI frame.

  report_f2.py SESSIONDIR TAGPREFIX      SESSIONDIR = test-output/session/<dev>; runs <prefix>.j1..jN (JIT) and <prefix>.h1..hN (native) of one session (tools/session/host.sh, ENV IDLE=1+FRAMELOG=1)

The window is the difference of the two MARK lines of the shim's rspt log (cumulative RSP ms, emulation thread CPU ms, process CPU ms); frame times are swap to swap from foir's frame log
(FOIR_FRAMELOG, tools/session/framelog_read.py) for the frames of the window.
"""
import os, sys, statistics, glob
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import framelog_read


def marks(path):
    m = []
    for l in open(path, errors='replace'):
        p = l.split()
        if p and p[0] == 'MARK':
            m.append([float(x) for x in p[1:]])
    return m


def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))]


def run(d, tag):
    out = os.path.join(d, tag)
    fr = [int(x) for x in open(os.path.join(out, 'frames')).read().strip().split(',')]
    m = marks(os.path.join(out, 'rspt'))
    r = {}
    if len(m) >= 2:
        n = fr[-1] - fr[0]
        r['rsp'] = (m[-1][1] - m[0][1]) / n
        r['emu'] = (m[-1][2] - m[0][2]) / n
        r['proc'] = (m[-1][3] - m[0][3]) / n
        r['calls'] = (m[-1][0] - m[0][0]) / n
    fl = os.path.join(out, 'framelog.bin')
    if os.path.exists(fl):
        recs = framelog_read.read(fl)
        recs = [x for x in recs if fr[0] <= x['frame'] <= fr[-1]]
        ts = [(b['t_enter'] - a['t_enter']) / 1e6 for a, b in zip(recs, recs[1:])]
        if ts:
            r['p50'], r['p95'], r['p99'], r['mean'] = pct(ts, .5), pct(ts, .95), pct(ts, .99), statistics.mean(ts)
    return r


def main():
    d, pre = sys.argv[1], sys.argv[2]
    res = {'j': [], 'h': []}
    for kind in 'jh':
        for t in sorted(glob.glob(os.path.join(d, pre + '.' + kind + '[0-9]*'))):
            r = run(d, os.path.basename(t))
            if r:
                res[kind].append(r)
    keys = [('rsp', 'RSP ms/frame'), ('emu', 'emu thread ms/frame'), ('proc', 'process CPU ms/frame'), ('mean', 'frame mean ms'), ('p50', 'frame p50 ms'), ('p95', 'frame p95 ms'), ('p99', 'frame p99 ms')]
    print('%-24s %20s %20s' % (pre, 'JIT (n=%d)' % len(res['j']), 'native (n=%d)' % len(res['h'])))
    for k, name in keys:
        a = [r[k] for r in res['j'] if k in r]
        b = [r[k] for r in res['h'] if k in r]
        if a and b:
            f = lambda v: '%.2f (%.2f-%.2f)' % (statistics.median(v), min(v), max(v))
            print('%-24s %20s %20s   %+.1f %%' % (name, f(a), f(b), (statistics.median(b) / statistics.median(a) - 1) * 100))


main()
