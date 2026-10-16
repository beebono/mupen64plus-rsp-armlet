#!/usr/bin/env python3
"""ddmin_snap.py REPLAY ORIG PERTURBED: the smallest set of byte changes of a perturbed snapshot (tools/rsp2c/fuzz_snaps.py) that still makes `REPLAY --oracle` report a mismatch (delta debugging), printed as
the RDRAM address, the old and the new byte, and the 8-byte display list word (as two host words) around each. The changes are mostly one to three bytes; what the word is (a command, a vertex, a matrix) says
which front-end rule is missing. Snapshots are ROM-derived: the output belongs in test-output/, not in the tree."""
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "native"))
import snaplib

replay, orig, pert = sys.argv[1:4]
O = open(orig, "rb").read()
P = open(pert, "rb").read()
tmp = tempfile.NamedTemporaryFile(suffix=".snap", delete=False).name


def bad(idx):
    v = bytearray(O)
    for i in idx:
        v[i] = P[i]
    open(tmp, "wb").write(v)
    # a memory-capped scope and no stdin: an uncapped run once OOM-killed the desktop, and a replay that waits on a terminal stalls
    r = subprocess.run(["systemd-run", "--user", "--scope", "-q", "-p", "MemoryMax=2G", "-p", "MemorySwapMax=0", "timeout", "-s", "KILL", "60", replay, "--oracle", "--quiet", tmp],
                       capture_output=True, text=True, stdin=subprocess.DEVNULL)
    return "MISMATCH native vs JIT" in r.stdout + r.stderr


cur = [i for i in range(len(O)) if O[i] != P[i]]
if not bad(cur):
    sys.exit("the perturbed snapshot does not mismatch")
n = 2
while len(cur) >= 2:
    ch = max(1, len(cur) // n)
    subs = [cur[i:i + ch] for i in range(0, len(cur), ch)]
    nxt = next((s for s in subs if bad(s)), None)
    if nxt is None:
        nxt = next((c for c in ([x for x in cur if x not in s] for s in subs) if c and bad(c)), None)
        n = max(n - 1, 2) if nxt else n
    else:
        n = 2
    if nxt is None:
        if n >= len(cur):
            break
        n = min(len(cur), n * 2)
    else:
        cur = nxt
hsize = struct.calcsize("<II48sQIIIII4x")
hdr = struct.unpack("<II48sQIIIII4x", O[:hsize])
o = hsize + snaplib.SNAP_SIZE
spans = []
for _ in range(hdr[6]):
    a, ln = struct.unpack("<II", O[o:o + 8])
    spans.append((o + 8, ln, a))
    o += 8 + ln
for i in cur:
    for (s, ln, a) in spans:
        if s <= i < s + ln:
            r = (i - s) & ~7
            print("rdram %08x: %02x -> %02x   word %08x %08x -> %08x %08x   (row %08x, %d bytes)" % (a + i - s, O[i], P[i], *struct.unpack("<II", O[s + r:s + r + 8]), *struct.unpack("<II", P[s + r:s + r + 8]), a, ln))
            break
os.unlink(tmp)
