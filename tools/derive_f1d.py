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
        return d


def load_f1_records(T, C):
    recs = {}
    for var, f in T.ucodes:
        if f.get("_macro") != "U":
            continue
        bn = f["fam_data"].lstrip("&").strip("()")
        if bn not in T.vars or T.vars[bn][0] != "f3dex_bld":
            continue
        bld = T.vars[bn][1]
        fx = T.vars[bld[0][1]][1] if isinstance(bld[0], tuple) else None
        r = recs.get(bn)
        if r is None:
            r = recs[bn] = RecV("f3dex1", bn)
            r.num["reject_mask"] = (fx[0], "mask")
            r.flags["non"] = fx[1]
            r.flags["dlx"] = fx[2]
            r.flags["v095"] = bld[1]
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
