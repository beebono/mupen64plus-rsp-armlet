#!/usr/bin/env python3
"""deadwrites.py: how much of the vector-unit work in a microcode image is dead, and how much VU state traffic a register-allocating JIT tier would save.

  deadwrites.py IMEM [--endian be|le] [--patch FILE:FILEOFF:IMEMADDR:LEN ...] [--ranges NAME=LO-HI,...] [--counts PROFDIR/counts.LABEL:KEY] [--json OUT]
  deadwrites.py IMEM [--endian ...] [--patch ...] [--window LO-HI ...] --list LO-HI
  deadwrites.py --selftest

IMEM is a 4 KiB IMEM image: tools/ucode_dump_shim.c's `.imem` (big-endian, the default) or the profiler's ucode/<id>.imem (host-endian: --endian le);
without --endian the byte order with more COP2 words wins. --patch copies LEN bytes of a big-endian file (from FILEOFF) over IMEM at IMEMADDR, so an
image with another overlay in place can be made from a dump's `.ext` (the F3DEX2 lighting overlay is not in the `.imem` of a task that clipped last).
Nothing is changed or written but the report.

**What is counted.** Every instruction's outputs are its definitions in the state a VU op can write: vd, the three accumulator slices (ACC_HI, ACC_MD,
ACC_LO), VCO, VCC, VCE and the divide unit (DIVOUT, DIVIN), plus the scalar GPRs. The read and write sets follow the fork's own semantics
(ext/parallel-rsp/rsp/vfunctions.cpp, the code the JIT's NEON bodies are checked against): VADD / VSUB / logic / select / compares / VCH / VCL / VCR /
VMOV / VRCP* write ACC_LO only; the multiplies write all three slices, the multiply-accumulates also read them; VSAR reads one slice and writes no
accumulator; VLT / VEQ / VNE / VGE leave VCE alone; VMRG clears VCO. A partial write (a vector load, MTC2, VMOV, VRCP*: one lane or a part of the
register) is a read and a write, so it never makes an earlier write look dead. An output is **dead** when no path from the instruction reads it before
it is written again.

**Two region models**, because the answer depends on how much code a tier compiles at once:

- `bb`: a region is one basic block (a leader is a branch or jump target, the instruction after a delay slot, and after any COP0 access); everything is
  live at the end of every block. This is what a tier gets with no analysis across blocks: the lower bound.
- `global`: liveness over the whole image along the static control flow (conditional branches, j and jal are exact; delay slots as in liveness.py).
  Every register jump (jr, jalr: the command dispatch, returns), `break` and every COP0 access (mfc0 / mtc0: the SP_STATUS poll time-out can stop the
  JIT there, DMA can change IMEM) is an exit at which **all** state is live. That is sound for any region shape a tier could compile inside those exits,
  so it is the upper bound of a tier that never looks through a register jump. tools/native/liveness.py's warning applies to the opposite claim (state
  dead at a routine exit, which needs the targets of the register jumps); this tool never needs them.

**State traffic.** For each basic block: `now` = the VU-state loads and stores of the current code generation, one load per vector input (vs, vt, each
accumulator slice and flag read) and one store per output, less a reload of vs / vt when it is the previous op's vd (the fork keeps v0 across
straight-line code); `alloc` = the same block with VU state in host registers: one load per item read before it is written in the block, one store per
item written in the block (`alloc-bb`: all of them; `alloc-live`: only those live at the block's end by the global liveness). Scalar state is not in
these counts (the fork already caches GPRs). `pressure` is the number of distinct VU items (vector registers, slices, flags) a block touches: the host
registers an allocation needs (the NEON file has 32, the op bodies use a few of them as scratch).

**Weights.** Without --counts every instruction counts once (static). With `--counts PROFDIR/counts.LABEL:KEY` (a level-3 profiler run: `sites` next to
the counts file, `C KEY SITE COUNT` lines, cumulative per snapshot) each instruction counts as often as it ran, for the sites whose pc and word match the
image (the report says how many matched).

**Overlays.** An image holds one overlay in each window; code outside a window may jump straight into an overlay that is not the one in the image
(F3DEX2's vertex handler branches into the lighting overlay, which the clipping overlay replaces). `--window LO-HI` makes every jump or fall-through
into the window from outside it an exit with all state live, which is right whatever overlay is resident: analyse the code outside the windows in any
image, and each overlay in an image that holds it.

--ranges names pc ranges (IMEM addresses, the 0x1000 bit ignored), e.g. `vtx=0x4f8-0x7b4,tri=0x7b4-0xc40`; the report has a row per range and one for all.
"""
import argparse
import collections
import json
import os
import struct
import sys

