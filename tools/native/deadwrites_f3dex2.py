#!/usr/bin/env python3
"""deadwrites_f3dex2.py: tools/native/deadwrites.py over every build of a reassembled F3DEX2 / F3DZEX family (docs/tier2-deadwrites.md).

  deadwrites_f3dex2.py BUILD_DIR [--bodies VU_INLINE.s] [--json OUT]

BUILD_DIR is the output of the public disassembly https://github.com/Mr-Wiseguy/f3dex2 (`make ok ARMIPS=... BUILD_DIR=...`): one directory per build
with <name>.code, <name>.data and <name>.sym (the assembler's symbol file; `make ok` checks the code and data against the retail MD5s). Nothing is
read from a ROM and nothing is written but the report.

Per build two steady-state IMEM images are made in memory: the 0xF80 bytes of main code at IMEM 0x080, overlay 1 (the command handlers loaded after
the start-up code) at its IMEM address, and overlay 3 (clipping) or overlay 2 (lighting) in the shared window; the overlay table is read from the
data segment. The ranges come from the symbols: `vtx` G_VTX_handler .. G_TRI2_handler, `tri` .. G_CULLDL_handler, `clip` / `light` the overlay window
in the image that holds that overlay, `ovl1` overlay 1. Both overlay windows are passed as --window, so a jump into them from outside is an exit.

--bodies takes the fork's VU op bodies compiled to assembly (ext/parallel-rsp/rsp/vu_inline.cpp with the flags of its CMakeLists.txt, `-S`) and adds
a first-order instruction estimate: per basic block, the instructions of the VU ops as the JIT emits them now (load vs and vt, the element shuffle,
the body, a BL / RET pair for a body over PRSP_INLINE_WORDS = 8, store vd; the body's own accumulator and flag loads / stores) against the same block with
VU state in host registers (the body's computation and shuffles, plus one load per item read before written and one store per item written).
VMOV, VSAR and VRCP* (inline JIT sequences, not bodies) are left out on both sides. Dead outputs are *not* removed in that estimate.
"""
import argparse
import collections
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deadwrites as dw  # noqa: E402

BODY = {0x00: "VMULF", 0x01: "VMULU", 0x04: "VMUDL", 0x05: "VMUDM", 0x06: "VMUDN", 0x07: "VMUDH", 0x08: "VMACF", 0x09: "VMACU", 0x0C: "VMADL",
        0x0D: "VMADM", 0x0E: "VMADN", 0x0F: "VMADH", 0x10: "VADD", 0x11: "VSUB", 0x13: "VABS", 0x14: "VADDC", 0x15: "VSUBC", 0x20: "VLT", 0x21: "VEQ",
        0x22: "VNE", 0x23: "VGE", 0x24: "VCL", 0x25: "VCH", 0x26: "VCR", 0x27: "VMRG", 0x28: "VAND", 0x29: "VNAND", 0x2A: "VOR", 0x2B: "VNOR",
        0x2C: "VXOR", 0x2D: "VNXOR"}
INLINE_WORDS = 8


def read_build(d, name):
    code = open(os.path.join(d, name, name + ".code"), "rb").read()
    data = open(os.path.join(d, name, name + ".data"), "rb").read()
    sym = {}
    for l in open(os.path.join(d, name, name + ".sym")):
        p = l.split()
        if len(p) >= 2 and not p[1].startswith("."):
            sym.setdefault(p[1], int(p[0], 16))
    return code, data, sym


def images(code, data, sym):
    def ovl(k):
        off = sym["overlayInfo%d" % k]
        load, ln, imem = struct.unpack(">IHH", data[off:off + 8])
        return load, ln + 1, imem & 0xFFF
    base = bytearray(0x1000)
    base[0x80:0x1000] = code[:0xF80]
    l1, n1, a1 = ovl(1)
    base[a1:a1 + n1] = code[l1:l1 + n1]
    out = {}
    for k, tag in ((3, "clip"), (2, "light")):
        im = bytearray(base)
        l, n, a = ovl(k)
        im[a:a + n] = code[l:l + n]
        out[tag] = dw.Image(struct.unpack(">1024I", bytes(im)))
    o2, o3 = ovl(2), ovl(3)
    g = lambda n: sym[n] & 0xFFF
    win23 = (o3[2], o3[2] + max(o2[1], o3[1]))
    ranges = {"vtx": (g("G_VTX_handler"), g("G_TRI2_handler")), "tri": (g("G_TRI2_handler"), g("G_CULLDL_handler")), "ovl23": win23,
              "ovl1": (0, g("ovl01_end"))}
    return out, ranges, [(0, g("ovl01_end")), win23]


def read_bodies(path):
    """RSP_VUS_<op>: (instructions without the ret, loads, stores) from the compiler's assembly"""
    out, name = {}, None
    for l in open(path):
        m = re.match(r"^RSP_VUS_(\w+):", l)
        if m:
            name, c, ld, st = m.group(1), 0, 0, 0
            continue
        if name and l.startswith("\t.size"):
            out[name] = (c - 1, ld, st)
            name = None
        elif name and re.match(r"^\t[a-z]", l):
            c += 1
            op = l.split()[0]
            ld += op.startswith(("ldr", "ldp", "ldur", "ld1"))
            st += op.startswith(("str", "stp", "stur", "st1"))
    return out


def shuffle_cost(e):
    return 0 if e < 2 else (1 if e >= 8 else 2)            # none; dup for a single element; trn1 / trn2 for the quarter and half forms


