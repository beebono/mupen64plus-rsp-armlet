#!/usr/bin/env python3
"""bench_summary.py FILE: groups the output of armlet-native-replay --bench by routine and exit pc"""
import re, collections, sys
cur = None
g = collections.defaultdict(list)
for l in open(sys.argv[1]):
    m = re.match(r'.*?(f3dex\.\w+) entry (\w+) exit (\w+)', l)
    if m:
        cur = (m.group(1), m.group(3)); continue
    m = re.search(r'native ([\d.]+) ns/call, JIT ([\d.]+) ns/call.*?([\d.]+) units/call', l)
    if m and cur:
        g[cur].append((float(m.group(1)), float(m.group(2)), float(m.group(3))))
for k, v in sorted(g.items()):
    n = len(v)
    a, b, c = (sum(x[i] for x in v) / n for i in range(3))
    print('%-10s exit %-4s n=%3d  native %6.0f ns/call  JIT %6.0f ns/call  x%.2f   %5.1f units/call  -> native %6.1f  JIT %6.1f ns/unit' % (k[0], k[1], n, a, b, b / a, c, a / c, b / c))
