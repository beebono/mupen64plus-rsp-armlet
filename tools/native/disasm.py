#!/usr/bin/env python3
"""disasm.py IMEM-or-text-dump [first_pc last_pc]: disassembles a 4 KiB RSP IMEM dump (host-endian words, as the profiler's
ucode/<id>.imem files) with tools/ucodemap/rspdis, one line per instruction: 'pc  word  text'."""
import os, struct, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__))
rspdis = os.path.join(here, "..", "ucodemap", "rspdis")
data = open(sys.argv[1], "rb").read()[:4096]
lo = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
hi = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x1000
w = struct.unpack("<%dI" % (len(data) // 4), data)
inp = "".join("%d %d %08x\n" % (i, i * 4, w[i]) for i in range(lo // 4, min(hi, len(data)) // 4))
out = subprocess.run([rspdis], input=inp, capture_output=True, text=True).stdout
for l in out.splitlines():
    head, _, text = l.partition(" | ")
    f = head.split()
    print("%03x  %s  %s" % (int(f[1]), f[2], text))
