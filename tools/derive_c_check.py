#!/usr/bin/env python3
"""Runs the Python derivation (tools/derive_build.py) and the C derivation (src/hle/derive.c, through tools/derive_c_check.c) on the same dumps and diffs everything the two compute:
the family, the verdict, the aligned and residual counts, the primary reference and the composite, and every field of the derived record (docs/ucode-derive.md, "The C port").

  derive_c_check.py DUMPS [--bin PATH] [--texts] [--loo] [--stems] [--time]
        --texts   every distinct text of the dumps against all the references (family auto)         [default]
        --loo     every registered build derived without its own record (the leave-one-out of derive_build.py validate)
        --stems   every (text, data) pair of the dumps (about 800; slow: the Python tool takes ~0.3 s each)
        --shortcut every (text, data) pair, C only: the production shortcut (the other families are skipped when F3DEX2 explains the text) gives the result of computing all of them
        --mutants Donkey Kong 64's text with the deliberate errors of derive_build.py mutate: C and Python must agree on each and each must change the result
        --bin     the compiled derive_c_check (default: build it into a temp dir with cc)
Exit status 1 when anything differs."""
import collections
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import derive_build as B  # noqa: E402
import derive_f1d as D1  # noqa: E402
import derive_f2 as F2  # noqa: E402


def build_tool(out_dir):
    exe = os.path.join(out_dir, "derive_c_check")
    src = [os.path.join(HERE, "derive_c_check.c"), os.path.join(HERE, "..", "src", "hle", "derive.c"), os.path.join(HERE, "..", "src", "hle", "derive_refs.c")]
    subprocess.check_call(["cc", "-O2", "-Wall", "-Wextra", "-o", exe] + src)
    return exe


def fmt(stem, fam, res, U):
    o = ["%s" % stem, "family=%s" % fam, "verdict=%s" % res.verdict, "aligned=%d/%d" % (len(res.prim.pu), U.img.n)]
    near = collections.Counter(getattr(res, "near", {}).values())
    xr = collections.Counter(getattr(res, "xr_res", {}).values())
    nxr = sum(xr.values())
    if fam in ("f3dex2", "f3dex1"):
        nx = len(res.x) + len(res.near) + nxr
    else:
        nx = len(res.x) + nxr
    o.append("nx=%d" % nx)
    o.append("x=%d" % len(res.x))
    o.append("near=%d,%d,%d,%d" % (near["init"], near["light-positional"], near["light-small"], near["geom-guarded"]))
    o.append("xr=%d,%d,%d" % (xr["init"], xr["extra"], xr["deleted"]))
    o.append("primary=%s" % res.primary)
    o.append("contrib=%s" % ",".join("%s:%d" % kv for kv in res.contrib.items()))
    trouble = fam in ("f3dex2", "f3dex1") and bool(res.prim.conflict or res.prim.ambig)
    o.append("trouble=%d" % trouble)
    if fam == "f3dex2":
        f = res.rec.fields()
        for k, v in f.items():
            if k == "ext":
                continue
            o.append("%s=%x" % (k, (v if v is not None else 0) & (0xffffffffffffffff if k == "load_key" else 0xffffffff)))
        o.append("ext=%s" % ",".join("%x:%x" % (p & 0xffffffff, kd) for p, kd in sorted(res.rec.ext)))
    elif fam == "f3dex1":
        for k, v in res.rec.fields().items():
            o.append("%s=%x" % (k, (v if v is not None else 0) & (0xffffffffffffffff if k == "load_key" else 0xffffffff)))
    return " ".join(o)


def py_derive(reg, uc, family="auto", exclude=()):
    for fam in reg.fam.values():
        for it in fam:
            it.img.active = None            # (derive_f2.derive now resets these itself)
    ex = set()
    fam, res, U = B.derive_any(reg, uc, family, set(exclude))
    return fam, res, U


def run_c(exe, paths, extra):
    out = subprocess.check_output([exe, "--full"] + extra + paths).decode()
    return out.strip().split("\n")


