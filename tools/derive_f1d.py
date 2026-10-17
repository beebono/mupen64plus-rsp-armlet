"""The F3DEX 1.x (F3DEX / F3DLX / .NoN, 0.95 .. 1.23) and F3D (2.0D / 2.0G / 2.0H) families of tools/derive_build.py. Their build records are small (src/hle/f3dex.c, f3d.c): a few
constants and variant flags; the layouts are fixed per variant. The derivation is the same alignment as for F3DEX2, with the references' own flags for the variant."""
import derive_f2 as F2
import derive_lib as L
import rsp_isa as isa


class RecV:
    """a variant-family record: numeric fields with a kind, plus variant flags"""

    def __init__(self, fam, name):
        self.fam = fam
        self.name = name
        self.ids = []
        self.num = {}       # field -> (value, kind)  kind: addr | mask | pc | const
        self.flags = {}     # field -> value (discrete variant attributes: switches, slot counts; decided by which references explain the code)
        self.ext = []
        self.load_key = None
        self.lay = {}
        self.pc = {}

    def knobs(self):
        out = []
        for k, (v, kind) in self.num.items():
            if kind in ("addr", "mask") and v >= 0x10:
                out.append(F2.Knob(k, v, kind))
        return out

    def fields(self):
        d = {}
        for k, (v, kind) in self.num.items():
            d[k] = v
        for k, v in self.flags.items():
            d["flag." + k] = v
        if self.fam == "f3dex1":
            d["load_key"] = self.load_key
        return d


# the F3DEX 1.x record (src/hle/f3dex_bld.h, f3dex_dmem.h): the DMEM layout (the first F1_LAY_NKNOB fields are addresses the code names, the rest constants of the data segment that are copied), the handler
# pcs, the reject mask and three variant attributes
F1_LAY_FIELDS = ["jt0", "jt2", "half1", "persp", "geom", "omh", "oml", "texture", "rdp_wp", "lights", "mtx_sp", "mtx_lim", "depth", "fifo", "segs", "light", "mm_tab", "mw_tab", "vtab",
                 "viewport", "fog", "retstack", "mv", "proj", "mvp", "vbuf", "window", "operand", "outbuf",
                 "lim", "tgbias", "vsign", "clip_k", "planes", "tg", "plane_masks"]
F1_LAY_NKNOB = 29
F1_PC_FIELDS = ["head", "nop", "mtx", "movemem", "vtx", "dl", "loaduc", "branchz", "tri2", "modvtx", "half2", "half1", "persp", "quad", "clrgeom", "setgeom", "enddl", "oml", "omh", "texture",
                "moveword", "popmtx", "culldl", "tri1"]
F1_FLAGS = ["non", "dlx", "culldl_off"]
F1_BASE29 = 0x110           # $29: the constant base register of the whole run (`addi sp, zero, 0x110` at IMEM 0x84); state words are addressed relative to it
F1_GEOM_DEF = 0x1 | 0x2 | 0x4 | 0x200 | 0x1000 | 0x2000 | 0x10000 | 0x20000 | 0x40000 | 0x80000 | 0x800000     # F3DEX_G_DEFINED: the geometry bits the front-end gives a meaning to


def new_f1_record(name):
    r = RecV("f3dex1", name)
    return r


def load_f1_records(T, C):
    """one record per registered F3DEX 1.x build table (struct f3dex1_build of f3dex.c)"""
    recs = {}
    for var, f in T.ucodes:
        if f.get("_macro") != "U":
            continue
        bn = f["fam_data"].lstrip("&").strip("()")
        if bn not in T.vars or T.vars[bn][0] != "f3dex1_build":
            continue
        b = T.vars[bn][1]
        r = recs.get(bn)
        if r is None:
            r = recs[bn] = new_f1_record(bn)
            fx = b["fx"]
            lay = T.vars[fx[3][1]][1]
            for k in F1_LAY_FIELDS[:F1_LAY_NKNOB]:
                r.num["lay." + k] = (lay[k], "addr")
            r.num["reject_mask"] = (fx[0], "mask")
            for k in F1_LAY_FIELDS[F1_LAY_NKNOB:]:
                r.num["lay." + k] = (lay[k], "const")
            for k, v in zip(F1_PC_FIELDS, b["pc"]):
                r.num["pc." + k] = (v, "const" if k == "head" else "pc")
            r.flags["non"] = fx[1]
            r.flags["dlx"] = fx[2]
            r.flags["culldl_off"] = int(b.get("culldl_off", 0))
            r.load_key = b.get("load_key") or None
        r.ids.append(L.uhex(f["id"]))
    return list(recs.values())


