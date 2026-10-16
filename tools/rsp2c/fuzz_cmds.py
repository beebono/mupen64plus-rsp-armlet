#!/usr/bin/env python3
"""fuzz_cmds.py OUTDIR N SEED OPCODE FIELD SNAP...: N copies of each task snapshot (ARMLET_HLE_CAPTURE) with one to four display list commands of one opcode changed, for armlet-native-replay --oracle.
fuzz_snaps.py flips bytes anywhere in the rows the task's DMA reads read; this one aims at a command: OPCODE (hex, w0 >> 24) is searched in the rows that go to DMEM above the data segment (not the microcode's
text), and FIELD says what is replaced by a random value: w0lo (w0 & 0xfff: vertex numbers, counts, indices), w0mid (bits 16 .. 23: flags, lengths, offsets) or w1lo (w1 & 0xfff). Snapshots are ROM-derived:
OUTDIR belongs in test-output/."""
import os
import random
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "native"))
import snaplib

out, n, seed, opb, field = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4], 16), sys.argv[5]
rng = random.Random(seed)
os.makedirs(out, exist_ok=True)
for path in sys.argv[6:]:
    d = bytearray(open(path, "rb").read())
    hsize = struct.calcsize("<II48sQIIIII4x")
    hdr = struct.unpack("<II48sQIIIII4x", bytes(d[:hsize]))
    nrows, nev = hdr[6], hdr[7]
    o = hsize + snaplib.SNAP_SIZE
    allspans = []
    for _ in range(nrows):
        a, ln = struct.unpack("<II", d[o:o + 8])
        allspans.append((o + 8, ln))
        o += 8 + ln
    o += snaplib.SNAP_SIZE
    cands, k = [], 0
    for i in range(nev):
        kind, a, b, c, h = struct.unpack("<IIIIQ", d[o + 24 * i:o + 24 * i + 24])
        if kind != 1:
            continue
        for r in range(((c >> 12) & 0xff) + 1):
            if k < len(allspans) and not (a & 0x1000) and (a & 0xfff) >= 0x100 and allspans[k][1] <= 0x400:
                s0, ln = allspans[k]
                for x in range(0, ln - 7, 8):
                    if struct.unpack("<I", d[s0 + x:s0 + x + 4])[0] >> 24 == opb:
                        cands.append(s0 + x)
            k += 1
    if not cands:
        continue
    base = os.path.basename(path)[:-5]
    for k in range(n):
        v = bytearray(d)
        for c in rng.sample(cands, min(len(cands), rng.choice([1, 1, 2, 4]))):
            w0, w1 = struct.unpack("<II", v[c:c + 8])
            r = rng.choice([rng.randrange(0x1000), rng.randrange(0x100) | 1, 0x7f, 0xfff, 0x41, 0x3f, rng.randrange(0, 0x80) * 2, rng.randrange(0, 0x80) * 2 + 1])
            if field == "w0lo":
                w0 = (w0 & ~0xfff) | r
            elif field == "w1lo":
                w1 = (w1 & ~0xfff) | r
            elif field == "w0mid":
                w0 = (w0 & ~0xff0000) | ((r & 0xff) << 16)
            struct.pack_into("<II", v, c, w0, w1)
        open(os.path.join(out, "%s.t%d.snap" % (base, k)), "wb").write(v)
