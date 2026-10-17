"""The F3DEX2 / S2DEX family of tools/derive_build.py: the build record (src/hle/hle_f3dex2.c, hle_f3dex2_dialects.c, hle_s2dex2.c), the alignment of an
unknown microcode with the registered builds, the classification of every difference, and the derivation of the record."""
import collections
import struct

import derive_lib as L
import rsp_isa as isa

# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
# the record
# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
PC_FIELDS = ["head", "copy", "sync", "segcopy", "ef", "ed", "tri1", "tri2", "vtx", "mtx", "geom", "enddl", "half2", "modvtx", "culldl", "branchz", "mvp", "tri4"]
F2PCS_FIELDS = ["dl", "texture", "texrect", "half1", "moveword", "popmtx", "movemem", "omh", "oml"]
LAY_ADDR = ["mv", "p", "mvp", "persp_norm", "viewport_scale", "viewport_trans", "clip_ratio", "const31", "const30", "fog_mul", "fog_ofs", "texture", "tex_scale_s", "tex_scale_t", "geom",
            "mvp_valid", "lights_valid", "lights_size", "light_raw", "light_base", "tg", "light_scale", "light_scale_zex", "vtab", "clipmask", "culltab", "in", "in_end", "jump_table",
            "movemem_tab", "moveword_adj", "desc_light", "desc_clip", "clip_planes", "clip_masks", "out_toggle"]
LAY_OTHER = ["geom_def", "mw_max", "out_bytes", "pipe_sync", "no_lit", "lit_vtx"]
LAY_FIELDS = LAY_ADDR + LAY_OTHER

# a representative opcode of each handler pc field (the standard F3DEX2 command numbers)
PC_OPS = {"vtx": 0x01, "modvtx": 0x02, "culldl": 0x03, "branchz": 0x04, "tri1": 0x05, "tri2": 0x06, "mtx": 0xda, "geom": 0xd9, "enddl": 0xdf, "half2": 0xf1, "mvp": 0xd5, "tri4": 0x10,
          "ef": 0xef, "ed": 0xed, "copy": 0xf8, "sync": 0xe9, "segcopy": 0xfd}
F2PCS_OPS = {"dl": 0xde, "texture": 0xd7, "texrect": 0xe4, "half1": 0xe1, "moveword": 0xdb, "popmtx": 0xd8, "movemem": 0xdc, "omh": 0xe3, "oml": 0xe2}
OP_LOADER, OP_DMAIO = 0xdd, 0xd6
DECLINED_OPS = {OP_DMAIO}


class Rec:
    """A normalised build record (hand-written or derived)."""

    def __init__(self):
        self.name = ""
        self.ids = []
        self.pc = {}        # PC_FIELDS
        self.f2pcs = {}     # F2PCS_FIELDS
        self.lay = {}       # LAY_FIELDS
        self.gen204 = 0
        self.zex = 0
        self.bz_z = 0       # zex build whose G_BRANCH_Z still reads the screen z (lw 28), not the clip w (lh 6)
        self.reject_mask = 0
        self.load_key = None
        self.ext = []       # [(pc, kind)]
        self.flags = set()  # HLE_F_NEAR_CLIP / HLE_F_ZEX (informational: nothing in the front-end reads them)
        self.extra = {}

    def fields(self):
        """name -> value, the comparison basis"""
        out = {}
        for k in PC_FIELDS:
            out["pc." + k] = self.pc.get(k, 0)
        for k in F2PCS_FIELDS:
            out["f2pcs." + k] = self.f2pcs.get(k, 0)
        for k in LAY_FIELDS:
            out["lay." + k] = self.lay.get(k, 0)
        out["gen204"] = self.gen204
        out["zex"] = self.zex
        out["bz_z"] = self.bz_z
        out["reject_mask"] = self.reject_mask
        out["load_key"] = self.load_key
        out["ext"] = tuple(sorted(self.ext))
        return out


def lookup(T, ref):
    """follow ('ref', name) / ('sym', name) to the fields dict"""
    if isinstance(ref, tuple) and ref[0] in ("ref", "sym"):
        n = ref[1]
        return T.vars[n][1] if n in T.vars else None
    return None


def load_f2_records(T, C):
    """-> list of Rec for every HLE_UCODE entry of the F3DEX2 front-end (name -> Rec); same-build entries (one build table, several ids) are one record with several ids"""
    std_lay = T.vars["l_lay_std"][1]
    recs = {}
    for var, f in T.ucodes:
        fd = f.get("fam_data", "")
        if "f3dex2_fam" not in f.get("fam", ""):
            continue
        bn = fd.lstrip("&").strip("()")
        if bn not in T.vars:
            continue
        b = T.vars[bn][1]
        r = recs.get(bn)
        if r is None:
            r = recs[bn] = Rec()
            r.name = bn
            for k in PC_FIELDS:
                v = b.get(k, 0)
                r.pc[k] = v if isinstance(v, int) else 0
            lay = lookup(T, b.get("lay")) if b.get("lay") else std_lay
            for k in LAY_FIELDS:
                v = lay.get(k, 0)
                r.lay[k] = v if isinstance(v, int) else (1 if k == "lit_vtx" and v else 0)
            pcs = lookup(T, b.get("pcs")) if b.get("pcs") else None
            for k in F2PCS_FIELDS:
                std = {"dl": 0x000, "texture": 0x028, "texrect": 0x02c, "half1": 0x030, "moveword": 0x038, "popmtx": 0x04c, "movemem": 0x120, "omh": 0x13c, "oml": 0x140}[k]
                v = pcs.get(k, std) if pcs else std
                r.f2pcs[k] = v
            r.gen204 = int(b.get("gen204", 0))
            r.zex = int(b.get("zex", 0))
            r.bz_z = int(b.get("bz_z", 0))
            r.reject_mask = int(b.get("reject_mask", 0))
            r.load_key = b.get("load_key")
            ext = b.get("ext")
            if ext:
                tab = T.vars[ext[1]][1] if isinstance(ext, tuple) else ext
                r.ext = [(p, k) for p, k in tab if p != 0xffff]
        r.ids.append(L.uhex(f["id"]))
        r.name = bn
        fl = f.get("flags", "")
        r.flags |= {x for x in ("HLE_F_NEAR_CLIP", "HLE_F_ZEX") if x in fl}
    return list(recs.values())


