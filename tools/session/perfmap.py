#!/usr/bin/env python3
"""perfmap.py: maps the perf samples of the emulation thread to ucode routines and compares the shares with the cost model.

  perfmap.py RUNDIR UCODEMAP.json [--out result.json]

RUNDIR is a run of the P session (ARMLET_PROF=2 + PERF=n: prof/map and prof/jit.bin of the JIT, perf.script.gz = `perf script -F tid,ip,sym,dso`).
UCODEMAP.json is ucodemap.py's output for the same game (its `site_owner` table: for every (pc, instruction word) site the owners its
executions were charged to, with the instruction count).

 - a sample inside a compiled region is mapped to the instruction (the per-instruction code offsets of prof/map) and from there through
   (pc, instruction word) to the owners of that site, split in proportion to the executions per owner;
 - a sample inside a VU op stub (a shared out-of-line body) is spread over the sites that execute that VU op, in proportion to the
   executions per owner; a sample in the enter / return thunks over the branch and jump sites; samples in the C++ helpers (RSP_MTC0 / MFC0,
   the SP DMA, VRCP ...) over the sites of their kind (DMA triggers, divides ...), by symbol name;
 - samples in rsp_enter / get_jit_block (block lookup), and everything outside parallel-rsp's code (the video plugin, the core, libc, the
   kernel) are counted as "not routine time" and are left out of the shares; the share of JIT proper they would make is printed.
"""
import argparse
import collections
import json
import gzip
import lzma
import os
import re
import sys
import bisect

SAMPLE = re.compile(r"^\s*(\d+)\s+([0-9a-f]+)\s+(.*?)\s+\((.*)\)\s*$")


def read_map(prof):
    regions, stubs, thunks = [], [], []
    cur = None
    for l in open(os.path.join(prof, "map")):
        p = l.split()
        if p[0] == "R":
            cur = dict(addr=int(p[2], 16), size=int(p[3]), pc_word=int(p[4]), count=int(p[5]), hash=p[6])
            regions.append(cur)
        elif p[0] == "O" and cur is not None:
            cur["offs"] = [int(x) for x in p[2:]]
        elif p[0] == "W" and cur is not None:
            cur["instrs"] = [int(x, 16) for x in p[2:]]
        elif p[0] == "S":
            stubs.append(dict(addr=int(p[1], 16), size=int(p[2]), name=p[3]))
        elif p[0] == "T":
            thunks.append(dict(addr=int(p[1], 16), size=int(p[2])))
    return regions, stubs, thunks


