"""RSP instruction decoding for tools/derive_build.py: fields, classes of operands that a build can relocate, and a readable disassembly."""
import struct

GPR = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra"]
SPECIAL = {0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv", 0x07: "srav", 0x08: "jr", 0x09: "jalr", 0x0d: "break",
           0x20: "add", 0x21: "addu", 0x22: "sub", 0x23: "subu", 0x24: "and", 0x25: "or", 0x26: "xor", 0x27: "nor", 0x2a: "slt", 0x2b: "sltu"}
OPS = {0x02: "j", 0x03: "jal", 0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz", 0x08: "addi", 0x09: "addiu", 0x0a: "slti", 0x0b: "sltiu",
       0x0c: "andi", 0x0d: "ori", 0x0e: "xori", 0x0f: "lui", 0x20: "lb", 0x21: "lh", 0x23: "lw", 0x24: "lbu", 0x25: "lhu", 0x27: "lwu",
       0x28: "sb", 0x29: "sh", 0x2b: "sw"}
VEC = {0x00: "vmulf", 0x01: "vmulu", 0x02: "vrndp", 0x03: "vmulq", 0x04: "vmudl", 0x05: "vmudm", 0x06: "vmudn", 0x07: "vmudh", 0x08: "vmacf", 0x09: "vmacu",
       0x0a: "vrndn", 0x0b: "vmacq", 0x0c: "vmadl", 0x0d: "vmadm", 0x0e: "vmadn", 0x0f: "vmadh", 0x10: "vadd", 0x11: "vsub", 0x13: "vabs", 0x14: "vaddc",
       0x15: "vsubc", 0x1d: "vsar", 0x20: "vlt", 0x21: "veq", 0x22: "vne", 0x23: "vge", 0x24: "vcl", 0x25: "vch", 0x26: "vcr", 0x27: "vmrg", 0x28: "vand",
       0x29: "vnand", 0x2a: "vor", 0x2b: "vnor", 0x2c: "vxor", 0x2d: "vnxor", 0x30: "vrcp", 0x31: "vrcpl", 0x32: "vrcph", 0x33: "vmov", 0x34: "vrsq",
       0x35: "vrsql", 0x36: "vrsqh", 0x37: "vnop"}
LV = {0: "lbv", 1: "lsv", 2: "llv", 3: "ldv", 4: "lqv", 5: "lrv", 6: "lpv", 7: "luv", 8: "lhv", 9: "lfv", 0xb: "ltv"}
SV = {0: "sbv", 1: "ssv", 2: "slv", 3: "sdv", 4: "sqv", 5: "srv", 6: "spv", 7: "suv", 8: "shv", 9: "sfv", 0xa: "swv", 0xb: "stv"}
VSCALE = {0: 1, 1: 2, 2: 4, 3: 8, 4: 16, 5: 16, 6: 8, 7: 8, 8: 16, 9: 16, 0xa: 16, 0xb: 16}