VEC_ITEMS = ["v%d" % i for i in range(32)]
ACC = ("AH", "AM", "AL")
FLAGS = ("VCO", "VCC", "VCE")
DIV = ("DIVOUT", "DIVIN")
VU_STATE = set(VEC_ITEMS) | set(ACC) | set(FLAGS) | set(DIV)
ALL = frozenset(["r%d" % i for i in range(1, 32)]) | VU_STATE

MUL = {0x00, 0x01, 0x03, 0x04, 0x05, 0x06, 0x07}            # VMULF VMULU VMULQ VMUDL VMUDM VMUDN VMUDH: write the three slices
MAC = {0x08, 0x09, 0x0C, 0x0D, 0x0E, 0x0F}                  # VMACF VMACU VMADL VMADM VMADN VMADH: read and write them
LOGIC = {0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D}
FAMILY = {}
for _f in MUL:
    FAMILY[_f] = "mul"
for _f in MAC:
    FAMILY[_f] = "mac"
for _f in (0x10, 0x11, 0x13, 0x14, 0x15):
    FAMILY[_f] = "addsub"
for _f in LOGIC:
    FAMILY[_f] = "logic"
for _f in (0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27):
    FAMILY[_f] = "cmp/sel"
for _f in (0x30, 0x31, 0x32, 0x34, 0x35, 0x36):
    FAMILY[_f] = "div"
FAMILY[0x33] = "move"
FAMILY[0x1D] = "sar"


def R(i):
    return "r%d" % i if i else None


def V(i):
    return "v%d" % i


class Ins:
    """uses / defs: item sets; partial: defs that are also uses (a part of the item is written); kind: seq br brl j jal jr jalr break sync; vu: VU family or None"""
    __slots__ = ("w", "uses", "defs", "kind", "arg", "vu", "vregs_in")

    def __init__(self, w):
        self.w = w
        self.uses, self.defs = set(), set()
        self.kind, self.arg, self.vu = "seq", None, None
        self.vregs_in = []


def decode(w):
    d = Ins(w)
    op = w >> 26
    rs, rt, rd = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    imm = w & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm
    u, df = d.uses, d.defs

    def use(*x):
        u.update(i for i in x if i)

    def define(*x):
        df.update(i for i in x if i)

    if op == 0:
        f = w & 63
        if f in (0, 2, 3):
            use(R(rt)); define(R(rd))
        elif f in (4, 6, 7):
            use(R(rt), R(rs)); define(R(rd))
        elif f == 8:
            use(R(rs)); d.kind = "jr"
        elif f == 9:
            use(R(rs)); define(R(rd)); d.kind = "jalr"
        elif f == 13:
            d.kind = "break"
        elif f in (32, 33, 34, 35, 36, 37, 38, 39, 42, 43):
            use(R(rs), R(rt)); define(R(rd))
    elif op == 1:
        use(R(rs))
        d.kind, d.arg = "br", simm
        if rt in (16, 17):
            define("r31"); d.kind = "brl"
    elif op == 2:
        d.kind, d.arg = "j", (w & 0x3FF) << 2
    elif op == 3:
        define("r31"); d.kind, d.arg = "jal", (w & 0x3FF) << 2
    elif op in (4, 5):
        use(R(rs), R(rt)); d.kind, d.arg = "br", simm
    elif op in (6, 7):
        use(R(rs)); d.kind, d.arg = "br", simm
    elif op in (8, 9, 10, 11, 12, 13, 14):
        use(R(rs)); define(R(rt))
    elif op == 15:
        define(R(rt))
    elif op == 16:                                          # COP0: an exit at which all state is live
        if rs == 0:
            define(R(rt))
        elif rs == 4:
            use(R(rt))
        d.kind = "sync"
    elif op == 18:
        if w >> 25 & 1:
            decode_vu(d, w)
        elif rs == 0:                                       # MFC2 rt <- a halfword of v[rd]
            use(V(rd)); define(R(rt))
        elif rs == 2:                                       # CFC2
            use(("VCO", "VCC", "VCE", "VCE")[rd & 3]); define(R(rt))
        elif rs == 4:                                       # MTC2: partial
            use(R(rt), V(rd)); define(V(rd))
        elif rs == 6:                                       # CTC2
            use(R(rt)); define(("VCO", "VCC", "VCE", "VCE")[rd & 3])
    elif op in (32, 33, 35, 36, 37, 39):
        use(R(rs)); define(R(rt))
    elif op in (40, 41, 43):
        use(R(rs), R(rt))
    elif op == 50:                                          # LWC2: partial writes
        sub = (w >> 11) & 31
        use(R(rs))
        regs = [(rt & ~7) + k for k in range(8)] if sub == 11 else [rt]
        for r in regs:
            use(V(r)); define(V(r))
        d.vu = "vload"
    elif op == 58:                                          # SWC2
        sub = (w >> 11) & 31
        use(R(rs))
        regs = [(rt & ~7) + k for k in range(8)] if sub == 11 else [rt]
        for r in regs:
            use(V(r))
        d.vu = "vstore"
    return d


