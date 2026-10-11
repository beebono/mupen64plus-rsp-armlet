#!/usr/bin/env python3
"""ucodemap.py: attributes the dynamic RSP instructions of a graphics ucode (a level-3 run of the arMLEt profiler) to routines.

  ucodemap.py PROFDIR [--window M0:M1] [--cost cost.json] [--rspdis PATH] [--json out.json]

PROFDIR is the <out> directory of a run with ARMLET_PROF=3 and ARMLET_PROF_WALK=1 (sites, counts.<label>, stats, ucode/). The window is the
difference of two snapshots (default: the last two marks M<n>). Per gfx ucode key:

 1. sites are (pc, instruction word) pairs, decoded by rspdis (the fork's disassembler). The profiler counts every site, every control-flow
    edge, and charges every instruction to a *context*: the display-list command being processed. J1, the first-level dispatch jump, is the
    register jump executed exactly once per command (found while running by comparing with the display-list walker); a context is the
    target of J1 and, for ucodes that dispatch in two levels (F3D, F3DEX 1.x: first by command class, then by opcode), the target of the
    table-driven jump that follows. Here a first-level target is a *stage* when almost every entry goes on to a second-level jump; its own
    instructions are then shared among the second-level handlers in proportion to their entries. Shared subroutines are charged to the
    command that called them by construction.
 2. each owner (handler) is named by the opcodes it serves: from the jump table in DMEM when the walker's command counts of clean tasks
    (no CULLDL / BRANCH_Z, so the ucode ran exactly the commands the walker saw) fit it exactly (F3DEX2 family), otherwise by matching the
    owner's entry count against the walker's per-opcode counts (a single opcode, or the group of RDP commands 0xC0 and up).
 3. sites executed about once per command whatever the command (the command fetch and loop) are the "dispatch" owner; mtc0 writes to the DMA
    registers and DMA-wait polls are "dma", writes of CMD_START / CMD_END are "rdp-submit"; the code before the first dispatch of a task is
    "task init". Everything else is charged to the command's category (vertex, triangle, matrix, dl, state, rdp).
 4. validation: J1's executions against the walker's command count, the handler entries against the walker's counts per opcode (clean tasks).
"""
import argparse
import collections
import json
import os
import re
import struct
import subprocess

CLS = ["vmul", "vmac", "vaddsub", "vcmp", "vlogic", "vdiv", "vsar", "vload", "vstore", "salu", "sload", "sstore", "branch", "cop0",
       "cop2mov", "other"]

# opcode -> (name, category) per ucode family (1 F3D, 2 F3DEX / F3DLX 1.x, 3 F3DEX2 family)
OPS = {}
OPS[3] = {0x00: ("NOOP", "state"), 0x01: ("VTX", "vertex"), 0x02: ("MODIFYVTX", "vertex"), 0x03: ("CULLDL", "dl"), 0x04: ("BRANCH_Z", "dl"),
          0x05: ("TRI1", "triangle"), 0x06: ("TRI2", "triangle"), 0x07: ("QUAD", "triangle"), 0x08: ("LINE3D", "triangle"),
          0xD3: ("SPECIAL_3", "state"), 0xD4: ("SPECIAL_2", "state"), 0xD5: ("SPECIAL_1", "state"), 0xD6: ("DMA_IO", "state"),
          0xD7: ("TEXTURE", "state"), 0xD8: ("POPMTX", "matrix"), 0xD9: ("GEOMETRYMODE", "state"), 0xDA: ("MTX", "matrix"),
          0xDB: ("MOVEWORD", "matrix"), 0xDC: ("MOVEMEM", "matrix"), 0xDD: ("LOAD_UCODE", "state"), 0xDE: ("DL", "dl"), 0xDF: ("ENDDL", "dl"),
          0xE0: ("SPNOOP", "state"), 0xE1: ("RDPHALF_1", "state"), 0xE2: ("SETOTHERMODE_L", "state"), 0xE3: ("SETOTHERMODE_H", "state"),
          0xF1: ("RDPHALF_2", "state")}
