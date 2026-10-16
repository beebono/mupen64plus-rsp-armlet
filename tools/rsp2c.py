#!/usr/bin/env python3
"""rsp2c.py: translates ranges of RSP microcode into C, instruction by instruction, on the exact VU library (tools/hle/ref_vu.h).

  rsp2c.py --name NAME [--entry PC]... [--stop PC]... [--seg IMAGE LO HI]... [--out FILE]

IMAGE is a file of 1024 big-endian words (an IMEM image, e.g. from a captured task snapshot); --seg takes the instructions at pcs [LO, HI) of that
image (later segments override earlier ones: an overlay segment goes after the base). --entry PC: a pc the generated function can be entered at
(the first one is the default). --stop PC: control that reaches this pc leaves the function (a leave through any pc outside the segments does too;
the pc is left in x->pc). A BREAK ends the function with the return value 1.

The generated function is `int NAME(struct rctx *x, unsigned entry)`: registers in x->r, the vector unit in x->u (with x->u.dmem = DMEM), the task
in x->t for MTC0 / MFC0 (the SP / DPC registers and the DMA go through hle_mtc0, as the interpreter does). It does what the interpreter does for the
same instructions, bit for bit, with the vector unit on the VU library (NEON backend; VU_FORCE_SCALAR builds the scalar one). Used to make the reference the closed forms of S2DEX's
background and F3DEXBG's lit vertices are checked against (docs/testing.md, "Regenerating the microcode oracles"), and to draft kernels (--straight); the generated files are not part of the tree.

Not supported (the translation stops with an error): LRV SRV LHV SHV LFV SFV SWV, VMULQ / VRNDP / ..., a branch in a delay slot, a BREAK in a delay slot.
"""
import argparse
import re
import struct
import subprocess
import sys

HERE = __import__("os").path.dirname(__import__("os").path.abspath(__file__))
RSPDIS = HERE + "/ucodemap/rspdis"

VOPS = {0: "VMULF", 1: "VMULU", 4: "VMUDL", 5: "VMUDM", 6: "VMUDN", 7: "VMUDH", 8: "VMACF", 9: "VMACU", 12: "VMADL", 13: "VMADM", 14: "VMADN", 15: "VMADH",
        16: "VADD", 17: "VSUB", 19: "VABS", 20: "VADDC", 21: "VSUBC", 29: "VSAR", 32: "VLT", 33: "VEQ", 34: "VNE", 35: "VGE", 36: "VCL", 37: "VCH", 38: "VCR",
        39: "VMRG", 40: "VAND", 41: "VNAND", 42: "VOR", 43: "VNOR", 44: "VXOR", 45: "VNXOR"}
VDIV = {48: "VRCP", 49: "VRCPL", 50: "VRCPH", 51: "VMOV", 52: "VRSQ", 53: "VRSQL", 54: "VRSQH"}
# LWC2 / SWC2 opcodes (the rd field): name, address shift
LOADS = {0: ("LBV", 0), 1: ("LSV", 1), 2: ("LLV", 2), 3: ("LDV", 3), 4: ("LQV", 4), 6: ("LPV", 3), 7: ("LUV", 3), 11: ("LTV", 4)}
STORES = {0: ("SBV", 0), 1: ("SSV", 1), 2: ("SLV", 2), 3: ("SDV", 3), 4: ("SQV", 4), 6: ("SPV", 3), 7: ("SUV", 3), 11: ("STV", 4)}


class Err(Exception):
    pass


def sx16(v):
    return v - 0x10000 if v & 0x8000 else v


