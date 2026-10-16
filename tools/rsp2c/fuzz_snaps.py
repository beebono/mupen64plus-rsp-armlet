#!/usr/bin/env python3
"""fuzz_snaps.py OUTDIR N SEED SNAP...: N perturbed copies of each task snapshot (ARMLET_HLE_CAPTURE), for armlet-native-replay: the bytes of the RDRAM rows the task's DMA reads
read (the display list, vertices, triangles, matrices) are flipped at random, in a few different densities, and some words of the display list are replaced by words of other
commands. The replay (--oracle) runs the native front-end and the JIT from the same perturbed state, so every difference between them is an error of the front-end. Snapshots are
ROM-derived: OUTDIR belongs in test-output/."""
import os
import random
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "native"))
import snaplib

out, n, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
# FUZZ_DONOR=<snapshot>: the rows of that task (its DMA reads: the overlay code, the other microcode's text and data) are added to every snapshot that lacks the same RDRAM address, so that a perturbed list
# that reaches them (a lit G_VTX, a clipped triangle, a G_LOAD_UCODE in a task that did not) reads the real code and not zeros: the front-ends take an overlay to be what the build says it is
donor = []
if os.environ.get("FUZZ_DONOR"):
    dd = open(os.environ["FUZZ_DONOR"], "rb").read()
    dh = struct.unpack("<II48sQIIIII4x", dd[:struct.calcsize("<II48sQIIIII4x")])
    o = struct.calcsize("<II48sQIIIII4x") + snaplib.SNAP_SIZE
    for _ in range(dh[6]):
        a, ln = struct.unpack("<II", dd[o:o + 8])
        donor.append((a, dd[o:o + 8 + ln]))
        o += 8 + ln
# FUZZ_DEST=lo-hi,lo-hi (hex DMEM addresses): only the rows the DMA reads put there are perturbed (default: everything above the data segment that is not code)
dest = [tuple(int(x, 16) for x in r.split("-")) for r in os.environ.get("FUZZ_DEST", "").split(",") if r]
rng = random.Random(seed)
os.makedirs(out, exist_ok=True)
for path in sys.argv[4:]:
    d = bytearray(open(path, "rb").read())
    hsize = struct.calcsize("<II48sQIIIII4x")
    hdr = struct.unpack("<II48sQIIIII4x", bytes(d[:hsize]))
    nrows, nev = hdr[6], hdr[7]
    o = hsize + snaplib.SNAP_SIZE
    allspans, have = [], set()
    for _ in range(nrows):
        a, ln = struct.unpack("<II", d[o:o + 8])
        allspans.append((o + 8, ln))
        have.add((a, ln))
        o += 8 + ln
    if donor:                                               # (the extra rows go behind the DMA-read rows, so that the rows of the events keep their order)
        extra = b"".join(blob for a, blob in donor if (a, struct.unpack("<II", blob[:8])[1]) not in have)
        nextra = sum(1 for a, blob in donor if (a, struct.unpack("<II", blob[:8])[1]) not in have)
        d[o:o] = extra
        struct.pack_into("<I", d, struct.calcsize("<II48sQII"), nrows + nextra)
        o += len(extra)
    o += snaplib.SNAP_SIZE
    # the rows are those of the DMA reads in event order: keep the ones that go to DMEM above the data segment (display list, vertices, triangles, matrices), not the code
    spans, k = [], 0
    for i in range(nev):
        kind, a, b, c, h = struct.unpack("<IIIIQ", d[o + 24 * i:o + 24 * i + 24])
        if kind != 1:
            continue
        for r in range(((c >> 12) & 0xff) + 1):
            if k < len(allspans) and not (a & 0x1000) and (a & 0xfff) >= 0x100 and allspans[k][1] <= 0x400 and (not dest or any(lo <= (a & 0xfff) < hi for lo, hi in dest)):
                spans.append(allspans[k])
            k += 1
    if not spans:
        continue
    base = os.path.basename(path)[:-5]
    for k in range(n):
        v = bytearray(d)
        p = rng.choice([0.0005, 0.002, 0.01, 0.03])
        for (s, ln) in spans:
            if rng.random() < 0.7:
                for i in range(ln):
                    if rng.random() < p:
                        v[s + i] = rng.choice([rng.randrange(256), v[s + i] ^ (1 << rng.randrange(8)), 0, 0xff, 0x7f, 0x80])
            elif rng.random() < 0.3 and ln >= 16:         # copy a stretch of another span's bytes over this one
                s2, l2 = rng.choice(spans)
                m = min(ln, l2, 64) & ~3
                v[s:s + m] = d[s2:s2 + m]
        open(os.path.join(out, "%s.f%d.snap" % (base, k)), "wb").write(v)