OPS[2] = {0x00: ("SPNOOP", "state"), 0x01: ("MTX", "matrix"), 0x03: ("MOVEMEM", "matrix"), 0x04: ("VTX", "vertex"), 0x06: ("DL", "dl"),
          0xB0: ("BRANCH_Z", "dl"), 0xB1: ("TRI2", "triangle"), 0xB2: ("RDPHALF_CONT/MODIFYVTX", "state"), 0xB3: ("RDPHALF_2", "state"),
          0xB4: ("RDPHALF_1", "state"), 0xB5: ("LINE3D", "triangle"), 0xB6: ("CLEARGEOMETRYMODE", "state"), 0xB7: ("SETGEOMETRYMODE", "state"),
          0xB8: ("ENDDL", "dl"), 0xB9: ("SETOTHERMODE_L", "state"), 0xBA: ("SETOTHERMODE_H", "state"), 0xBB: ("TEXTURE", "state"),
          0xBC: ("MOVEWORD", "matrix"), 0xBD: ("POPMTX", "matrix"), 0xBE: ("CULLDL", "dl"), 0xBF: ("TRI1", "triangle")}
OPS[1] = dict(OPS[2])
OPS[1][0xB1] = ("RDPHALF_CONT", "state")
RDP_NAMES = {0xE4: "TEXRECT", 0xE5: "TEXRECTFLIP", 0xE6: "RDPLOADSYNC", 0xE7: "RDPPIPESYNC", 0xE8: "RDPTILESYNC", 0xE9: "RDPFULLSYNC",
             0xEA: "SETKEYGB", 0xEB: "SETKEYR", 0xEC: "SETCONVERT", 0xED: "SETSCISSOR", 0xEE: "SETPRIMDEPTH", 0xEF: "RDPSETOTHERMODE",
             0xF0: "LOADTLUT", 0xF2: "SETTILESIZE", 0xF3: "LOADBLOCK", 0xF4: "LOADTILE", 0xF5: "SETTILE", 0xF6: "FILLRECT",
             0xF7: "SETFILLCOLOR", 0xF8: "SETFOGCOLOR", 0xF9: "SETBLENDCOLOR", 0xFA: "SETPRIMCOLOR", 0xFB: "SETENVCOLOR", 0xFC: "SETCOMBINE",
             0xFD: "SETTIMG", 0xFE: "SETZIMG", 0xFF: "SETCIMG"}
for _f in OPS:
    for _o, _n in RDP_NAMES.items():
        OPS[_f].setdefault(_o, (_n, "rdp"))
for _o in range(0xC0, 0x100):          # F3D / F3DEX: every RDP command 0xC0..0xFF
    for _f in (1, 2):
        OPS[_f].setdefault(_o, ("RDP_%02X" % _o, "rdp"))


def op_info(fam, op):
    return OPS.get(fam, {}).get(op, ("op%02X" % op, "state"))


def read_keys(d):
    keys = {}
    for l in open(os.path.join(d, "keys")):
        m = re.match(r"K (\d+) (\d+) ([0-9a-f]+) (\d+) \| ?(.*)", l.strip())
        if m:
            keys[int(m.group(1))] = dict(type=int(m.group(2)), id=m.group(3), family=int(m.group(4)), name=m.group(5))
    return keys


def read_stats(d):
    """label -> {line tag + ' ' + key: tokens}"""
    S = collections.OrderedDict()
    cur = None
    for l in open(os.path.join(d, "stats")):
        p = l.split()
        if p[0] == "S":
            cur = p[1]
            S[cur] = {}
        elif cur is not None and len(p) > 1:
            S[cur][p[0] + " " + p[1]] = p[2:]
    return S


def ops_of(tokens):
    r = {}
    for t in tokens:
        a, b = t.split(":")
        r[int(a, 16)] = int(b)
    return r