def corpus_ucode(C, rec):
    """the dump of a registered build: by its load key (a loaded text), else by an id of one of its entries"""
    u = C.find_id(rec.load_key) if rec.load_key else None
    if u is None:
        for i in rec.ids:
            u = C.find_id(i)
            if u:
                break
    return u


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
# reachability
# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
def u16(data, a):
    return struct.unpack(">H", data[a:a + 2])[0] if 0 <= a and a + 2 <= len(data) else 0


def jtab(data, jt, op):
    return u16(data, jt + 2 * (op - 256 if op > 127 else op))


def handler_pc(data, jt, op):
    v = jtab(data, jt, op)
    return v & 0xfff if (v & 0xf000) == 0x1000 else None


def reach(img, entries, stop=(), block=()):
    """word indices reachable from the IMEM addresses `entries` by static control flow (branches, j, jal and the code after it; jr ends a path; `addi ra, zero, pc` adds the
    return address as an entry). Paths end at the IMEM addresses in `stop` (when reached from another entry)."""
    seen = set(block)
    work = []
    stopset = set(stop)
    for e in entries:
        for k in img.resolve(e, 0):
            work.append((k, True))
    while work:
        i, first = work.pop()
        while 0 <= i < img.n and i not in seen:
            if not first and (img.pc[i] in stopset) and img.seg[i] == 0:
                break
            first = False
            seen.add(i)
            d = isa.decode(img.w[i])
            c = d["cls"]
            s = img.seg[i]
            nxt = i + 1 if i + 1 < img.n and img.seg[i + 1] == s else None
            if c == isa.C_ALUI and d["name"] in ("addi", "addiu", "ori") and d["rs"] == 0 and d["rt"] == 31 and (d["imm"] & 0xf000):
                for k in img.resolve(d["imm"] & 0xfff, s):
                    work.append((k, False))
            if c == isa.C_J or (c == isa.C_OTHER and d["name"] in ("jr", "jalr")) or c == isa.C_BR:
                if nxt is not None:
                    seen.add(nxt)
                    dn = isa.decode(img.w[nxt])
                    if dn["cls"] == isa.C_ALUI and dn["name"] in ("addi", "addiu", "ori") and dn["rs"] == 0 and dn["rt"] == 31 and (dn["imm"] & 0xf000):
                        for k in img.resolve(dn["imm"] & 0xfff, s):
                            work.append((k, False))
                after = i + 2 if i + 2 < img.n and img.seg[i + 2] == s else None
                if c == isa.C_J:
                    for k in img.resolve(d["tgt"], s):
                        work.append((k, False))
                    if d["name"] == "jal" and after is not None:
                        i = after
                        continue
                    break
                if c == isa.C_BR:
                    for k in img.resolve((img.pc[i] + 4 + 4 * d["off"]) & 0xfff, s):
                        work.append((k, False))
                    if d["name"] in ("bltzal", "bgezal") and False:
                        pass
                    if after is not None:
                        i = after
                        continue
                    break
                if d["name"] == "jalr" and after is not None:
                    i = after
                    continue
                break           # jr
            if d["name"] == "break":
                break
            if nxt is None:
                break
            i = nxt
    return seen - set(block)


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
# images with their tail overlay
# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
def build_image(uc):
    """text + the overlays that lie (partly) behind the 4 KiB: those with an offset >= 0x1000, and the tail of the descriptor of the exit code (text + 0xf80, 0x98 bytes at IMEM 0)"""
    ovs = []
    for (off, imem, ln) in L.find_descriptors(uc.data):
        if off >= 0x1000:
            ovs.append((off, imem, ln))
        elif off + ln > 0x1000:
            ovs.append((0x1000, imem + (0x1000 - off), off + ln - 0x1000))
    return L.Image(uc, sorted(set(ovs)))


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
# one pairing: a reference build against the unknown text
# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
class Knob:
    __slots__ = ("name", "val", "kind")

    def __init__(self, name, val, kind):
        self.name, self.val, self.kind = name, val, kind


def knobs_of(rec):
    if hasattr(rec, "knobs"):
        return rec.knobs()
    ks = []
    for k in LAY_ADDR:
        v = rec.lay.get(k, 0)
        if v >= 0x10 and k not in ("moveword_adj",):
            ks.append(Knob("lay." + k, v, "addr"))
    ks.append(Knob("lay.moveword_adj", rec.lay.get("moveword_adj", 0), "addr"))
    ks.append(Knob("lay.out_bytes", rec.lay.get("out_bytes", 0), "size"))
    if rec.reject_mask:
        ks.append(Knob("reject_mask", rec.reject_mask, "mask"))
    return [k for k in ks if k.val]


