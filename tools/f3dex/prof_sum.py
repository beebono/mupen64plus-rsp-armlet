#!/usr/bin/env python3
"""prof_sum.py < output of armlet-native-replay with F3DEX_PROF=1 (one 'F3DEXP pid=' line per finished task; the counters are cumulative per replay child process): the last line of each pid is summed.
Prints ns per vertex, per drawn / rejected / culled / degenerate / clip-flagged triangle, per matrix product, relight, clip, and the per-task split (interpreter init, output side)."""
import re, sys
last = {}
for l in sys.stdin:
    if l.startswith('F3DEXP '):
        d = dict(kv.split('=') for kv in l.split()[1:])
        last[d['pid']] = {k: int(v) for k, v in d.items()}
if not last:
    sys.exit('no F3DEXP lines')
T = {}
for d in last.values():
    for k, v in d.items():
        if k != 'pid':
            T[k] = T.get(k, 0) + v
def per(n, c): return n / c if c else 0.0
print('%d replay runs (scalar=%d): vertices %d in %d... %.1f ns/vertex | tri drawn %.0f ns (n=%d), reject %.0f (%d), cull %.0f (%d), degen %.0f (%d), clip-flagged %.0f (%d) | mtx %.0f ns (%d), relight %.0f (%d), clip %.0f (%d)' % (
    len(last), T['scalar'] // len(last), T['verts'], T['vcalls'], per(T['vns'], T['verts']), per(T['tn0'], T['tc0']), T['tc0'], per(T['tn1'], T['tc1']), T['tc1'], per(T['tn3'], T['tc3']), T['tc3'],
    per(T['tn4'], T['tc4']), T['tc4'], per(T['tn2'], T['tc2']), T['tc2'], per(T['mn'], T['mc']), T['mc'], per(T['rn'], T['rc']), T['rc'], per(T['cn'], T['cc']), T['cc']))
print('per task (%d tasks): total %.1f us, interpreter init %.1f us, %.0f commands, output %.1f us, sync (DMA + DPC_END) %.1f us (%.1f syncs, %.0f ns each)' % (
    T['tasks'], per(T['task'], T['tasks']) / 1e3, per(T['init'], T['tasks']) / 1e3, per(T['cmds'], T['tasks']), per(T['emitn'], T['tasks']) / 1e3, per(T['syncn'], T['tasks']) / 1e3, per(T['syncc'], T['tasks']), per(T['syncn'], T['syncc'])))
if T.get('dmac'):
    print('  of which: %d DMAs of %.0f bytes at %.0f ns each (%.2f ns/byte), %d DPC_END writes at %.0f ns each' % (T['dmac'], T['dmab'] / T['dmac'], T['dman'] / T['dmac'], T['dman'] / T['dmab'], T['endc'], T['endn'] / T['endc'] if T['endc'] else 0))