def read_samples(path):
    f = lzma.open(path, "rt", errors="replace") if path.endswith(".xz") else gzip.open(path, "rt", errors="replace") if path.endswith(".gz") else open(path, errors="replace")
    out = []
    for l in f:
        m = SAMPLE.match(l)
        if m:
            out.append((int(m.group(1)), int(m.group(2), 16), m.group(3), m.group(4)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("ucodemap")
    ap.add_argument("--out")
    ap.add_argument("--key", type=int, help="which gfx key of the ucodemap file (default: the first)")
    a = ap.parse_args()
    prof = os.path.join(a.run, "prof")
    regions, stubs, thunks = read_map(prof)
    um = json.load(open(a.ucodemap))
    um = [r for r in um if "site_owner" in r]
    um = um[0] if a.key is None else [r for r in um if r["key"] == a.key][0]
    site = {}                                   # (pc, instr) -> entry
    for s, e in um["site_owner"].items():
        site[(e["pc"], int(e["instr"], 16))] = e
    owner_cat = um["owner_category"]
    # per-owner instruction counts by kind, for spreading the stub / thunk / helper samples
    by_mn = collections.defaultdict(lambda: collections.defaultdict(float))     # VU mnemonic -> {owner: executions}
    by_kind = collections.defaultdict(lambda: collections.defaultdict(float))
    tot_owner = collections.defaultdict(float)
    for e in um["site_owner"].values():
        for o, n in e["owners"].items():
            by_mn[e["mn"]][o] += n
            k = e["kind"]
            if k in ("br", "brl", "j", "jal", "jr", "jalr"):
                by_kind["branch"][o] += n
            if e["carve"] == "dma":
                by_kind["dma"][o] += n
            if e["mn"] in ("VRCP", "VRCPL", "VRCPH", "VRSQ", "VRSQL", "VRSQH"):
                by_kind["vdiv"][o] += n
            if e["kind"] in ("mtc0", "mfc0"):
                by_kind["cop0"][o] += n
            tot_owner[o] += n
    cls_exec = collections.defaultdict(float)
    for e in um["site_owner"].values():
        cls_exec[e["cls"]] += e["count"]
    mn_cls = {}
    for e in um["site_owner"].values():
        mn_cls.setdefault(e["mn"], e["cls"])
    cls_samples = collections.defaultdict(float)
    # region lookup
    regions.sort(key=lambda r: r["addr"])
    starts = [r["addr"] for r in regions]
    stub_by = sorted(stubs, key=lambda s: s["addr"])
    stub_starts = [s["addr"] for s in stub_by]
    thunk_ranges = [(t["addr"], t["addr"] + t["size"]) for t in thunks]
    samples = read_samples(os.path.join(a.run, "perf.script.gz" if os.path.exists(os.path.join(a.run, "perf.script.gz")) else "perf.script.xz"))
    # the emulation thread: the thread with the most samples inside the JIT code
    in_jit = collections.Counter()
    for tid, ip, sym, dso in samples:
        i = bisect.bisect_right(starts, ip) - 1
        if i >= 0 and ip < regions[i]["addr"] + regions[i]["size"]:
            in_jit[tid] += 1
    if not in_jit:
        print("no sample inside the JIT code (%d samples)" % len(samples))
        return 1
    emu = in_jit.most_common(1)[0][0]
    es = [s for s in samples if s[0] == emu]
    res = dict(samples_total=len(samples), emu_tid=emu, emu_samples=len(es))
    cat = collections.defaultdict(float)
    own = collections.defaultdict(float)
    buckets = collections.Counter()
    unknown_site = 0.0

    def spread(weights, n=1.0, force_cat=None):
        tot = sum(weights.values())
        if tot <= 0:
            return False
        for o, w in weights.items():
            own[o] += n * w / tot
            cat[force_cat or owner_cat.get(o, "unmapped")] += n * w / tot
        return True

    for tid, ip, sym, dso in es:
        i = bisect.bisect_right(starts, ip) - 1
        if i >= 0 and ip < regions[i]["addr"] + regions[i]["size"]:
            r = regions[i]
            off = ip - r["addr"]
            j = bisect.bisect_right(r["offs"], off) - 1
            buckets["jit region"] += 1
            if 0 <= j < r["count"]:
                e = site.get(((r["pc_word"] + j) * 4, r["instrs"][j]))
                if e:
                    cls_samples[e["cls"]] += 1
                if e and spread(e["owners"], force_cat=e["carve"]):
                    continue
                unknown_site += 1
                buckets["jit region, site not in the count window"] += 1
            else:
                # the tail: end of block, latent delay slot, slow paths: spread over every instruction
                buckets["jit tail"] += 1
                spread(tot_owner)
            continue
        k = bisect.bisect_right(stub_starts, ip) - 1
        if k >= 0 and ip < stub_by[k]["addr"] + stub_by[k]["size"]:
            buckets["vu stub"] += 1
            nm = stub_by[k]["name"]
            cls_samples[mn_cls.get(nm, "vmul")] += 1
            if not spread(by_mn.get(nm, {}) if nm != "RESERVED" else {}):
                spread(tot_owner)
            continue
        if any(lo <= ip < hi for lo, hi in thunk_ranges):
            buckets["thunks"] += 1
            cls_samples["branch"] += 1
            spread(by_kind["branch"])
            continue
        s = sym.split("+")[0]
        if os.path.basename(dso).startswith(("armlet", "mupen64plus-rsp")):
            if re.search(r"rsp_enter|get_jit_block|hash_imem|analyze_static_end|unordered_map|_Hashtable|jit_region|jit_|invalidate", s):
                buckets["block lookup / compile"] += 1
            elif re.search(r"RSP_MTC0|RSP_MFC0|dma|copy_to_imem|memcpy|memmove", s):
                buckets["dma / cop0 helpers"] += 1
                cls_samples["cop0"] += 1
                spread(by_kind["dma"], force_cat="dma") if by_kind["dma"] else spread(by_kind["cop0"])
            elif re.search(r"VRCP|VRSQ|reciprocal|RSP_VMOV|CFC2", s):
                buckets["divide helpers"] += 1
                cls_samples["vdiv"] += 1
                spread(by_kind["vdiv"] or tot_owner)
            elif re.search(r"RSP_|ls_|rsp_", s):
                buckets["other rsp helpers"] += 1
                spread(tot_owner)
            else:
                buckets["armlet other (%s)" % s] += 1
        elif "libc" in dso and re.search(r"memcpy|memmove|memcmp", s):
            buckets["libc mem*"] += 1
        else:
            buckets["outside parallel-rsp: %s" % os.path.basename(dso)] += 1
    jitp = sum(n for k, n in buckets.items() if k in ("jit region", "vu stub", "thunks", "jit tail", "dma / cop0 helpers", "divide helpers", "other rsp helpers",
                                                       "block lookup / compile"))
    res["buckets"] = dict(buckets.most_common())
    res["jit_proper_samples"] = jitp
    routine = sum(cat.values())
    res["routine_samples"] = routine
    res["perf_share"] = {k: v / routine for k, v in sorted(cat.items(), key=lambda x: -x[1])}
    res["perf_owner_share"] = {k: v / routine for k, v in sorted(own.items(), key=lambda x: -x[1])}
    res["share_of_emu_thread"] = routine / len(es)
    print("thread %d: %d samples (of %d in all threads); JIT proper %.1f %% of them; routine time %.1f %% (block lookup %.1f %%)" % (
        emu, len(es), len(samples), 100 * jitp / len(es), 100 * routine / len(es), 100 * buckets["block lookup / compile"] / len(es)))
    for k, v in buckets.most_common(12):
        print("   %-48s %6.2f %%" % (k, 100 * v / len(es)))
    print("perf share of routine time by category:")
    for k, v in res["perf_share"].items():
        print("   %-12s %6.2f %%" % (k, 100 * v))
    # compare with the cost model
    mc = {k: v["cost"] for k, v in um["categories"].items()}
    mi = {k: v["instr"] for k, v in um["categories"].items()}
    tc, ti = sum(mc.values()), sum(mi.values())
    rows = []
    cats = sorted(set(mc) | set(res["perf_share"]), key=lambda k: -mc.get(k, 0))
    l1 = 0.0
    for k in cats:
        m = mc.get(k, 0) / tc if tc else 0
        p = res["perf_share"].get(k, 0)
        c = mi.get(k, 0) / ti
        rows.append((k, p, m, c))
        l1 += abs(p - m)
    res["compare"] = [dict(category=k, perf=p, model=m, instr=c) for k, p, m, c in rows]
    # relative cost of an executed instruction by class: the class's share of the samples over its share of the executions (1 = the average)
    ts = sum(cls_samples.values()) or 1
    te = sum(cls_exec.values()) or 1
    res["class_samples"] = dict(cls_samples)
    res["class_exec"] = dict(cls_exec)
    res["class_relative_cost"] = {c: (cls_samples[c] / ts) / (cls_exec[c] / te) for c in cls_exec if cls_exec[c] / te > 0.002}
    res["l1_perf_vs_model"] = l1 / 2
    res["l1_perf_vs_instr"] = sum(abs(p - c) for _, p, _, c in rows) / 2
    print("\n%-12s %8s %8s %8s" % ("category", "perf", "model", "instr"))
    for k, p, m, c in rows:
        print("%-12s %7.2f%% %7.2f%% %7.2f%%" % (k, 100 * p, 100 * m, 100 * c))
    print("total variation distance perf vs cost model %.3f, perf vs instruction counts %.3f" % (res["l1_perf_vs_model"], res["l1_perf_vs_instr"]))
    print("\nrelative cost of an executed instruction by class (share of samples / share of executions; 1 = average):")
    print("   " + ", ".join("%s %.2f" % (c, v) for c, v in sorted(res["class_relative_cost"].items(), key=lambda x: -x[1])))
    if a.out:
        json.dump(res, open(a.out, "w"), indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