def size_encs(v):
    """immediates that a buffer size v appears as: +v, -v, v - 1, 1 - v, all modulo 0x1000 (the DMEM address space wraps)"""
    return {"+v": v & 0xfff, "-v": (-v) & 0xfff, "v-1": (v - 1) & 0xfff, "1-v": (1 - v) & 0xfff}


def inv_size(enc, imm):
    imm &= 0xfff
    return {"+v": imm, "-v": (-imm) & 0xfff, "v-1": (imm + 1) & 0xfff, "1-v": (1 - imm) & 0xfff}[enc]


class Item:
    """a microcode with its image; a reference also has its record"""

    def __init__(self, uc, rec=None, name=None):
        self.uc = uc
        self.rec = rec
        self.name = name or (rec.name if rec else uc.stem[:16])
        self.img = build_image(uc)
        self.data = uc.data


NOP = 0


def is_nop(w):
    return w == 0


def operand_vals(w):
    """-> (kind, value, base_is_zero) of the one operand a build can relocate, or None"""
    d = isa.decode(w)
    c = d["cls"]
    if c in (isa.C_MEM, isa.C_VMEM):
        return ("mem", d["imm"] & 0xffff, d["rs"] == 0)
    if c == isa.C_ALUI:
        return ("imm", d["imm"], d["rs"] == 0)
    return None


