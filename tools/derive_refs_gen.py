#!/usr/bin/env python3
"""Generates src/hle/derive_refs.c: the reference data of the C port of the derivation (src/hle/derive.c, docs/ucode-derive.md "The C port").

  derive_refs_gen.py DUMPS OUT.c            write the file
  derive_refs_gen.py DUMPS OUT.c --check    exit 1 when OUT.c differs from what would be written (the registered builds changed: regenerate)

WHAT IS EMBEDDED. Not the text of any microcode. For every reference (the text of a registered build) the file holds, per instruction word of the image (the 4 KiB text and the
overlay segments behind it), 32 bits: an index into a table of 64-bit hashes of the *shape* of an instruction (its word with the immediate / jump target / load-store offset
masked out, hashed; for the instructions that have no such field, the whole word, hashed) and the 16-bit immediate. Nothing here is an instruction word, and the shape table
cannot be turned back into words without the text (the hash is one-way); it is what the alignment needs to find out which words of an unknown text correspond to which words of a
reference, and which operands differ. Besides: the build records (the numbers of src/hle/hle_f3dex2*.c, hle_s2dex2.c, f3dex.c, f3d.c: pcs, DMEM layout, flags), the
jump tables of the references' data segments (handler pcs, 256 halfwords each), the word indices of the handlers a reference has and a build may lack (G_SPECIAL_1, G_TRI4),
and the overlay geometry of each image (base address, word count). The records come from the C tables themselves, as the Python tool reads them."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import derive_build as B  # noqa: E402
import derive_f2 as F2  # noqa: E402
import derive_lib as L  # noqa: E402
import rsp_isa as isa  # noqa: E402

GEN_VERSION = 2
M64 = 0xffffffffffffffff


def mix64(x):
    """splitmix64 finaliser: the one-way mix of a masked word (derive.c has the same)"""
    x &= M64
    x = ((x ^ (x >> 30)) * 0xbf58476d1ce4e5b9) & M64
    x = ((x ^ (x >> 27)) * 0x94d049bb133111eb) & M64
    return x ^ (x >> 31)


CLS_J, CLS_BR, CLS_MEM, CLS_VMEM, CLS_ALUI, CLS_OTHER, CLS_NOP = range(7)
INIT_EXCLUDED = ("jr", "jalr", "break", "mtc0", "mfc0", "j", "jal")


def shape(w):
    """-> (hash64, cls, rs0, initable, operand). The same function, in C, is derive.c's wshape_of."""
    d = isa.decode(w)
    c = d["cls"]
    rs0 = 0
    operand = 0
    if c == isa.C_NOP:
        m, cls = 0, CLS_NOP
    elif c == isa.C_J:
        m, cls = w & 0xfc000000, CLS_J
        operand = w & 0x3ffffff                 # (the packed word of a jump keeps all 26 bits: see below)
    elif c == isa.C_BR:
        m, cls, operand, rs0 = w & 0xffff0000, CLS_BR, w & 0xffff, int(d["rs"] == 0)
    elif c == isa.C_MEM:
        m, cls, operand, rs0 = w & 0xffff0000, CLS_MEM, w & 0xffff, int(d["rs"] == 0)
    elif c == isa.C_VMEM:
        # (the names of an undefined sub opcode are "?v<sub>" for both lwc2 and swc2: one shape)
        tab = isa.LV if d["op"] == 0x32 else isa.SV
        mm = w & ~0x7f
        if d["sub"] not in tab:
            mm = (mm & ~0xfc000000) | (0x32 << 26)
        m, cls, operand, rs0 = mm, CLS_VMEM, d["imm"] & 0xffff, int(d["rs"] == 0)
    elif c == isa.C_ALUI:
        m, cls, operand, rs0 = w & 0xffff0000, CLS_ALUI, w & 0xffff, int(d["rs"] == 0)
    else:
        m, cls = w, CLS_OTHER
    init = int(c == isa.C_ALUI or (c == isa.C_OTHER and d["name"] not in INIT_EXCLUDED))
    return mix64(mix64(m) ^ cls), cls, rs0, init, operand


