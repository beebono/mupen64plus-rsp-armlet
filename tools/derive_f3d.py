"""The F3D (Fast3D 2.0D / 2.0G / 2.0H) family of tools/derive_build.py: the record of struct f3d_build (src/hle/f3d_bld.h, f3d_dmem.h) from the alignment with the registered builds
(docs/ucode-derive.md, "F3D records that run"). The layout and the masks are knobs as in the other families; the handler pcs come from the alignment, checked against the unknown's own handler
tables; everything else (the dispatch tables, the loop head, the exit BREAK, the attributes) is read from signatures in the unknown's text: the references cannot isolate an attribute by pairs
(no two builds differ in one attribute alone)."""
import struct

import derive_f1d as D1
import derive_f2 as F2
import derive_lib as L
import rsp_isa as isa

# the layout (F3D_LAY_FIELDS of f3d_dmem.h): F3_LAY_NKNOB addresses the code names (knobs), then the two table bases (read from the dispatchers), then constants of the data segment (copied)
F3_LAY_FIELDS = ["half1", "persp", "geom", "omh", "oml", "texture", "rdp_wp", "lights", "mtx_sp", "mtx_lim", "depth", "fifo", "segs", "mm_tab", "mw_tab", "retstack",
                 "viewport", "fog", "mv", "mp", "vtx_base", "cmdbuf", "geombuf", "outbuf", "ring_end", "vret",
                 "jt0", "jt2", "tri4_tab",
                 "light", "lim", "vsign", "clip_k", "planes", "tg", "plane_masks"]
F3_LAY_NKNOB = 26
F3_LAY_NSIG = 29
F3_PC_FIELDS = ["head", "nop", "brk", "mtx", "movemem", "vtx", "dl", "tri1", "quad", "halfc", "half2", "half1", "persp", "clrgeom", "setgeom", "enddl", "oml", "omh", "texture", "moveword",
                "popmtx", "culldl", "tri4"]
F3_PC_SIG = ("head", "brk")             # read from the text, not mapped
F3_FLAGS = ["flags_variant", "vtx_wr", "gp_end", "tri_wr", "popmtx_stale", "xbus", "vptr_mul"]
F3_BASE29 = 0x110
FLAGS_SM64, FLAGS_PW = 0, 1


def new_f3d_record(name):
    return D1.RecV("f3d", name)


def load_f3d_records(T, C):
    """one record per registered F3D build table (struct f3d_build of f3d.c)"""
    recs = {}
    for var, f in T.ucodes:
        if f.get("_macro") not in ("F3D_UCODE", "F3D_UCODE_R"):
            continue
        bn = f["fam_data"].lstrip("&").strip("()")
        if bn not in T.vars or T.vars[bn][0] != "f3d_build":
            continue
        b = T.vars[bn][1]
        r = recs.get(bn)
        if r is None:
            r = recs[bn] = new_f3d_record(bn)
            lay = T.vars[b["lay"][1]][1]
            for k in F3_LAY_FIELDS[:F3_LAY_NKNOB]:
                r.num["lay." + k] = (lay[k], "addr")
            r.num["reject_mask"] = (b.get("reject_mask", 0), "mask")
            r.num["clip_mask"] = (b.get("clip_mask", 0), "mask")
            for k in F3_LAY_FIELDS[F3_LAY_NKNOB:F3_LAY_NSIG]:
                r.num["lay." + k] = (lay[k], "sig")
            for k in F3_LAY_FIELDS[F3_LAY_NSIG:]:
                r.num["lay." + k] = (lay[k], "const")
            r.num["culldl_mask"] = (b.get("culldl_mask", 0), "sig")
            for k, v in zip(F3_PC_FIELDS, b["pc"]):
                r.num["pc." + k] = (v, "pc")
            fv = lay.get("flags_variant", 0)
            r.flags["flags_variant"] = fv if isinstance(fv, int) else T.consts.get(fv[1], 0)
            for k in F3_FLAGS[1:]:
                r.flags[k] = int(b.get(k, 0))
            r.load_key = b.get("load_key") or None
        r.ids.append(L.uhex(f["id"]))
    return list(recs.values())


