#!/usr/bin/env python3
"""imemhash.py IMEM-dump LO HI: the FNV-1a-64 hash of the IMEM words [LO, HI) (byte offsets) in the form struct armlet_routine.imem_hash expects."""
import struct, sys
d = open(sys.argv[1], "rb").read()[:4096]
lo, hi = int(sys.argv[2], 0), int(sys.argv[3], 0)
w = struct.unpack("<%dI" % (len(d) // 4), d)
h = 0xcbf29ce484222325
for x in w[lo // 4:hi // 4]:
    h = ((h ^ x) * 0x100000001b3) & (2**64 - 1)
print("0x%016xull" % h)
