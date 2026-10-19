#!/usr/bin/env python3
"""Offline derivation of a graphics microcode's build record (docs/ucode-derive.md).

  derive_build.py derive DUMPS KEY [--exclude A,B] [--family auto|f3dex2|f3dex1|f3d] [--all]
        KEY: a ucode id (hex, as the log prints it), a 4 KiB text hash, or a dump file stem prefix. Prints the best family, the derived build record as C, every unexplained
        difference, and the verdict (EXACT / NEAR / NONE).
  derive_build.py validate DUMPS      leave-one-out over every registered build: derives each one without its own table and compares it with the hand-written record, field by field
  derive_build.py mutate DUMPS        mutation check: deliberate errors in a text, which the derivation must report
  derive_build.py hot DUMPS STATS     the non-native ucodes of tools/ucode_coverage.py's STATS directory, derived one by one
  derive_build.py selftest            synthetic checks of the decoder, the alignment and the classification (no dumps needed)

DUMPS is a directory of tools/ucode_dump_shim.c runs (DUMPS/<set>/<game>/dump/<text>.<data>.{text,data,ext,imem}); it is ROM-derived and stays out of git."""
import argparse
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import derive_f1d as D1  # noqa: E402
import derive_f2 as F2  # noqa: E402
import derive_f3d as F3  # noqa: E402
import derive_lib as L  # noqa: E402
import rsp_isa as isa  # noqa: E402

TOOL_VERSION = 1
FAMILIES = ("f3dex2", "f3dex1", "f3d")


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
class Registry:
    def __init__(self, dumps):
        self.C = L.Corpus(dumps)
        self.T = L.CTables()
        self.fam = {}       # family -> [Item]  (one per distinct text of every record)
        self.recs = {}      # family -> {record name: record}
        for fam, loader in (("f3dex2", F2.load_f2_records), ("f3dex1", D1.load_f1_records), ("f3d", F3.load_f3d_records)):
            recs = {r.name: r for r in loader(self.T, self.C)}
            items = []
            for n, r in recs.items():
                seen = set()
                cands = []
                if fam == "f3dex2" and r.load_key:
                    u = self.C.find_id(r.load_key)
                    if u:
                        cands.append(u)
                for i in r.ids:
                    cands.extend(self.C.by_id.get(i, []))
                for u in sorted(cands, key=lambda u: -sum(u.games.values())):
                    key = (u.h4k, u.hf48)
                    if key in seen:
                        continue
                    seen.add(key)
                    it = F2.Item(u, r) if fam == "f3dex2" else D1.ItemV(u, r)
                    it.name = r.name if not items or all(x.rec is not r for x in items) else "%s:%s" % (r.name, u.stem[:6])
                    items.append(it)
            self.recs[fam] = recs
            self.fam[fam] = items

    def refs(self, fam, exclude_recs=()):
        return [it for it in self.fam[fam] if it.rec.name not in exclude_recs]

    def lookup(self, key):
        key = key.lower().replace("0x", "")
        try:
            v = int(key, 16)
        except ValueError:
            v = None
        if v is not None:
            u = self.C.find_id(v)
            if u:
                return u
            for u in self.C.items:
                if u.h4k == v or u.hf48 == v:
                    return u
        for u in self.C.distinct_texts():
            if u.stem.startswith(key):
                return u
        return None


def derive_any(reg, U_uc, family="auto", exclude_recs=()):
    """-> (family, result, Item)"""
    cands = []
    for fam in FAMILIES if family == "auto" else (family,):
        refs = reg.refs(fam, exclude_recs)
        if not refs:
            continue
        if fam == "f3dex2":
            U = F2.Item(U_uc, None, "unknown")
            res = F2.derive(U, refs)
            nx = len(res.x) + len(res.near) + len(getattr(res, "xr_res", {}))
        elif fam == "f3dex1":
            U = D1.ItemV(U_uc, None, "unknown")
            res = D1.derive_f1(U, refs)
            nx = len(res.x) + len(res.near) + len(res.xr_res)
        else:
            U = D1.ItemV(U_uc, None, "unknown")
            res = F3.derive_f3d(U, refs)
            nx = len(res.x) + len(res.xr_res)
        aligned = len(res.prim.pu) / float(U.img.n)
        cands.append((fam, res, U, aligned, nx))
    cands.sort(key=lambda c: (-round(c[3], 2), c[4]))
    fam, res, U, aligned, nx = cands[0]
    res.family_scores = [(c[0], round(c[3], 3), c[4], c[1].verdict) for c in cands]
    return fam, res, U


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
# output
# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
def hx(v, w=3):
    return "0x%0*x" % (w, v)


