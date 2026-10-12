#!/usr/bin/env python3
"""disasm.py IMEM-dump-or-snapshot [first_pc last_pc]: disassembles a 4 KiB RSP IMEM dump (host-endian words, as the profiler's
ucode/<id>.imem files, which are the IMEM at the start of the first task: low addresses may differ at run time) or the IMEM of a routine-entry snapshot (.snap) with tools/ucodemap/rspdis, one line per instruction: 'pc  word  text'."""
import os, struct, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__))
rspdis = os.path.join(here, "..", "ucodemap", "rspdis")
lo = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
hi = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x1000
if sys.argv[1].endswith(".snap"):           # a routine-entry snapshot: the IMEM the routine was entered with
    sys.path.insert(0, here)
    import snaplib
    w = snaplib.read(sys.argv[1])["before"].imem
    data = b"\0" * (len(w) * 4)
else:
    data = open(sys.argv[1], "rb").read()[:4096]
    w = struct.unpack("<%dI" % (len(data) // 4), data)
inp = "".join("%d %d %08x\n" % (i, i * 4, w[i]) for i in range(lo // 4, min(hi, len(data)) // 4))
out = subprocess.run([rspdis], input=inp, capture_output=True, text=True).stdout
import re
GPR = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
       "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra"]


def fix(text):
    """the fork's disassembler prints three-register ALU ops as (rd, rt, rs) and CP0 moves as (cp0 register named like a GPR, rt): rewrite them in the usual order"""
    m = re.match(r"(0x[0-9a-f]+)\s+(\w+)\s+(.*)", text)
    if not m:
        return text
    addr, op, args = m.groups()
    a = [x.strip() for x in args.split(",")]
    if op in ("addu", "subu", "and", "or", "xor", "nor", "slt", "sltu") and len(a) == 3:
        return "%s  %s %s, %s, %s" % (addr, op, a[0], a[2], a[1])
    if op == "mtc0" and len(a) == 2:
        return "%s  mtc0 %s, c0_%d" % (addr, a[1], GPR.index(a[0]) if a[0] in GPR else -1)
    if op == "mfc0" and len(a) == 2:
        return "%s  mfc0 %s, c0_%d" % (addr, a[0], GPR.index(a[1]) if a[1] in GPR else -1)
    return text


for l in out.splitlines():
    head, _, text = l.partition(" | ")
    f = head.split()
    print("%03x  %s  %s" % (int(f[1]), f[2], fix(text)))
