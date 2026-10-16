#!/usr/bin/env python3
"""ucode_survey.py: groups the graphics ucodes dumped by tools/ucode_dump_shim.c (tools/ucode_dump_run.sh) by exact hash and by normalised code.

  ucode_survey.py RUNSDIR [--json out.json] [--handlers]

RUNSDIR/<romset>/<game>/dump/{seq.txt,<texthash>.<datahash>.{text,data,imem}}. Prints the tables of docs/ucode-survey.md. Nothing ROM-derived is written
except what --json is told to (hashes, counts, game names).

Normalisation levels of the ucode text (4 KiB, RSP instruction words):
  L0  exact bytes (what the FNV-1a-64 ucode id keys on)
  S   L0 with printable-ASCII runs (version banners) masked
  L1  S + J / JAL targets and branch offsets masked
  L2  L1 + offsets of scalar loads / stores and of LWC2 / SWC2 vector loads / stores masked
  L3  L2 + ALU immediates (ADDI, ADDIU, ORI, ANDI, XORI, SLTI, SLTIU, LUI) masked
  L4  skeleton: only the major opcode (and funct for SPECIAL, rt for REGIMM, the COP2 op) kept
"""
import argparse
import collections
import difflib
import hashlib
import json
import os
import re
import struct
import sys

TEXT_BASE = 0             # handler / jump addresses are IMEM addresses; imem_image() puts the text where the boot code and the microcode's own init leave it
LEVELS = ["L0", "S", "L1", "L2", "L3", "L4"]


