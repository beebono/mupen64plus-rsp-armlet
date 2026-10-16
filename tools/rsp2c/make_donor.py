#!/usr/bin/env python3
"""make_donor.py OUT BASE RANGE... -- SNAP...: the donor snapshot for FUZZ_DONOR (fuzz_snaps.py, fuzz_cmds.py): BASE (a task snapshot) with the RDRAM rows of the other snapshots that lie in the ranges (hex LO-HI, RDRAM
addresses) added when BASE does not have the same row. The ranges are the static parts of a game that a perturbed list can reach and that the unperturbed task did not read: the microcode texts and their overlays,
the other microcode's data. Kirby 64: make_donor.py donor.snap <task>.snap 39000-3e000 41ff0-42a00 -- <all tasks>.snap. Snapshots are ROM-derived: OUT belongs in test-output/."""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "native"))
import snaplib

out, base, rest = sys.argv[1], sys.argv[2], sys.argv[3:]
ranges = [tuple(int(x, 16) for x in r.split("-")) for r in rest[:rest.index("--")]]
snaps = rest[rest.index("--") + 1:]
hs = struct.calcsize("<II48sQIIIII4x")


def rows(d):
    h = struct.unpack("<II48sQIIIII4x", bytes(d[:hs]))
    o = hs + snaplib.SNAP_SIZE
    res = []
    for _ in range(h[6]):
        a, ln = struct.unpack("<II", d[o:o + 8])
        res.append((a, ln, bytes(d[o:o + 8 + ln])))
        o += 8 + ln
    return o, res


b = bytearray(open(base, "rb").read())
end, brows = rows(b)
have = {(a, ln) for a, ln, _ in brows}
extra = {}
for fn in snaps:
    for a, ln, blob in rows(open(fn, "rb").read())[1]:
        if ln >= 64 and (a, ln) not in have and any(lo <= a < hi for lo, hi in ranges):
            extra.setdefault((a, ln), blob)
b[end:end] = b"".join(extra.values())
struct.pack_into("<I", b, struct.calcsize("<II48sQII"), len(brows) + len(extra))
open(out, "wb").write(b)
print("%d rows added to %s" % (len(extra), out))