class Pairing:
    """Alignment of reference R with the unknown U and the classification of all differences between them."""

    def __init__(self, R, U):
        self.R, self.U = R, U
        ir, iu = R.img, U.img
        # nops are never aligned (a lone nop would match any other nop): padding is judged from the words around it
        self.pr = L.align_keys([L.wkey(w) if w else ("nop", "r", i) for i, w in enumerate(ir.w)], [L.wkey(w) if w else ("nop", "u", j) for j, w in enumerate(iu.w)])
        self.pu = {j: i for i, j in self.pr.items()}
        self.ev = []
        self._collect()
        self._knobs()
        self._classify()

    # ------------------------------------------------------------------------------------------------------------------------------------------------------
    def pc_ok(self, vr, vu, seg_r, seg_u):
        ur = self.U.img.resolve(vu & 0xfff, seg_u)
        for a in self.R.img.resolve(vr & 0xfff, seg_r):
            if self.pr.get(a) in ur:
                return True
        return False

    def _collect(self):
        ir, iu = self.R.img, self.U.img
        self.same_ops = []          # (value, kind) of unchanged operands, for the confirmation of knobs
        for i, j in sorted(self.pr.items()):
            x, y = ir.w[i], iu.w[j]
            if x == y:
                o = operand_vals(x)
                if o:
                    self.same_ops.append((o[1], o[0], i))
                continue
            dx, dy = isa.decode(x), isa.decode(y)
            c = dx["cls"]
            sr, su = ir.seg[i], iu.seg[j]
            if c in (isa.C_J, isa.C_BR):
                tr = dx["tgt"] if c == isa.C_J else (ir.pc[i] + 4 + 4 * dx["off"]) & 0xfff
                tu = dy["tgt"] if c == isa.C_J else (iu.pc[j] + 4 + 4 * dy["off"]) & 0xfff
                self.ev.append(dict(kind="pc" if self.pc_ok(tr, tu, sr, su) else "cf", i=i, j=j, vr=tr, vu=tu))
            elif c == isa.C_ALUI:
                vr, vu = dx["imm"], dy["imm"]
                lit = dx["rs"] == 0 and (vr & 3) == 0 and (vu & 3) == 0 and (vr >> 12) == (vu >> 12) and (vr >> 12) != 0
                if lit and self.pc_ok(vr, vu, sr, su):
                    self.ev.append(dict(kind="pc", i=i, j=j, vr=vr, vu=vu))
                else:
                    self.ev.append(dict(kind="imm", i=i, j=j, vr=vr, vu=vu, rs=dx["rs"], op=dx["name"]))
            elif c in (isa.C_MEM, isa.C_VMEM):
                self.ev.append(dict(kind="mem", i=i, j=j, vr=dx["imm"] & 0xffff, vu=dy["imm"] & 0xffff, rs=dx["rs"], op=dx["name"]))
            else:
                self.ev.append(dict(kind="other", i=i, j=j))

    # ------------------------------------------------------------------------------------------------------------------------------------------------------
    def _match(self, ks, e):
        """the knobs (and encodings) the R operand of evidence e can be: -> [(knob, enc, derived U value)]"""
        out = []
        vr, vu = e["vr"], e["vu"]
        for k in ks:
            if k.kind == "addr":
                if vr == k.val:
                    out.append((k, "=", vu))
                elif vr == (k.val | 0x1000) and (vu >> 12) == (vr >> 12):
                    out.append((k, "=", vu & 0xfff))
            elif k.kind == "mask":
                if e["kind"] == "imm" and vr == k.val:
                    out.append((k, "=", vu))
            elif k.kind == "size":
                for enc, v in size_encs(k.val).items():
                    if (vr & 0xfff) == v:
                        out.append((k, enc, inv_size(enc, vu)))
        return out

    def _knobs(self):
        ks = knobs_of(self.R.rec)
        self.ks = ks
        cands = []
        for e in self.ev:
            if e["kind"] in ("imm", "mem"):
                e["cand"] = self._match(ks, e)
                cands.append(e)
        # unchanged anchors: operands of aligned identical instructions that hold the knob's value (the knob was not moved there)
        self.same_n = {}
        common = (0xffff, 0x7fff, 0x8000, 0xfff, 0x1000, 0x3fff, 0x7ff, 0xfff0, 0xffff)
        for k in ks:
            n = 0
            for v, kind, i in self.same_ops:
                if k.kind == "addr" and v == k.val:
                    n += 1
                elif k.kind == "size" and (v & 0xfff) in size_encs(k.val).values():
                    n += 1
                elif k.kind == "mask" and kind == "imm" and v == k.val and v >= 0x100 and v not in common:
                    n += 1
            if n:
                self.same_n[k.name] = n
        votes = {}
        for e in cands:
            if len(e["cand"]) == 1:
                k, enc, v = e["cand"][0]
                votes.setdefault(k.name, {}).setdefault(v, []).append(e)
        self.derived = {}       # knob name -> U value
        self.conflict = {}
        self.ambig = {}
        for name, d in votes.items():
            best = max(d, key=lambda v: len(d[v]))
            m = sum(len(es) for es in d.values())
            u = self.same_n.get(name, 0)
            if len(d) > 1:
                self.conflict[name] = {hex(v): len(es) for v, es in d.items()}
            if u and m <= 1 and best != next(k.val for k in ks if k.name == name):
                self.ambig[name] = (hex(best), m, u)       # one instruction says it moved, others that use the value say it did not
                continue
            self.derived[name] = best
        # ambiguous evidence: explained when any candidate agrees with the derived value
        self.anchor_n = {}
        for e in cands:
            for k, enc, v in e["cand"]:
                if self.derived.get(k.name) == v:
                    self.anchor_n[k.name] = self.anchor_n.get(k.name, 0) + 1

    # ------------------------------------------------------------------------------------------------------------------------------------------------------
    def _classify(self):
        iu, ir = self.U.img, self.R.img
        self.status = {}        # U word -> ('ok'|'pc'|'knob:..'|'pad'|'pat:..'|'X', detail)
        self.xr = {}            # R word -> detail (code of the reference the unknown lacks)
        for e in self.ev:
            j = e["j"]
            if e["kind"] == "pc":
                self.status[j] = ("pc", "")
            elif e["kind"] == "cf":
                self.status[j] = ("X", "branch/jump retargeted: R %03x -> %03x, U -> %03x" % (ir.pc[e["i"]], e["vr"], e["vu"]))
            elif e["kind"] in ("imm", "mem"):
                ok = None
                for k, enc, v in e["cand"]:
                    if self.derived.get(k.name, k.val) == v or (k.name not in self.derived):
                        ok = (k, enc)
                        break
                if ok:
                    self.status[j] = ("knob", ok[0].name)
                else:
                    self.status[j] = ("X", "operand %s -> %s (%s %s) is no field of the record" % (hex(e["vr"]), hex(e["vu"]), e["op"], e["kind"]))
        # unaligned U words
        un = [j for j in range(iu.n) if j not in self.pu]
        runs = []
        for j in un:
            if runs and runs[-1][-1] == j - 1 and iu.seg[j] == iu.seg[runs[-1][0]]:
                runs[-1].append(j)
            else:
                runs.append([j])
        self.u_runs = runs
        def hunk_r(run):
            j0, j1 = run[0], run[-1]
            lo = next((self.pu[k] for k in range(j0 - 1, -1, -1) if k in self.pu and iu.seg[k] == iu.seg[j0]), None)
            hi = next((self.pu[k] for k in range(j1 + 1, iu.n) if k in self.pu and iu.seg[k] == iu.seg[j1]), None)
            if lo is None or hi is None or hi <= lo or hi - lo > 4 * len(run) + 8:
                return []
            return [i for i in range(lo + 1, hi) if i not in self.pr]
        for run in runs:
            ws = [iu.w[j] for j in run]
            if all(is_nop(w) for w in ws):
                if all(is_nop(ir.w[i]) for i in hunk_r(run)):
                    for j in run:
                        self.status[j] = ("pad", "nop")
                else:
                    for j in run:
                        self.status[j] = ("X", "nop in place of code of " + self.R.name)
                continue
            pat = self._pattern(run)
            for j in run:
                if pat:
                    self.status[j] = ("pat", pat)
                elif is_nop(iu.w[j]) and len(run) <= 2:
                    self.status[j] = ("pad", "nop")
                else:
                    self.status[j] = ("X", "no counterpart in " + self.R.name)
        # an overlay descriptor may declare more bytes than the code has (Paper Mario's two descriptors both say 0x4a8): the unaligned tail of an overlay segment is slack
        for img, pmap, setter in ((iu, self.pu, lambda j, v: self.status.__setitem__(j, v)), (ir, self.pr, None)):
            for k in range(1, len(img.segs)):
                nm, base, first, n = img.segs[k]
                last = max([x for x in range(first, first + n) if x in pmap], default=first - 1)
                for x in range(last + 1, first + n):
                    if img is iu:
                        self.status[x] = ("pad", "overlay slack")
        for j in range(iu.n):
            self.status.setdefault(j, ("ok", ""))
        # replace hunks: the reference words that sit between the same anchors as an unaligned run of U
        self.hunk_of_r = {}
        for run in runs:
            j0, j1 = run[0], run[-1]
            lo = next((self.pu[k] for k in range(j0 - 1, -1, -1) if k in self.pu and iu.seg[k] == iu.seg[j0]), None)
            hi = next((self.pu[k] for k in range(j1 + 1, iu.n) if k in self.pu and iu.seg[k] == iu.seg[j1]), None)
            if lo is None or hi is None or hi <= lo or hi - lo > 4 * len(run) + 8:
                continue
            for i in range(lo + 1, hi):
                if i not in self.pr:
                    self.hunk_of_r[i] = tuple(run)
        # unaligned R words (code the reference has and the unknown lacks)
        for i in range(ir.n):
            if i not in self.pr and not is_nop(ir.w[i]):
                sg = ir.seg[i]
                if sg:
                    nm, base, first, n = ir.segs[sg]
                    if i > max([x for x in range(first, first + n) if x in self.pr], default=first - 1):
                        continue
                self.xr[i] = "only in " + self.R.name

    def _pattern(self, run):
        """a known harmless-by-construction insertion: G_RDPPIPESYNC written in front of the flush check (S2DEX 2.05 [Safe]): addi rX, zero, 0xe7 ; sb rX, 0(s7) ; addi s7, s7, 8"""
        iu = self.U.img
        if len(run) != 3:
            return None
        d = [isa.decode(iu.w[j]) for j in run]
        if (d[0]["cls"] == isa.C_ALUI and d[0]["name"] == "addi" and d[0]["rs"] == 0 and d[0]["imm"] == 0xe7 and d[1]["cls"] == isa.C_MEM and d[1]["name"] == "sb" and d[1]["rs"] == 23
                and d[1]["rt"] == d[0]["rt"] and d[1]["imm"] == 0 and d[2]["cls"] == isa.C_ALUI and d[2]["name"] == "addi" and d[2]["rs"] == 23 and d[2]["rt"] == 23 and d[2]["imm"] == 8):
            return "pipe_sync"
        return None

    # ------------------------------------------------------------------------------------------------------------------------------------------------------
    def x_set(self):
        return {j for j, (k, _) in self.status.items() if k == "X"}

    def stats(self):
        n = {}
        for k, _ in self.status.values():
            n[k] = n.get(k, 0) + 1
        return n


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
# the derivation
# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
class Result:
    def __init__(self):
        self.rec = Rec()
        self.notes = []
        self.verdict = "NONE"
        self.primary = None
        self.contrib = {}       # ref name -> number of U words it explains that the primary does not
        self.x = {}             # U word -> reason (residual, after the composite)
        self.xr = {}            # reference-only code
        self.near = {}          # U word -> near category
        self.waived = {}        # U word -> category (explained by a rule of the tool, not by a reference)
        self.unanchored = []
        self.flag_evidence = {}
        self.conflicts = {}
        self.pcs_alt = {}