def decode_vu(d, w):
    f = w & 63
    e = (w >> 21) & 15
    vt, vs, vd = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    use, define = d.uses.update, d.defs.update
    d.vu = FAMILY.get(f, "other")
    if f in MUL:
        use((V(vs), V(vt))); define((V(vd),) + ACC)
        d.vregs_in = [vs, vt]
    elif f in MAC:
        use((V(vs), V(vt)) + ACC); define((V(vd),) + ACC)
        d.vregs_in = [vs, vt]
    elif f in (0x10, 0x11):                                 # VADD VSUB: carry in from VCO, VCO cleared
        use((V(vs), V(vt), "VCO")); define((V(vd), "AL", "VCO"))
        d.vregs_in = [vs, vt]
    elif f in (0x14, 0x15):                                 # VADDC VSUBC
        use((V(vs), V(vt))); define((V(vd), "AL", "VCO"))
        d.vregs_in = [vs, vt]
    elif f == 0x13 or f in LOGIC:                           # VABS, logic
        use((V(vs), V(vt))); define((V(vd), "AL"))
        d.vregs_in = [vs, vt]
    elif f == 0x1D:                                         # VSAR: vd = one slice (e 8 / 9 / 10), else 0; no accumulator write
        if e in (8, 9, 10):
            use((ACC[e - 8],))
        define((V(vd),))
    elif f in (0x20, 0x21, 0x22, 0x23):                     # VLT VEQ VNE VGE: read VCO, write VCC, clear VCO; VCE untouched
        use((V(vs), V(vt), "VCO")); define((V(vd), "AL", "VCC", "VCO"))
        d.vregs_in = [vs, vt]
    elif f == 0x24:                                         # VCL
        use((V(vs), V(vt)) + FLAGS); define((V(vd), "AL") + FLAGS)
        d.vregs_in = [vs, vt]
    elif f in (0x25, 0x26):                                 # VCH VCR
        use((V(vs), V(vt))); define((V(vd), "AL") + FLAGS)
        d.vregs_in = [vs, vt]
    elif f == 0x27:                                         # VMRG: select by VCC, VCO cleared
        use((V(vs), V(vt), "VCC")); define((V(vd), "AL", "VCO"))
        d.vregs_in = [vs, vt]
    elif f in (0x30, 0x34):                                 # VRCP VRSQ: one lane of vd; ACC_LO = vt
        use((V(vt), V(vd))); define((V(vd), "AL") + DIV)
        d.vregs_in = [vt]
    elif f in (0x31, 0x35):                                 # VRCPL VRSQL
        use((V(vt), V(vd), "DIVIN")); define((V(vd), "AL") + DIV)
        d.vregs_in = [vt]
    elif f in (0x32, 0x36):                                 # VRCPH VRSQH
        use((V(vt), V(vd), "DIVOUT")); define((V(vd), "AL", "DIVIN"))
        d.vregs_in = [vt]
    elif f == 0x33:                                         # VMOV: one lane of vd; ACC_LO = vt
        use((V(vt), V(vd))); define((V(vd), "AL"))
        d.vregs_in = [vt]
    elif f in (0x37, 0x3F):                                 # VNOP
        d.vu = "nop"
    else:                                                   # VMACQ, VRNDN / VRNDP and the undefined ones: conservative
        use((V(vs), V(vt), V(vd)) + ACC + FLAGS); define((V(vd),) + ACC)
        d.vregs_in = [vs, vt]


class Image:
    def __init__(self, words):
        self.w = list(words)
        self.dec = [decode(x) for x in self.w]


def load_image(data, endian=None, patches=()):
    data = bytearray(data[:4096].ljust(4096, b"\0"))
    if endian is None:
        cop2 = lambda fmt: sum(1 for x in struct.unpack(fmt, bytes(data)) if x >> 26 in (0x12, 0x32, 0x3a))
        endian = "be" if cop2(">1024I") >= cop2("<1024I") else "le"
    if endian == "le":
        data = bytearray(struct.pack(">1024I", *struct.unpack("<1024I", bytes(data))))
    for buf, off, addr, n in patches:
        addr &= 0xFFF
        data[addr:addr + n] = buf[off:off + n]
    return Image(struct.unpack(">1024I", bytes(data))), endian


