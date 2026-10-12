#!/usr/bin/env python3
"""liveness.py: which RSP registers does the code at a routine's exit pcs read before it writes them?

  liveness.py IMEM DMEM --exits 0x1a0,0x1b4 [--callers ENTRY] [--observed PROFDIR:KEY] [--json]

IMEM and DMEM are 4 KiB dumps of host-endian words (the profiler's ucode/<id>.imem and .dmem; DMEM at the time of the dump, i.e. with the ucode's
jump tables and constants loaded). The tool computes backward liveness over the whole IMEM image and prints, for every exit pc, the registers that are
live on entry to it, and for the union of the exits the masks to put into `struct armlet_routine` (dead_sr, dead_vr, dead_acc, dead_flags, dead_div):
what no path from any exit pc reads before writing.

Items tracked: GPRs $1..$31, vector registers v0..v31 (a partial write, such as a load of 8 bytes or a single-lane move, counts as a read and a write), the
accumulator, VCO, VCC, VCE, the divide unit's result (div_out) and its input half (div_in, with the double-precision flag).

Control flow is modelled with delay slots (the slot is a copy of the instruction for each branch that reaches it; the branch reads its registers before the slot
executes). Conditional branches, j and jal are exact. Indirect jumps (jr, jalr: the display-list dispatch, subroutine returns) cannot be resolved statically; each is
given every candidate target: the pc after every jal / jalr (+8), every constant that is put in a register with addi / ori / lui+ori from $zero and looks like a code address
(IMEM addresses carry bit 12: 0x1xxx), and every aligned halfword of DMEM in 0x1000..0x1fff (jump tables, saved return addresses). That over-approximates the targets, so the
result over-approximates the live set: a register reported dead is dead on every path through any of those targets. It is not a proof about code the tables do not point to, and
about ucode that is overlaid later (the IMEM image is the one at the dump); the claim is also checked by running with ARMLET_NATIVE_POISON=1.

With --observed PROFDIR:KEY the targets of an indirect jump that the profiler saw (the control-flow edges of a level-3 run, `counts.*` and `sites` in PROFDIR, ucode key KEY
of the run's `keys` file) replace the static candidates at that jump; a jump that was never seen executing keeps the static set. The static set alone is very coarse: it lets every
`jr ra` return to every call site, including the middle of other routines, where nearly everything is live.

`break` ends the task and is treated as reading nothing: registers are not an interface between tasks (the next task's boot code initialises what it uses).
--callers ENTRY lists the candidate return addresses of a routine entered at ENTRY (the pc after every `jal ENTRY`, and the constants set in $ra just before a `j ENTRY`),
which are the exit pcs of a routine that returns with jr ra.
"""
import argparse
import json
import struct
import sys

REG_ITEMS = ["r%d" % i for i in range(32)] + ["v%d" % i for i in range(32)] + ["ACC", "VCO", "VCC", "VCE", "DIVOUT", "DIVIN"]


def R(i):
    return "r%d" % i if i else None


def V(i):
    return "v%d" % i


def decode(w):
    """-> (uses, defs, kind, arg): uses / defs are sets of item names; kind in seq, br, brl, j, jal, jr, jalr, break; arg a target pc or register."""
    op = w >> 26
    rs, rt, rd = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    imm = w & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm
    u, d = set(), set()
    kind, arg = "seq", None

    def use(*x):
        for i in x:
            if i:
                u.add(i)

    def define(*x):
        for i in x:
            if i:
                d.add(i)

    if op == 0:
        f = w & 63
        if f in (0, 2, 3):
            use(R(rt)); define(R(rd))
        elif f in (4, 6, 7):
            use(R(rt), R(rs)); define(R(rd))
        elif f == 8:
            use(R(rs)); kind, arg = "jr", rs
        elif f == 9:
            use(R(rs)); define(R(rd)); kind, arg = "jalr", rs
        elif f == 13:
            kind = "break"
        elif f in (32, 33, 34, 35, 36, 37, 38, 39, 42, 43):
            use(R(rs), R(rt)); define(R(rd))
    elif op == 1:
        use(R(rs))
        t = rt
        kind, arg = "br", simm
        if t in (16, 17):
            define("r31"); kind = "brl"
    elif op == 2:
        kind, arg = "j", (w & 0x3FF) << 2
    elif op == 3:
        define("r31"); kind, arg = "jal", (w & 0x3FF) << 2
    elif op in (4, 5):
        use(R(rs), R(rt)); kind, arg = "br", simm
    elif op in (6, 7):
        use(R(rs)); kind, arg = "br", simm
    elif op in (8, 9, 10, 11, 12, 13, 14):
        use(R(rs)); define(R(rt))
    elif op == 15:
        define(R(rt))
    elif op == 16:                      # COP0
        if rs == 0:
            define(R(rt))
        elif rs == 4:
            use(R(rt))
    elif op == 18:                      # COP2
        if w >> 25 & 1:
            return decode_vu(w)
        if rs == 0:                     # MFC2 rt <- v[rd]
            use(V(rd)); define(R(rt))
        elif rs == 2:                   # CFC2
            use(["VCO", "VCC", "VCE", "VCE"][rd & 3]); define(R(rt))
        elif rs == 4:                   # MTC2: a halfword / byte of v[rd]
            use(R(rt), V(rd)); define(V(rd))
        elif rs == 6:                   # CTC2
            use(R(rt)); define(["VCO", "VCC", "VCE", "VCE"][rd & 3])
    elif op in (32, 33, 35, 36, 37):
        use(R(rs)); define(R(rt))
    elif op in (40, 41, 43):
        use(R(rs), R(rt))
    elif op == 50:                      # LWC2
        sub = (w >> 11) & 31
        use(R(rs))
        vt = rt
        if sub == 11:                   # LTV: loads 8 registers, partially
            base = vt & ~7
            for k in range(8):
                use(V(base + k)); define(V(base + k))
        else:
            use(V(vt)); define(V(vt))   # every load writes part of vt (LQV of an aligned quad is a full write; counted as partial)
    elif op == 58:                      # SWC2
        sub = (w >> 11) & 31
        use(R(rs))
        if sub == 11:                   # STV
            base = rt & ~7
            for k in range(8):
                use(V(base + k))
        else:
            use(V(rt))
    return u, d, kind, arg