# ---- signatures read from the unknown's own code -------------------------------------------------------------------------------------------------------------------------------
def dec(w):
    return isa.decode(w)


def text_words_at(U, pc, n):
    """n words of the text image from IMEM pc (pc >= 0x80: the resident code)"""
    i0 = U.img.idx_text(pc)
    return [U.img.w[i] for i in range(i0, min(i0 + n, 1024))]


def overlay(U, k):
    """overlay k of the data's DMA table (descriptor at DMEM 8 + 8 k: text offset, length - 1 << 16 | imem): (imem, [words]) from the text or the RDRAM behind it, or (None, [])"""
    d = U.data
    a = 8 + 8 * k
    if a + 8 > len(d):
        return None, []
    off, b = struct.unpack(">II", d[a:a + 8])
    ln = ((b >> 16) & 0xfff) + 1
    imem = b & 0xfff
    ram = U.uc.text_b[:0x1000] + U.uc.ext        # the RDRAM from the text on
    if off + ln > len(ram):
        return None, []
    src = ram[off:off + ln]
    return imem, isa.words(src[:ln & ~3])


def sig_tables(U):
    """the dispatchers: class 0 `andi v0, v0, 0x1fe ; lh v0, jt0(v0)`, class 2 `andi v0, v0, 0xfe ; lh v0, jt2(v0)`; -> (jt0, jt2), None where not exactly one"""
    out = []
    ws = U.img.w[:1024]
    for m in (0x1fe, 0xfe):
        hits = set()
        for i in range(len(ws) - 1):
            a, b = dec(ws[i]), dec(ws[i + 1])
            if a.get("name") == "andi" and a["imm"] == m and a["rs"] == a["rt"] and b.get("name") == "lh" and b["rs"] == a["rt"] and b["rt"] == a["rt"]:
                hits.add(b["imm"] & 0xffff)
        out.append(hits.pop() if len(hits) == 1 else None)
    return tuple(out)


def u_table_targets(U, jt0, jt2):
    """the handler pcs the unknown's own tables lead the GBI's opcodes to (class 0: 0x00 .. 0x09 at jt0 + 2 op; class 2: 0xb0 .. 0xbf at jt2 + 2 (op - 0x80)), as f3d.c's make_kinds reads them"""
    out = set()
    d = U.data
    for op in list(range(0x00, 0x0a)) + list(range(0xb0, 0xc0)):
        a = (jt0 + 2 * op) if op < 0x40 else (jt2 + 2 * (op - 0x80))
        if 0 <= a and a + 2 <= len(d):
            out.add(((d[a] << 8) | d[a + 1]) & 0xfff)
    return out


def sig_loop(U, nop):
    """the loop head and the loop's form: the instruction 16 bytes after the return point branches back to the head, `bgtz gp, head` (gp counts the bytes left) or `bne gp, k1, head` (gp is the
    end address of the buffer); the counting form also has `addi gp, gp, -8` in the head code (overlay 0). -> (head, gp_end), None where undecided"""
    if not nop:
        return None, None
    w = text_words_at(U, nop + 16, 1)
    if not w:
        return None, None
    d = dec(w[0])
    tgt = (nop + 16 + 4 + 4 * d["off"]) & 0xfff if d.get("cls") == isa.C_BR else None
    form = None
    if d.get("name") == "bgtz" and d["rs"] == 28:
        form = 0
    elif d.get("name") == "bne" and {d["rs"], d["rt"]} == {28, 27}:
        form = 1
    _, ov0 = overlay(U, 0)
    count = any(dec(x).get("name") == "addi" and dec(x)["rs"] == 28 and dec(x)["rt"] == 28 and dec(x)["imm"] == 0xfff8 for x in ov0)
    if form is None or tgt is None:
        return None, None
    if form == 0 and count:
        return tgt, 0
    if form == 1 and not count:
        return tgt, 1
    return tgt, None