def be_words(b):
    b = b.ljust(0x1000, b"\0") if len(b) < 0x1000 and len(b) > 0xe00 else b
    return list(struct.unpack(">%dI" % (len(b) // 4), b[: len(b) // 4 * 4]))


def imem_image(words, ident=False):
    """the 1024 IMEM words a task starts with: the boot code loads text[0:0xf80] to IMEM 0x80, the microcode's init loads text[0xf80:0x1000] to IMEM 0"""
    return words[0x3e0:0x400] + words[:0x3e0] if len(words) == 0x400 and not ident else words[:]


def ascii_word_mask(text):
    """indices of words that lie in a printable-ASCII run of >= 8 bytes"""
    m = set()
    for r in re.finditer(rb"[ -~]{8,}", text):
        for i in range(r.start() // 4, (r.end() + 3) // 4):
            m.add(i)
    return m


def norm_word(w, level):
    op = w >> 26
    if level >= 1:
        if op in (2, 3):
            w &= ~0x3ffffff
        elif op in (4, 5, 6, 7) or (op == 1):
            w &= ~0xffff
    if level >= 2:
        if op in (32, 33, 35, 36, 37, 39, 40, 41, 43):
            w &= ~0xffff
        elif op in (50, 58):
            w &= ~0x7f
    if level >= 3:
        if op in (8, 9, 10, 11, 12, 13, 14, 15):
            w &= ~0xffff
    if level >= 4:
        if op == 0:
            w &= 0xfc00003f
        elif op == 1:
            w &= 0xfc1f0000
        elif op == 18:
            w &= 0xfe00003f if (w >> 25) & 1 else 0xffe00000   # COP2 vector op (bit 25) keeps funct, moves keep the sub-op
        elif op in (50, 58):
            w &= 0xfc00f800
        elif op == 16:
            w &= 0xffe00000
        else:
            w &= 0xfc000000
    return w


LEVEL_NUM = {"L0": None, "S": 0, "L1": 1, "L2": 2, "L3": 3, "L4": 4}


def norm_words(words, strmask, level):
    """level None: the raw words; 0: only the printable-ASCII words masked; 1..4 as in the header"""
    if level is None:
        return tuple(words)
    return tuple(0xdeadbeef if i in strmask else norm_word(w, level) for i, w in enumerate(words))


def h8(x):
    return hashlib.blake2b(repr(x).encode(), digest_size=6).hexdigest()


def family_of(name):
    if "S2DEX" in name:
        return "S2DEX"
    g = re.search(r"(fifo|xbus) +([0-9][0-9.]*[A-Za-z]*)", name)
    if g and re.search(r"F3D|L3D", name):
        v = g.group(2)
        return "F3DEX2" if v[0] == "2" else "F3DEX"
    if re.search(r"F3DEX2|F3DZEX|F3DLX2", name):
        return "F3DEX2"
    if re.search(r"F3DEX|F3DLX|F3DLP", name):
        return "F3DEX"
    if re.search(r"F3D|SW Version", name):
        return "F3D"
    if "S2DEX" in name:
        return "S2DEX"
    if "Turbo" in name or "T3D" in name:
        return "Turbo3D"
    return "other"


def banner(text, data):
    out = []
    for blob in (text, data):
        for r in re.finditer(rb"[ -~]{10,}", blob):
            s = r.group().decode()
            if re.search(r"RSP|ucode|F3D|Gfx|Version|SW|Nintendo|fifo", s):
                out.append(re.sub(r" +", " ", s.strip()))
    return " | ".join(out)


def short_name(b):
    """the build label: ucode name + version of the first banner, e.g. 'F3DEX2 fifo 2.07'"""
    m = re.search(r"RSP Gfx(?:\[\w+\])? ucode (\S+) +(fifo|xbus)? *([0-9][0-9.]*[A-Za-z]*)", b)
    if m:
        return "%s %s %s" % (m.group(1), m.group(2) or "", m.group(3))
    m = re.search(r"SW Version: *(\S+)", b)
    if m:
        return "F3D SW %s" % m.group(1)
    m = re.search(r"RSP (\S+)", b)
    return m.group(0) if m else (b[:30] or "(no banner)")


def load(runs):
    uc = {}
    games = collections.OrderedDict()
    for rs in sorted(os.listdir(runs)):
        for g in sorted(os.listdir(os.path.join(runs, rs))):
            d = os.path.join(runs, rs, g, "dump")
            sq = os.path.join(d, "seq.txt")
            if not os.path.exists(sq):
                continue
            seq = []
            for ln in open(sq):
                f = ln.split()
                if len(f) >= 5:
                    seq.append((f[1], int(f[2], 16), int(f[3], 16), int(f[4], 16)))
            games[(rs, g)] = seq
            for fn in os.listdir(d):
                if fn.endswith(".text"):
                    key = fn[:-5]
                    if key not in uc:
                        t = open(os.path.join(d, fn), "rb").read()
                        da = open(os.path.join(d, key + ".data"), "rb").read()
                        im = open(os.path.join(d, key + ".imem"), "rb").read()
                        uc[key] = dict(key=key, th=key.split(".")[0], dh=key.split(".")[1], text=t, data=da, imem=im, games=set(), tasks=collections.Counter())
                    uc[key]["games"].add((rs, g))
            for k, us, ds, fl in seq:
                if k in uc:
                    uc[k]["tasks"][(rs, g)] += 1
    return uc, games


def runs_of(seq):
    out = []
    for k, *_ in seq:
        if not out or out[-1][0] != k:
            out.append([k, 0])
        out[-1][1] += 1
    return out


def reach_tables(u):
    """halfword tables in the data segment that hold IMEM code addresses: runs of >= 12 entries in [0x1080, 0x2000), multiples of 4"""
    d = u["data"]
    h = struct.unpack(">%dH" % (len(d) // 2), d[: len(d) // 2 * 2])
    ok = [0x1000 <= v < 0x2000 and v % 4 == 0 for v in h]
    tabs, i = [], 0
    while i < len(h):
        if ok[i]:
            j = i
            while j < len(h) and ok[j]:
                j += 1
            if j - i >= 12:
                tabs.append((i * 2, [v & 0xfff for v in h[i:j]]))
            i = j
        else:
            i += 1
    return tabs


def decode_flow(words, pc):
    """(successors, ends) of the instruction at IMEM address pc (delay slot handled by the caller); returns kind, targets"""
    idx = (pc - TEXT_BASE) // 4
    if not 0 <= idx < len(words):
        return "out", []
    w = words[idx]
    op = w >> 26
    if op == 2:
        return "j", [((w & 0x3ffffff) << 2) & 0xfff]
    if op == 3:
        return "jal", [((w & 0x3ffffff) << 2) & 0xfff]
    if op in (4, 5, 6, 7) or (op == 1 and ((w >> 16) & 0x1f) in (0, 1, 16, 17)):
        off = w & 0xffff
        if off & 0x8000:
            off -= 0x10000
        return ("bal" if op == 1 and ((w >> 16) & 0x1f) >= 16 else "br"), [(pc + 4 + off * 4) & 0xfff]
    if op == 0 and (w & 0x3f) == 8:
        return "jr", []
    if op == 0 and (w & 0x3f) == 9:
        return "jalr", []
    if op == 0 and (w & 0x3f) == 13:
        return "break", []
    return "seq", []



F3DEX2_OPS = {0x00: "NOOP", 0x01: "VTX", 0x02: "MODIFYVTX", 0x03: "CULLDL", 0x04: "BRANCH_Z", 0x05: "TRI1", 0x06: "TRI2", 0x07: "QUAD", 0x08: "LINE3D",
              0xd5: "SPECIAL_1", 0xd7: "TEXTURE", 0xd8: "POPMTX", 0xd9: "GEOMETRYMODE", 0xda: "MTX", 0xdb: "MOVEWORD", 0xdc: "MOVEMEM", 0xde: "DL",
              0xdf: "ENDDL", 0xe0: "SPNOOP", 0xe1: "RDPHALF_1", 0xe2: "SETOTHERMODE_L", 0xe3: "SETOTHERMODE_H", 0xf1: "RDPHALF_2", 0xe4: "TEXRECT",
              0xed: "SETSCISSOR", 0xfd: "SETTIMG"}


def extent(words, root, stops):
    """addresses reachable from root (delay slots included) following fall-through, branches and j, not the callee of a jal / jalr; stops at the addresses in stops.
    Returns (addresses, jal targets met, stop addresses hit)."""
    seen, calls, hit, stack = set(), set(), set(), [root]
    while stack:
        pc = stack.pop()
        while 0 <= pc < 0x1000 and pc not in seen:
            if pc != root and pc in stops:
                hit.add(pc)
                break
            seen.add(pc)
            kind, tg = decode_flow(words, pc)
            if kind == "seq":
                pc += 4
                continue
            seen.add(pc + 4)
            if kind == "j":
                pc = tg[0]
                continue
            if kind in ("br", "bal"):
                stack.append(tg[0])
                pc += 8
                continue
            if kind == "jal":
                calls.add(tg[0])
                pc += 8
                continue
            if kind == "jalr":
                pc += 8
                continue
            break
    return seen, calls, hit


def dispatch_roots(u, fam, words):
    """handler entry pcs from the jump tables of the data segment; F3DEX2: the 256-entry opcode table at 0x26e..0x36e only (opcode -> pc)"""
    d = u["data"]
    if fam == "F3DEX2" and len(d) >= 0x370:
        op2pc = {}
        for op in range(256):
            sop = op - 256 if op >= 128 else op
            v = struct.unpack(">H", d[0x36e + 2 * sop: 0x370 + 2 * sop])[0]
            if 0x1000 <= v < 0x2000:
                op2pc[op] = v & 0xfff
        return set(op2pc.values()), op2pc
    ents = set()
    for _, tb in reach_tables(u):
        ents.update(tb)
    return ents, {}


def closure(words, root):
    """addresses of the handler at root: its extent plus, transitively, the extents of the jal targets and tail-jump targets that leave it"""
    seen, todo, out = set(), [root], set()
    while todo:
        r = todo.pop()
        if r in seen:
            continue
        seen.add(r)
        s_, calls, _ = extent(words, r, set())
        out |= s_
        todo.extend(calls)
    return out


def word_index(addr):
    """IMEM address -> index in the text words (inverse of imem_image)"""
    return addr // 4 + 0x3e0 - 0 if addr < 0x80 else addr // 4 - 32


def match_set(x, y):
    """indices of x that lie in a matching block of >= 4 words of an alignment with y (position-independent apart from the order of blocks)"""
    sm = difflib.SequenceMatcher(None, x, y, autojunk=False)
    m = set()
    for b in sm.get_matching_blocks():
        if b.size >= 4:
            m.update(range(b.a, b.a + b.size))
    return m


def selftest():
    """mutation check of the normaliser: a change of a masked field must vanish exactly from the levels that mask it, a change of an opcode bit never"""
    w = [0x08000040, 0x8c0b00f0, 0x20010030, 0x10200003, 0x4a10802a, 0xe8030801]    # j, lw, addi, beq, a vector op, swc2
    base = [h8(norm_words(w, set(), LEVEL_NUM[l])) for l in LEVELS]
    cases = [(0, 0x155, ["L1", "L2", "L3", "L4"]), (1, 0x14, ["L2", "L3", "L4"]), (2, 0x30, ["L3", "L4"]), (3, 0x3, ["L1", "L2", "L3", "L4"]), (5, 0x40, ["L2", "L3", "L4"])]
    ok = True
    for i, x, expect in cases:
        w2 = list(w)
        w2[i] ^= x
        got = [l for k, l in enumerate(LEVELS) if h8(norm_words(w2, set(), LEVEL_NUM[l])) == base[k]]
        ok &= got == expect
        w3 = list(w)
        w3[i] ^= 1 << 27
        ok &= all(h8(norm_words(w3, set(), LEVEL_NUM[l])) != base[k] for k, l in enumerate(LEVELS))
        print("  word %d ^ 0x%x: unchanged at %s (expected %s); opcode change always detected" % (i, x, got, expect))
    print("selftest %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="?")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--json")
    ap.add_argument("--handlers", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        sys.exit(selftest())
    uc, games = load(a.runs)
    print("games run: %d (%s)" % (len(games), ", ".join("%s %d" % (k, sum(1 for g in games if g[0] == k)) for k in sorted({g[0] for g in games}))))
    print("games with no graphics task: %s" % ", ".join(g[1] for g, s in games.items() if not s) or "none")
    tot_tasks = sum(len(s) for s in games.values())
    print("graphics tasks seen: %d" % tot_tasks)

    # labels
    for u in uc.values():
        b = banner(u["text"], u["data"])
        u["banner"] = b
        u["name"] = short_name(b)
        first = next((x for x in b.split(" | ") if re.search(r"ucode|SW Version", x)), b.split(" | ")[0])
        u["fam"] = family_of(first)
        tw = be_words(u["text"])
        u["words"] = tw
        sm = ascii_word_mask(u["text"])
        u["strm"] = sm
        for lv in LEVELS:
            u[lv] = h8(norm_words(tw, sm, LEVEL_NUM[lv]))
        # imem vs text: words of IMEM[0x80..] that equal the text
        iw = be_words(u["imem"])
        rot = imem_image(tw)
        eq_rot = sum(1 for i in range(1024) if iw[i] == rot[i])
        eq_id = sum(1 for i in range(1024) if iw[i] == tw[i])
        u["ident"] = eq_id > eq_rot          # text loaded at IMEM 0 (no boot rotation): a custom ucode
        u["imem_eq"] = max(eq_rot, eq_id) / 1024.0
        u["full"] = h8(tuple(tw))
        u["dh_only"] = None

    texts = {}
    for u in uc.values():
        texts.setdefault(u["th"], []).append(u)
    print("\n== exact groups")
    print("distinct (text, data) pairs: %d; distinct texts (the ucode id): %d" % (len(uc), len(texts)))
    multi = [(th, us) for th, us in texts.items() if len(us) > 1]
    for th, us in multi:
        print("  text %s has %d data segments (%s)" % (th, len(us), ", ".join(sorted({g[1] for u in us for g in u["games"]}))[:90]))

    # coverage numbers per level, over distinct texts
    byfull = {}
    for u in uc.values():
        byfull.setdefault(u["full"], []).append(u)
    print("distinct 4 KiB texts: %d (the ucode id also depends on ucode_size: %d ids)" % (len(byfull), len(texts)))
    reps = {th: us[0] for th, us in texts.items()}
    for th, u in list(reps.items()):
        pass
    fams = collections.defaultdict(list)
    for th, u in reps.items():
        fams[u["fam"]].append(u)
    print("\n== distinct classes per level (distinct texts: %d)" % len(reps))
    print("%-8s %6s " % ("family", "texts") + " ".join("%5s" % lv for lv in LEVELS))
    for f in ["F3D", "F3DEX", "F3DEX2", "S2DEX", "Turbo3D", "other"]:
        us = fams.get(f, [])
        if us:
            print("%-8s %6d " % (f, len(us)) + " ".join("%5d" % len({u[lv] for u in us}) for lv in LEVELS))
    print("%-8s %6d " % ("all", len(reps)) + " ".join("%5d" % len({u[lv] for u in reps.values()}) for lv in LEVELS))

    # task coverage: fraction of the tasks whose ucode shares a class with at least one other text, by level
    def tasks_of(u):
        return sum(u["tasks"].values())
    print("\n== games per class (a class = same normalised text), tasks in %% of all graphics tasks")
    classes = {}
    for lv in LEVELS:
        cl = collections.defaultdict(list)
        for u in reps.values():
            cl[u[lv]].append(u)
        classes[lv] = cl
    for lv in ("L0", "L1", "L2", "L3"):
        multi_t = sum(sum(tasks_of(x) for x in us) for us in classes[lv].values() if len(us) > 1)
        print("  %s: %d classes, %d with >1 distinct text, tasks in merged classes %.1f%%" % (lv, len(classes[lv]), sum(1 for us in classes[lv].values() if len(us) > 1), 100.0 * multi_t / max(1, tot_tasks)))

    print("\n== per ucode (distinct text), by family")
    print("%-16s %-8s %-28s %-4s %5s %4s %s" % ("text id", "family", "build", "imem", "games", "S", "classes L1/L2/L3 | games"))
    rows = sorted(reps.values(), key=lambda u: (u["fam"], u["name"], u["th"]))
    for u in rows:
        gs = sorted({g[1] for x in texts[u["th"]] for g in x["games"]})
        print("%s %-8s %-28s %3.0f%% %5d  %s %s %s | %s" % (u["th"], u["fam"], u["name"][:28], 100 * u["imem_eq"], len(gs), u["S"][:4], u["L1"][:4], u["L2"][:4] + "/" + u["L3"][:4], ", ".join(gs)[:110]))

    # class detail per level for the F3D-ish families
    print("\n== classes with more than one distinct text")
    for lv in ("S", "L1", "L2", "L3"):
        print(" -- %s" % lv)
        for h, us in sorted(classes[lv].items(), key=lambda kv: -len(kv[1])):
            if len(us) > 1:
                print("   %s %d texts: %s" % (h[:6], len(us), "; ".join("%s(%s)" % (u["name"][:22], "/".join(sorted({g[1][:14] for x in texts[u["th"]] for g in x["games"]}))[:40]) for u in us)[:300]))

    # pairwise alignment coverage of all distinct 4 KiB texts at L1 / L2 / L3
    ufull = {}
    for u in rows:
        ufull.setdefault(u["full"], u)
    urows = list(ufull.values())
    nz = {u["full"]: {i for i, w in enumerate(u["words"]) if w != 0 and i not in u["strm"]} for u in urows}
    norm = {(u["full"], lv): norm_words(u["words"], u["strm"], lv) for u in urows for lv in (1, 2, 3)}
    mset = {}
    cov = {}
    for i in range(len(urows)):
        for j in range(i + 1, len(urows)):
            x, y = urows[i], urows[j]
            for lv in (1, 2, 3):
                sm = difflib.SequenceMatcher(None, norm[(x["full"], lv)], norm[(y["full"], lv)], autojunk=False)
                ma, mb = set(), set()
                for b_ in sm.get_matching_blocks():
                    if b_.size >= 4:
                        ma.update(range(b_.a, b_.a + b_.size))
                        mb.update(range(b_.b, b_.b + b_.size))
                mset[(x["full"], y["full"], lv)] = ma
                mset[(y["full"], x["full"], lv)] = mb
                cov[(x["full"], y["full"], lv)] = len(ma & nz[x["full"]]) / max(1, len(nz[x["full"]]))
                cov[(y["full"], x["full"], lv)] = len(mb & nz[y["full"]]) / max(1, len(nz[y["full"]]))

    def pc(x, y, lv):
        return min(cov[(x["full"], y["full"], lv)], cov[(y["full"], x["full"], lv)])

    print("\n== clusters: texts joined when each covers >= T of the other (aligned blocks of >= 4 equal normalised words)")
    for T in (0.995, 0.98, 0.95):
        print(" -- T = %.3f" % T)
        row = ["   %-8s %6s" % ("level", "texts")]
        res = {}
        for lv in (1, 2, 3):
            par = {u["full"]: u["full"] for u in urows}

            def find(a_):
                while par[a_] != a_:
                    par[a_] = par[par[a_]]
                    a_ = par[a_]
                return a_
            for i in range(len(urows)):
                for j in range(i + 1, len(urows)):
                    if pc(urows[i], urows[j], lv) >= T:
                        par[find(urows[i]["full"])] = find(urows[j]["full"])
            cl = collections.defaultdict(list)
            for u in urows:
                cl[find(u["full"])].append(u)
            res[lv] = cl
        print("   texts %d; clusters L1 %d, L2 %d, L3 %d (a cluster = builds one parameterised routine set could serve)" % (len(urows), len(res[1]), len(res[2]), len(res[3])))
        if T == 0.98:
            clusters98 = [cl for cl in res[3].values()]
            for lv in (3,):
                print("   L3 clusters with > 1 text:")
                for cl in sorted(res[lv].values(), key=lambda c: -len(c)):
                    if len(cl) > 1:
                        gs = sorted({g[1] for u in cl for x in uc.values() if x["full"] == u["full"] for g in x["games"]})
                        print("     %2d texts [%s]: %s | %d games: %s" % (len(cl), ",".join(sorted({u["fam"] for u in cl})), "; ".join(sorted({u["name"] + " " + u["th"][:4] for u in cl})), len(gs), ", ".join(gs)))
                print("   L3 singletons: %s" % "; ".join("%s %s (%s)" % (cl[0]["name"][:22], cl[0]["th"][:4], sorted({g[1] for x in uc.values() if x["full"] == cl[0]["full"] for g in x["games"]})[0][:18]) for cl in res[lv].values() if len(cl) == 1))

    # generalisation: unseen games against the texts of the test romset, and every game against the ids the tree already claims
    claimed = set()
    hd = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "hle")
    if os.path.isdir(hd):
        for fn in os.listdir(hd):
            if fn.endswith(".c"):
                claimed |= {int(m, 16) for m in re.findall(r"0x([0-9a-f]{16})ull", open(os.path.join(hd, fn)).read())}
    game_tasks = collections.defaultdict(collections.Counter)    # game -> full text -> tasks
    for u in uc.values():
        for g, c in u["tasks"].items():
            game_tasks[g][u["full"]] += c

    def gen_table(title, known, targets):
        """known: set of full-text hashes; targets: games. Per tier: tasks share and games whose every ucode (>= 1% of the game's tasks) is in the tier"""
        print("\n== %s" % title)
        print("   tier                                       mean share of a game's tasks   games fully covered (of %d)" % len(targets))
        kn = [ufull[k] for k in known if k in ufull]
        tiers = [("exact (same 4 KiB text)", None, 0), ("L1 all words matched (blocks may move)", 1, 1.0), ("L2 all words matched", 2, 1.0), ("L3 all words matched", 3, 1.0), ("L1 >= 99.5% (jump/branch targets masked)", 1, 0.995), ("L2 >= 99.5% (+ load/store offsets)", 2, 0.995),
                 ("L3 >= 99.5% (+ ALU immediates)", 3, 0.995), ("L3 >= 98%", 3, 0.98), ("L3 >= 95%", 3, 0.95)]
        for name, lv, thr in tiers:
            def ok(h):
                if h in known:
                    return True
                return lv is not None and any(pc(ufull[h], k, lv) >= thr for k in kn)
            shares = []
            gfull = 0
            for g in targets:
                gt = game_tasks[g]
                n = sum(gt.values())
                shares.append(sum(c for h, c in gt.items() if ok(h)) / max(1, n))
                if all(ok(h) for h, c in gt.items() if c >= 0.01 * n):
                    gfull += 1
            print("   %-42s %5.1f%%                          %d" % (name, 100.0 * sum(shares) / max(1, len(shares)), gfull))
    test_games = [g for g in games if g[0] == "test-romset"]
    unseen_games = [g for g in games if g[0] == "unseen-romset"]
    known_test = {u["full"] for u in rows if any(g[0] == "test-romset" for g in u["games"])}
    gen_table("generalisation: the 40 unseen games against the texts used by the 23 test-romset games", known_test, unseen_games)
    known_claimed = {u["full"] for u in rows if int(u["th"], 16) in claimed}
    print("\n(ids registered in src/hle: %d; %d of them seen in this survey, as %d distinct texts)" % (len(claimed), sum(1 for u in rows if int(u["th"], 16) in claimed), len(known_claimed)))
    gen_table("what the tree claims today (exact ids registered in src/hle) and what normalised matching would add, all 63 games", known_claimed, list(games))
    print("\n== games with a ucode (>= 1% of their tasks) that no claimed build covers at L3 >= 98%: ucode, best coverage by a claimed build")
    kn_ = [ufull[k] for k in known_claimed if k in ufull]
    for g in games:
        gt = game_tasks[g]
        n = sum(gt.values())
        bad = []
        for h, c in gt.items():
            if c >= 0.01 * n and h not in known_claimed:
                b_ = max((pc(ufull[h], k, 3) for k in kn_), default=0.0)
                if b_ < 0.98:
                    bad.append("%s %s (%.0f%%, %d%% of tasks)" % (ufull[h]["name"][:24], ufull[h]["th"][:4], 100 * b_, 100 * c // n))
        if bad:
            print("   %-40s %s" % (g[1][:40], "; ".join(bad)))
    gen_table("the same, test-romset games only", known_claimed, test_games)
    gen_table("the same, unseen-romset games only", known_claimed, unseen_games)

    # what differs between a build and its closest partner once L3-equal words are aligned: relocation, layout (load/store offsets), constants, real code
    print("\n== closest-partner differences: words of a build in the L3 alignment with its best partner (matching blocks of >= 1 word), split by what differs in the raw word")
    print("   columns: build, partner, non-zero words, then aligned words (zero padding included) that are equal / differ only in a jump or branch field / in a load-store offset / in an ALU immediate, then the build's non-zero words with no counterpart (a real code change)")
    def cat_of(w1, w2):
        op = w1 >> 26
        if op in (2, 3):
            return "jmp"
        if op in (4, 5, 6, 7, 1):
            return "jmp"
        if op in (32, 33, 35, 36, 37, 39, 40, 41, 43, 50, 58):
            return "ldst"
        if op in (8, 9, 10, 11, 12, 13, 14, 15):
            return "alu"
        return "other"
    tot_c = collections.Counter()
    for x in urows:
        if x["fam"] == "other" and x["words"].count(0) > 1000:
            continue
        best = None
        for y in urows:
            if y is not x and (best is None or pc(x, y, 3) > pc(x, best, 3)):
                best = y
        if best is None:
            continue
        a_ = norm[(x["full"], 3)]
        b_ = norm[(best["full"], 3)]
        sm = difflib.SequenceMatcher(None, a_, b_, autojunk=False)
        c = collections.Counter()
        mi = set()
        for b2 in sm.get_matching_blocks():
            mi.update(range(b2.a, b2.a + b2.size))
            for k in range(b2.size):
                w1, w2 = x["words"][b2.a + k], best["words"][b2.b + k]
                if w1 == w2:
                    c["same"] += 1
                else:
                    # which fields differ: compare with the L1 / L2 / L3 masks
                    if norm_word(w1, 1) == norm_word(w2, 1):
                        c["jmp"] += 1
                    elif norm_word(w1, 2) == norm_word(w2, 2):
                        c["ldst"] += 1
                    elif norm_word(w1, 3) == norm_word(w2, 3):
                        c["alu"] += 1
                    else:
                        c["other"] += 1
        nzx = len(nz[x["full"]])
        c["unmatched"] = len(nz[x["full"]] - mi)
        if pc(x, best, 3) >= 0.9:
            for k, v in c.items():
                tot_c[k] += v
            print("   %-26s %-26s %4d %4d %4d %4d %4d | %4d" % (x["name"][:22] + " " + x["th"][:4], best["name"][:22] + " " + best["th"][:4], nzx, c["same"], c["jmp"], c["ldst"], c["alu"], c["unmatched"]))
    print("   total over pairs with >= 90%% L3 coverage: %s" % dict(tot_c))

    # swaps / sequence behaviour
    print("\n== ucode swaps (per game: distinct ucode texts, number of changes of text between consecutive graphics tasks, A-B-A returns)")
    for g, seq in games.items():
        ks = [k.split(".")[0] for k, *_ in seq]
        if len(set(ks)) > 1:
            ch = sum(1 for i in range(1, len(ks)) if ks[i] != ks[i - 1])
            rn = sum(1 for i in range(2, len(ks)) if ks[i] == ks[i - 2] and ks[i] != ks[i - 1])
            cnt = collections.Counter(ks)
            print("  %-45s texts %d, changes %d (%.1f per 100 tasks), returns %d, mix %s" % (g[1][:45], len(set(ks)), ch, 100.0 * ch / len(ks), rn,
                  "/".join(str(c) for _, c in cnt.most_common())))
    print("\n== data segments: same text, several data hashes")
    for th, us in multi:
        print("  %s: %s" % (th, [(u["dh"][:6], sum(u["tasks"].values())) for u in us]))
    print("\n== ucode_size / data_size values seen")
    sz = collections.Counter()
    for g, seq in games.items():
        for k, us, ds, fl in seq:
            sz[(us, ds)] += 1
    for (us, ds), c in sz.most_common(12):
        print("  text 0x%x data 0x%x: %d tasks" % (us, ds, c))

    print("\n== IMEM after the first task vs text (words equal, of 0xf80 bytes) -- below ~60%% means overlays / self-modification are visible in IMEM")
    for u in rows:
        if u["imem_eq"] < 0.6:
            print("  %s %-24s %.0f%%" % (u["th"], u["name"][:24], 100 * u["imem_eq"]))

    if a.handlers:
        print("\n== alignment coverage (difflib blocks of >= 4 equal normalised words), per family")
        print("   cov(A|B) = share of A's code words (not zero, not text) that lie in a matched block of B. 'any' = union over all other builds of the family.")
        for f in ("F3DEX2", "F3DEX", "F3D", "other", "S2DEX"):
            us = [u for u in urows if u["fam"] == f]
            if len(us) < 2:
                continue
            print("\n-- %s (%d builds; %s)" % (f, len(us), "L1 / L2 / L3"))
            print("   %-30s %-17s | cov any other build, best partner (L3)" % ("build", "games"))
            for x in us:
                anyc = []
                best = (-1, None)
                for lv in (1, 2, 3):
                    un = set()
                    for y in us:
                        if y is x:
                            continue
                        key = (x["full"], y["full"], lv)
                        m = mset[key] & nz[x["full"]]
                        un |= m
                        if lv == 3 and len(m) > best[0]:
                            best = (len(m), y)
                    anyc.append(100.0 * len(un) / max(1, len(nz[x["full"]])))
                print("   %-30s %-17s | %3.0f%% %3.0f%% %3.0f%% | %3.0f%% %s" % (x["name"][:24] + " " + x["th"][:4], sorted({g[1] for g in x["games"]})[0][:17], anyc[0], anyc[1], anyc[2],
                      100.0 * best[0] / max(1, len(nz[x["full"]])), (best[1]["name"][:20] + " " + best[1]["th"][:4]) if best[1] else ""))
        # handler level: table targets of a reference build and how many of the other builds carry the handler (closure fully matched)
        print("\n== handlers: for the reference build of each family (the one with the highest mean coverage), the share of the other builds in which a handler's code")
        print("   (everything it reaches by branch / j / jal, minus code reached from > 50% of the handlers: loop, refill, flush) is entirely inside a matched block")
        groups = [("cluster", cl) for cl in sorted(clusters98, key=lambda c: -len(c)) if len(cl) >= 3 and cl[0]["fam"] in ("F3DEX2", "F3DEX", "F3D")]
        groups += [("whole family", [u for u in urows if u["fam"] == f_]) for f_ in ("F3DEX2", "F3DEX", "F3D")]
        for kind, cl in groups:
            us = cl
            f = cl[0]["fam"]
            def meancov(x):
                tot = 0
                for y in us:
                    if y is not x:
                        tot += len(mset[(x["full"], y["full"], 3)] & nz[x["full"]])
                return tot
            ref = max(us, key=meancov)
            words = imem_image(ref["words"], ref["ident"])
            ents, op2pc = dispatch_roots(ref, f, words)
            ents = sorted(e for e in ents if 0 <= e < 0x1000)
            cls = {e: closure(words, e) for e in ents}
            cnt = collections.Counter(a_ for c in cls.values() for a_ in c)
            uni = {a_ for a_, c in cnt.items() if c > 0.5 * len(cls)}
            print("\n-- %s %s of %d builds (%s), reference %s %s (%d handler entries, %d universal instructions)" % (f, kind, len(us), "; ".join(sorted({u["name"] for u in us}))[:120], ref["name"], ref["th"][:6], len(ents), len(uni)))
            others = [y for y in us if y is not ref]
            rows_h = []
            for e in ents:
                idx = {word_index(a_) for a_ in cls[e] if a_ not in uni} & nz[ref["full"]]
                if not idx:
                    continue
                un = {1: [], 2: [], 3: []}
                for lv in (1, 2, 3):
                    for y in others:
                        key = (ref["full"], y["full"], lv, 1)
                        if key not in mset:
                            sm = difflib.SequenceMatcher(None, norm[(ref["full"], lv)], norm[(y["full"], lv)], autojunk=False)
                            m = set()
                            for b_ in sm.get_matching_blocks():
                                m.update(range(b_.a, b_.a + b_.size))
                            mset[key] = m
                        un[lv].append(len(idx - mset[key]))
                names = "/".join(F3DEX2_OPS.get(o, "0x%02x" % o) for o in sorted(o for o, pc in op2pc.items() if pc == e))[:26] if op2pc else ""
                rows_h.append((e, len(idx), un, names))
            n_o = len(others)
            tot = sum(r[1] for r in rows_h) * n_o
            print("   %d handlers, %d instructions (x %d other builds). Share of (handler, build) instructions whose handler has 0 / <= 4 / <= 16 unmatched instructions in the other build:" % (len(rows_h), sum(r[1] for r in rows_h), n_o))
            for lv in (1, 2, 3):
                cells = []
                for lim in (0, 4, 16):
                    cells.append("%3.0f%%" % (100.0 * sum(r[1] for r in rows_h for v in r[2][lv] if v <= lim) / max(1, tot)))
                print("     L%d: %s" % (lv, " / ".join(cells)))
            print("   %-7s %5s  %-26s builds (of %d) with 0 / <=4 / <=16 unmatched insns: L2 | L3 ; median unmatched L2 | L3" % ("pc", "insn", "opcodes", n_o))
            for e, n, un, names in sorted(rows_h, key=lambda r: -r[1])[:36]:
                c = lambda lv, lim: sum(1 for v in un[lv] if v <= lim)
                med = lambda lv: sorted(un[lv])[len(un[lv]) // 2]
                print("   0x%04x %5d  %-26s %2d /%2d /%2d | %2d /%2d /%2d ; %3d | %3d" % (e, n, names, c(2, 0), c(2, 4), c(2, 16), c(3, 0), c(3, 4), c(3, 16), med(2), med(3)))
    if a.json:
        out = dict(ucodes=[dict(key=u["key"], name=u["name"], fam=u["fam"], banner=u["banner"][:200], games=sorted(g[1] for g in u["games"]), tasks=sum(u["tasks"].values()),
                                imem_eq=round(u["imem_eq"], 3), **{lv: u[lv] for lv in LEVELS}) for u in uc.values()])
        json.dump(out, open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