MUL_W = {0x00, 0x01, 0x03, 0x04, 0x05, 0x06, 0x07}         # VMULF VMULU VMULQ VMUDL VMUDM VMUDN VMUDH: overwrite the accumulator
MAC = {0x02, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F}  # VRNDP VMACF VMACU VRNDN VMACQ VMADL VMADM VMADN VMADH: read-modify-write
LOGIC = {0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D}


def decode_vu(w):
    f = w & 63
    vt, vs, vd = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    u, d = set(), set()

    def use(*x):
        u.update(x)

    def define(*x):
        d.update(x)

    if f in MUL_W:
        use(V(vs), V(vt)); define(V(vd), "ACC")
    elif f in MAC:
        use(V(vs), V(vt), "ACC"); define(V(vd), "ACC")
    elif f in (0x10, 0x11):             # VADD VSUB: carry in from VCO, VCO cleared
        use(V(vs), V(vt), "VCO"); define(V(vd), "ACC", "VCO")
    elif f in (0x14, 0x15):             # VADDC VSUBC
        use(V(vs), V(vt)); define(V(vd), "ACC", "VCO")
    elif f == 0x13:                     # VABS
        use(V(vs), V(vt)); define(V(vd), "ACC")
    elif f == 0x1D:                     # VSAR
        use("ACC"); define(V(vd))
    elif f in (0x20, 0x21, 0x22, 0x23):  # VLT VEQ VNE VGE
        use(V(vs), V(vt), "VCO", "VCE"); define(V(vd), "ACC", "VCC", "VCO", "VCE")
    elif f == 0x24:                     # VCL
        use(V(vs), V(vt), "VCO", "VCC", "VCE"); define(V(vd), "ACC", "VCC", "VCO", "VCE")
    elif f in (0x25, 0x26):             # VCH VCR
        use(V(vs), V(vt)); define(V(vd), "ACC", "VCC", "VCO", "VCE")
    elif f == 0x27:                     # VMRG
        use(V(vs), V(vt), "VCC"); define(V(vd), "ACC")
    elif f in LOGIC:
        use(V(vs), V(vt)); define(V(vd), "ACC")
    elif f in (0x30, 0x34):             # VRCP VRSQ: one lane of vd, the result in the divide unit
        use(V(vt), V(vd)); define(V(vd), "DIVOUT", "DIVIN")
    elif f in (0x31, 0x35):             # VRCPL VRSQL
        use(V(vt), V(vd), "DIVIN"); define(V(vd), "DIVOUT", "DIVIN")
    elif f in (0x32, 0x36):             # VRCPH VRSQH
        use(V(vt), V(vd), "DIVOUT"); define(V(vd), "DIVIN")
    elif f == 0x33:                     # VMOV
        use(V(vt), V(vd)); define(V(vd))
    elif f in (0x37, 0x3F):
        pass
    else:                               # VSUT VRNDP-likes and the unused ones: be conservative
        use(V(vs), V(vt), V(vd), "ACC", "VCO", "VCC", "VCE"); define(V(vd), "ACC")
    return u, d, "seq", None