def selfcheck_shapes(items):
    """shape equality must be wkey equality (the key the Python alignment uses), over every word of every reference"""
    by_key, by_shape = {}, {}
    for it in items:
        for w in it.img.w:
            k = L.wkey(w)
            s = shape(w)[0]
            if by_key.setdefault(k, s) != s:
                raise SystemExit("wkey %r has two shapes" % (k,))
            if by_shape.setdefault(s, k) != k:
                raise SystemExit("shape %x has two wkeys: %r %r" % (s, by_shape[s], k))


def wrap(vals, per, fmt):
    out = []
    for i in range(0, len(vals), per):
        out.append("    " + ", ".join(fmt % v for v in vals[i:i + per]) + ",")
    return "\n".join(out)


def generate(dumps):
    reg = B.Registry(dumps)
    items = []          # (fam index, Item)
    recs = []           # unique records, in order
    rec_index = {}
    for fi, fam in enumerate(B.FAMILIES):
        for it in reg.fam[fam]:
            items.append((fi, it))
            if it.rec.name not in rec_index:
                rec_index[it.rec.name] = len(recs)
                recs.append((fi, it.rec))
    selfcheck_shapes([it for _, it in items])
    # the shape table, sorted by hash (derive.c binary-searches it)
    sh = {}
    for fi, it in items:
        for w in it.img.w:
            h, cls, rs0, init, operand = shape(w)
            d = isa.decode(w)
            rs29 = int(d["cls"] in (isa.C_MEM, isa.C_VMEM) and d["rs"] == 29)
            sh[h] = cls | rs0 << 3 | init << 4 | rs29 << 5
    shapes = sorted(sh)
    shape_info = [sh[h] for h in shapes]
    sidx = {h: i for i, h in enumerate(shapes)}
    assert len(shapes) < 0x8000
    words = []
    refs = []
    jts = []
    sets = []
    for fi, it in items:
        img = it.img
        woff = len(words)
        for w in img.w:
            h, cls, rs0, init, operand = shape(w)
            if cls == CLS_J:
                words.append(0x80000000 | (w >> 26 & 1) << 26 | operand)     # bit 31: a jump; bit 26: jal; the target field
            else:
                words.append(sidx[h] << 16 | operand)
        jt_off = -1
        set_ref = [(0, 0), (0, 0)]
        if fi == 0:
            rec = it.rec
            tabR = F2.read_table(it.data, rec.lay["jump_table"])
            jt_off = len(jts)
            jts.extend(-1 if tabR[op] is None else tabR[op] for op in range(256))
            F2.set_active(img, it.data, rec.lay)
            stop = [tabR.get(F2.OP_LOADER) or 0xfac, rec.pc["head"], rec.pc["head"] - 4]
            for n, role in enumerate(("mvp", "tri4")):
                pcR = rec.pc.get(role, 0)
                if pcR:
                    s = F2.reach(img, [pcR], stop=stop) - F2.reach(img, [rec.pc["head"]], stop=[0xfac])
                    set_ref[n] = (len(sets), len(s))
                    sets.extend(sorted(s))
            img.active = None
        segs = []
        for nm, base, first, n in img.segs:
            off = 0 if nm == "text" else int(nm.split("+")[1], 16)
            segs.append((base, first, n, off))
        refs.append((it, fi, woff, segs, jt_off, set_ref))
    o = []
    P = o.append
    P("/* GENERATED by tools/derive_refs_gen.py (generator version %d) from the C tables of src/hle and the ucode dumps of the registered builds. Do not edit." % GEN_VERSION)
    P(" * What is in it, and what is not: tools/derive_refs_gen.py, docs/ucode-derive.md \"The C port\". No instruction word of any microcode is stored: per word a 16-bit index into")
    P(" * a table of one-way 64-bit hashes of the instruction's shape, and the 16-bit immediate. */")
    P('#include "derive_int.h"')
    P("")
    P("const unsigned derive_gen_version = %d;" % GEN_VERSION)
    P("const unsigned derive_n_shapes = %d;" % len(shapes))
    P("const uint64_t derive_shape_hash[%d] = {" % len(shapes))
    P(wrap(["0x%016x" % h for h in shapes], 4, "%s"))
    P("};")
    P("const uint8_t derive_shape_info[%d] = {" % len(shapes))
    P(wrap(shape_info, 24, "%d"))
    P("};")
    P("const uint32_t derive_words[%d] = {" % len(words))
    P(wrap(words, 12, "0x%08x"))
    P("};")
    P("const int16_t derive_jt[%d] = {" % max(1, len(jts)))
    P(wrap(jts or [-1], 24, "%d"))
    P("};")
    P("const uint16_t derive_sets[%d] = {" % max(1, len(sets)))
    P(wrap(sets or [0], 16, "%d"))
    P("};")
    P("const struct derive_rec derive_recs[%d] = {" % len(recs))
    for fi, r in recs:
        if fi == 0:
            fs = ["    { .name = \"%s\", .fam = 0" % r.name]
            fs.append(".gen204 = %d, .zex = %d, .bz_z = %d, .reject_mask = 0x%x, .load_key = 0x%016xull" % (r.gen204, r.zex, r.bz_z, r.reject_mask, r.load_key))
            fs.append(".pc = { %s }" % ", ".join(str(r.pc.get(k, 0)) for k in F2.PC_FIELDS))
            fs.append(".f2pcs = { %s }" % ", ".join(str(r.f2pcs.get(k, 0)) for k in F2.F2PCS_FIELDS))
            fs.append(".lay = { %s }" % ", ".join(str(r.lay.get(k, 0)) for k in F2.LAY_FIELDS))
            if r.ext:
                fs.append(".n_ext = %d, .ext = { %s }" % (len(r.ext), ", ".join("{ %d, %d }" % (p, k) for p, k in r.ext)))
            P(",\n      ".join(fs) + " },")
        elif fi == 1:
            import derive_f1d as D1
            fs = ["    { .name = \"%s\", .fam = 1" % r.name]
            fs.append(".reject_mask = 0x%x, .load_key = 0x%016xull, .non = %d, .dlx = %d, .culldl_off = %d" % (r.num["reject_mask"][0], r.load_key or 0, r.flags["non"], r.flags["dlx"], r.flags["culldl_off"]))
            fs.append(".lay = { %s }" % ", ".join("0x%x" % r.num["lay." + k][0] for k in D1.F1_LAY_FIELDS))
            fs.append(".f1pc = { %s }" % ", ".join("0x%x" % r.num["pc." + k][0] for k in D1.F1_PC_FIELDS))
            P(",\n      ".join(fs) + " },")
        else:
            kn = r.knobs()
            P("    { .name = \"%s\", .fam = %d, .n_knobs = %d, .knobs = { %s } }," % (r.name, fi, len(kn), ", ".join("{ %d, %d }" % (k.val, 0 if k.kind == "addr" else 1) for k in kn)))
    P("};")
    P("const unsigned derive_n_recs = %d;" % len(recs))
    P("const struct derive_ref derive_refs[%d] = {" % len(refs))
    for it, fi, woff, segs, jt_off, set_ref in refs:
        P("    { .name = \"%s\", .rec = %d, .n = %d, .nsegs = %d, .segs = { %s }, .woff = %d, .jt = %d, .set_off = { %d, %d }, .set_n = { %d, %d } }," % (
            it.name, rec_index[it.rec.name], img_n(it), len(segs), ", ".join("{ 0x%x, %d, %d, 0x%x }" % s for s in segs), woff, jt_off, set_ref[0][0], set_ref[1][0], set_ref[0][1], set_ref[1][1]))
    P("};")
    P("const unsigned derive_n_refs = %d;" % len(refs))
    return "\n".join(o) + "\n", len(words), len(shapes)


def img_n(it):
    return it.img.n


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        sys.exit(__doc__)
    text, nw, ns = generate(args[0])
    if "--check" in sys.argv:
        cur = open(args[1]).read() if os.path.exists(args[1]) else ""
        if cur != text:
            sys.exit("%s is out of date: regenerate with tools/derive_refs_gen.py" % args[1])
        print("%s up to date (%d words, %d shapes)" % (args[1], nw, ns))
        return
    open(args[1], "w").write(text)
    print("wrote %s: %d words, %d shapes, %d bytes" % (args[1], nw, ns, len(text)))


if __name__ == "__main__":
    main()
