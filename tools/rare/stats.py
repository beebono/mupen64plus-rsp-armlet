#!/usr/bin/env python3
"""stats.py FILE: sums the per-task lines HLE_RARE_STATS=FILE makes (what a test set exercised: tasks that ran natively to the end)"""
import collections
import sys

tot = collections.Counter()
n = 0
for line in open(sys.argv[1]):
    n += 1
    for kv in line.split():
        k, v = kv.split("=")
        tot[k] += int(v)
names = ["bad", "nop/rdp", "mtx", "vec", "vtx", "tri", "dl", "blk", "half1", "half2", "half3", "persp", "gclr", "gset", "enddl", "oml", "omh", "texture", "moveword", "texload", "bases"]
print("%d tasks" % n)
print("commands:", ", ".join("%s %d" % (names[i], tot["cmd.%d" % i]) for i in range(21) if tot["cmd.%d" % i]))
print("triangles: drawn %d, rejected %d, culled %d, clipped %d (fan %d, discarded %d); blocks shade %d tex %d z %d; flat %d" % tuple(tot[k] for k in ("tri_ok", "tri_reject", "tri_cull", "tri_clip", "clip_tris", "clip_discard", "shade", "tex", "z", "flat")))
print("block combinations s/t/z (index = geometry low bits z=1 tex=2 shade=4):", [tot["combo.%d" % i] for i in range(8)])
print("new vertices per clipped triangle:", [tot["nnew.%d" % i] for i in range(16)])
print("vertices %d (fog %d, offset %d), matrix products %d" % (tot["vtx"], tot["fog_vtx"], tot["off_vtx"], tot["mmul"]))