def words(b):
    return list(struct.unpack(">%dI" % (len(b) // 4), b[:len(b) // 4 * 4]))


def sx16(v):
    return v - 0x10000 if v & 0x8000 else v


def sx7(v):
    return v - 0x80 if v & 0x40 else v


# classes of an instruction for diffing
C_J, C_BR, C_MEM, C_VMEM, C_ALUI, C_OTHER, C_NOP = range(7)


def decode(w):
    """-> dict(cls, op, rs, rt, rd, imm, ...): cls tells which field a build may legitimately relocate"""
    op = w >> 26
    rs, rt, rd = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    if w == 0:
        return dict(cls=C_NOP, name="nop", w=w)
    if op in (2, 3):
        return dict(cls=C_J, name=OPS[op], tgt=(w & 0x3ffffff) << 2 & 0xfff, w=w)
    if op in (4, 5, 6, 7) or (op == 1 and rt in (0, 1, 16, 17)):
        nm = OPS.get(op) or {0: "bltz", 1: "bgez", 16: "bltzal", 17: "bgezal"}[rt]
        return dict(cls=C_BR, name=nm, rs=rs, rt=rt, off=sx16(w & 0xffff), w=w, regimm=(op == 1))
    if op in (0x20, 0x21, 0x23, 0x24, 0x25, 0x27, 0x28, 0x29, 0x2b):
        return dict(cls=C_MEM, name=OPS[op], rs=rs, rt=rt, imm=sx16(w & 0xffff), w=w, op=op)
    if op in (0x32, 0x3a):
        sub = rd
        tab = LV if op == 0x32 else SV
        off = sx7(w & 0x7f) * VSCALE.get(sub, 1)
        return dict(cls=C_VMEM, name=tab.get(sub, "?v%x" % sub), rs=rs, vt=rt, sub=sub, elem=(w >> 7) & 15, imm=off, w=w, op=op, off7=w & 0x7f)
    if op in (8, 9, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f):
        return dict(cls=C_ALUI, name=OPS[op], rs=rs, rt=rt, imm=w & 0xffff, simm=sx16(w & 0xffff), w=w, op=op)
    if op == 0 and (w & 0x3f) in SPECIAL:
        f = w & 0x3f
        return dict(cls=C_OTHER, name=SPECIAL[f], rs=rs, rt=rt, rd=rd, sa=(w >> 6) & 31, w=w)
    if op == 0x10:
        return dict(cls=C_OTHER, name="mfc0" if rs == 0 else "mtc0" if rs == 4 else "cop0?", rt=rt, rd=rd, w=w)
    if op == 0x12:
        if w & 0x02000000:
            f = w & 0x3f
            return dict(cls=C_OTHER, name=VEC.get(f, "v?%02x" % f), vd=(w >> 6) & 31, vs=rd, vt=rt, e=rs & 15, w=w)
        nm = {0: "mfc2", 2: "cfc2", 4: "mtc2", 6: "ctc2"}.get(rs, "cop2?")
        return dict(cls=C_OTHER, name=nm, rt=rt, rd=rd, w=w)
    return dict(cls=C_OTHER, name="op%02x" % op, w=w)


def regs(i, r):
    return GPR[r]


def disasm(w, pc=None):
    d = decode(w)
    n = d["name"]
    c = d["cls"]
    if c == C_NOP:
        return "nop"
    if c == C_J:
        return "%s 0x%03x" % (n, d["tgt"])
    if c == C_BR:
        tgt = None if pc is None else (pc + 4 + 4 * d["off"]) & 0xfff
        t = "%+d" % d["off"] if tgt is None else "0x%03x" % tgt
        if d["regimm"] or n in ("blez", "bgtz"):
            return "%s %s, %s" % (n, GPR[d["rs"]], t)
        return "%s %s, %s, %s" % (n, GPR[d["rs"]], GPR[d["rt"]], t)
    if c == C_MEM:
        return "%s %s, %d(%s)" % (n, GPR[d["rt"]], d["imm"], GPR[d["rs"]])
    if c == C_VMEM:
        return "%s v%d[%d], %d(%s)" % (n, d["vt"], d["elem"], d["imm"], GPR[d["rs"]])
    if c == C_ALUI:
        if n == "lui":
            return "lui %s, 0x%x" % (GPR[d["rt"]], d["imm"])
        if n in ("andi", "ori", "xori"):
            return "%s %s, %s, 0x%x" % (n, GPR[d["rt"]], GPR[d["rs"]], d["imm"])
        return "%s %s, %s, %d" % (n, GPR[d["rt"]], GPR[d["rs"]], d["simm"])
    if n in ("sll", "srl", "sra"):
        return "%s %s, %s, %d" % (n, GPR[d["rd"]], GPR[d["rt"]], d["sa"])
    if n in ("jr",):
        return "jr %s" % GPR[d["rs"]]
    if n == "jalr":
        return "jalr %s, %s" % (GPR[d["rd"]], GPR[d["rs"]])
    if n == "break":
        return "break"
    if n in SPECIAL.values():
        return "%s %s, %s, %s" % (n, GPR[d["rd"]], GPR[d["rs"]], GPR[d["rt"]])
    if n in ("mfc0", "mtc0"):
        return "%s %s, c0_%d" % (n, GPR[d["rt"]], d["rd"])
    if "vd" in d:
        return "%s v%d, v%d, v%d[%d]" % (n, d["vd"], d["vs"], d["vt"], d["e"])
    if n in ("mfc2", "mtc2", "cfc2", "ctc2"):
        return "%s %s, v%d[%d]" % (n, GPR[d["rt"]], d["rd"], (d["w"] >> 7) & 15)
    return "%s %08x" % (n, d["w"])