EXIT = ("x", 0)


def graph(img, model, windows=()):
    """nodes ('i', idx) and ('s', idx of the branch: its delay slot); EXIT has every item live. model: 'global' or 'bb'.
    windows: (lo, hi) IMEM ranges that hold an overlay; an edge into a window from outside it goes to EXIT (another overlay may be resident)."""
    n = 1024
    win = lambda i: any(lo <= i * 4 < hi for lo, hi in windows)
    succ, node_ins = {}, {}
    leaders = set()
    for i in range(n):
        d = img.dec[i]
        if d.kind in ("br", "brl"):
            leaders.add((i + 1 + d.arg) & 1023)
            leaders.add((i + 2) & 1023)
        elif d.kind in ("j", "jal"):
            leaders.add((d.arg >> 2) & 1023)
            leaders.add((i + 2) & 1023)
        elif d.kind in ("jr", "jalr", "break"):
            leaders.add((i + 2) & 1023)
        elif d.kind == "sync":
            leaders.add((i + 1) & 1023)

    def to(i, frm):
        if model == "bb" and i in leaders:
            return EXIT
        if win(i) and not win(frm):
            return EXIT
        return ("i", i)

    for i in range(n):
        d = img.dec[i]
        node = ("i", i)
        node_ins[node] = d
        nxt = (i + 1) & 1023
        if d.kind in ("br", "brl", "j", "jal", "jr", "jalr"):
            succ[node] = [("s", i)]
            sn = ("s", i)
            node_ins[sn] = img.dec[nxt]
            after = (i + 2) & 1023
            if model == "bb":
                succ[sn] = [EXIT]
            elif d.kind in ("br", "brl"):
                succ[sn] = [to((i + 1 + d.arg) & 1023, i), to(after, i)]
            elif d.kind in ("j", "jal"):
                succ[sn] = [to((d.arg >> 2) & 1023, i)]
            else:
                succ[sn] = [EXIT]
        elif d.kind == "break":
            succ[node] = [EXIT]
        elif d.kind == "sync":
            succ[node] = [EXIT]
        else:
            succ[node] = [to(nxt, i)]
    return succ, node_ins, leaders


def liveness(succ, node_ins):
    live_in = {k: set() for k in succ}
    pred = collections.defaultdict(list)
    for k, ss in succ.items():
        for s in ss:
            pred[s].append(k)
    live_out = {}
    work = list(succ)
    inq = set(work)
    while work:
        k = work.pop()
        inq.discard(k)
        out = set()
        for s in succ[k]:
            out |= ALL if s == EXIT else live_in[s]
        live_out[k] = out
        d = node_ins[k]
        new = (out - d.defs) | d.uses
        if new != live_in[k]:
            live_in[k] = new
            for p in pred[k]:
                if p not in inq:
                    work.append(p)
                    inq.add(p)
    for k in succ:
        if k not in live_out:
            out = set()
            for s in succ[k]:
                out |= ALL if s == EXIT else live_in[s]
            live_out[k] = out
    return live_in, live_out


def blocks(img, leaders):
    """basic blocks as lists of instruction indices in execution order (a branch's delay slot included); a block also ends after a sync or break"""
    out, cur = [], []
    i = 0
    while i < 1024:
        if i in leaders and cur:
            out.append(cur)
            cur = []
        d = img.dec[i]
        cur.append(i)
        if d.kind in ("br", "brl", "j", "jal", "jr", "jalr"):
            if i + 1 < 1024:
                cur.append(i + 1)
            out.append(cur)
            cur = []
            i += 2
            continue
        if d.kind in ("break", "sync"):
            out.append(cur)
            cur = []
        i += 1
    if cur:
        out.append(cur)
    return out


def node_of(img, blk, pos):
    """the graph node of the instruction at position pos of a block (a delay slot is the branch's slot node)"""
    i = blk[pos]
    if pos > 0 and img.dec[blk[pos - 1]].kind in ("br", "brl", "j", "jal", "jr", "jalr"):
        return ("s", blk[pos - 1])
    return ("i", i)