def sig_geombuf(U):
    """the geometry DMA buffer: the loop head code (overlay 0) sets `addi s6, zero, geombuf` for the class-0 DMA"""
    _, ov0 = overlay(U, 0)
    hits = {dec(w)["imm"] for w in ov0 if dec(w).get("name") == "addi" and dec(w)["rs"] == 0 and dec(w)["rt"] == 22}
    return hits.pop() if len(hits) == 1 else None


def sig_tri4_tab(U, tpc):
    """GoldenEye's G_TRI4 looks the vertex nibbles up in a byte table: `lbu rX, TAB(rX)` among its first words (0: no such command)"""
    if not tpc:
        return 0
    hits = {dec(w)["imm"] & 0xffff for w in text_words_at(U, tpc, 20) if dec(w).get("name") == "lbu" and dec(w)["rs"] == dec(w)["rt"] and dec(w)["rs"] != 0}
    return hits.pop() if len(hits) == 1 else None


def sig_brk(U):
    """the BREAK of the exit code (overlay 3)"""
    imem, ws = overlay(U, 3)
    for i, w in enumerate(ws):
        if dec(w).get("name") == "break":
            return imem + 4 * i
    return None


def sig_vtx_wr(U, vpc):
    """Wave Race's G_VTX takes the count from byte 2 of w0 (`lbu rX, -6(k1) ; srl rX, rX, 1`), the others from the high nibble of byte 1 (`srl at, at, 4`)"""
    if not vpc:
        return None
    ds = [dec(w) for w in text_words_at(U, vpc, 8)]
    a = any(d.get("name") == "lbu" and d["rs"] == 27 and d["imm"] == -6 for d in ds)
    b = any(d.get("name") == "srl" and d["sa"] == 1 and d["rd"] == d["rt"] for d in ds)
    s = any(d.get("name") == "srl" and d["sa"] == 4 and d["rd"] == d["rt"] == 1 for d in ds)
    if a and b and not s:
        return 1
    if s and not a and not b:
        return 0
    return None


def sig_vptr(U, tpc, vbase):
    """G_TRI1's vertex pointers: three `sll rX, rX, s` followed by three `addi rX, rX, vtx_base` (in the handler, or in the routine it calls first); the multiplier 1 << s"""
    if not tpc:
        return None
    ds = [dec(w) for w in text_words_at(U, tpc, 16)]
    for d in ds[:3]:
        if d.get("name") == "jal":
            ds = ds + [dec(w) for w in text_words_at(U, d["tgt"], 16)]
            break
    shifts = [d["sa"] for d in ds if d.get("name") == "sll" and d["rd"] == d["rt"] and d["rd"] in (1, 2, 3)]
    adds = [d for d in ds if d.get("name") == "addi" and d["rs"] == d["rt"] and d["rt"] in (1, 2, 3) and d["imm"] == vbase]
    if len(shifts) >= 3 and len(set(shifts[-3:])) == 1 and shifts[-1] in (2, 3) and len(adds) >= 3:
        return 1 << shifts[-1]
    return None


def sig_tri_wr(U):
    """Wave Race's triangle code has no front-face cull (`andi rX, rY, 0x1000`) and no flat-shading choice (`andi rX, rY, 0x200`); a text with one and not the other is undecided"""
    ws = U.img.w[:1024]
    c1000 = any(dec(w).get("name") == "andi" and dec(w)["imm"] == 0x1000 for w in ws)
    c200 = any(dec(w).get("name") == "andi" and dec(w)["imm"] == 0x200 for w in ws)
    if c1000 and c200:
        return 0
    if not c1000 and not c200:
        return 1
    return None


def sig_flags_variant(U):
    """Pilotwings' clip flag word: the near-plane bit masked off one vertex (`andi rX, rY, 0x703`, `0x7030`) and the signs of w added (`andi rX, rX, 0x8000 ; srl rX, rX, 13` and `srl 9`), as F3DEX.NoN's"""
    ds = [dec(w) for w in U.img.w[:1024]]
    n703 = sum(1 for d in ds if d.get("name") == "andi" and d["imm"] == 0x703)
    n707 = sum(1 for d in ds if d.get("name") == "andi" and d["imm"] == 0x707)
    nsign = sum(1 for q in range(1, len(ds)) if ds[q].get("name") == "srl" and ds[q]["sa"] in (9, 13) and ds[q]["rd"] == ds[q]["rt"] and ds[q - 1].get("name") == "andi"
                and ds[q - 1]["imm"] == 0x8000 and ds[q - 1]["rt"] == ds[q]["rt"])
    if n703 and nsign >= 2:
        return FLAGS_PW
    if not n703 and not nsign and n707:
        return FLAGS_SM64
    return None