class Image:
    def __init__(self, imem, dmem):
        self.w = struct.unpack("<1024I", imem[:4096])
        self.dm = struct.unpack("<1024I", dmem[:4096])
        self.dec = [decode(x) for x in self.w]

    def targets(self):
        """candidate targets of an indirect jump (word-aligned pcs, 0..0xffc)"""
        t = set()
        for i, w in enumerate(self.w):
            kind = self.dec[i][2]
            if kind in ("jal", "jalr"):
                t.add(((i + 2) * 4) & 0xFFC)
        # constants: addi/ori rX, $zero, imm
        for i, w in enumerate(self.w):
            op = w >> 26
            if op in (8, 9, 13) and ((w >> 21) & 31) == 0:
                v = w & 0xFFFF
                if 0x1000 <= v < 0x2000 and v % 4 == 0:
                    t.add(v & 0xFFC)
            # lui+ori is not used for IMEM addresses in these ucodes
        for w in self.dm:
            for h in ((w >> 16) & 0xFFFF, w & 0xFFFF):
                if 0x1000 <= h < 0x2000 and h % 4 == 0:
                    t.add(h & 0xFFC)
        return sorted(t)


def observed_edges(path):
    """profdir:key -> {jr_pc: set(target pcs)}  from the sites file and every counts.* file"""
    import glob
    import os
    d, k = path.rsplit(":", 1)
    k = int(k)
    pcs = {}
    for l in open(os.path.join(d, "sites")):
        f = l.split()
        pcs[int(f[0])] = (int(f[1]), int(f[2], 16))
    out = {}
    for fn in glob.glob(os.path.join(d, "counts.*")):
        for l in open(fn):
            p = l.split()
            if p[0] != "E" or int(p[1]) != k:
                continue
            a, b = int(p[2]), int(p[3])
            if a not in pcs or b not in pcs:
                continue
            pa, pb = pcs[a][0], pcs[b][0]
            # an edge that leaves the delay slot of a register jump (only the jr / jalr nodes look at this table)
            out.setdefault((pa - 4) & 0xFFF, set()).add(pb & 0xFFC)
    return out


def liveness(img, observed=None, observed_only=False, unobserved=None):
    unobserved = unobserved if unobserved is not None else []
    """returns live_in[node] for nodes ('i', idx) and ('s', idx_of_branch)"""
    n = 1024
    tgt = img.targets()
    # successors
    succ = {}
    uses, defs = {}, {}
    for i in range(n):
        u, d, kind, arg = img.dec[i]
        node = ("i", i)
        uses[node], defs[node] = u, d
        nxt = (i + 1) & 1023
        if kind in ("br", "brl", "j", "jal", "jr", "jalr"):
            succ[node] = [("s", i)]
        elif kind == "break":
            succ[node] = []
        else:
            succ[node] = [("i", nxt)]
        # the delay-slot copy: executes the instruction after the branch, then goes to the branch's destinations
        if kind in ("br", "brl", "j", "jal", "jr", "jalr"):
            s = (i + 1) & 1023
            su, sd, skind, _ = img.dec[s]
            sn = ("s", i)
            uses[sn], defs[sn] = su, sd
            after = (i + 2) & 1023
            if kind in ("br", "brl"):
                t = ((i + 1 + arg) & 1023)
                succ[sn] = [("i", t), ("i", after)]
            elif kind in ("j", "jal"):
                succ[sn] = [("i", (arg >> 2) & 1023)]
            else:
                obs = observed.get(i * 4) if observed else None
                if observed is not None and observed_only and not obs:
                    unobserved.append(i * 4)
                    obs = set()
                    succ[sn] = []
                else:
                    succ[sn] = [("i", (x >> 2) & 1023) for x in (sorted(obs) if obs else tgt)]
            # a branch in a delay slot is "impossible"; the slot copy ignores its own branch semantics (conservative: it reads its regs)
            if skind in ("br", "brl", "j", "jal", "jr", "jalr", "break"):
                succ[sn] = succ[sn]
    live = {k: set() for k in succ}
    work = list(succ)
    pred = {k: [] for k in succ}
    for k, ss in succ.items():
        for s in ss:
            pred[s].append(k)
    inq = set(work)
    while work:
        k = work.pop()
        inq.discard(k)
        out = set()
        for s in succ[k]:
            out |= live[s]
        new = (out - defs[k]) | uses[k]
        if new != live[k]:
            live[k] = new
            for p in pred[k]:
                if p not in inq:
                    work.append(p)
                    inq.add(p)
    return live, tgt, (succ, uses, defs)