def load_weights(spec, img):
    path, key = spec.rsplit(":", 1)
    key = int(key)
    sites = {}
    for l in open(os.path.join(os.path.dirname(path) or ".", "sites")):
        p = l.split()
        sites[int(p[0])] = (int(p[1]) & 0xFFC, int(p[2], 16))
    w = collections.Counter()
    seen = matched = 0
    for l in open(path):
        p = l.split()
        if not p or p[0] != "C" or int(p[1]) != key:
            continue
        s, c = int(p[2]), int(p[3])
        if s not in sites:
            continue
        seen += 1
        pc, word = sites[s]
        if img.w[pc >> 2] == word:
            w[pc >> 2] += c
            matched += 1
    return w, seen, matched


def parse_ranges(s):
    out = []
    for part in (s or "").split(","):
        if not part:
            continue
        name, r = part.split("=")
        lo, hi = r.split("-")
        out.append((name, int(lo, 0) & 0xFFF, int(hi, 0) & 0xFFF))
    return out


def in_range(pc, r):
    return r[1] <= pc < r[2]


def analyse(img, weights=None, ranges=(), windows=()):
    W = (lambda i: weights.get(i, 0)) if weights is not None else (lambda i: 1)
    res = {}
    lives = {}
    for model in ("bb", "global"):
        succ, node_ins, leaders = graph(img, model, windows)
        lives[model] = liveness(succ, node_ins)[1]
    rows = [("all", 0, 0x1000)] + list(ranges)
    stats = {r[0]: collections.defaultdict(float) for r in rows}
    per_fam = {r[0]: collections.defaultdict(lambda: collections.defaultdict(float)) for r in rows}

    def add(pc, key, v, fam=None):
        for r in rows:
            if in_range(pc, r):
                stats[r[0]][key] += v
                if fam:
                    per_fam[r[0]][fam][key] += v

    _, _, leaders = graph(img, "global")
    blks = blocks(img, leaders)
    for blk in blks:
        bw = W(blk[0])
        upward, written, written_live = set(), set(), set()
        prev_vd = None
        now_ld = now_st = 0
        nvu = 0
        touched = set()
        for pos, i in enumerate(blk):
            node = node_of(img, blk, pos)
            d = img.dec[i]
            pc = i * 4
            wt = W(i)
            if d.vu is None or d.vu == "nop":
                prev_vd = None if d.vu is None and (d.defs & VU_STATE) else prev_vd
                for it in d.uses & VU_STATE:
                    if it not in written:
                        upward.add(it)
                for it in d.defs & VU_STATE:
                    written.add(it)
                touched |= (d.uses | d.defs) & VU_STATE
                continue
            vudefs = d.defs & VU_STATE
            vuuses = d.uses & VU_STATE
            touched |= vudefs | vuuses
            if d.vu not in ("vload", "vstore"):
                nvu += 1
                add(pc, "vu_ops", wt, d.vu)
                # state traffic of the current code generation
                loads = len(vuuses)
                if prev_vd is not None and prev_vd in d.vregs_in:
                    loads -= 1
                now_ld += loads
                now_st += len(vudefs)
                vdn = (d.w >> 6) & 31
                prev_vd = vdn if V(vdn) in vudefs else None
                for model in ("bb", "global"):
                    lo = lives[model][node]
                    dead = vudefs - lo
                    add(pc, model + ":outputs", wt * len(vudefs), d.vu)
                    add(pc, model + ":dead", wt * len(dead), d.vu)
                    if vudefs and not (vudefs & lo):
                        add(pc, model + ":dead_ops", wt, d.vu)
                    vd = V(vdn)
                    if vd in vudefs:
                        add(pc, model + ":vd", wt, d.vu)
                        if vd not in lo:
                            add(pc, model + ":vd_dead", wt, d.vu)
                    acc = vudefs & set(ACC)
                    if acc:
                        add(pc, model + ":acc", wt * len(acc), d.vu)
                        add(pc, model + ":acc_dead", wt * len(acc - lo), d.vu)
                    fl = vudefs & set(FLAGS)
                    if fl:
                        add(pc, model + ":flags", wt * len(fl), d.vu)
                        add(pc, model + ":flags_dead", wt * len(fl - lo), d.vu)
                    if d.vu in ("mul", "mac"):
                        if vd not in lo and (acc & lo):
                            add(pc, model + ":clamp_only_dead", wt, d.vu)
                        if not (acc & lo) and vd in lo:
                            add(pc, model + ":acc_all_dead", wt, d.vu)
            else:
                prev_vd = None
                if d.vu == "vload":
                    now_st += 1                              # the register file write of the loaded value
                else:
                    now_ld += 1
            # a vector load's own target is a partial write: a read for liveness, but no load of the old value in either traffic model
            for it in (vuuses - vudefs if d.vu == "vload" else vuuses):
                if it not in written:
                    upward.add(it)
            written |= vudefs
        last = node_of(img, blk, len(blk) - 1)
        lo_end = lives["global"][last]
        add(blk[0] * 4, "blocks", bw)
        add(blk[0] * 4, "now_ld", bw * now_ld)
        add(blk[0] * 4, "now_st", bw * now_st)
        add(blk[0] * 4, "alloc_ld", bw * len(upward))
        add(blk[0] * 4, "alloc_st_bb", bw * len(written))
        add(blk[0] * 4, "alloc_st_live", bw * len(written & lo_end))
        add(blk[0] * 4, "vu_in_blocks", bw * nvu)
        if nvu:
            for r in rows:
                if in_range(blk[0] * 4, r):
                    stats[r[0]].setdefault("_runs", [])
                    stats[r[0]]["_runs"].append((nvu, len(touched), bw))
    for r in rows:
        s = stats[r[0]]
        runs = s.pop("_runs", [])
        tot = sum(w for _, _, w in runs) or 1
        s["vu_run_mean"] = sum(n * w for n, _, w in runs) / tot
        s["vu_ops_in_runs_ge8"] = sum(n * w for n, _, w in runs if n >= 8) / (sum(n * w for n, _, w in runs) or 1)
        s["pressure_max"] = max((p for _, p, _ in runs), default=0)
        s["blocks_pressure_le20"] = sum(w for _, p, w in runs if p <= 20) / tot
        res[r[0]] = dict(s)
        res[r[0]]["families"] = {f: dict(v) for f, v in per_fam[r[0]].items()}
    return res