def load_f3d_records(T, C):
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
            r = recs[bn] = RecV("f3d", bn)
            lay = T.vars[b["lay"][1]][1]
            for nm, v in zip(("mp", "mv", "viewport", "fog", "vtx_base", "vtx_stride", "flags_variant", "nslots"), lay):
                v = v if isinstance(v, int) else T.consts.get(v[1], 0)
                if nm in ("mp", "mv", "viewport", "fog", "vtx_base"):
                    r.num["lay." + nm] = (v, "addr")
                else:
                    r.flags["lay." + nm] = v
            for nm, kind in (("cmdbuf", "addr"), ("geom", "addr"), ("outbuf", "addr"), ("geom_end", "addr"), ("loop_pc", "pc"), ("ov_base", "addr"), ("clip_mask", "mask"),
                             ("reject_mask", "mask"), ("dl_depth", "addr"), ("culldl_mask", "mask")):
                r.num[nm] = (b.get(nm, 0), kind)
            r.flags["vptr_mul"] = b.get("vptr_mul", 0)
            for nm in ("vtx_wr", "gp_end", "quad", "tri_wr", "popmtx_stale", "xbus", "half_shift"):
                r.flags[nm] = int(b.get(nm, 0))
        r.ids.append(L.uhex(f["id"]))
    return list(recs.values())


class ItemV:
    def __init__(self, uc, rec=None, name=None):
        self.uc = uc
        self.rec = rec
        self.name = name or (rec.name if rec else uc.stem[:16])
        self.img = L.Image(uc, [])
        self.data = uc.data


class ResultV:
    def __init__(self):
        self.rec = None
        self.verdict = "NONE"
        self.primary = None
        self.contrib = {}
        self.x = {}
        self.xr = {}
        self.near = {}
        self.notes = []
        self.unanchored = []
        self.conflicts = {}
        self.derived = {}
        self.flag_evidence = {}


def u_table_targets(U, lay):
    """the handler pcs the unknown's own tables lead the GBI's opcodes to (class 0: 0x00 .. 0x09 at jt0 + 2 op; class 2: 0xaf .. 0xbf at jt2 + 2 (op & 0x7f)), as f3dex.c's make_kinds reads them"""
    out = set()
    d = U.data
    for op in list(range(0x00, 0x0a)) + list(range(0xaf, 0xc0)):
        a = (lay["jt0"] + 2 * op) if op < 0x40 else (lay["jt2"] + 2 * (op & 0x7f))
        if 0 <= a and a + 2 <= len(d):
            out.add(((d[a] << 8) | d[a + 1]) & 0xffc)
    return out


def tail_calls(P):
    """The one known equivalent rewrite between the F3DEX 1.x versions: a call that returns through a saved ra (`add rX, zero, ra ; ... jal T ; ... jr rX`, 1.21 / 1.22, in the display-list window refill)
    against the tail jump `j T` (1.23); the only difference is the value left in rX, which the code sets again before it reads it (it is the overlay entry register $21). -> (U words, R words) it explains"""
    iu, ir = P.U.img, P.R.img
    xu = sorted(j for j, (k, _) in P.status.items() if k == "X")
    xr = set(P.xr)
    uw, rw = set(), set()

    def anchor_r(j):
        k = j - 1
        while k >= 0 and k not in P.pu:
            k -= 1
        return P.pu[k] if k >= 0 else -1

    def save_reg(w):
        d = isa.decode(w)
        return d["rd"] if d.get("name") == "add" and d.get("rs") == 0 and d.get("rt") == 31 and d.get("rd", 0) != 0 else -1
    def jal_tgt(w):
        d = isa.decode(w)
        return d["tgt"] if d.get("name") == "jal" else -1
    def j_tgt(w):
        d = isa.decode(w)
        return d["tgt"] if d.get("name") == "j" else -1
    def jr_reg(w):
        d = isa.decode(w)
        return d["rs"] if d.get("name") == "jr" else -1

    def triple(img, cand, k0):
        """the call form starting at index k0 among the candidate indices: (k0, k1, k2, target) or None"""
        r = save_reg(img.w[k0])
        if r < 0:
            return None
        for k1 in cand:
            if k0 < k1 <= k0 + 8 and jal_tgt(img.w[k1]) >= 0:
                for k2 in cand:
                    if k1 < k2 <= k0 + 8 and jr_reg(img.w[k2]) == r:
                        return (k0, k1, k2, jal_tgt(img.w[k1]))
                return None
        return None
    # the unknown has the call form, the reference the tail jump
    for j0 in xu:
        t = triple(iu, xu, j0)
        if not t:
            continue
        a = anchor_r(j0)
        hit = [i for i in sorted(xr - rw) if a - 12 <= i <= a + 12 and j_tgt(ir.w[i]) == t[3]]
        if hit:
            uw |= {t[0], t[1], t[2]}
            rw.add(hit[0])
    # the unknown has the tail jump, the reference the call form
    xrs = sorted(xr - rw)
    for j0 in xu:
        if j0 in uw or j_tgt(iu.w[j0]) < 0:
            continue
        a = anchor_r(j0)
        for i0 in xrs:
            if not (a - 12 <= i0 <= a + 12):
                continue
            t = triple(ir, xrs, i0)
            if t and t[3] == j_tgt(iu.w[j0]):
                uw.add(j0)
                rw |= {t[0], t[1], t[2]}
                break
    return uw, rw