def fmt_f2_record(reg, res, U, name, ident):
    rec = res.rec
    T = reg.T
    std = reg.recs["f3dex2"]["l_b_dk64"].lay
    lines = []
    lay_diff = {k: v for k, v in rec.lay.items() if k in F2.LAY_FIELDS and v != std.get(k, 0) and k not in ("lit_vtx",)}
    is_s2 = bool(res.prim.R.rec.ext)
    lay_name = None
    if lay_diff or is_s2:
        lay_name = "l_lay_" + name
        fs = []
        for k in F2.LAY_FIELDS:
            v = rec.lay.get(k, 0)
            if (is_s2 and v) or (not is_s2 and k in lay_diff):
                fs.append(".%s = %s" % (k, hx(v) if k not in ("geom_def", "mw_max", "out_bytes", "pipe_sync", "no_lit", "moveword_adj") else str(v)))
        lines.append("static const struct f2_layout %s = {\n    %s };" % (lay_name, ", ".join(fs)))
    pcs_name = None
    stdp = reg.recs["f3dex2"]["l_b_dk64"].f2pcs
    if any(rec.f2pcs.get(k) != stdp.get(k) for k in F2.F2PCS_FIELDS):
        pcs_name = "l_pcs_" + name
        lines.append("static const struct f2_pcs %s = { %s };" % (pcs_name, ", ".join(".%s = %s" % (k, hx(rec.f2pcs[k])) for k in F2.F2PCS_FIELDS)))
    ext_name = None
    if rec.ext:
        ext_name = "l_ext_" + name
        lines.append("static const uint16_t %s[][2] = {\n    %s, { 0xffff, 0 } };" % (ext_name, ", ".join("{ %s, %d }" % (hx(p), kd) for p, kd in sorted(rec.ext, key=lambda e: e[1]))))
    fs = [".%s = %s" % (k, hx(rec.pc[k])) for k in F2.PC_FIELDS if rec.pc.get(k) not in (0, None, -1) or k in ("head", "tri1", "tri2", "vtx")]
    if rec.gen204:
        fs.append(".gen204 = 1")
    if rec.clip204:
        fs.append(".clip204 = 1")
    if rec.zex:
        fs.append(".zex = 1")
    if rec.bz_z:
        fs.append(".bz_z = 1")
    if rec.bz_w:
        fs.append(".bz_w = 1")
    fs.append(".reject_mask = %s" % hx(rec.reject_mask, 4))
    if lay_name:
        fs.append(".lay = &%s" % lay_name)
    if pcs_name:
        fs.append(".pcs = &%s" % pcs_name)
    if ext_name:
        fs.append(".ext = %s, .ext_cmd = s2_ext_cmd" % ext_name)
    fs.append(".load_key = 0x%016xull" % rec.load_key)
    lines.append("static const struct f3dex2_build l_b_%s = {\n    %s };" % (name, ", ".join(fs)))
    banner = U.uc.banner.split(" | ")[0].replace("RSP Gfx ucode ", "")[:60]
    lines.append("/* the HLE_UCODE entries; .flags are informational (nothing in the front-end reads them) */")
    lines.append('HLE_UCODE(l_%s) = { .id = %sull, .name = "%s", .fam = &f3dex2_fam, .fam_data = &l_b_%s, F3DEX2_DEAD };   /* dead ranges: the primary reference\'s, no poison evidence yet */' % (name, ident, banner, name))
    return "\n".join(lines)


def emit(reg, keys, path):
    """the derived F3DEX2-family records of the given texts as one C file (for a scratch build: the experiment of docs/ucode-derive.md; never part of the tree)"""
    out = ['/* generated by tools/derive_build.py emit: candidate builds derived offline, NOT registered in the tree */', '#include <stdint.h>', '#include "hle_f3dex2_int.h"', '#ifdef ARMLET_NATIVE', '']
    for key in keys:
        u = reg.lookup(key)
        fam, res, U = derive_any(reg, u, "f3dex2")
        nm = "c%s" % u.stem[:8]
        ids = sorted({st.split(".")[0] for st, x in reg.C.by_stem.items() if x.h4k == u.h4k and not st.startswith("L")})
        out.append("/* %s %s: %s, primary %s + %s; ids %s */" % (u.stem[:16], u.banner[:50], res.verdict, res.primary, sorted(res.contrib), ids))
        txt = fmt_f2_record(reg, res, U, nm, "0x" + ids[0])
        # one HLE_UCODE entry per id
        lines = txt.split("\n")
        body = [l for l in lines if not l.startswith("HLE_UCODE") and not l.startswith("/* the HLE_UCODE")]
        out += body
        registered = {L.uhex(f["id"]) for _, f in reg.T.ucodes}
        for i, idh in enumerate(ids):
            if int(idh, 16) in registered:
                continue
            out.append('HLE_UCODE(l_%s_%d) = { .id = 0x%sull, .name = "candidate %s", .fam = &f3dex2_fam, .fam_data = &l_b_%s, F3DEX2_DEAD };' % (nm, i, idh, nm, nm))
        out.append("")
    out.append("#endif")
    open(path, "w").write("\n".join(out) + "\n")


def disasm_u(U, j):
    return "%s %03x  %s" % (U.img.segs[U.img.seg[j]][0][:3], U.img.pc[j], isa.disasm(U.img.w[j], U.img.pc[j]))