def sig_popmtx_stale(U, ppc, lights):
    """G_POPMTX marks the lights stale: a store of a vector byte to the light count word (`sbv vX[e], lights - 0x110(sp)`) among its first words"""
    if not ppc:
        return 0
    for w in text_words_at(U, ppc, 3):
        d = dec(w)
        if d.get("name") in ("sbv", "sb") and ((d["imm"] + (F3_BASE29 if d["rs"] == 29 else 0)) & 0xffff) == lights:
            return 1
    return 0


def sig_xbus(U, outbuf):
    """The XBUS transport: the start-up code sets DPC_STATUS' xbus bit (`addi rA, zero, 2 ; ... mtc0 rA, c0_11`) and the flush points DPC_START into DMEM (`addi rB, zero, outbuf ; ... mtc0 rB, c0_8`);
    the FIFO builds clear the bit (`addi rA, zero, 1 ; ... mtc0 rA, c0_11`) and do neither"""
    ds = [dec(w) for w in U.img.w[:1024]]

    def pair(val, creg):
        for i, d in enumerate(ds):
            if d.get("name") == "addi" and d["rs"] == 0 and d["imm"] == val:
                for e in ds[i + 1:i + 5]:
                    if e.get("name") == "mtc0" and e["rt"] == d["rt"] and e["rd"] == creg:
                        return True
        return False
    st, clr, dm = pair(2, 11), pair(1, 11), pair(outbuf, 8)
    if st and dm and not clr:
        return 1
    if clr and not st and not dm:
        return 0
    return None


def sig_culldl_mask(U, cpc):
    """G_CULLDL starts its accumulator with the mask: `ori rX, zero, MASK` among its first words"""
    if not cpc:
        return None
    for w in text_words_at(U, cpc, 4):
        d = dec(w)
        if d.get("name") == "ori" and d["rs"] == 0:
            return d["imm"]
    return None