def geom_guarded(P, lay):
    """unaligned runs of the unknown that start with a test of a geometry-mode bit the front-end does not define (`lw rA, geom ; andi rA, rA, BIT ; beq rA, zero, END` where END is the first word after
    the run): code that runs only with that bit set, which the front-end declines (f3dex.c, the G_VTX / triangle guard). Iggy's Reckin' Balls' F3DEX.NoN 1.22 has 14 such words in its vertex loop
    (bit 0x4000). -> the U words"""
    iu = P.U.img
    out = set()
    for run in P.u_runs:
        if len(run) < 3 or any(P.status[j][0] != "X" for j in run):
            continue
        d0, d1, d2 = (isa.decode(iu.w[j]) for j in run[:3])
        if not (d0.get("name") == "lw" and d1.get("name") == "andi" and d2.get("name") == "beq"):
            continue
        addr = ((d0["imm"] + (F1_BASE29 if d0["rs"] == 29 else 0)) & 0xffff) if d0["rs"] in (0, 29) else -1
        bit = d1["imm"]
        if addr != lay["geom"] or d1["rs"] != d0["rt"] or d1["rt"] != d0["rt"] or bit == 0 or (bit & (bit - 1)) or (bit & F1_GEOM_DEF):
            continue
        if d2["rs"] != d0["rt"] or d2["rt"] != 0:
            continue
        end = run[-1] + 1
        tgt = (iu.pc[run[2]] + 4 + 4 * d2["off"]) & 0xfff
        if end >= iu.n or iu.seg[end] != iu.seg[run[0]] or iu.pc[end] != tgt or end not in P.pu:
            continue
        out |= set(run)
    return out