def report(reg, fam, res, U, args):
    uc = U.uc
    out = []
    P = out.append
    P("ucode %s  %s" % (uc.stem, uc.banner[:100]))
    gm = collections.Counter()
    for x in reg.C.by_stem.values():
        if x.h4k == uc.h4k:
            for g, n in x.games.items():
                gm[g] += n
    P("games (tasks in the survey run): %s" % ", ".join("%s (%d)" % kv for kv in gm.most_common(8)))
    P("family scores (family, aligned fraction, residual words, verdict): %s" % res.family_scores)
    P("best family %s; primary reference %s; also explained by %s" % (fam, res.primary, res.contrib or "-"))
    P("VERDICT %s" % res.verdict)
    n_near = collections.Counter(res.near.values()) if hasattr(res, "near") else {}
    P("  unexplained words: %d; near-class words: %s; reference-only words: %s" % (len(res.x), dict(n_near) or "-", dict(collections.Counter(getattr(res, "xr_res", {}).values())) or "-"))
    xc, xrc = F2.region_counts(res, U)
    P("  unexplained words by region (%s): %s; deleted reference-only words: %s" % ("/".join(F2.REGIONS), "/".join(map(str, xc)), "/".join(map(str, xrc))))
    if fam == "f3dex2":
        rec = res.rec
        P("record (derived):")
        P("  pcs: " + ", ".join("%s=%s" % (k, hx(rec.pc[k])) for k in F2.PC_FIELDS if rec.pc.get(k, 0)))
        P("  gen204=%d clip204=%d zex=%d reject_mask=%s load_key=0x%016x pipe_sync=%d" % (rec.gen204, rec.clip204, rec.zex, hx(rec.reject_mask, 4), rec.load_key, rec.lay.get("pipe_sync", 0)))
        std = reg.recs["f3dex2"]["l_b_dk64"].lay
        P("  layout fields that differ from the standard layout: %s" % ({k: hx(v) for k, v in rec.lay.items() if k in F2.LAY_ADDR and v != std.get(k)} or "none"))
        P("  layout fields / constants confirmed by aligned instructions (moved or unchanged): %d; not confirmed by any aligned instruction (kept from the primary reference): %s" % (
            len(res.confirmed), ", ".join("%s=%s" % (k, hx(v)) for k, v, c in res.unanchored) or "none"))
        for k, v in res.conflicts.items():
            P("  CONFLICT %s: %s" % (k, v))
        for n in res.notes:
            P("  note: " + n)
        P("  flag evidence (words that only references with the value explain: value 0, value 1): %s" % res.flag_evidence)
        P("")
        ids = sorted({st.split(".")[0] for st, x in reg.C.by_stem.items() if x.h4k == uc.h4k and not st.startswith("L")}) or ["%016x" % uc.h4k]
        P("/* ucode ids of this text (FNV over the 4 KiB; dumps taken before 2026-10-16 are named by the FNV over ucode_size bytes, one per size the games pass): %s */" % ", ".join(ids))
        P(fmt_f2_record(reg, res, U, "new", "0x" + ids[0]))
    else:
        P("record (derived): " + ", ".join("%s=%s" % (k, hx(v[0], 3) if isinstance(v[0], int) and v[0] > 9 else v[0]) for k, v in res.rec.num.items()) + "; " + ", ".join("%s=%s" % kv for kv in res.rec.flags.items()))
        if res.unanchored:
            P("  fields no aligned instruction confirms (kept from the primary reference): %s" % ", ".join("%s=%s" % (k, hx(v)) for k, v, c in res.unanchored[:40]))
        for n in res.notes:
            P("  note: " + n)
        P("  attribute evidence (pairs, votes): %s" % res.flag_evidence)
    if fam == "f3dex2":
        rows, final = F2.handler_table(res, U)
        P("")
        P("per handler (the words reachable from its entry; shared code is counted in every handler that reaches it): same = identical word, reloc = pc / DMEM address / derived constant, composite = explained by another reference, pad = nop / overlay slack, X = unexplained")
        P("  %-34s %5s %6s %6s %6s %6s %6s %5s %5s" % ("handler (opcodes)", "pc", "words", "same", "reloc", "compo", "pad", "near", "X"))
        for names, pc, n, c in rows:
            P("  %-34s %5s %6d %6d %6d %6d %6d %5d %5d" % (names[:34], "%03x" % pc, n, c["same"], c["reloc"], c["composite"], c["pad"] + c["pattern"] + c["rule"], c["near"], c["X"]))
        by_h = {}
        for names, pc, n, c in rows:
            if c["X"]:
                by_h[names] = c["X"]
    P("")
    if res.x:
        P("UNEXPLAINED differences (%d words), by U address:" % len(res.x))
        runs = []
        for j in sorted(res.x):
            if runs and runs[-1][-1] == j - 1:
                runs[-1].append(j)
            else:
                runs.append([j])
        for run in runs[: (None if args.all else 25)]:
            P("  [%d] %s .. %03x: %s" % (len(run), disasm_u(U, run[0]), U.img.pc[run[-1]], res.x[run[0]]))
            for j in run[:3]:
                P("        %s" % disasm_u(U, j))
        if len(runs) > 25 and not args.all:
            P("  ... %d more runs (--all)" % (len(runs) - 25))
    if res.near:
        P("NEAR-class differences (admitted by the tolerance): %s" % dict(n_near))
    return "\n".join(out)


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
# validate
# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
# mismatches between a derived and a hand-written record that are understood (docs/ucode-derive.md, "Validation"); every other mismatch makes `validate` fail
EXPECTED = {
    ("l_b_bg", "lay.geom_def"): "a front-end decision (which geometry-mode bits the build gives a meaning to): BG's extra bit 0x400000 is the positional-lighting bit, implemented by its own kernel; not visible as an operand",
    ("l_b_bg", "lay.mw_max"): "the size of the G_MOVEWORD table (16 in BG): a bound in a compare no other build's code shows",
    ("l_b_bg", "lay.no_lit"): "BG's lighting is another kernel (f3dexbg_fx): the tool reports the lighting overlay as different (X) but cannot name the kernel",
    ("l_b_bg", "lay.lit_vtx"): "the same",
    ("l_b_095", "pc.persp"): "F3DEX 0.95's G_PERSPNORM handler is in no other reference (F3DLX 0.95, the only other build with it, has no dump): no handler to map it from; the front-end of the derived build declines the command. Verdict NONE (32 unexplained words)",
    ("l_b_095", "pc.quad"): "the same for G_QUAD",
    ("l_b_095", "pc.culldl"): "the same for the 0.95 form of G_CULLDL (byte offsets, the last vertex not tested): its code differs from the 1.2x handler",
    ("l_b_095", "flag.culldl_off"): "follows from the handler: without a G_CULLDL pc the attribute is the primary reference's (and the command declines)",
    ("l_wr", "pc.tri1"): "Wave Race's G_TRI1 entry is a call into a routine it shares with its quad command (`jal 0x288`): no other reference's handler maps onto it; the derived build declines G_TRI1. Verdict NONE (80 unexplained words)",
    ("l_wr", "pc.quad"): "Wave Race's quad command is in no other reference: no handler to map it from (the derived build declines the command)",
    ("l_ge", "pc.tri4"): "GoldenEye's G_TRI4 (2.0G) is in no other reference: no handler to map it from (the derived build declines the command). Verdict NONE (its 17 words unexplained)",
    ("l_b_s2_xbus", "lay.out_bytes"): "an XBUS build has no output buffer (hand value 0): the words that carry Worms' buffer size are replaced by the ring code (init, the flush routine at 0x648), and the tool reads an unrelated immediate there as a size (16)",
    ("l_ge", "lay.tri4_tab"): "follows from the handler: without a G_TRI4 pc there is no table to read",
    ("l_b_ex2_208n_xbus", "lay.out_bytes"): "an XBUS build has no RDP output buffers (its list is a ring in DMEM, the layout's xbus / ring_start): the buffer sizes that its flush code does not name are 0 in the hand record; the transport is not derived (the XBUS work); verdict NONE",
    ("l_b_ex2_208n_xbus", "lay.out_toggle"): "the same",
    ("l_b_rej_xbus", "lay.out_bytes"): "the same: Excitebike 64's Rej text is an XBUS build, derived here from the fifo Rej texts (the flush code is unexplained words); xbus / ring_start are the primary reference's, not derived",
    ("l_b_rej_xbus", "lay.out_toggle"): "the same",
    ("l_b_rej_203", "lay.rej_flags"): "REJ_LIT_LX (the unsigned multiply-accumulate lighting) is in no other reference, so no remaining reference can show it; REJ_FLAT_A is flx's alone, and flx is the primary reference (no discriminating evidence: the primary's bit)",
    ("l_b_rej_206", "lay.rej_flags"): "every other Rej reference rounds y (REJ_YQ): none shows the text that does not, so the bit is the primary reference's",
    ("l_b_rej_206", "lay.no_lit"): "F3DEX.Rej 2.06's lighting is not modelled (lit vertex commands decline): a front-end decision no operand shows",
    ("l_b_rej_206", "lay.light_scale"): "2.06 lights no vertex (no_lit); the hand record's 0x1c6 is a copy of the 2.08 text's, its constant vector has the scale in lane 7 (0x1ce), which is what the derivation reads",
    ("l_b_rej_flx", "lay.rej_flags"): "REJ_FLAT_A and REJ_LIT_FLX are in no other reference (this build is the only one with them), REJ_LIT_LX is the primary reference's: bits that no remaining reference can show",
}