# ---- the derivation -----------------------------------------------------------------------------------------------------------------------------------------------------------
def derive_f3d(U, refs):
    res = D1.ResultV()
    prs = {R.name: F2.Pairing(R, U) for R in refs}
    order = sorted(prs.values(), key=lambda P: (len(P.x_set()), len(P.xr), -len(P.pr)))
    expl = {P.R.name: {j for j in range(U.img.n) if P.status[j][0] != "X"} for P in order}
    undecided = []

    def composite(prim):
        cur = set(prim.x_set())
        pool = [P for P in order if P is not prim]
        contrib = {}
        while cur and pool:
            best = max(pool, key=lambda P: len(cur & expl[P.R.name]))
            n = len(cur & expl[best.R.name])
            if n == 0:
                break
            contrib[best.R.name] = n
            cur -= expl[best.R.name]
            pool.remove(best)
        return cur, contrib

    def cost(P):
        cur, _ = composite(P)
        nxr = sum(1 for i in P.xr if i not in P.hunk_of_r or any(j in cur for j in P.hunk_of_r[i]))
        return len(cur) + nxr
    costs = [cost(P) for P in order]
    prim = order[min(range(len(order)), key=lambda q: (costs[q], q))]
    order1 = [prim] + [P for P in order if P is not prim]
    res.primary, res.prs, res.prim = prim.R.name, prs, prim
    cur, res.contrib = composite(prim)
    rec = new_f3d_record("(derived)")
    merged = F2.merge_knobs(order1)
    res.merged = merged
    for k, (v, kind) in prim.R.rec.num.items():
        if kind in ("addr", "mask"):
            rec.num[k] = (merged[k][0] if k in merged else v, kind)
            if k not in merged and v >= 0x10:
                res.unanchored.append((k, v, 0))
        else:
            rec.num[k] = (v, kind)
    lay = {k[4:]: v for k, (v, kind) in rec.num.items() if k.startswith("lay.")}

    def decide(name, val):
        if val is None:
            undecided.append(name)
            res.notes.append("%s: the text's signature does not decide it; taken from the primary reference" % name)
            return False
        return True
    # the tables
    jt0, jt2 = sig_tables(U)
    for k, v in (("lay.jt0", jt0), ("lay.jt2", jt2)):
        if decide(k, v):
            rec.num[k] = (v, "sig")
    lay = {k[4:]: v for k, (v, kind) in rec.num.items() if k.startswith("lay.")}
    targets = u_table_targets(U, lay["jt0"], lay["jt2"])
    res.how = {}
    taken = set()               # a handler is one command's: a pc another field took (a reference's handler that the unknown lacks maps onto a neighbour) is not taken again
    for k in F3_PC_FIELDS:
        if k in F3_PC_SIG:
            continue
        v = 0
        for P in order1:
            pr = P.R.rec.num["pc." + k][0]
            if not pr:
                continue
            m = F2.map_pc(P, pr)
            if m is not None and m > 0 and m in targets and m not in taken:
                v = m
                res.how["pc." + k] = P.R.name
                break
        rec.num["pc." + k] = (v, "pc")
        if v:
            taken.add(v)
    pc = {k: rec.num["pc." + k][0] for k in F3_PC_FIELDS}
    head, gp_end = sig_loop(U, pc["nop"])
    if decide("pc.head", head):
        rec.num["pc.head"] = (head, "pc")
    gb = sig_geombuf(U)
    if gb is not None:
        rec.num["lay.geombuf"] = (gb, "addr")
        res.unanchored = [u for u in res.unanchored if u[0] != "lay.geombuf"]
    brk = sig_brk(U)
    if decide("pc.brk", brk):
        rec.num["pc.brk"] = (brk, "pc")
    t4 = sig_tri4_tab(U, pc["tri4"])
    if decide("lay.tri4_tab", t4):
        rec.num["lay.tri4_tab"] = (t4, "sig")
    cm = sig_culldl_mask(U, pc["culldl"])
    if pc["culldl"] and decide("culldl_mask", cm):
        rec.num["culldl_mask"] = (cm, "sig")
    flags = dict(prim.R.rec.flags)
    for k, v in (("gp_end", gp_end), ("vtx_wr", sig_vtx_wr(U, pc["vtx"])), ("vptr_mul", sig_vptr(U, pc["tri1"], lay["vtx_base"])), ("tri_wr", sig_tri_wr(U)),
                 ("flags_variant", sig_flags_variant(U)), ("popmtx_stale", sig_popmtx_stale(U, pc["popmtx"], lay["lights"])), ("xbus", sig_xbus(U, lay["outbuf"]))):
        res.flag_evidence[k] = v
        if decide("flag." + k, v):
            flags[k] = v
    for k in F3_FLAGS:
        rec.flags[k] = flags[k]
    rec.load_key = L.fnv(U.uc.text_b, 0xf48)
    res.rec = rec
    res.conflicts = dict(prim.conflict)
    res.x = {j: prim.status[j][1] for j in cur}
    res.xr = dict(prim.xr)
    res.near = {}
    res.waived = {}
    xr = {i: "extra" for i in res.xr if i not in prim.hunk_of_r or any(j in res.x for j in prim.hunk_of_r[i])}
    runs = []
    for i in sorted(xr):
        if runs and runs[-1][-1] == i - 1:
            runs[-1].append(i)
        else:
            runs.append([i])
    for run in runs:
        if len(run) < 16:
            for i in run:
                xr[i] = "deleted"
    res.xr_res = xr
    res.undecided = undecided
    trouble = bool(prim.conflict or prim.ambig)
    if trouble:
        res.notes.append("a constant or address of the record is read differently at different places: %s" % dict(prim.conflict, **prim.ambig))
    res.trouble = trouble
    res.verdict = "NONE" if (res.x or any(c == "deleted" for c in xr.values()) or undecided or trouble) else ("NEAR" if xr else "EXACT")
    return res