def pct(a, b):
    return "%5.1f%%" % (100.0 * a / b) if b else "    -"


def report(res, out=sys.stdout):
    for name, s in res.items():
        if not s.get("vu_ops"):
            continue
        print("== %s: %d VU ops in %d blocks, mean VU run %.1f ops per block (%s of the VU ops in blocks of >= 8), max pressure %d items, %s of the blocks <= 20"
              % (name, s["vu_ops"], s.get("blocks", 0), s["vu_run_mean"], pct(s["vu_ops_in_runs_ge8"], 1), s["pressure_max"], pct(s["blocks_pressure_le20"], 1)), file=out)
        print("   %-8s %8s %8s %8s %8s %8s %8s %8s" % ("model", "dead out", "dead ops", "vd dead", "acc dead", "flg dead", "clamp", "acc-only"), file=out)
        for m in ("bb", "global"):
            print("   %-8s %8s %8s %8s %8s %8s %8s %8s" % (
                m, pct(s.get(m + ":dead", 0), s.get(m + ":outputs", 0)), pct(s.get(m + ":dead_ops", 0), s["vu_ops"]),
                pct(s.get(m + ":vd_dead", 0), s.get(m + ":vd", 0)), pct(s.get(m + ":acc_dead", 0), s.get(m + ":acc", 0)),
                pct(s.get(m + ":flags_dead", 0), s.get(m + ":flags", 0)),
                pct(s.get(m + ":clamp_only_dead", 0), s["families"].get("mac", {}).get("vu_ops", 0) + s["families"].get("mul", {}).get("vu_ops", 0)),
                pct(s.get(m + ":acc_all_dead", 0), s["families"].get("mac", {}).get("vu_ops", 0) + s["families"].get("mul", {}).get("vu_ops", 0))), file=out)
        now = s.get("now_ld", 0) + s.get("now_st", 0)
        print("   VU state traffic per block: now %d loads + %d stores; allocated %d + %d (all written items stored, %s of now), %d + %d (live ones only, %s)" % (
            s.get("now_ld", 0), s.get("now_st", 0), s.get("alloc_ld", 0), s.get("alloc_st_bb", 0),
            pct(s.get("alloc_ld", 0) + s.get("alloc_st_bb", 0), now), s.get("alloc_ld", 0), s.get("alloc_st_live", 0),
            pct(s.get("alloc_ld", 0) + s.get("alloc_st_live", 0), now)), file=out)
        fams = sorted(s["families"].items(), key=lambda kv: -kv[1].get("vu_ops", 0))
        print("   by family (global): " + ", ".join("%s %d ops, %s outputs dead" % (f, v.get("vu_ops", 0), pct(v.get("global:dead", 0), v.get("global:outputs", 0)))
                                             for f, v in fams if v.get("vu_ops")), file=out)


# ---- self test -------------------------------------------------------------------------------------------------------------------------------

