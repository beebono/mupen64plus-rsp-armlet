#!/usr/bin/env python3
"""rej_img.py DUMP_PREFIX OUT.be [DESC]: the IMEM image of a graphics text (1024 big-endian words, the input of tools/rsp2c/rsp2c.py) from a dump of tools/ucode_dump_run.sh (<prefix>.text, .ext, .data):
the text at IMEM 0x80, and the code of IMEM [0, 0x170) that the boot code loads from behind the text (the second descriptor of the data segment, at DMEM DESC = 0x2e8 by default; 0x288 in F3DFLX)."""
import struct, sys
f, out = sys.argv[1], sys.argv[2]
desc = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x2e8
t = open(f + ".text", "rb").read(); e = open(f + ".ext", "rb").read(); d = open(f + ".data", "rb").read()
im = bytearray(4096)
n = 0xf48
im[0x80:0x80 + n] = t[:n]
dram, ln, imm = struct.unpack(">IHH", d[desc:desc + 8])
blob = (t + e)[dram:dram + ln + 1]
im[(imm & 0xfff):(imm & 0xfff) + len(blob)] = blob
open(out, "wb").write(bytes(im))