def explain(res, name, hand, der):
    base = name
    if any(u[0] == base for u in res.unanchored):
        return "unconfirmed: no aligned instruction anchors it, kept from the primary reference (reported as such)"
    attr = base[5:] if base.startswith("flag.") else base
    if any(attr in n for n in getattr(res, "notes", [])):
        return "noted: the tool says it could not decide it"
    return None


def compare(hand, der):
    h, d = hand.fields(), der.fields()
    return [(k, h[k], d.get(k)) for k in h if h[k] != d.get(k)]


def validate(reg, args):
    rows = []
    for fam in FAMILIES:
        recs = reg.recs[fam]
        for Uit in reg.fam[fam]:
            n = Uit.name
            refs = reg.refs(fam, {Uit.rec.name})
            if fam == "f3dex2":
                res = F2.derive(F2.Item(Uit.uc, None, "unknown"), refs)
                near = collections.Counter(res.near.values())
            elif fam == "f3dex1":
                res = D1.derive_f1(D1.ItemV(Uit.uc, None, "unknown"), refs)
                near = collections.Counter(res.near.values())
            else:
                res = F3.derive_f3d(D1.ItemV(Uit.uc, None, "unknown"), refs)
                near = {}
            res.hand = Uit
            rows.append((fam, n, res, compare(Uit.rec, res.rec), len(res.xr_res), near))
    return rows