def vu(funct, vd, vs, vt, e=0):
    return (0x12 << 26) | (1 << 25) | (e << 21) | (vt << 16) | (vs << 11) | (vd << 6) | funct


def jr(r):
    return (r << 21) | 8


def beq(rs, rt, off):
    return (4 << 26) | (rs << 21) | (rt << 16) | (off & 0xFFFF)


def sqv(vt, base, off=0):
    return (0x3a << 26) | (base << 21) | (vt << 16) | (4 << 11) | (off & 0x7F)


def selftest():
    VMUDN, VMADN, VMADH, VADD, VADDC, VSUB, VLT, VMRG, VCH, VCL = 0x06, 0x0E, 0x0F, 0x10, 0x14, 0x11, 0x20, 0x27, 0x25, 0x24
    fails = []

    def check(name, prog, idx, expect_dead, model="global"):
        words = (prog + [0] * 1024)[:1024]
        img = Image(words)
        succ, node_ins, _ = graph(img, model)
        lo = liveness(succ, node_ins)[1][("i", idx)]
        dead = img.dec[idx].defs & VU_STATE - lo
        if dead != set(expect_dead):
            fails.append("%s: dead %s, expected %s" % (name, sorted(dead), sorted(expect_dead)))

    # a matrix-row chain: the first two clamped results are overwritten before use; the accumulator flows; the last op's acc is killed by vmudn below
    chain = [vu(VMUDN, 1, 2, 3), vu(VMADN, 1, 4, 5), vu(VMADH, 6, 7, 8), sqv(6, 0), vu(VMUDN, 9, 9, 9), jr(31), 0]
    check("chain vmudn", chain, 0, ["v1"])
    check("chain vmadn", chain, 1, [])
    check("chain vmadh", chain, 2, ["AH", "AM", "AL"])
    check("chain vmadh bb", chain, 2, ["AH", "AM", "AL"], "bb")
    # VADD: ACC_LO dead (overwritten by the next VADD), VCO read by the next VADD (carry in)
    adds = [vu(VADD, 1, 2, 3), vu(VADD, 4, 1, 3), sqv(4, 0), vu(VADDC, 5, 5, 5), jr(31), 0]
    check("vadd carry", adds, 0, ["AL"])
    check("vadd 2", adds, 1, ["AL", "VCO"])
    # VLT does not write VCE: a VCE written before survives to the VCL after it
    cmp_ = [vu(VCH, 1, 2, 3), vu(VLT, 4, 1, 3), vu(VCL, 5, 4, 3), sqv(5, 0), vu(VCH, 6, 6, 6), jr(31), 0]
    check("vch before vlt", cmp_, 0, ["AL", "VCC"])
    check("vlt", cmp_, 1, ["AL"])
    # VMRG clears VCO: an earlier VADDC's VCO is dead if only a VMRG follows before the next reader
    mrg = [vu(VADDC, 1, 2, 3), vu(VMRG, 4, 1, 3), vu(VADD, 5, 4, 3), sqv(5, 0), vu(VCH, 6, 6, 6), jr(31), 0]
    check("vmrg kills vco", mrg, 0, ["AL", "VCO"])
    # across a conditional branch: global sees the overwrite on both paths, bb does not
    br = [vu(VMUDN, 1, 2, 3), beq(0, 0, 2), 0, vu(VMUDN, 1, 2, 3), vu(VMUDN, 1, 2, 3), sqv(1, 0), vu(VMUDN, 9, 9, 9), jr(31), 0]
    check("branch global", br, 0, ["v1", "AH", "AM", "AL"])
    check("branch bb", br, 0, [], "bb")
    # a COP0 access is an exit: nothing written before it is dead
    sync = [vu(VMUDN, 1, 2, 3), (0x10 << 26) | (0 << 21) | (8 << 16) | (4 << 11), vu(VMUDN, 1, 2, 3), jr(31), 0]
    check("sync", sync, 0, [])
    # break is an exit with everything live
    brk = [vu(VMUDN, 1, 2, 3), 13]
    check("break", brk, 0, [])
    # a jump into an overlay window from outside is an exit
    win = [vu(VMUDN, 1, 2, 3), beq(0, 0, 6), 0, vu(VMUDN, 1, 2, 3), jr(31), 0, 0, 0, vu(VMUDN, 1, 2, 3), jr(31), 0]
    words = (win + [0] * 1024)[:1024]
    img = Image(words)
    for wins, expect in (((), {"v1", "AH", "AM", "AL"}), (((0x20, 0x40),), set())):
        succ, node_ins, _ = graph(img, "global", wins)
        dead = img.dec[0].defs & VU_STATE - liveness(succ, node_ins)[1][("i", 0)]
        if dead != expect:
            fails.append("window %s: dead %s, expected %s" % (wins, sorted(dead), sorted(expect)))
    # the analysis runs on a whole image and reports
    res = analyse(Image((chain + [0] * 1024)[:1024]), ranges=[("chain", 0, 0x20)])
    if res["chain"]["vu_ops"] != 4 or res["chain"].get("global:dead_ops", 0) != 0:
        fails.append("analyse: %s" % {k: v for k, v in res["chain"].items() if k != "families"})
    # weights: a counts file with one site
    import tempfile
    with tempfile.TemporaryDirectory() as t:
        open(os.path.join(t, "sites"), "w").write("1 0 %08x 0\n2 4 %08x 0\n3 8 deadbeef 0\n" % (chain[0], chain[1]))
        open(os.path.join(t, "counts.M1"), "w").write("C 0 1 100\nC 0 2 50\nC 0 3 7\nC 1 1 9\n")
        w, seen, matched = load_weights(os.path.join(t, "counts.M1") + ":0", Image((chain + [0] * 1024)[:1024]))
        if dict(w) != {0: 100, 1: 50} or (seen, matched) != (3, 2):
            fails.append("weights: %s %d %d" % (dict(w), seen, matched))
    # byte order detection
    be = struct.pack(">1024I", *((chain + [0] * 1024)[:1024]))
    le = struct.pack("<1024I", *((chain + [0] * 1024)[:1024]))
    if load_image(be)[1] != "be" or load_image(le)[1] != "le" or load_image(le)[0].w[:4] != chain[:4]:
        fails.append("byte order detection")
    for f in fails:
        print("FAIL", f)
    print("selftest: %d failures" % len(fails))
    return 1 if fails else 0