def callers(img, entry):
    out = set()
    for i, w in enumerate(img.w):
        kind, arg = img.dec[i][2], img.dec[i][3]
        if kind == "jal" and arg == entry:
            out.add(((i + 2) * 4) & 0xFFC)
        if kind == "j" and arg == entry:
            # a constant put in $ra within the 6 instructions before the jump (or in its delay slot)
            for k in range(max(0, i - 6), min(1024, i + 2)):
                x = img.w[k]
                if (x >> 26) in (8, 9, 13) and ((x >> 21) & 31) == 0 and ((x >> 16) & 31) == 31:
                    out.add((x & 0xFFFF) & 0xFFC)
    return sorted(out)


def fmt_items(items):
    g = sorted((int(x[1:]) for x in items if x[0] == "r"))
    v = sorted((int(x[1:]) for x in items if x[0] == "v"))
    o = [x for x in items if x[0] not in "rv"]
    return "GPR %s | VR %s | %s" % (",".join(map(str, g)) or "-", ",".join(map(str, v)) or "-", ",".join(o) or "-")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("imem")
    ap.add_argument("dmem")
    ap.add_argument("--exits", default="")
    ap.add_argument("--callers", help="entry pc: list the candidate return addresses and use them as the exits (with --exits: both)")
    ap.add_argument("--observed", help="PROFDIR:KEY, see above")
    ap.add_argument("--observed-only", action="store_true", help="a register jump that the profiler never saw executing has no successors (reported): liveness over what real runs did")
    ap.add_argument("--why", help="item (v5, r12, ACC, VCO ...): print a path from the first exit pc to a read of it that no write precedes")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    img = Image(open(a.imem, "rb").read(), open(a.dmem, "rb").read())
    obs = observed_edges(a.observed) if a.observed else None
    unobs = []
    live, tgt, graph = liveness(img, obs, a.observed_only, unobs)
    exits = [int(x, 0) & 0xFFC for x in a.exits.split(",") if x]
    if a.callers:
        c = callers(img, int(a.callers, 0))
        print("candidate return addresses of %s: %s" % (a.callers, " ".join("0x%03x" % x for x in c)), file=sys.stderr)
        exits += c
    exits = sorted(set(exits))
    if not exits:
        ap.error("no exit pcs")
    if a.why:
        succ, uses, defs = graph
        start = ("i", exits[0] >> 2)
        prev = {start: None}
        queue = [start]
        found = None
        while queue and not found:
            nxt = []
            for k in queue:
                if a.why in uses[k]:
                    found = k
                    break
                if a.why in defs[k]:
                    continue
                for t in succ[k]:
                    if t not in prev:
                        prev[t] = k
                        nxt.append(t)
            queue = nxt
        if not found:
            print("%s is not live at 0x%03x" % (a.why, exits[0]))
        else:
            path = []
            k = found
            while k is not None:
                path.append(k)
                k = prev[k]
            path.reverse()
            print("%s read at 0x%03x, reached from 0x%03x through:" % (a.why, (found[1] * 4 + (4 if found[0] == "s" else 0)) & 0xfff, exits[0]))
            print(" ".join("%s%03x" % ("" if n[0] == "i" else "d", n[1] * 4 + (4 if n[0] == "s" else 0)) for n in path))
        return
    union = set()
    per = {}
    for e in exits:
        li = live[("i", e >> 2)]
        per[e] = li
        union |= li
    dead_sr = sum(1 << i for i in range(1, 32) if ("r%d" % i) not in union)
    dead_vr = sum(1 << i for i in range(32) if ("v%d" % i) not in union)
    res = dict(exits=["0x%03x" % e for e in exits], indirect_targets=len(tgt), live_union=sorted(union),
               dead_sr="0x%08x" % dead_sr, dead_vr="0x%08x" % dead_vr, dead_acc="ACC" not in union,
               dead_flags=not ({"VCO", "VCC", "VCE"} & union), dead_div=not ({"DIVOUT", "DIVIN"} & union),
               dead_vco="VCO" not in union, dead_vcc="VCC" not in union, dead_vce="VCE" not in union)
    if a.json:
        print(json.dumps(res, indent=1))
        return
    print("%d candidate targets for indirect jumps%s" % (len(tgt), "; register jumps never observed (no successors): " + " ".join("0x%03x" % x for x in unobs) if unobs else ""))
    for e in exits:
        print("exit 0x%03x: live-in: %s" % (e, fmt_items(per[e])))
    print("union:      %s" % fmt_items(union))
    print("dead_sr = 0x%08x  dead_vr = 0x%08x  dead_acc = %d  dead_flags = %d (VCO %d VCC %d VCE %d)  dead_div = %d" %
          (dead_sr, dead_vr, res["dead_acc"], res["dead_flags"], res["dead_vco"], res["dead_vcc"], res["dead_vce"], res["dead_div"]))


if __name__ == "__main__":
    main()