def derive_f1(U, refs):
    """The F3DEX 1.x derivation (docs/ucode-derive.md, "F3DEX 1.x records that run"): the record of struct f3dex1_build from the alignment with the registered builds."""
    res = ResultV()
    prs = {R.name: F2.Pairing(R, U) for R in refs}
    order = sorted(prs.values(), key=lambda P: (len(P.x_set()), len(P.xr), -len(P.pr)))
    expl = {P.R.name: {j for j in range(U.img.n) if P.status[j][0] != "X"} for P in order}
    undecided = []
    flags = {}
    # dlx: the words that only references of one value explain (the F3DLX triangle code and flush differ in hundreds of words)
    g = {0: set(), 1: set()}
    for P in order:
        g[P.R.rec.flags["dlx"]] |= expl[P.R.name]
    ev0, ev1 = len(g[0] - g[1]), len(g[1] - g[0])
    res.flag_evidence["dlx"] = (ev0, ev1)
    hi, lo = max(ev0, ev1), min(ev0, ev1)
    if hi >= 8 and hi >= 4 * lo:
        flags["dlx"] = 1 if ev1 > ev0 else 0
    else:
        flags["dlx"] = order[0].R.rec.flags["dlx"]
        undecided.append("dlx")
        res.notes.append("dlx: the evidence (%d / %d words) does not decide it; taken from the best reference" % (ev0, ev1))
    # non: read from the text. The NoN vertex code masks the near-plane bit off one vertex's flags (`andi rX, rY, 0x703`; the others use 0x707 only) and adds the signs of w
    # (`andi rX, rX, 0x8000 ; srl rX, rX, 13` and `... srl 9`); both signatures, or neither (and a 0x707 mask), decide it; anything else leaves it undecided (NONE)
    dec = [isa.decode(w) for w in U.img.w]
    n703 = sum(1 for d in dec if d.get("name") == "andi" and d["imm"] == 0x703)
    n707 = sum(1 for d in dec if d.get("name") == "andi" and d["imm"] == 0x707)
    nsign = sum(1 for q in range(1, len(dec)) if dec[q].get("name") == "srl" and dec[q]["sa"] in (9, 13) and dec[q]["rd"] == dec[q]["rt"] and dec[q - 1].get("name") == "andi"
                and dec[q - 1]["imm"] == 0x8000 and dec[q - 1]["rt"] == dec[q]["rt"])
    res.flag_evidence["non"] = (n707, n703, nsign)
    if n703 and nsign >= 2:
        flags["non"] = 1
    elif not n703 and not nsign and n707:
        flags["non"] = 0
    else:
        flags["non"] = order[0].R.rec.flags["non"]
        undecided.append("non")
        res.notes.append("non: the text has %d near-plane masks 0x703, %d masks 0x707 and %d w-sign shifts: no consistent NoN or non-NoN vertex code; taken from the best reference" % (n703, n707, nsign))
    # the primary reference: the one that leaves the fewest words unexplained, after the composite (its unaligned words that the others explain) and counting its own words the unknown lacks
    # (a reference of the other NoN value has 8 words of NoN code the unknown lacks, or the unknown 8 that the reference lacks, which the composite gives to a NoN reference)
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
    # the record: knobs (layout, reject mask), the constants and pcs, the flags
    rec = RecV("f3dex1", "(derived)")
    merged = F2.merge_knobs(order1)
    res.merged = merged
    for k, (v, kind) in prim.R.rec.num.items():
        if kind in ("addr", "mask"):
            rec.num[k] = (merged[k][0] if k in merged else v, kind)
            if k not in merged and v >= 0x10:
                res.unanchored.append((k, v, 0))
        elif kind == "const":
            rec.num[k] = (v, kind)
    lay = {k[4:]: v for k, (v, kind) in rec.num.items() if k.startswith("lay.")}
    targets = u_table_targets(U, lay)
    res.how = {}
    for k in F1_PC_FIELDS[1:]:
        v = 0
        for P in order1:
            pr = P.R.rec.num["pc." + k][0]
            if not pr:
                continue
            m = F2.map_pc(P, pr)
            if m is not None and m > 0 and (k == "head" or m in targets):
                v = m
                res.how["pc." + k] = P.R.name
                break
        rec.num["pc." + k] = (v, "pc")
    # culldl_off: the index form looks the two vertex numbers up in the vertex table (`lhu rX, vtab(rY)` in the handler's first words); the 0.95 form uses byte offsets
    cpc = rec.num["pc.culldl"][0]
    if cpc:
        ws = [U.img.w[U.img.idx_text(cpc) + q] for q in range(8) if U.img.idx_text(cpc) + q < U.img.n]
        flags["culldl_off"] = 0 if any(isa.decode(w).get("name") == "lhu" and isa.decode(w)["rs"] != 0 and (isa.decode(w)["imm"] & 0xffff) == lay["vtab"] for w in ws) else 1
    else:
        flags["culldl_off"] = prim.R.rec.flags["culldl_off"]
    for k in F1_FLAGS:
        rec.flags[k] = flags[k]
    rec.load_key = L.fnv(U.uc.text_b, 0xf48)
    res.rec = rec
    res.conflicts = dict(prim.conflict)
    # known equivalent rewrites and guarded features
    uw, rw = tail_calls(prim)
    gw = geom_guarded(prim, lay)
    for j in uw:
        cur.discard(j)
    res.waived = {j: "tail call" for j in uw}
    res.near = {}
    for j in gw:
        if j in cur:
            cur.discard(j)
            res.near[j] = "geom-guarded"
    res.x = {j: prim.status[j][1] for j in cur}
    res.xr = {i: v for i, v in prim.xr.items() if i not in rw}
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
    res.verdict = "NONE" if (res.x or any(c == "deleted" for c in xr.values()) or undecided or trouble) else ("NEAR" if (xr or res.near) else "EXACT")
    return res