def listing(img, lo, hi, windows=(), out=sys.stdout):
    """every instruction in [lo, hi): pc, disassembly, VU outputs dead in the bb / global model"""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    import rsp_isa
    lives = {}
    for model in ("bb", "global"):
        succ, node_ins, _ = graph(img, model, windows)
        lives[model] = liveness(succ, node_ins)[1]
    for i in range(lo >> 2, hi >> 2):
        d = img.dec[i]
        node = ("s", i - 1) if i and img.dec[i - 1].kind in ("br", "brl", "j", "jal", "jr", "jalr") else ("i", i)
        defs = d.defs & VU_STATE
        cols = ["%-12s" % ",".join(sorted(defs - lives[m][node])) or "-" for m in ("bb", "global")] if defs and d.vu not in ("vload",) else ["", ""]
        print("%03x  %08x  %-36s %s %s" % (i * 4, d.w, rsp_isa.disasm(d.w, i * 4), cols[0], cols[1]), file=out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("imem", nargs="?")
    ap.add_argument("--endian", choices=("be", "le"))
    ap.add_argument("--patch", action="append", default=[], help="FILE:FILEOFF:IMEMADDR:LEN, see above")
    ap.add_argument("--ranges", default="")
    ap.add_argument("--counts", help="PROFDIR/counts.LABEL:KEY, see above")
    ap.add_argument("--json")
    ap.add_argument("--window", action="append", default=[], help="LO-HI: an overlay window; jumps into it from outside are exits (repeatable)")
    ap.add_argument("--list", help="LO-HI: print every instruction of the range with its dead VU outputs (bb, global)")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.imem:
        ap.error("no IMEM image")
    patches = []
    for p in a.patch:
        f, off, addr, n = p.rsplit(":", 3)
        patches.append((open(f, "rb").read(), int(off, 0), int(addr, 0), int(n, 0)))
    img, endian = load_image(open(a.imem, "rb").read(), a.endian, patches)
    weights = None
    if a.counts:
        weights, seen, matched = load_weights(a.counts, img)
        print("weights: %d of %d counted sites match the image" % (matched, seen))
    windows = [tuple(int(x, 0) & 0xFFF for x in w.split("-")) for w in a.window]
    if a.list:
        lo, hi = (int(x, 0) & 0xFFF for x in a.list.split("-"))
        listing(img, lo, hi, windows)
        return 0
    res = analyse(img, weights, parse_ranges(a.ranges), windows)
    print("image %s (%s)%s" % (a.imem, endian, ", dynamic weights" if weights is not None else ", static counts"))
    report(res)
    if a.json:
        json.dump(res, open(a.json, "w"), indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