def print_validate(reg, rows, args):
    """-> number of mismatching fields nobody explained"""
    bad = 0
    print("| family | build (hand record) | ids | primary reference | also needs | verdict | unexplained | near / reference-only | fields compared | fields that differ from the hand record |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    notes = []
    for fam, n, res, mism, xr, near in rows:
        nf = len(res.hand.rec.fields())
        cats = collections.Counter()
        for k, a, b in mism:
            e = explain(res, k, a, b)
            tag = "unconfirmed" if e and e.startswith("unconfirmed") else "noted" if e and e.startswith("noted") else None
            if tag is None and (res.hand.rec.name, k) in EXPECTED:
                tag = "expected"
                e = EXPECTED[(res.hand.rec.name, k)]
            if tag is None:
                tag = "UNEXPLAINED"
                bad += 1
            cats[tag] += 1
            notes.append((n, k, a, b, tag, e))
        ms = ", ".join("%s %d" % kv for kv in cats.items()) or "none"
        print("| %s | %s | %s | %s | %s | %s | %d | %s | %d | %s |" % (fam, n, res.hand.uc.stem[:8], res.primary, ", ".join(sorted(res.contrib)) or "-", res.verdict, len(res.x),
                                                                    "%s / %d" % (dict(near) or "-", xr), nf, ms))
    print("")
    print("| build | field | hand | derived | class | why |")
    print("|---|---|---|---|---|---|")
    for n, k, a, b, tag, e in notes:
        print("| %s | %s | %s | %s | %s | %s |" % (n, k, hx(a, 1) if isinstance(a, int) else a, hx(b, 1) if isinstance(b, int) else b, tag, e or ""))
    return bad


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
def hot(reg, args):
    import ucode_coverage as UC
    games, rows, hl, ng, regtexts = UC.compute(args.stats)
    by_h4k = {}
    for u in reg.C.distinct_texts():
        by_h4k[u.h4k] = u
    table = []
    blocks = []
    for i, h in enumerate(hl, 1):
        u = by_h4k.get(int(h["text"], 16))
        label = UC.short(h["name"])
        gl = sorted(set(h["games"]))
        if u is None:
            table.append((i, label, h, "-", "not in the dumps", 0, "", ""))
            continue
        alias = next((it.name for f in reg.fam.values() for it in f if it.uc.h4k == u.h4k), None)
        if alias:
            table.append((i, label, h, "-", "SAME TEXT as registered %s" % alias, 0, "", ""))
            continue
        fam, res, U = derive_any(reg, u)
        near = collections.Counter(res.near.values()) if hasattr(res, "near") else {}
        xr = collections.Counter(getattr(res, "xr_res", {}).values())
        extra = ", ".join("%s %d" % kv for kv in (list(near.items()) + list(xr.items())))
        table.append((i, label, h, fam, res.verdict, len(res.x), extra, res))
        blocks.append((i, label, h, fam, res, U))
    print("| # | ucode (text) | family | games | RSP time on JIT (mean over %d games) | task share | build handler today | derivation: best family | verdict | unexplained words | near / reference-only | primary + composite |" % ng)
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for i, label, h, fam, verdict, nx, extra, res in table:
        have = "registered (declines for other reasons)" if h["nohandler_tasks"] == 0 else "none" if h["nohandler_tasks"] == h["tasks_total"] else "partly"
        if res == "":
            print("| %d | %s `%s` | %s | %d | %.2f%% | %.2f%% | %s | - | %s | - | - | - |" % (i, label, h["text"][:8], UC.FAM[h["fam"]], len(set(h["games"])), h["share_sum"] / ng, h["task_share_sum"] / ng, have, verdict))
            continue
        print("| %d | %s `%s` | %s | %d | %.2f%% | %.2f%% | %s | %s | %s | %d | %s | %s |" % (i, label, h["text"][:8], UC.FAM[h["fam"]], len(set(h["games"])), h["share_sum"] / ng, h["task_share_sum"] / ng, have, fam, verdict, nx, extra or "-",
                                                                                 "%s%s" % (res.primary, (" + " + ", ".join(sorted(res.contrib))) if res.contrib else "")))
    if getattr(args, "details", True):
        for i, label, h, fam, res, U in blocks:
            print("\n==== #%d %s (%s) ====" % (i, label, h["text"][:8]))
            print("games: %s" % ", ".join(sorted(set(h["games"]))))
            print(report(reg, fam, res, U, args))


def mutate_text(uc, edits):
    """a copy of a Ucode with words replaced: edits = {text word index: new word}"""
    import copy
    m = copy.copy(uc)
    w = list(uc.w)
    for i, v in edits.items():
        w[i] = v
    m.w = w
    m.text_b = struct.pack(">%dI" % len(w), *w)
    m.h4k = L.fnv(m.text_b, 0x1000)
    m.hf48 = L.fnv(m.text_b, 0xf48)
    m.stem = "mutant"
    return m


def mutations(reg, args):
    """deliberate errors in Donkey Kong 64's text, derived against the other builds: each must change the result"""
    base = next(it for it in reg.fam["f3dex2"] if it.name == "l_b_dk64")
    refs = reg.refs("f3dex2", {"l_b_dk64"})
    ok_all = True
    res0 = F2.derive(F2.Item(base.uc, None, "unknown"), refs)
    print("unmutated Donkey Kong 64 against the other builds: %s, %d unexplained, derived reject_mask %s" % (res0.verdict, len(res0.x), hx(res0.rec.reject_mask, 4)))

    def idx(pc):
        return base.img.idx_text(pc)
    tests = []
    w = base.uc.w
    # 1: a vector instruction of the vertex pipeline replaced by another
    i = idx(0x5e0)
    tests.append(("vertex pipeline: opcode of the instruction at 0x5e0 changed", {i: w[i] ^ 0x00000003}, lambda r: r.verdict == "NONE" and len(r.x) >= 1))
    # 2: a lane (element) of a triangle-setup vector op changed (a zex-like difference without the flag)
    i = idx(0x94c)
    tests.append(("triangle setup: element selector of the instruction at 0x94c changed", {i: w[i] ^ (4 << 21)}, lambda r: r.verdict in ("NONE", "NEAR") and (len(r.x) >= 1 or r.rec.zex == 1)))
    # 3: the reject mask of G_TRI1 changed to a value no build has
    i = idx(0x820)
    tests.append(("G_TRI1 reject mask 0x7070 -> 0x7050", {i: (w[i] & 0xffff0000) | 0x7050}, lambda r: r.rec.reject_mask in (0x7050, 0x7070) and (r.rec.reject_mask == 0x7050 or r.verdict != "EXACT")))
    # 4: a jump retargeted to another handler
    i = idx(0x1d4)
    d = isa.decode(w[i])
    tests.append(("a branch of G_GEOMETRYMODE retargeted by 8 bytes", {i: (w[i] & ~0xffff) | ((w[i] + 2) & 0xffff)}, lambda r: len(r.x) >= 1 or r.verdict != "EXACT"))
    # 6: one use of a layout field moved (the geometry mode word 0x1ec -> 0x1f0 in one instruction): the record would have to read it at two places
    cand = [k for k in range(len(w)) if isa.decode(w[k])["cls"] == isa.C_MEM and isa.decode(w[k])["rs"] == 0 and isa.decode(w[k])["imm"] == 0x1ec]
    if len(cand) >= 2:
        k = cand[1]
        tests.append(("one load of the geometry mode word moved from 0x1ec to 0x1f0 (the other uses stay)", {k: (w[k] & 0xffff0000) | 0x1f0}, lambda r: r.verdict == "NONE"))
    # 7: an address that is no field of the record (the matrix stack pointer 0xf4)
    cand = [k for k in range(len(w)) if isa.decode(w[k])["cls"] == isa.C_MEM and isa.decode(w[k])["rs"] == 0 and isa.decode(w[k])["imm"] == 0xf4]
    if cand:
        k = cand[0]
        tests.append(("a use of the matrix stack pointer (0xf4, no layout field) moved to 0xf0", {k: (w[k] & 0xffff0000) | 0xf0}, lambda r: len(r.x) >= 1 and r.verdict == "NONE"))
    # 8: an inserted instruction (a nop run is padding; a real one is not)
    j = idx(0x600)
    tests.append(("an instruction in the vertex loop replaced by a nop", {j: 0}, lambda r: r.verdict in ("NONE",) and len(r.x) >= 0))
    for desc, ed, check in tests:
        m = mutate_text(base.uc, ed)
        r = F2.derive(F2.Item(m, None, "unknown"), refs)
        good = check(r)
        ok_all &= good
        print("  %-78s -> %-5s unexplained %3d near %s %s" % (desc, r.verdict, len(r.x), dict(collections.Counter(r.near.values())), "caught" if good else "NOT CAUGHT"))
    # a data mutation: the jump table entry of G_VTX
    d = bytearray(base.uc.data)
    off = 0x36e + 2 * 1
    d[off:off + 2] = struct.pack(">H", 0x1000 | 0x4f8)
    import copy
    m = copy.copy(base.uc)
    m.data = bytes(d)
    m.stem = "mutant-data"
    r = F2.derive(F2.Item(m, None, "unknown"), refs)
    good = r.rec.pc["vtx"] == 0x4f8
    ok_all &= good
    print("  %-78s -> derived vtx %s %s" % ("jump table entry of G_VTX 0x4f0 -> 0x4f8", hx(r.rec.pc["vtx"]), "caught" if good else "NOT CAUGHT"))
    for desc, m, check in f1_mutants(reg):
        r = D1.derive_f1(D1.ItemV(m, None, "unknown"), reg.refs("f3dex1", {"l_b_121"}))
        good = check(r)
        ok_all &= good
        print("  %-78s -> %-5s unexplained %3d %s" % (desc, r.verdict, len(r.x), "caught" if good else "NOT CAUGHT"))
    for desc, m, check in f3_mutants(reg):
        r = F3.derive_f3d(D1.ItemV(m, None, "unknown"), reg.refs("f3d", {"l_sm64"}))
        good = check(r)
        ok_all &= good
        print("  %-78s -> %-5s unexplained %3d %s" % (desc, r.verdict, len(r.x), "caught" if good else "NOT CAUGHT"))
    return ok_all


def f3_mutants(reg):
    """deliberate errors in Super Mario 64's F3D text and data (derived against the other F3D builds): -> [(description, Ucode, check of the result)]"""
    import copy
    base = next(it for it in reg.fam["f3d"] if it.rec.name == "l_sm64")
    w = base.uc.w
    idx = base.img.idx_text
    out = []
    i = idx(0x310)          # ori v0, zero, 0xffff (G_CULLDL)
    out.append(("F3D SM64: the G_CULLDL mask 0xffff -> 0x7070", mutate_text(base.uc, {i: (w[i] & 0xffff0000) | 0x7070}), lambda r: r.rec.num["culldl_mask"][0] == 0x7070))
    k = next(k for k in range(len(w)) if isa.decode(w[k]).get("name") == "andi" and isa.decode(w[k])["imm"] == 0x707)
    out.append(("F3D SM64: the near-plane flag mask 0x707 -> 0x703 (Pilotwings' without its code)", mutate_text(base.uc, {k: (w[k] & 0xffff0000) | 0x703}),
                lambda r: "flag.flags_variant" in r.undecided and r.verdict == "NONE"))
    k = next(k for k in range(len(w)) if isa.decode(w[k]).get("name") == "lw" and isa.decode(w[k])["rs"] == 29 and isa.decode(w[k])["imm"] == 4)
    out.append(("F3D SM64: one load of the geometry word 4(sp) -> 8(sp)", mutate_text(base.uc, {k: (w[k] & 0xffff0000) | 8}), lambda r: r.verdict == "NONE" and r.trouble))
    i = idx(0x220)          # sll at, at, 2 (G_TRI1's vertex pointer)
    out.append(("F3D SM64: one of G_TRI1's vertex pointer shifts 2 -> 3", mutate_text(base.uc, {i: (w[i] & ~(31 << 6)) | (3 << 6)}), lambda r: "flag.vptr_mul" in r.undecided and r.verdict == "NONE"))
    k = next(k for k in range(len(w)) if isa.decode(w[k]).get("name") == "andi" and isa.decode(w[k])["imm"] == 0x1000)
    out.append(("F3D SM64: the front-face cull test removed (half of Wave Race's triangle code)", mutate_text(base.uc, {k: (w[k] & 0xffff0000) | 0x800}),
                lambda r: "flag.tri_wr" in r.undecided and r.verdict == "NONE"))
    d = bytearray(base.uc.data)
    off = 0x76 + 2 * 0x3f   # the class-2 table cell of G_TRI1
    d[off:off + 2] = struct.pack(">H", 0x1000 | 0x210)
    m = copy.copy(base.uc)
    m.data = bytes(d)
    m.stem = "mutant-data"
    out.append(("F3D SM64: the handler table cell of G_TRI1 0x20c -> 0x210", m, lambda r: r.rec.num["pc.tri1"][0] != 0x20c))
    return out


def f1_mutants(reg):
    """deliberate errors in F3DEX 1.21's text and data (derived against the other F3DEX 1.x builds): -> [(description, Ucode, check of the result)]"""
    import copy
    base = next(it for it in reg.fam["f3dex1"] if it.rec.name == "l_b_121")
    w = base.uc.w
    idx = base.img.idx_text
    out = []
    i = idx(0x2c0)          # ori v0, zero, 0x7070 (G_CULLDL)
    out.append(("F3DEX 1.21: the G_CULLDL reject mask 0x7070 -> 0x7050", mutate_text(base.uc, {i: (w[i] & 0xffff0000) | 0x7050}),
                lambda r: r.verdict == "NONE" or (r.rec.num["reject_mask"][0] == 0x7050 and r.verdict != "EXACT")))
    k = next(k for k in range(len(w)) if isa.decode(w[k]).get("name") == "andi" and isa.decode(w[k])["imm"] == 0x707)
    out.append(("F3DEX 1.21: the near-plane flag mask 0x707 -> 0x703 (NoN without its code)", mutate_text(base.uc, {k: (w[k] & 0xffff0000) | 0x703}),
                lambda r: r.rec.flags["non"] == 1 and r.verdict == "NONE"))
    k = next(k for k in range(len(w)) if isa.decode(w[k]).get("name") == "lw" and isa.decode(w[k])["rs"] == 29 and isa.decode(w[k])["imm"] == 4)
    out.append(("F3DEX 1.21: one load of the geometry word 4(sp) -> 8(sp)", mutate_text(base.uc, {k: (w[k] & 0xffff0000) | 8}), lambda r: r.verdict == "NONE"))
    i = idx(0x0e4)          # jr s5 (the window refill's return)
    out.append(("F3DEX 1.21: the return of the window refill jr s5 -> jr s4", mutate_text(base.uc, {i: (w[i] & ~(31 << 21)) | (20 << 21)}), lambda r: r.verdict == "NONE"))
    d = bytearray(base.uc.data)
    off = 0x76 + 2 * 0x3f   # the class-2 table cell of G_TRI1
    d[off:off + 2] = struct.pack(">H", 0x1000 | 0x1c4)
    m = copy.copy(base.uc)
    m.data = bytes(d)
    m.stem = "mutant-data"
    out.append(("F3DEX 1.21: the handler table cell of G_TRI1 0x1bc -> 0x1c4", m, lambda r: r.rec.num["pc.tri1"][0] != 0x1bc))
    return out


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
def selftest():
    """no ROM data: the decoder, the alignment, the relocation classes, on a small synthetic microcode"""
    fails = []

    def check(cond, what):
        if not cond:
            fails.append(what)
        print("  %-70s %s" % (what, "ok" if cond else "FAILED"))
    # decoder round trips
    check(isa.disasm(0x20010004, 0) == "addi at, zero, 4", "addi")
    check(isa.decode(0x08000040)["tgt"] == 0x100, "j target")
    check(isa.decode(0x1420fffe)["off"] == -2, "branch offset")
    check(isa.decode(0xc8010001)["cls"] == isa.C_VMEM, "lwc2")
    # alignment: a block moved, one word changed
    a = [0x20010001 + (i << 16) % 0x1f0000 for i in range(40)]
    ka = [L.wkey(w) for w in a]
    b = a[20:] + a[:20]
    b[3] = (b[3] & 0xffff0000) | 0x1234      # an immediate differs: still aligned
    kb = [L.wkey(w) for w in b]
    pr = L.align_keys(ka, kb)
    check(len(pr) == 40, "moved block and a changed immediate stay aligned")
    b[5] = 0x00000000                          # a replaced word: unaligned
    pr = L.align_keys(ka, [L.wkey(w) for w in b])
    check(len(pr) == 39, "a replaced word is unaligned")
    # size encodings
    check(F2.size_encs(800)["v-1"] == 799 and F2.inv_size("-v", (-792) & 0xffff) == 792, "size encodings")
    check(F2.inv_size("v-1", 791) == 792, "size inverse")
    return not fails


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("derive")
    p.add_argument("dumps")
    p.add_argument("key")
    p.add_argument("--exclude", default="")
    p.add_argument("--family", default="auto")
    p.add_argument("--all", action="store_true")
    p = sub.add_parser("validate")
    p.add_argument("dumps")
    p = sub.add_parser("mutate")
    p.add_argument("dumps")
    p = sub.add_parser("hot")
    p.add_argument("dumps")
    p.add_argument("stats")
    p.add_argument("--all", action="store_true")
    p.add_argument("--no-details", dest="details", action="store_false")
    p = sub.add_parser("emit")
    p.add_argument("dumps")
    p.add_argument("out")
    p.add_argument("keys", nargs="+")
    sub.add_parser("selftest")
    args = ap.parse_args()
    if args.cmd == "selftest":
        sys.exit(0 if selftest() else 1)
    reg = Registry(args.dumps)
    if args.cmd == "derive":
        u = reg.lookup(args.key)
        if u is None:
            sys.exit("no such ucode in the dumps")
        ex = set()
        for k in filter(None, args.exclude.split(",")):
            ex.add(k)                           # a record name (l_sm64), or a key of its text
            e = reg.lookup(k)
            if e:
                ex |= {e.hf48, e.h4k}
        fam, res, U = derive_any(reg, u, args.family, ex)
        print(report(reg, fam, res, U, args))
    elif args.cmd == "validate":
        rows = validate(reg, args)
        sys.exit(1 if print_validate(reg, rows, args) else 0)
    elif args.cmd == "mutate":
        sys.exit(0 if mutations(reg, args) else 1)
    elif args.cmd == "hot":
        hot(reg, args)
    elif args.cmd == "emit":
        emit(reg, args.keys, args.out)


if __name__ == "__main__":
    main()