def merge_knobs(order):
    """knob name -> (value, reference, how): the first pairing (best explanation first) whose aligned code moved the knob decides it; else the first whose aligned code uses it unchanged"""
    out = {}
    names = []
    for P in order:
        for k in P.ks:
            if k.name not in names:
                names.append(k.name)
    for name in names:
        for idx, P in enumerate(order):
            k = next((k for k in P.ks if k.name == name), None)
            if name in P.derived and not (name in P.conflict) and (idx == 0 or P.anchor_n.get(name, 0) >= 2):
                out[name] = (P.derived[name], P.R.name, "moved")
                break
            if k and P.same_n.get(name, 0) >= 2:
                out[name] = (k.val, P.R.name, "unchanged")
                break
    return out


def derived_layout(P, merged=None):
    """the layout of U: the reference's with the derived knobs"""
    lay = dict(P.R.rec.lay)
    for k in P.ks:
        if k.name.startswith("lay."):
            if merged is not None and k.name in merged:
                lay[k.name[4:]] = merged[k.name][0]
            elif k.name in P.derived:
                lay[k.name[4:]] = P.derived[k.name]
    return lay


def read_table(data, jt):
    return {op: handler_pc(data, jt, op) for op in range(256)}


def valid_pc(v):
    return v is not None and v not in (0, 0xffff)


def map_pcs_by_table(prim, tabU, head_u):
    """handler pcs of U: for each pc the reference's record names, the opcodes that lead to it in the reference's table lead to the unknown's pc in the unknown's table"""
    R = prim.R
    tabR = read_table(R.data, R.rec.lay["jump_table"])
    ops_of = {}
    for op, pc in tabR.items():
        if valid_pc(pc):
            ops_of.setdefault(pc, []).append(op)

    def conv(pc):
        if pc in (0, 0xffff):
            return pc, "kept"
        ops = ops_of.get(pc)
        if ops:
            votes = {}
            for op in ops:
                v = tabU.get(op)
                if valid_pc(v):
                    votes[v] = votes.get(v, 0) + 1
            if votes:
                v = max(votes, key=lambda x: votes[x])
                return v, "table" if len(votes) == 1 else "table (%d opcodes, %d targets)" % (len(ops), len(votes))
        m = map_pc(prim, pc)
        return m, "alignment"
    return conv


def map_pc(P, pc):
    """the IMEM address in U of the instruction at pc in the reference: the place where the same run of 6 instruction shapes occurs in U, when that is unique, else the aligned word"""
    if pc in (0, 0xffff):
        return pc
    ir, iu = P.R.img, P.U.img
    for a in ir.resolve(pc, 0):
        run = [L.wkey(w) for w in ir.w[a:a + 6]]
        if len(run) == 6:
            ku = [L.wkey(w) for w in iu.w]
            hits = [j for j in range(len(ku) - 5) if ku[j:j + 6] == run and iu.seg[j] == 0]
            if len(hits) == 1:
                return iu.pc[hits[0]]
        b = P.pr.get(a)
        if b is not None:
            return iu.pc[b]
    return -1