def derive_variant(U, refs):
    res = ResultV()
    prs = {R.name: F2.Pairing(R, U) for R in refs}
    order = sorted(prs.values(), key=lambda P: (len(P.x_set()), len(P.xr), -len(P.pr)))
    prim = order[0]
    res.primary, res.prs, res.prim = prim.R.name, prs, prim
    expl = {P.R.name: {j for j in range(U.img.n) if P.status[j][0] != "X"} for P in order}
    cur = set(prim.x_set())
    pool = list(order[1:])
    while cur and pool:
        best = max(pool, key=lambda P: len(cur & expl[P.R.name]))
        n = len(cur & expl[best.R.name])
        if n == 0:
            break
        res.contrib[best.R.name] = n
        cur -= expl[best.R.name]
        pool.remove(best)
    res.x = {j: prim.status[j][1] for j in cur}
    res.xr = dict(prim.xr)
    rec = RecV(prim.R.rec.fam, "(derived)")
    merged = F2.merge_knobs(order)
    for k, (v, kind) in prim.R.rec.num.items():
        rec.num[k] = (merged[k][0] if k in merged else v, kind)
        if k not in merged and kind in ("addr", "mask") and v >= 0x10:
            res.unanchored.append((k, v, 0))
    # knobs that two fields share a value for in the reference cannot be told apart by the instruction that holds it
    for P in order[:1]:
        byval = {}
        for k in P.ks:
            byval.setdefault(k.val, []).append(k.name)
        for val, names in byval.items():
            if len(names) > 1:
                seen = sorted({hex(e["vu"]) for e in P.ev if e["kind"] in ("imm", "mem") and e["vr"] == val and e["vu"] != e["vr"]})
                if len(seen) > 1:
                    res.notes.append("%s share the value %s in %s; the aligned code shows %s: which is which needs the handler context" % ("/".join(names), hex(val), P.R.name, "/".join(seen)))
    for k, v in prim.R.rec.flags.items():
        rec.flags[k] = v
    # the discrete attributes: decided by the pairs of references that differ in that attribute only: a word of the unknown that one explains and the other does not is a vote
    for k in prim.R.rec.flags:
        votes = {}
        npairs = 0
        for a_ in refs:
            for b_ in refs:
                if a_.name >= b_.name or a_.rec.flags[k] == b_.rec.flags[k]:
                    continue
                if any(a_.rec.flags[o] != b_.rec.flags[o] for o in a_.rec.flags if o != k):
                    continue
                npairs += 1
                for j in expl[a_.name] - expl[b_.name]:
                    votes[a_.rec.flags[k]] = votes.get(a_.rec.flags[k], 0) + 1
                for j in expl[b_.name] - expl[a_.name]:
                    votes[b_.rec.flags[k]] = votes.get(b_.rec.flags[k], 0) + 1
        res.flag_evidence[k] = (npairs, votes)
        top = sorted(votes.items(), key=lambda kv: -kv[1])
        if npairs and top and (len(top) == 1 or top[0][1] >= 4 * top[1][1]):
            rec.flags[k] = top[0][0]
        elif not npairs:
            res.notes.append("%s: no pair of references differs in this attribute alone; taken from the primary reference" % k)
        else:
            res.notes.append("%s: the %d pairs give no majority (%s); taken from the primary reference" % (k, npairs, votes))
    # code addresses (the loop head, the overlay window) through the alignment
    for k, (v, kind) in prim.R.rec.num.items():
        if kind == "pc":
            # the command loop's head is a point inside a run of instructions the builds place differently; no aligned instruction names it, so it is not derived
            res.unanchored.append((k, v, 0))
    if "ov_base" in prim.R.rec.num and "ov_base" not in prim.derived:
        m = F2.map_pc(prim, prim.R.rec.num["ov_base"][0])
        if m >= 0:
            rec.num["ov_base"] = (m, "addr")
            res.unanchored = [u for u in res.unanchored if u[0] != "ov_base"]
    rec.load_key = None
    res.rec = rec
    res.conflicts = dict(prim.conflict)
    # reference words the unknown lacks and nothing replaces: a long run is a feature the unknown lacks, a short one an instruction missing from the unknown
    xr = {i: "extra" for i in prim.xr if i not in prim.hunk_of_r or any(j in res.x for j in prim.hunk_of_r[i])}
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
    res.near = {}
    res.verdict = "NONE" if res.x or any(c == "deleted" for c in xr.values()) else ("NEAR" if xr else "EXACT")
    return res