class Counts:
    """one counts.<label> file"""

    def __init__(self, path):
        self.C = collections.defaultdict(dict)            # key -> {site: count}
        self.E = collections.defaultdict(dict)            # key -> {(from, to): count}
        self.D = collections.defaultdict(dict)            # clean tasks
        self.X = collections.defaultdict(dict)            # key -> {(ctx, site): count}
        self.Y = collections.defaultdict(dict)            # key -> {ctx: (A, B)}
        self.N = collections.defaultdict(dict)            # key -> {ctx: entries}
        self.Z = {}
        for l in open(path):
            p = l.split()
            t = p[0]
            k = int(p[1])
            if t == "C":
                self.C[k][int(p[2])] = int(p[3])
            elif t == "E":
                self.E[k][(int(p[2]), int(p[3]))] = int(p[4])
            elif t == "D":
                self.D[k][(int(p[2]), int(p[3]))] = int(p[4])
            elif t == "X":
                self.X[k][(int(p[2]), int(p[3]))] = int(p[4])
            elif t == "Y":
                self.Y[k][int(p[2])] = (int(p[3]), int(p[4]))
            elif t == "N":
                self.N[k][int(p[2])] = int(p[3])
            elif t == "Z":
                self.Z[k] = int(p[2])


def diff(a, b):
    return {k: a[k] - b.get(k, 0) for k in a if a[k] - b.get(k, 0)}


def read_sites(d, rspdis):
    sites = {}
    raw = open(os.path.join(d, "sites")).read()
    out = subprocess.run([rspdis], input=raw, capture_output=True, text=True, check=True).stdout
    for l in out.splitlines():
        a, t = l.split(" | ", 1)
        i, pc, ins, kind, arg = a.split()
        sites[int(i)] = dict(pc=int(pc), instr=int(ins, 16), kind=kind, arg=int(arg), text=t.split(None, 1)[1] if " " in t else t)
    for l in open(os.path.join(d, "sites")):
        p = l.split()
        sites[int(p[0])]["cls"] = int(p[3])
    return sites