def compare(a, b, label):
    bad = 0
    ta, tb = a.split(" "), b.split(" ")
    if a == b:
        return 0
    da, db = dict(x.split("=", 1) if "=" in x else (x, "") for x in ta[1:]), dict(x.split("=", 1) if "=" in x else (x, "") for x in tb[1:])
    diffs = [k for k in sorted(set(da) | set(db)) if da.get(k) != db.get(k)]
    if not diffs and ta[0] == tb[0]:
        return 0                            # (the same fields in another order)
    shown = ", ".join("%s py=%s c=%s" % (k, da.get(k), db.get(k)) for k in diffs[:12])
    print("DIFF %s: %s%s" % (label, shown, " ... (%d fields)" % len(diffs) if len(diffs) > 12 else ""))
    return 1


def mutants(reg, exe):
    """derive_build.py's mutation check, on both implementations: the same edits of the same text, the two results equal, and different from the unmutated one"""
    import shutil
    import struct
    base = next(it for it in reg.fam["f3dex2"] if it.name == "l_b_dk64")
    refs = reg.refs("f3dex2", {"l_b_dk64"})
    w = base.uc.w

    def idx(pc):
        return base.img.idx_text(pc)
    cases = [("vertex pipeline opcode", {idx(0x5e0): w[idx(0x5e0)] ^ 3}, None),
             ("triangle setup element selector", {idx(0x94c): w[idx(0x94c)] ^ (4 << 21)}, None),
             ("G_TRI1 reject mask 0x7050", {idx(0x820): (w[idx(0x820)] & 0xffff0000) | 0x7050}, None),
             ("branch retargeted", {idx(0x1d4): (w[idx(0x1d4)] & ~0xffff) | ((w[idx(0x1d4)] + 2) & 0xffff)}, None),
             ("vertex loop instruction replaced by a nop", {idx(0x600): 0}, None),
             ("jump table entry of G_VTX", {}, (0x36e + 2, 0x1000 | 0x4f8))]
    import derive_lib as L
    tmp = tempfile.mkdtemp(prefix="derive_mut_")
    res0 = F2.derive(F2.Item(base.uc, None, "unknown"), refs)
    ref0 = fmt("m", "f3dex2", res0, F2.Item(base.uc, None, "unknown"))
    bad = 0
    for k, (desc, ed, dedit) in enumerate(cases):
        m = B.mutate_text(base.uc, ed)
        data = bytearray(base.uc.data)
        if dedit:
            data[dedit[0]:dedit[0] + 2] = struct.pack(">H", dedit[1])
        p = os.path.join(tmp, "m%d.text" % k)
        open(p, "wb").write(struct.pack(">1024I", *m.w))
        open(p[:-5] + ".data", "wb").write(bytes(data))
        shutil.copy(base.uc.path[:-5] + ".ext", p[:-5] + ".ext")
        import copy
        mu = copy.copy(m)
        mu.data = bytes(data)
        U = F2.Item(mu, None, "unknown")
        res = F2.derive(U, refs)
        pl = fmt("m%d" % k, "f3dex2", res, U)
        cl = run_c(exe, [p], ["--family", "f3dex2", "--exclude", "l_b_dk64"])[0]
        changed = pl.split(" ", 1)[1] != ref0.split(" ", 1)[1]
        d = compare(pl, cl, desc)
        bad += d
        print("  %-45s %s, %s" % (desc, "C = Python" if not d else "C != PYTHON", "result changed" if changed else "NOT CAUGHT"))
        bad += 0 if changed else 1
    # the F3DEX 1.x mutants of derive_build.py mutate (F3DEX 1.21's text and data against the other F3DEX 1.x builds)
    base1 = next(it for it in reg.fam["f3dex1"] if it.rec.name == "l_b_121")
    refs1 = reg.refs("f3dex1", {"l_b_121"})
    U0 = D1.ItemV(base1.uc, None, "unknown")
    ref1 = fmt("m", "f3dex1", D1.derive_f1(U0, refs1), U0)
    muts = B.f1_mutants(reg)
    for k, (desc, m, check) in enumerate(muts):
        p = os.path.join(tmp, "f%d.text" % k)
        open(p, "wb").write(struct.pack(">1024I", *m.w))
        open(p[:-5] + ".data", "wb").write(bytes(m.data))
        ext = base1.uc.path[:-5] + ".ext"
        if os.path.exists(ext):
            shutil.copy(ext, p[:-5] + ".ext")
        U = D1.ItemV(m, None, "unknown")
        res = D1.derive_f1(U, refs1)
        pl = fmt("f%d" % k, "f3dex1", res, U)
        cl = run_c(exe, [p], ["--family", "f3dex1", "--exclude", "l_b_121"])[0]
        changed = pl.split(" ", 1)[1] != ref1.split(" ", 1)[1]
        d = compare(pl, cl, desc)
        bad += d
        print("  %-45s %s, %s" % (desc[:45], "C = Python" if not d else "C != PYTHON", "result changed" if changed else "NOT CAUGHT"))
        bad += 0 if changed else 1
    return len(cases) + len(muts), bad


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = [a for a in sys.argv[1:] if a.startswith("--")]
    if len(args) < 1:
        sys.exit(__doc__)
    exe = None
    for i, a in enumerate(sys.argv):
        if a == "--bin":
            exe = sys.argv[i + 1]
            args = [x for x in args if x != exe]
    tmp = None
    if exe is None:
        tmp = tempfile.mkdtemp(prefix="derive_c_check_")
        exe = build_tool(tmp)
    reg = B.Registry(args[0])
    mode = "shortcut" if "--shortcut" in flags else "loo" if "--loo" in flags else "stems" if "--stems" in flags else "mutants" if "--mutants" in flags else "texts"
    bad = 0
    n = 0
    if mode in ("texts", "stems"):
        ucs = reg.C.items if mode == "stems" else reg.C.distinct_texts()
        ucs = [u for u in ucs if not u.loaded]
        paths = [u.path for u in ucs]
        clines = run_c(exe, paths, [])
        by_stem = {l.split(" ")[0]: l for l in clines}
        for u in ucs:
            fam, res, U = py_derive(reg, u)
            pl = fmt(u.stem, fam, res, U)
            cl = by_stem.get(u.stem, "MISSING")
            bad += compare(pl, cl, u.stem[:20])
            n += 1
    elif mode == "shortcut":
        ucs = [u for u in reg.C.items if not u.loaded]
        paths = [u.path for u in ucs]
        full = run_c(exe, paths, [])
        fast = subprocess.check_output([exe] + paths).decode().strip().split("\n")
        for a, b in zip(full, fast):
            n += 1
            if a != b:
                bad += 1
                print("DIFF shortcut %s" % a.split(" ")[0][:20])
        if len(full) != len(fast):
            bad += 1
    elif mode == "mutants":
        n, bad = mutants(reg, exe)
    else:
        for fam in B.FAMILIES:
            for it in reg.fam[fam]:
                name = it.rec.name
                refs = reg.refs(fam, {name})
                if fam == "f3dex2":
                    res = F2.derive(F2.Item(it.uc, None, "unknown"), refs)
                    U = F2.Item(it.uc, None, "unknown")
                    res = F2.derive(U, refs)
                elif fam == "f3dex1":
                    U = D1.ItemV(it.uc, None, "unknown")
                    res = D1.derive_f1(U, refs)
                else:
                    U = D1.ItemV(it.uc, None, "unknown")
                    res = D1.derive_variant(U, refs)
                pl = fmt(it.uc.stem, fam, res, U)
                cl = run_c(exe, [it.uc.path], ["--family", fam, "--exclude", name])[0]
                bad += compare(pl, cl, "%s (%s)" % (it.name, fam))
                n += 1
    print("%d derivations compared, %d differ (%s)" % (n, bad, mode))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