def text_range_words(img, off, ln):
    return set(range(off // 4, min((off + ln) // 4, 1024)))


def overlay_words(img, data, lay, which):
    """the word indices of the overlay named by the layout's descriptor ('desc_clip' / 'desc_light'): inside the text, or the segment that was added for it"""
    a = lay.get(which, 0)
    if not a or a + 8 > len(data):
        return set()
    off, b = struct.unpack(">II", data[a:a + 8])
    ln = ((b >> 16) & 0xfff) + 1
    imem = b & 0xfff
    if off < 0x1000:
        return set(range(off // 4, min((off + ln) // 4, 1024)))
    for k in range(1, len(img.segs)):
        nm, base, first, n = img.segs[k]
        if base == imem and nm.endswith("+%x" % off):
            return set(range(first, first + n))
    return set()


def set_active(img, data, lay):
    """only the overlays that the layout's descriptors name (and the tail of the exit code) are code of this build; the others are stale descriptors in the data image"""
    act = {0}
    for which in ("desc_light", "desc_clip"):
        a = lay.get(which, 0)
        if a and a + 8 <= len(data):
            off, b = struct.unpack(">II", data[a:a + 8])
            for k in range(1, len(img.segs)):
                nm, base, first, n = img.segs[k]
                if base == (b & 0xfff) and nm.endswith("+%x" % off):
                    act.add(k)
    for k in range(1, len(img.segs)):
        if img.segs[k][0].endswith("+1000"):
            act.add(k)
    img.active = act


def reach_sets(img, data, lay, tab, head):
    """the code regions of U by role"""
    set_active(img, data, lay)
    out = {}
    out["clip"] = overlay_words(img, data, lay, "desc_clip")
    out["light"] = overlay_words(img, data, lay, "desc_light")
    stop = [tab.get(OP_LOADER) or 0xfac, head, head - 4]
    blk = out["clip"] | out["light"]
    for role, op in (("vtx", 0x01), ("tri1", 0x05), ("tri2", 0x06), ("branchz", 0x04), ("culldl", 0x03), ("modvtx", 0x02), ("mtx", 0xda), ("mvp", 0xd5), ("tri4", 0x10)):
        pc = tab.get(op)
        out[role] = reach(img, [pc], stop=stop, block=blk) if valid_pc(pc) else set()
    return out


def derive(U, refs):
    res = Result()
    prs = {R.name: Pairing(R, U) for R in refs}
    order = sorted(prs.values(), key=lambda P: (len(P.x_set()), len(P.xr), -len(P.pr)))
    prim = order[0]
    res.primary = prim.R.name
    res.prs, res.prim = prs, prim
    explained_by = {P.R.name: {j for j in range(U.img.n) if P.status[j][0] != "X"} for P in order}
    rec = res.rec
    rec.name = "(derived)"
    merged = merge_knobs(order)
    res.merged = merged
    rec.lay = derived_layout(prim, merged)
    rec.reject_mask = merged["reject_mask"][0] if "reject_mask" in merged else prim.R.rec.reject_mask
    for k, v in prim.conflict.items():
        res.conflicts[k] = v
    for k, v in prim.ambig.items():
        res.notes.append("%s: one instruction says %s, %d others use the reference's value: kept the reference's" % (k, v[0], v[2]))
    res.confirmed = []
    for k in prim.ks:
        if k.name in merged:
            res.confirmed.append((k.name, merged[k.name][0], merged[k.name][2]))
        elif k.name.startswith("lay."):
            res.unanchored.append((k.name, k.val, 0))
    jt = rec.lay["jump_table"]
    tab = read_table(U.data, jt)
    res.tab = tab
    head_guess = map_pc(prim, prim.R.rec.pc["head"])
    conv = map_pcs_by_table(prim, tab, head_guess)
    how = {}
    for k in PC_FIELDS:
        v, h = conv(prim.R.rec.pc.get(k, 0))
        rec.pc[k] = v
        how["pc." + k] = h
    # roles the reference lacks and the unknown has (an extra command handler in the table)
    known = {v for v in rec.pc.values() if valid_pc(v)} | {v for v in prim.R.rec.f2pcs.values() if valid_pc(v)}
    tabR = read_table(prim.R.data, prim.R.rec.lay["jump_table"])
    for k in ("mvp", "tri4"):
        op = PC_OPS[k]
        if prim.R.rec.ext:
            break                               # S2DEX: its table has other commands (and variables that look like entries) at these opcodes
        if prim.R.rec.pc.get(k, 0) == 0:
            # the reference has no such handler: the unknown has one when its table leads the opcode to code that is not the loop head, the reference's table leads it to the head or nowhere
            if valid_pc(tab.get(op)) and tab[op] != rec.pc["head"] and tab[op] not in known and (not valid_pc(tabR.get(op)) or tabR[op] == prim.R.rec.pc["head"]):
                rec.pc[k] = tab[op]
                how["pc." + k] = "table (opcode %02x: a handler the reference lacks)" % op
        elif rec.pc[k] == rec.pc["head"]:
            rec.pc[k] = 0                       # the unknown's table sends the opcode to the loop head: no such command
            how["pc." + k] = "table (opcode %02x goes to the loop head)" % op
    for k in F2PCS_FIELDS:
        v, h = conv(prim.R.rec.f2pcs.get(k, 0))
        rec.f2pcs[k] = v
        how["f2pcs." + k] = h
    rec.ext = [(conv(p)[0], kd) for p, kd in prim.R.rec.ext]
    res.how = how
    rec.load_key = L.fnv(U.uc.text_b, 0xf48)
    n_pat = sum(1 for j, (k, d) in prim.status.items() if k == "pat" and d == "pipe_sync")
    rec.lay["pipe_sync"] = 1 if n_pat else 0
    # flags
    sets = reach_sets(U.img, U.data, rec.lay, tab, rec.pc["head"])
    res.sets = sets
    gen_own = sets["vtx"] | sets["clip"]
    zex_own = (sets["tri1"] | sets["tri2"]) - gen_own - sets["light"]
    for f, own in (("zex", zex_own), ("gen204", gen_own)):
        g = {0: set(), 1: set()}
        for R in refs:
            g[getattr(R.rec, f)] |= (explained_by[R.name] & own)
        ev1, ev0 = len(g[1] - g[0]), len(g[0] - g[1])
        res.flag_evidence[f] = (ev0, ev1)
        if ev1 == ev0:
            v = getattr(prim.R.rec, f)
            res.notes.append("%s: no discriminating evidence (%d/%d words), taken from the primary reference" % (f, ev0, ev1))
        else:
            v = 1 if ev1 > ev0 else 0
        setattr(rec, f, v)
    res.zex_units, res.gen_units = zex_own, gen_own
    # the composite: a reference may explain a word only if it has the derived value of the flag that owns the word
    def usable(R, j):
        if j in zex_own and R.rec.zex != rec.zex:
            return False
        if j in gen_own and R.rec.gen204 != rec.gen204:
            return False
        return True
    cur = set(prim.x_set())
    ok_by = {P.R.name: {j for j in explained_by[P.R.name] if usable(P.R, j)} for P in order[1:]}
    pool = list(order[1:])
    while cur and pool:
        best = max(pool, key=lambda P: len(cur & ok_by[P.R.name]))
        n = len(cur & ok_by[best.R.name])
        if n == 0:
            break
        res.contrib[best.R.name] = n
        cur -= ok_by[best.R.name]
        pool.remove(best)
    res.x = {j: prim.status[j][1] for j in cur}
    res.xr = dict(prim.xr)
    # the reference's optional handlers (not in the unknown's table): their code is not missing from the unknown
    set_active(prim.R.img, prim.R.data, prim.R.rec.lay)
    res.ref_sets = {}
    for role in ("mvp", "tri4"):
        pcR = prim.R.rec.pc.get(role, 0)
        if pcR and rec.pc.get(role, 0) == 0:
            res.ref_sets[role] = reach(prim.R.img, [pcR], stop=[tabR.get(OP_LOADER) or 0xfac, prim.R.rec.pc["head"], prim.R.rec.pc["head"] - 4])
            res.ref_sets[role] -= reach(prim.R.img, [prim.R.rec.pc["head"]], stop=[0xfac])
    apply_rules(res, U, prim)
    classify_near(res, U, prim)
    if prim.conflict or prim.ambig:
        res.knob_trouble = dict(prim.conflict)
        res.knob_trouble.update({k: v for k, v in prim.ambig.items()})
        res.verdict = "NONE"
        res.notes.append("a constant or address of the record is read differently at different places: %s" % res.knob_trouble)
    return res


def apply_rules(res, U, prim):
    """words that a rule of the tool explains: the branchz source (clip w, `lh 6`, for F3DZEX without gen204, else the screen z, `lw 28`). The source is read from the text itself: a build with the
    F3DZEX triangle code but the F3DEX branch (F3DEX 2.06: Blues Brothers 2000) gets bz_z = 1 (the flags alone would have said clip w)"""
    rec = res.rec
    bz = res.sets.get("branchz", set())
    src = None
    for j in sorted(bz):
        d = isa.decode(U.img.w[j])
        if d["cls"] == isa.C_MEM and (d["name"], d["imm"]) in (("lh", 6), ("lw", 28)):
            src = (d["name"], d["imm"])
    rec.bz_z = 1 if rec.zex and not rec.gen204 and src == ("lw", 28) else 0
    want = ("lh", 6) if rec.zex and not rec.gen204 and not rec.bz_z else ("lw", 28)
    for j in sorted(res.x):
        if j in bz:
            d = isa.decode(U.img.w[j])
            if d["cls"] == isa.C_MEM and (d["name"], d["imm"]) == want:
                res.waived[j] = "branchz source %s %d (zex=%d gen204=%d bz_z=%d)" % (want[0], want[1], rec.zex, rec.gen204, rec.bz_z)
    for j in res.waived:
        res.x.pop(j, None)


def classify_near(res, U, prim):
    """the residual X words that the near-match tolerance admits (docs/ucode-derive.md, "Verdicts"):
    - init: a vector / scalar ALU instruction (no store, no control flow) in the start-up code in front of the first command fetch; the interpreter runs that code for real, the front-end re-reads
      the registers and DMEM it leaves
    - light-small: at most 16 words in the lighting overlay (a lane, a rounding): the effect is confined to the lit vertex colours / texture coordinates, which the probe measures
    - light-positional: the lighting overlay of a build with the positional-light variant (a guard `andi rX, rY, 0x40 ; beq rX, zero` on the geometry bit 0x400000, which the front-end
      declines: geom_def lacks it)"""
    rec = res.rec
    img = U.img
    light = res.sets.get("light", set())
    head = rec.pc["head"]
    xs = sorted(res.x)
    lx = [j for j in xs if j in light]
    guard = False
    ws = sorted(light)
    for a, b in zip(ws, ws[1:]):
        da, db = isa.decode(img.w[a]), isa.decode(img.w[b])
        if da["cls"] == isa.C_ALUI and da["name"] == "andi" and da["imm"] == 0x40 and db["cls"] == isa.C_BR and db["name"] == "beq" and db["rs"] == da["rt"] and db["rt"] == 0:
            guard = True
    res.positional = guard
    for j in xs:
        d = isa.decode(img.w[j])
        if 0x80 <= img.pc[j] < head and img.seg[j] == 0 and d["cls"] in (isa.C_OTHER, isa.C_ALUI) and d["name"] not in ("jr", "jalr", "break", "mtc0", "mfc0", "j", "jal"):
            res.near[j] = "init"
        elif j in light and guard:
            res.near[j] = "light-positional"
        elif j in light and len(lx) <= 16:
            res.near[j] = "light-small"
    for j in res.near:
        res.x.pop(j, None)
    res.xr_res = xr_residual(res)
    # verdict
    if res.x or any(c == "deleted" for c in res.xr_res.values()):
        res.verdict = "NONE"
    elif res.near or res.xr_res:
        res.verdict = "NEAR"
    else:
        res.verdict = "EXACT"


OPNAMES = {0x00: "G_SPNOOP", 0x01: "G_VTX", 0x02: "G_MODIFYVTX", 0x03: "G_CULLDL", 0x04: "G_BRANCH_Z", 0x05: "G_TRI1", 0x06: "G_TRI2", 0x07: "G_QUAD", 0x10: "G_TRI4", 0xd5: "G_SPECIAL_1", 0xd6: "G_DMA_IO",
           0xd7: "G_TEXTURE", 0xd8: "G_POPMTX", 0xd9: "G_GEOMETRYMODE", 0xda: "G_MTX", 0xdb: "G_MOVEWORD", 0xdc: "G_MOVEMEM", 0xdd: "G_LOAD_UCODE", 0xde: "G_DL", 0xdf: "G_ENDDL", 0xe1: "G_RDPHALF_1",
           0xe2: "G_SETOTHERMODE_L", 0xe3: "G_SETOTHERMODE_H", 0xe4: "G_TEXRECT", 0xe9: "G_RDPFULLSYNC", 0xed: "G_SETSCISSOR", 0xef: "G_RDPSETOTHERMODE", 0xf1: "G_RDPHALF_2", 0xf8: "RDP command (copy)",
           0xfd: "G_SETTIMG"}


def handler_table(res, U):
    """per handler (a distinct pc of the unknown's jump table): the words reachable from its entry and what became of them. -> [(names, pc, counts dict)]"""
    img = U.img
    tab = res.tab
    by_pc = {}
    for op, pc in tab.items():
        if valid_pc(pc):
            by_pc.setdefault(pc, []).append(op)
    head = res.rec.pc["head"]
    stop = [tab.get(OP_LOADER) or 0xfac, head, head - 4]
    set_active(img, U.data, res.rec.lay)
    final = {}
    for j in range(img.n):
        if j in res.x:
            final[j] = "X"
        elif j in res.near:
            final[j] = "near"
        elif j in res.waived:
            final[j] = "rule"
        else:
            k = res.prim.status[j][0]
            final[j] = {"ok": "same", "pc": "reloc", "knob": "reloc", "pad": "pad", "pat": "pattern", "X": "composite"}[k]
    rows = []
    for pc, ops in sorted(by_pc.items()):
        if pc == head or pc == (tab.get(OP_LOADER) or -1) and False:
            continue
        r = reach(img, [pc], stop=stop)
        c = collections.Counter(final[j] for j in r)
        names = sorted({OPNAMES.get(o, "op%02x" % o) for o in ops if o < 0x80 or o >= 0xb0} or {OPNAMES.get(o, "op%02x" % o) for o in ops})
        if len(names) > 4:
            names = names[:3] + ["+%d" % (len(names) - 3)]
        rows.append((", ".join(names), pc, len(r), c))
    return rows, final


def xr_residual(res):
    """reference words the unknown lacks and nothing in the unknown replaces: -> {i: category}"""
    P = res.prim
    ir = P.R.img
    out = {}
    for i in P.xr:
        run = P.hunk_of_r.get(i)
        if run is not None and all(j not in res.x for j in run):
            continue                            # replaced by words that the composite or a rule explains
        if any(i in st for st in res.ref_sets.values()):
            continue                            # the reference's optional handler (G_SPECIAL_1, G_TRI4); the unknown has no such command
        d = isa.decode(ir.w[i])
        if 0x80 <= ir.pc[i] < P.R.rec.pc["head"] and ir.seg[i] == 0 and d["cls"] in (isa.C_OTHER, isa.C_ALUI) and d["name"] not in ("jr", "jalr", "break", "mtc0", "mfc0", "j", "jal"):
            out[i] = "init"
        else:
            out[i] = "extra"
    # a long run of reference-only code is a feature the unknown lacks (the reference's positional-light path, a handler); a short one is an instruction missing from the unknown
    idxs = sorted(i for i, c in out.items() if c == "extra")
    runs = []
    for i in idxs:
        if runs and runs[-1][-1] == i - 1:
            runs[-1].append(i)
        else:
            runs.append([i])
    for run in runs:
        if len(run) < 16:
            for i in run:
                out[i] = "deleted"
    return out


def derive_unknown(U, refs):
    return derive(U, refs)