def load_image(path):
    d = open(path, "rb").read()
    if len(d) < 4096:
        raise Err("%s: %d bytes, 4096 expected" % (path, len(d)))
    return list(struct.unpack(">1024I", d[:4096]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--entry", action="append", default=[], type=lambda s: int(s, 0))
    ap.add_argument("--stop", action="append", default=[], type=lambda s: int(s, 0))
    ap.add_argument("--seg", action="append", nargs=3, default=[], metavar=("IMAGE", "LO", "HI"))
    ap.add_argument("--out", default="-")
    ap.add_argument("--mut", default="", metavar="PC,PC,...", help="mutation hooks: at each listed pc the immediate / shift amount / vector element is wrapped in MUT(base+i, ok, bad) (bad = ok ^ 1; -DARMLET_HLE_MUTATE=n selects it)")
    ap.add_argument("--mut-base", default="50", type=lambda v: int(v, 0))
    ap.add_argument("--jr-regs", default="", metavar="N,N,...", help="only constants loaded into these registers (numbers) count as possible JR / JALR targets (default: any register); the return addresses of the JALs always do")
    ap.add_argument("--ext", action="append", default=[], metavar="PC:NAME", help="a JAL to PC is a call of the C function NAME(x, r) (nonzero return ends the function with 3); the instructions of the routine at PC are not translated")
    ap.add_argument("--straight", nargs=2, metavar=("LO", "HI"), help="print the statements of the instructions at [LO, HI) one per line (no labels, branches only as comments): a drafting aid for hand-written kernels")
    ap.add_argument("--mutate", action="store_true", help="emit MUT() hooks: -DRSP2C_MUTATE=n changes the n-th ALU immediate / vector element of the routine (mutation checks)")
    a = ap.parse_args()
    if not a.seg or not a.entry:
        ap.error("--seg and --entry are required")

    words = {}
    for img, lo, hi in a.seg:
        im = load_image(img)
        for pc in range(int(lo, 0), int(hi, 0), 4):
            words[pc] = im[pc >> 2]
    stops = set(a.stop)
    exts = {}
    for e in a.ext:
        k, _, v = e.partition(":")
        exts[int(k, 0)] = v
    stops |= set(exts)                                                     # the routines behind a call are not translated (JAL only; a J to one leaves the function)

    # reachability from the entries: branches, jumps, calls (and the instruction after a call), the delay slots; a JR / JALR may go to any return address that
    # reachable code loads as a constant (addi / ori rt, zero, 0x1xxx) or that a reachable JAL leaves
    def reach():
        seen = set()
        work = list(a.entry)
        consts = set()
        jrs = []
        while work:
            pc = work.pop()
            if pc in seen or pc not in words or pc in stops:
                continue
            seen.add(pc)
            w = words[pc]
            op = w >> 26
            if op in (8, 9, 13) and ((w >> 21) & 31) == 0 and 0x1000 <= (w & 0xffff) < 0x2000 and (w & 3) == 0 and (not jr_regs or ((w >> 16) & 31) in jr_regs):
                consts.add(w & 0xffc)
            is_br = op in (2, 3, 4, 5, 6, 7) or (op == 1 and ((w >> 16) & 31) in (0, 1, 16, 17)) or (op == 0 and (w & 63) in (8, 9))
            if not is_br:
                if not (op == 0 and (w & 63) == 13):
                    work.append((pc + 4) & 0xffc)
                continue
            work.append((pc + 4) & 0xffc)                                  # the delay slot
            simm = sx16(w & 0xffff)
            if op == 2:
                work.append((w & 0x3ff) << 2)
            elif op == 3:
                work.append((w & 0x3ff) << 2)
                work.append((pc + 8) & 0xffc)
            elif op == 0:
                jrs.append(pc)
                if (w & 63) == 9:
                    work.append((pc + 8) & 0xffc)
            else:
                work.append((pc + 4 + (simm << 2)) & 0xffc)
                work.append((pc + 8) & 0xffc)
            if jrs:
                work.extend(consts)
        return seen
    if a.straight:
        slo, shi = int(a.straight[0], 0), int(a.straight[1], 0)
        a.entry = a.entry or [slo]
    jr_regs = set(int(v, 0) for v in a.jr_regs.split(",") if v)
    keep = reach() if not a.straight else set(words)
    words = {pc: w for pc, w in words.items() if pc in keep}
    pcs = sorted(words)

    # annotation text from rspdis
    dis = {}
    try:
        inp = "".join("%d %d %08x\n" % (pc >> 2, pc, words[pc]) for pc in pcs)
        out = subprocess.run([RSPDIS], input=inp, capture_output=True, text=True).stdout
        for l in out.splitlines():
            head, _, text = l.partition(" | ")
            f = head.split()
            dis[int(f[1])] = re.sub(r"^0x[0-9a-f]+\s+", "", text.strip())
    except OSError:
        pass

    def inset(pc):
        return pc in words and pc not in stops

    labels = set(a.entry)
    mut = [0]

    def target_ok(t):
        return t in words and t not in stops

    # pass 1: labels (branch targets, return addresses, constants loaded as return addresses)
    for pc in pcs:
        w = words[pc]
        op = w >> 26
        simm = sx16(w & 0xffff)
        if op == 2 or op == 3:
            labels.add((w & 0x3ff) << 2)
            if op == 3:
                labels.add((pc + 8) & 0xffc)
        elif op in (4, 5, 6, 7) or (op == 1 and ((w >> 16) & 31) in (0, 1, 16, 17)):
            labels.add((pc + 4 + (simm << 2)) & 0xffc)
            labels.add((pc + 8) & 0xffc)
            if op == 1 and ((w >> 16) & 31) in (16, 17):
                labels.add((pc + 8) & 0xffc)
        elif op == 0 and (w & 63) == 9:
            labels.add((pc + 8) & 0xffc)
        elif op in (8, 9, 13) and ((w >> 21) & 31) == 0:           # addi / ori rt, zero, imm: a return address as the microcode loads them (0x1000 | pc)
            imm = w & 0xffff
            if 0x1000 <= imm < 0x2000 and (imm & 3) == 0 and (not jr_regs or ((w >> 16) & 31) in jr_regs):
                labels.add(imm & 0xffc)
    labels = sorted(l for l in labels if l in words)
    lset = set(labels)

    L = []
    emit = L.append
    emit("/* generated by tools/rsp2c.py: %s; do not edit. Segments: %s; entries: %s; stops: %s */" % (a.name, ", ".join("%s [%s,%s)" % (s[0].split("/")[-1], s[1], s[2]) for s in a.seg),
         " ".join("%03x" % e for e in a.entry), " ".join("%03x" % s for s in sorted(stops)) or "-"))
    emit("static int %s(struct rctx *x, unsigned entry)" % a.name)
    emit("{")
    emit("    struct vu *u = &x->u;")
    emit("    uint32_t *dm = u->dmem;")
    emit("    uint32_t r[32];")
    emit("    unsigned jt;")
    emit("    int ret = 0;")
    emit("    memcpy(r, x->r, sizeof r);")
    emit("    (void)dm;")
    emit("    switch (entry) {")
    for e in a.entry:
        emit("    case 0x%03x: goto L%03x;" % (e, e))
    emit("    default: return -1;")
    emit("    }")

    def leave(t):
        return "{ x->pc = 0x%03x; goto out; }" % t

    def goto(t):
        return "goto L%03x;" % t if target_ok(t) else leave(t)

    def reg(n):
        return "r[%d]" % n if n else "0u"

    def setr(n, expr):
        return ("r[%d] = (%s);" % (n, expr)) if n else "(void)(%s);" % expr

    def addr(rs, simm):
        return "(%s + %du)" % (reg(rs), simm & 0xffffffff) if simm else reg(rs)

    def vec(w, pc):
        rs = (w >> 21) & 31
        rt = (w >> 16) & 31
        rd = (w >> 11) & 31
        sa = (w >> 6) & 31
        f = w & 63
        e = rs & 15
        mk = mutpcs.get(pc)
        if mk is not None and e < 8 and f in VOPS:
            e = e ^ 1 if False else e
        if f in VOPS:
            if mk is not None:
                return "%s(u, %d, %d, %d, MUT(%d, %d, %d));" % (VOPS[f], sa, rd, rt, mk, e, e ^ 1 if e < 8 else e ^ 1)
            return "%s(u, %d, %d, %d, %d);" % (VOPS[f], sa, rd, rt, e)
        if f in VDIV:
            return "%s(u, %d, %d, %d, %d);" % (VDIV[f], sa, rd, rt, e)
        if f == 55:
            return "VNOP(u);"
        raise Err("%03x: unsupported vector op %d (%08x)" % (pc, f, w))

    mutpcs = {int(v, 0): a.mut_base + i for i, v in enumerate([x for x in a.mut.split(",") if x])}

    def stmt(pc):
        """the code of the instruction at pc (not a branch); returns a list of lines"""
        w = words[pc]
        mk = mutpcs.get(pc)

        def mi(v):                                                          # an immediate, with the mutation hook when this pc is listed
            return "MUT(%d, %du, %du)" % (mk, v, v ^ 1) if mk is not None else "%du" % v
        def mh(v):
            return "MUT(%d, 0x%xu, 0x%xu)" % (mk, v, v ^ 1) if mk is not None else "0x%xu" % v
        op = w >> 26
        rs = (w >> 21) & 31
        rt = (w >> 16) & 31
        rd = (w >> 11) & 31
        sa = (w >> 6) & 31
        simm = sx16(w & 0xffff)
        uimm = w & 0xffff
        if (w >> 25) == 0x25:
            return [vec(w, pc)]
        if op == 0:
            f = w & 63
            sam = ("MUT(%d, %d, %d)" % (mk, sa, sa ^ 1)) if mk is not None else "%d" % sa
            tab = {0: "%s << %s" % (reg(rt), sam), 2: "%s >> %s" % (reg(rt), sam), 3: "(uint32_t)((int32_t)%s >> %s)" % (reg(rt), sam),
                   4: "%s << (%s & 31)" % (reg(rt), reg(rs)), 6: "%s >> (%s & 31)" % (reg(rt), reg(rs)), 7: "(uint32_t)((int32_t)%s >> (%s & 31))" % (reg(rt), reg(rs)),
                   32: "%s + %s" % (reg(rs), reg(rt)), 33: "%s + %s" % (reg(rs), reg(rt)), 34: "%s - %s" % (reg(rs), reg(rt)), 35: "%s - %s" % (reg(rs), reg(rt)),
                   36: "%s & %s" % (reg(rs), reg(rt)), 37: "%s | %s" % (reg(rs), reg(rt)), 38: "%s ^ %s" % (reg(rs), reg(rt)), 39: "~(%s | %s)" % (reg(rs), reg(rt)),
                   42: "(int32_t)%s < (int32_t)%s" % (reg(rs), reg(rt)), 43: "%s < %s" % (reg(rs), reg(rt))}
            if f in tab:
                return [setr(rd, tab[f])]
            raise Err("%03x: unsupported SPECIAL %08x" % (pc, w))
        imm_mut = lambda v: v
        if op in (8, 9):
            return [setr(rt, "%s + %s" % (reg(rs), mi(simm & 0xffffffff)))]
        if op == 10:
            return [setr(rt, "(int32_t)%s < %d" % (reg(rs), simm))]
        if op == 11:
            return [setr(rt, "%s < %du" % (reg(rs), simm & 0xffffffff))]
        if op == 12:
            return [setr(rt, "%s & %s" % (reg(rs), mh(uimm)))]
        if op == 13:
            return [setr(rt, "%s | %s" % (reg(rs), mh(uimm)))]
        if op == 14:
            return [setr(rt, "%s ^ 0x%xu" % (reg(rs), uimm))]
        if op == 15:
            return [setr(rt, "0x%xu" % (uimm << 16))]
        if op == 16:
            if rs == 0:
                if (rd & 15) == 4:
                    return ["if (hle_poll_status(x->t, %d, &jt)) { x->pc = 0x%03x; ret = 2; goto out; }" % (rt, pc), setr(rt, "jt") if rt else ""]
                return [setr(rt, "*x->t->cr[%d]" % (rd & 15))]
            if rs == 4:
                return ["if (hle_mtc0(x->t, %d, %s)) { x->pc = 0x%03x; ret = 2; goto out; }" % (rd, reg(rt), (pc + 4) & 0xffc)]
            raise Err("%03x: unsupported COP0 %08x" % (pc, w))
        if op == 18:
            e = (w >> 7) & 15
            if rs == 0:
                return ["MFC2(u, &jt, %d, %d);" % (rd, e), setr(rt, "jt") if rt else ""]
            if rs == 2:
                return ["CFC2(u, &jt, %d);" % rd, setr(rt, "jt") if rt else ""]
            if rs == 4:
                return ["MTC2(u, %s, %d, %d);" % (reg(rt), rd, e)]
            if rs == 6:
                return ["CTC2(u, %s, %d);" % (reg(rt), rd)]
            raise Err("%03x: unsupported COP2 %08x" % (pc, w))
        if op == 32:
            return [setr(rt, "(uint32_t)(int32_t)(int8_t)vu_m8(dm, %s)" % addr(rs, simm))]
        if op == 33:
            return [setr(rt, "(uint32_t)(int32_t)(int16_t)((vu_m8(dm, %s) << 8) | vu_m8(dm, %s + 1))" % (addr(rs, simm), addr(rs, simm)))]
        if op == 35:
            a_ = addr(rs, simm)
            return [setr(rt, "(uint32_t)vu_m8(dm, %s) << 24 | (uint32_t)vu_m8(dm, %s + 1) << 16 | (uint32_t)vu_m8(dm, %s + 2) << 8 | vu_m8(dm, %s + 3)" % (a_, a_, a_, a_))]
        if op == 36:
            return [setr(rt, "vu_m8(dm, %s)" % addr(rs, simm))]
        if op == 37:
            return [setr(rt, "(vu_m8(dm, %s) << 8) | vu_m8(dm, %s + 1)" % (addr(rs, simm), addr(rs, simm)))]
        if op == 40:
            return ["vu_w8(dm, %s, (uint8_t)%s);" % (addr(rs, simm), reg(rt))]
        if op == 41:
            a_ = addr(rs, simm)
            return ["vu_w8(dm, %s, (uint8_t)(%s >> 8)); vu_w8(dm, %s + 1, (uint8_t)%s);" % (a_, reg(rt), a_, reg(rt))]
        if op == 43:
            a_ = addr(rs, simm)
            return ["{ unsigned a_ = %s; vu_w8(dm, a_, (uint8_t)(%s >> 24)); vu_w8(dm, a_ + 1, (uint8_t)(%s >> 16)); vu_w8(dm, a_ + 2, (uint8_t)(%s >> 8)); vu_w8(dm, a_ + 3, (uint8_t)%s); }" % (a_, reg(rt), reg(rt), reg(rt), reg(rt))]
        if op in (50, 58):
            tab = LOADS if op == 50 else STORES
            if rd not in tab:
                raise Err("%03x: unsupported vector %s %08x" % (pc, "load" if op == 50 else "store", w))
            name, sh = tab[rd]
            off = ((w & 0x7f) ^ 0x40) - 0x40
            e = (w >> 7) & 15
            return ["%s(u, %d, %d, %s + %d);" % (name, rt, e, reg(rs), (off << sh))]
        raise Err("%03x: unsupported instruction %08x" % (pc, w))

    def branch_cond(pc):
        w = words[pc]
        op = w >> 26
        rs = (w >> 21) & 31
        rt = (w >> 16) & 31
        if op == 4:
            return "%s == %s" % (reg(rs), reg(rt))
        if op == 5:
            return "%s != %s" % (reg(rs), reg(rt))
        if op == 6:
            return "(int32_t)%s <= 0" % reg(rs)
        if op == 7:
            return "(int32_t)%s > 0" % reg(rs)
        if op == 1:
            return {0: "(int32_t)%s < 0", 1: "(int32_t)%s >= 0", 16: "(int32_t)%s < 0", 17: "(int32_t)%s >= 0"}[rt] % reg(rs)
        raise Err("branch %08x" % w)

    def is_branch(w):
        op = w >> 26
        return op in (2, 3, 4, 5, 6, 7) or (op == 1 and ((w >> 16) & 31) in (0, 1, 16, 17)) or (op == 0 and (w & 63) in (8, 9))

    def comment(pc):
        return "/* %03x %08x %s */" % (pc, words[pc], dis.get(pc, ""))

    def jr_dispatch(rsn):
        cases = [l for l in labels if l not in a.entry or True]
        lines = ["jt = %s & 0xffc;" % reg(rsn), "switch (jt) {"]
        for l in cases:
            lines.append("case 0x%03x: goto L%03x;" % (l, l))
        lines.append("default: x->pc = jt; goto out;")
        lines.append("}")
        return lines

    if a.straight:
        for pc in range(slo, shi, 4):
            if pc not in words:
                continue
            if is_branch(words[pc]):
                print("    /* %03x %08x %s */" % (pc, words[pc], dis.get(pc, "")))
                continue
            for st in stmt(pc):
                if st:
                    print("    %s /* %03x %s */" % (st, pc, dis.get(pc, "")))
        return
    for pc in pcs:
        if pc in stops:
            continue
        w = words[pc]
        emit("L%03x:" % pc)
        if not is_branch(w):
            if (w >> 26) == 0 and (w & 63) == 13:                              # BREAK
                emit("    %s hle_break(x->t, 0x%03x); x->pc = 0x%03x; ret = 1; goto out;" % (comment(pc), (pc + 4) & 0xffc, (pc + 4) & 0xffc))
                continue
            first = True
            for s in stmt(pc):
                if s:
                    emit("    %s%s" % (comment(pc) + " " if first else "", s))
                    first = False
            nxt = (pc + 4) & 0xffc
            emit("    " + goto(nxt))
            continue
        # a branch with its delay slot
        ds = (pc + 4) & 0xffc
        if ds not in words:
            raise Err("%03x: delay slot outside the segments" % pc)
        if is_branch(words[ds]):
            raise Err("%03x: branch in a delay slot" % ds)
        if (words[ds] >> 26) == 0 and (words[ds] & 63) == 13:
            raise Err("%03x: BREAK in a delay slot" % ds)
        op = w >> 26
        rs = (w >> 21) & 31
        rt = (w >> 16) & 31
        rd = (w >> 11) & 31
        simm = sx16(w & 0xffff)
        emit("    %s" % comment(pc))
        emit("    {")
        if op == 2 or op == 3:
            tgt = (w & 0x3ff) << 2
            if op == 3:
                emit("        r[31] = 0x%03x;" % ((pc + 8) & 0xffc))
            for s in stmt(ds):
                if s:
                    emit("        %s %s" % (comment(ds), s))
            if op == 3 and tgt in exts:
                emit("        if (%s) { ret = 3; x->pc = 0x%03x; goto out; }" % (exts[tgt], (pc + 8) & 0xffc))
                emit("        " + goto((pc + 8) & 0xffc))
            else:
                emit("        " + goto(tgt))
        elif op == 0:                                                           # jr / jalr
            emit("        unsigned tg = %s & 0xffc;" % reg(rs))
            if (w & 63) == 9:
                emit("        %s" % setr(rd, "0x%03xu" % ((pc + 8) & 0xffc)))
            for s in stmt(ds):
                if s:
                    emit("        %s %s" % (comment(ds), s))
            emit("        switch (tg) {")
            for l in labels:
                emit("        case 0x%03x: goto L%03x;" % (l, l))
            emit("        default: x->pc = tg; goto out;")
            emit("        }")
        else:
            tgt = (pc + 4 + (simm << 2)) & 0xffc
            emit("        int tk = %s;" % branch_cond(pc))
            if op == 1 and rt in (16, 17):
                emit("        r[31] = 0x%03x;" % ((pc + 8) & 0xffc))
            for s in stmt(ds):
                if s:
                    emit("        %s %s" % (comment(ds), s))
            emit("        if (tk) %s" % goto(tgt))
            emit("        " + goto((pc + 8) & 0xffc))
        emit("    }")
    emit("out:")
    emit("    memcpy(x->r, r, sizeof r);")
    emit("    x->r[0] = 0;")
    emit("    return ret;")
    emit("}")
    text = "\n".join(L) + "\n"
    if a.out == "-":
        sys.stdout.write(text)
    else:
        open(a.out, "w").write(text)


try:
    main()
except Err as e:
    sys.stderr.write("rsp2c: %s\n" % e)
    sys.exit(1)