def instr_estimate(img, lo, hi, bodies):
    _, _, leaders = dw.graph(img, "global")
    now = alloc = skipped = 0
    for blk in dw.blocks(img, leaders):
        if not (lo <= blk[0] * 4 < hi):
            continue
        written, upward = set(), set()
        prev_vd, bnow, bcomp = None, 0, 0
        for i in blk:
            d = img.dec[i]
            vuu, vud = d.uses & dw.VU_STATE, d.defs & dw.VU_STATE
            if d.vu not in (None, "vload", "vstore", "nop"):
                f = d.w & 63
                if BODY.get(f) not in bodies:
                    skipped += 1
                    prev_vd = None
                else:
                    c, ld, st = bodies[BODY[f]]
                    e, vs, vt, vd = (d.w >> 21) & 15, (d.w >> 11) & 31, (d.w >> 16) & 31, (d.w >> 6) & 31
                    loads = 2 - (1 if prev_vd is not None and prev_vd in (vs, vt) else 0)
                    bnow += loads + 1 + shuffle_cost(e) + c + (0 if c <= INLINE_WORDS else 2)
                    bcomp += shuffle_cost(e) + c - ld - st
                    prev_vd = vd
            elif d.vu in ("vload", "vstore"):
                prev_vd = None
            for it in (vuu - vud if d.vu == "vload" else vuu):
                if it not in written:
                    upward.add(it)
            written |= vud
        if bnow:
            now += bnow
            alloc += bcomp + len(upward) + len(written)
    return now, alloc, skipped


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("build_dir")
    ap.add_argument("--bodies")
    ap.add_argument("--json")
    a = ap.parse_args()
    bodies = read_bodies(a.bodies) if a.bodies else None
    P = lambda x, y: "%5.1f" % (100.0 * x / y) if y else "    -"
    out = {}
    tot = collections.defaultdict(collections.Counter)
    print("%-20s %-5s %5s %6s %6s | %6s %6s %6s %6s %6s | %6s %6s | %5s %4s %5s%s" % (
        "build", "range", "VUops", "dO bb", "dO gl", "vd", "acc", "flags", "clamp", "acc-o", "trafBB", "trafLv", "run", "pmax", "p<=20",
        " | instr now  alloc  fewer" if bodies else ""))
    for name in sorted(os.listdir(a.build_dir)):
        if not os.path.exists(os.path.join(a.build_dir, name, name + ".sym")):
            continue
        imgs, rng, wins = images(*read_build(a.build_dir, name))
        res = {}
        for tag, img in imgs.items():
            r = dw.analyse(img, None, [(k,) + v for k, v in rng.items()], wins)
            if tag == "clip":
                res.update(vtx=r["vtx"], tri=r["tri"], ovl1=r["ovl1"], clip=r["ovl23"])
            else:
                res["light"] = r["ovl23"]
        out[name] = res
        for rg in ("vtx", "tri", "clip", "light", "ovl1"):
            s = res[rg]
            if not s.get("vu_ops"):
                continue
            mm = s["families"].get("mac", {}).get("vu_ops", 0) + s["families"].get("mul", {}).get("vu_ops", 0)
            now = s["now_ld"] + s["now_st"]
            ins = ""
            if bodies:
                img = imgs["light" if rg == "light" else "clip"]
                lo, hi = rng["ovl23" if rg in ("clip", "light") else rg]
                n, al, _ = instr_estimate(img, lo, hi, bodies)
                s["instr_now"], s["instr_alloc"] = n, al
                ins = " | %9d %6d %5s%%" % (n, al, P(n - al, n))
            print("%-20s %-5s %5d %6s %6s | %6s %6s %6s %6s %6s | %6s %6s | %5.1f %4d %5s%s" % (
                name, rg, s["vu_ops"], P(s.get("bb:dead", 0), s.get("bb:outputs", 0)), P(s.get("global:dead", 0), s.get("global:outputs", 0)),
                P(s.get("global:vd_dead", 0), s.get("global:vd", 0)), P(s.get("global:acc_dead", 0), s.get("global:acc", 0)),
                P(s.get("global:flags_dead", 0), s.get("global:flags", 0)), P(s.get("global:clamp_only_dead", 0), mm), P(s.get("global:acc_all_dead", 0), mm),
                P(s["alloc_ld"] + s["alloc_st_bb"], now), P(s["alloc_ld"] + s["alloc_st_live"], now), s["vu_run_mean"], s["pressure_max"],
                P(s["blocks_pressure_le20"], 1), ins))
            for k, v in s.items():
                if isinstance(v, (int, float)):
                    tot[rg][k] += v
            tot[rg]["mm"] += mm
    print()
    for rg, s in tot.items():
        now = s["now_ld"] + s["now_st"]
        print("all builds, %-5s: %5d VU ops; dead outputs bb %s%% global %s%% (vd %s, acc %s, flags %s); of the mul / mac ops clamp-only dead %s%%, "
              "accumulator-only dead %s%%; VU state traffic allocated %s%% (live stores %s%%) of now%s" % (
                  rg, s["vu_ops"], P(s["bb:dead"], s["bb:outputs"]), P(s["global:dead"], s["global:outputs"]), P(s["global:vd_dead"], s["global:vd"]),
                  P(s["global:acc_dead"], s["global:acc"]), P(s["global:flags_dead"], s["global:flags"]), P(s["global:clamp_only_dead"], s["mm"]),
                  P(s["global:acc_all_dead"], s["mm"]), P(s["alloc_ld"] + s["alloc_st_bb"], now), P(s["alloc_ld"] + s["alloc_st_live"], now),
                  "; VU instructions %s%% fewer" % P(s["instr_now"] - s["instr_alloc"], s["instr_now"]) if bodies else ""))
    if a.json:
        json.dump(out, open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