def infer_table(dmem, handler_pcs, obs, nops):
    """finds the jump table: handler(op) = the halfword at B + scale * idx(op), where idx is int8(op) (F3DEX2 family) or op & 0x7f, so that the
    walker's per-opcode command counts `nops` map onto the observed handler entry counts `obs`. returns (B, scale, mode, {op: pc}, score) or None"""
    words = struct.unpack("<1024I", dmem[:0x1000])
    hw = []
    for w in words:
        hw += [w >> 16, w & 0xFFFF]
    idx = {"int8": lambda o: o - 256 if o >= 128 else o, "and7f": lambda o: o & 0x7F}
    best = None
    ops = [o for o, n in nops.items() if n > 0]
    tot = sum(obs.values()) or 1
    for mode, f in idx.items():
        for scale in (1, 2, 4):
            for B in range(0, 0x1000, 2):
                pred = collections.defaultdict(int)
                ok = True
                for o in ops:
                    a = B + scale * f(o)
                    if a < 0 or a >= 0x1000 - 1:
                        ok = False
                        break
                    v = hw[a // 2] & 0xFFF
                    pred[v] += nops[o]          # a command the ucode skipped (BRANCH_Z, CULLDL) may point at a handler that never ran
                if not ok:
                    continue
                err = sum(abs(pred.get(h, 0) - obs.get(h, 0)) for h in set(handler_pcs) | set(pred))
                score = 1.0 - err / tot
                if best is None or score > best[4]:
                    best = (B, scale, mode, f, score)
    if not best:
        return None
    B, scale, mode, f, score = best
    table = {}
    for o in range(256):
        a = B + scale * f(o)
        if 0 <= a < 0x1000 - 1:
            table[o] = hw[a // 2] & 0xFFF
    return B, scale, mode, table, score


def match_by_counts(owner_n, nops, tol=0.002):
    """owner -> [opcodes] by entry count: one opcode with the same count, or the group of every RDP opcode (0xC0 and up) together"""
    res = {}
    amb = set()
    used = set()
    rdp_sum = sum(n for o, n in nops.items() if o >= 0xC0)
    for o_, n in sorted(owner_n.items(), key=lambda x: -x[1]):
        if n <= 0:
            continue
        if rdp_sum and abs(n - rdp_sum) <= max(2, tol * rdp_sum) and not any(op >= 0xC0 for op in used):
            res[o_] = [op for op in nops if op >= 0xC0]
            used.update(res[o_])
            continue
        cand = [op for op, m in nops.items() if op not in used and abs(m - n) <= max(1, tol * n)]
        if len(cand) == 1:
            res[o_] = cand
            used.add(cand[0])
        elif len(cand) > 1:
            res[o_] = cand                     # several opcodes with this count (e.g. the halves of a texture rectangle): ambiguous
            amb.add(o_)
    return res, amb


def analyse(d, key, kinfo, sites, c0, c1, opts):
    fam = kinfo["family"]
    cnt = diff(c1.C[key], c0.C[key])
    edges = diff(c1.E[key], c0.E[key])
    cedges = diff(c1.D[key], c0.D[key])
    if not cnt:
        return None
    total = sum(cnt.values())
    res = dict(key=key, id=kinfo["id"], family=fam, name=kinfo["name"], total_instr=total)
    succ_of = collections.defaultdict(dict)
    for (a, b), c in edges.items():
        succ_of[a][b] = c
    j1 = c1.Z.get(key, 0)
    if not j1 or j1 not in cnt:
        res["error"] = "no first-level dispatch jump was found (a clean walked task per three in a row is needed)"
        return res
    d1 = max(succ_of[j1], key=lambda b: cnt.get(b, 0))     # the delay slot of J1
    entries_A = dict(succ_of[d1])                           # J1 target site -> dispatches
    res["j1_pc"] = sites[j1]["pc"]
    res["j1_count"] = cnt[j1]
    # ---- walker ground truth
    stats = opts["stats"]
    m0, m1 = opts["m0"], opts["m1"]

    def opdiff(tag):
        k_ = "%s %d" % (tag, key)
        if k_ not in stats[m1]:
            return {}
        o1 = ops_of(stats[m1][k_])
        o0 = ops_of(stats[m0][k_]) if k_ in stats[m0] else {}
        return {o: o1[o] - o0.get(o, 0) for o in o1 if o1[o] - o0.get(o, 0) > 0}

    def wf(tag, name):
        def get(lbl):
            for t in stats[lbl].get("%s %d" % (tag, key), []):
                if t.startswith(name + "="):
                    return int(t.split("=")[1])
            return 0
        return get(m1) - get(m0)
    nclean, nall = opdiff("q"), opdiff("o")
    res["walker"] = dict(cmds=wf("w", "cmds"), clean_cmds=wf("u", "cmds"), tasks=wf("w", "tasks"), clean_tasks=wf("u", "tasks"),
                         culldl=wf("w", "culldl"), branchz=wf("w", "branchz"))
    # ---- contexts -> owners
    Y = c1.Y[key]
    N = diff(c1.N[key], c0.N[key])
    X = diff(c1.X[key], c0.X[key])
    ctx_site = collections.defaultdict(dict)                # ctx -> {site: count}
    for (x, s), n in X.items():
        ctx_site[x][s] = n
    byA = collections.defaultdict(dict)                     # A -> {B or 0: ctx}
    for x, (A, B) in Y.items():
        byA[A][B] = x
    owner_ctx = collections.defaultdict(list)               # owner -> [ctx whose code is the owner's own]
    owner_entries = {}
    shared = {}                                             # stage ctx -> {owner: weight}
    pcname = lambda s: "%03x" % sites[s]["pc"]
    stage_of = {}
    for A, bs in byA.items():
        nA = entries_A.get(A, 0)
        nb = sum(N.get(x, 0) for B, x in bs.items() if B)
        own_instr = sum(ctx_site[bs[0]].values()) / nA if (bs.get(0) is not None and nA) else 1e9
        # a stage only selects the second-level handler: nearly every entry goes on to a table-driven jump after a few instructions (a handler
        # that happens to switch on a table inside, such as a triangle handler choosing its variant, does a lot of work before)
        if bs.get(0) is not None and nA and nb >= 0.9 * nA and own_instr <= 80:
            stage_of[A] = True
            for B, x in bs.items():
                if B:
                    o = "H%s.%s" % (pcname(A), pcname(B))
                    owner_ctx[o].append(x)
                    owner_entries[o] = owner_entries.get(o, 0) + N.get(x, 0)
            shared[bs[0]] = {"H%s.%s" % (pcname(A), pcname(B)): N.get(x, 0) for B, x in bs.items() if B}
        else:
            o = "H%s" % pcname(A)
            for B, x in bs.items():
                owner_ctx[o].append(x)
            owner_entries[o] = nA
    # ---- opcode naming: the jump table (single-level ucodes), then entry counts against the walker
    handler_pcs = {}
    for A in entries_A:
        handler_pcs[sites[A]["pc"]] = A
    owner_ops = {}
    amb = set()
    table = None
    obs_used = {}
    dm_path = os.path.join(d, "ucode", kinfo["id"] + ".dmem")
    if not stage_of and os.path.exists(dm_path):
        obs_clean = {}
        for (a_, b_), n in cedges.items():
            if a_ == d1 and b_ in entries_A:
                obs_clean[sites[b_]["pc"]] = obs_clean.get(sites[b_]["pc"], 0) + n
        obs_all = {}
        for A, n in entries_A.items():
            obs_all[sites[A]["pc"]] = obs_all.get(sites[A]["pc"], 0) + n
        for basis, ops_, obs_, thr in (("clean tasks", nclean, obs_clean, 0.9), ("all tasks (the walker assumes no skipped commands)", nall, obs_all, 0.8)):
            if ops_ and obs_:
                t = infer_table(open(dm_path, "rb").read(), set(handler_pcs), obs_, ops_)
                if t and t[4] > thr:
                    table = t
                    obs_used = obs_
                    res["table"] = dict(base=t[0], scale=t[1], mode=t[2], score=round(t[4], 4), basis=basis)
                    res["table_nops"] = ops_
                    for op, pc in t[3].items():
                        if pc in handler_pcs:
                            owner_ops.setdefault("H%03x" % pc, []).append(op)
                    break
    nops_used = res.get("table_nops") or (nclean if sum(nclean.values()) >= 0.8 * sum(nall.values()) else nall)
    if not table:
        m_, amb = match_by_counts(owner_entries, nops_used)
        owner_ops.update(m_)
        res["naming"] = "entry counts against the walker's opcode counts (%s)" % ("clean tasks" if nops_used is nclean else "all tasks")
    # ---- per-owner category and name
    cats = {}
    names = {}
    for o in owner_entries:
        ops = owner_ops.get(o, [])
        w = collections.defaultdict(float)
        nm = []
        for op in ops:
            n_, c_ = op_info(fam, op)
            w[c_] += nops_used.get(op, 0) + 1e-6
            if nops_used.get(op, 0):
                nm.append((nops_used.get(op, 0), n_))
        cats[o] = max(w, key=w.get) if w else "unmapped"
        nm.sort(reverse=True)
        names[o] = "%s%s [%s]" % ("/".join(x for _, x in nm[:4]) or "?", "+%d" % (len(nm) - 4) if len(nm) > 4 else "", o[1:])
        if o in amb:
            names[o] = "one of %s [%s]" % ("|".join(x for _, x in nm[:5]), o[1:])
    cats["init"], names["init"] = "task init", "boot + task init (before the first command)"
    cats["dispatch"], names["dispatch"] = "dispatch", "dispatch (command fetch and loop, shared by every command)"
    # ---- weights per site
    sl = sorted(cnt)
    w = {s: 1.0 for s in sl}
    if opts.get("cost"):
        w = {s: opts["cost"].get(CLS[sites[s]["cls"]], 0.0) for s in sl}
    # dispatch-like sites: executed about once (or a fixed fraction of once) per command whatever the command is
    big = [o for o, n in owner_entries.items() if n >= 0.01 * cnt[j1]]
    dsites = set()
    for s in sl:
        fr = []
        for o in big:
            fr.append(sum(ctx_site[x].get(s, 0) for x in owner_ctx[o]) / owner_entries[o])
        if len(fr) < 3:
            continue
        med = sorted(fr)[len(fr) // 2]
        if med >= 0.005 and all(abs(f - med) <= 0.25 * med for f in fr):
            dsites.add(s)
    carve = {}
    for s in sl:
        st = sites[s]
        if st["kind"] == "mtc0" and st["arg"] in (0, 1, 2, 3):
            carve[s] = "dma"
        elif st["kind"] == "mfc0" and st["arg"] in (5, 6):
            carve[s] = "dma"
        elif st["kind"] == "mtc0" and st["arg"] in (8, 9):
            carve[s] = "rdp-submit"
    # ---- tallies
    cat_cost = collections.defaultdict(float)
    cat_instr = collections.defaultdict(float)
    own_cost = collections.defaultdict(float)
    own_instr = collections.defaultdict(float)
    own_cls = collections.defaultdict(lambda: collections.defaultdict(float))
    band = collections.defaultdict(lambda: collections.defaultdict(float))
    site_own = collections.defaultdict(lambda: collections.defaultdict(float))   # site -> {owner: instructions}
    ctx_owner = {}
    for o, xs in owner_ctx.items():
        for x in xs:
            ctx_owner[x] = o
    attributed = [0.0]

    def add(o, s, n, frac=1.0):
        tag = carve.get(s)
        own = "dispatch" if s in dsites else o
        cat = tag if tag else cats.get(own, "unmapped")
        m = n * frac
        cat_cost[cat] += m * w[s]
        cat_instr[cat] += m
        own_cost[own] += m * w[s]
        own_instr[own] += m
        own_cls[own][CLS[sites[s]["cls"]]] += m
        site_own[s][own] += m
        attributed[0] += m
        if own == o and owner_entries.get(o):
            r = n / owner_entries[o]
            band[o]["every command" if r >= 0.9 else ("some commands" if r >= 0.3 else "rare paths")] += m
    for x, d_ in ctx_site.items():
        for s, n in d_.items():
            if s not in cnt:
                continue
            if x == 0:
                add("init", s, n)
            elif x in shared:
                tot = sum(shared[x].values())
                for o, wt in shared[x].items():
                    add(o, s, n, wt / tot if tot else 0)
            elif x in ctx_owner:
                add(ctx_owner[x], s, n)
            else:
                add("overflow", s, n)
    res["unattributed_instr"] = total - attributed[0]
    res["categories"] = {k: dict(instr=cat_instr[k], cost=cat_cost[k]) for k in sorted(cat_cost, key=lambda x: -cat_cost[x])}
    res["owners"] = {}
    for o in sorted(own_cost, key=lambda x: -own_cost[x]):
        e = owner_entries.get(o)
        res["owners"][o] = dict(name=names.get(o, o), category=cats.get(o, "unmapped"), instr=own_instr[o], cost=own_cost[o], entries=e,
                                instr_per_entry=(own_instr[o] / e if e else None), mix={k: float(v) for k, v in own_cls[o].items() if v},
                                bands={k: float(v) for k, v in band[o].items()} if o in band else None,
                                ops=["%02X" % op for op in owner_ops.get(o, []) if nops_used.get(op, 0)])
    # per site: where its executions went (the perf cross-check maps samples through this)
    res["site_owner"] = {str(s): dict(pc=sites[s]["pc"], instr="%08x" % sites[s]["instr"], cls=CLS[sites[s]["cls"]], kind=sites[s]["kind"], arg=sites[s]["arg"], mn=sites[s]["text"].split()[0],
                                      count=cnt[s], owners={o: n for o, n in site_own[s].items()}, carve=carve.get(s)) for s in site_own}
    res["owner_category"] = {o: ("dispatch" if o == "dispatch" else cats.get(o, "unmapped")) for o in own_cost}
    # ---- validation
    wk = res["walker"]
    v = dict(j1_count=cnt[j1], walker_cmds_all_tasks=wk["cmds"], owner_entries_total=sum(owner_entries.values()))
    v["j1_count_clean_tasks"] = sum(n for (a_, b_), n in cedges.items() if a_ == d1)
    v["walker_cmds_clean_tasks"] = wk["clean_cmds"]
    if table:
        pred = collections.defaultdict(int)
        for op, n in res["table_nops"].items():
            pred[table[3].get(op)] += n
        v["per_handler"] = [dict(handler="%03x" % pc, ops=["%02X" % o for o in owner_ops.get("H%03x" % pc, []) if res["table_nops"].get(o, 0)],
                                 walker=pred.get(pc, 0), entries=obs_used.get(pc, 0)) for pc, A in handler_pcs.items()]
    else:
        v["per_owner_matched"] = [dict(owner=o, name=names[o], entries=owner_entries[o], walker=sum(nops_used.get(op, 0) for op in owner_ops.get(o, [])))
                                  for o in owner_entries if owner_entries[o] >= 0.001 * cnt[j1]]
    res["validation"] = v
    res.pop("table_nops", None)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("profdir")
    ap.add_argument("--window", help="M0:M1")
    ap.add_argument("--cost", help="json {class: ns per instruction}, or the output of costmodel.py with --cost-key")
    ap.add_argument("--cost-key", help="entry of the costmodel.py output to use (a game, or pooled)")
    ap.add_argument("--rspdis", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "rspdis"))
    ap.add_argument("--json")
    a = ap.parse_args()
    d = a.profdir
    keys = read_keys(d)
    stats = read_stats(d)
    labels = [l for l in stats if re.match(r"M\d+$", l)]
    if a.window:
        m0, m1 = a.window.split(":")
    else:
        m0, m1 = labels[-2], labels[-1]
    c0 = Counts(os.path.join(d, "counts." + m0))
    c1 = Counts(os.path.join(d, "counts." + m1))
    sites = read_sites(d, a.rspdis)
    cost = json.load(open(a.cost)) if a.cost else None
    if cost and a.cost_key:
        cost = cost[a.cost_key]
    out = []
    for k, ki in keys.items():
        if ki["type"] != 1:
            continue
        r = analyse(d, k, ki, sites, c0, c1, dict(stats=stats, m0=m0, m1=m1, cost=cost))
        if r:
            out.append(r)
    if a.json:
        json.dump(out, open(a.json, "w"), indent=1)
    for r in out:
        print("== key %d id %s family %d %s" % (r["key"], r["id"], r["family"], r["name"][:60]))
        if "error" in r:
            print("  ", r["error"])
            continue
        print("  window %s:%s  instr %d  J1 pc %03x executed %d  table %s  %s" % (m0, m1, r["total_instr"], r["j1_pc"], r["j1_count"], r.get("table"), r.get("naming", "")))
        tot = sum(v["cost"] for v in r["categories"].values()) or 1
        for cat, v in r["categories"].items():
            print("   %-12s %6.2f%%  (instr %6.2f%%)" % (cat, 100 * v["cost"] / tot, 100 * v["instr"] / r["total_instr"]))
        print("   unattributed: %.3f%% of instr" % (100 * r["unattributed_instr"] / r["total_instr"]))
        for o, v in list(r["owners"].items())[:16]:
            print("     %-40s %-9s %6.2f%%  entries %s  instr/entry %s" % (v["name"][:40], v["category"], 100 * v["cost"] / tot, v["entries"],
                                                                         ("%.0f" % v["instr_per_entry"]) if v["instr_per_entry"] else "-"))
        print("   validation:", {k: v for k, v in r["validation"].items() if not isinstance(v, list)})


if __name__ == "__main__":
    main()
