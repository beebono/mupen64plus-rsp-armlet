#!/usr/bin/env python3
"""Native coverage recount from ARMLET_UCODE_STATS files (docs/ucode-coverage.md).

  tools/ucode_coverage.py STATSDIR [--md]

STATSDIR/<set>/<game>/ustats.txt is what a headless run with GfxMode=hle and ARMLET_UCODE_STATS=<file> leaves
(src/ustats.c; tools/ucode_coverage_run.sh makes the directory). Without --md a plain-text summary is printed.
Time columns: native_ns = time of native runs, wasted_ns = time of declined native attempts, jit_ns = the JIT's time
for the tasks it ran; all without the time spent inside the video plugin's ProcessRdpList callback.
"""
import os, re, sys, collections

FAM = ["other", "F3D", "F3DEX", "F3DEX2"]


def short(name):
    """'F3DEX fifo 2.07' style label(s) from the banner strings; Rare/Indy texts have none."""
    parts = []
    for m in re.finditer(r"Gfx(?:\[Safe\])? ucode (\S+(?: fifo| xbus)?) (\d\.\d+\w*)|Gfx\[Safe\] (\S+ fifo) (\d\.\d+)|SW Version: (2\.0\w)", name):
        if m.group(5):
            parts.append("F3D " + m.group(5))
        elif m.group(1):
            parts.append(m.group(1) + " " + m.group(2))
        else:
            parts.append(m.group(3) + " " + m.group(4))
    for m in re.finditer(r"Gfx\[Safe\] (\S+ fifo) (\d\.\d+)", name):
        s = m.group(1) + " " + m.group(2) + " [Safe]"
        if s not in parts:
            parts.append(s)
    if not parts:
        return "(no banner)"
    return " / ".join(dict.fromkeys(parts))


def load(root):
    games = collections.OrderedDict()
    for st in sorted(os.listdir(root)):
        for g in sorted(os.listdir(os.path.join(root, st))):
            p = os.path.join(root, st, g, "ustats.txt")
            if not os.path.isfile(p):
                continue
            us = collections.OrderedDict()
            for ln in open(p):
                f = ln.rstrip("\n").split(" ", 11)
                if f[0] == "U":
                    us[f[1]] = dict(id=f[1], text=f[2], fam=int(f[3]), tasks=int(f[4]), native=int(f[5]), declined=int(f[6]), nohandler=int(f[7]),
                                    native_ns=int(f[8]), wasted_ns=int(f[9]), jit_ns=int(f[10]), name=f[11], reasons=[])
                elif f[0] == "R":
                    f = ln.rstrip("\n").split(" ", 3)
                    us[f[1]]["reasons"].append((int(f[2]), f[3]))
            games[(st, g)] = us
    return games


def merge_by_text(us):
    """ids that differ only by ucode_size (same 4 KiB text) are one ucode (counters of before 2026-10-16; the id is the 4 KiB hash now)"""
    m = collections.OrderedDict()
    for u in us.values():
        k = u["text"]
        if k not in m:
            m[k] = dict(u, ids=[u["id"]], reasons=list(u["reasons"]))
        else:
            t = m[k]
            for f in ("tasks", "native", "declined", "nohandler", "native_ns", "wasted_ns", "jit_ns"):
                t[f] += u[f]
            t["ids"].append(u["id"])
            t["reasons"] += u["reasons"]
    return m


def kind(u):
    if u["nohandler"] == u["tasks"] or (u["nohandler"] and not u["native"]):
        return "no handler"
    if u["native"] == 0:
        return "handler, always declines"
    if u["declined"] or u["nohandler"]:
        return "partly native"
    return "native"


def reasons(u):
    d = collections.Counter()
    for n, w in u["reasons"]:
        d[w] += n
    return d


def game_row(us):
    t = sum(u["tasks"] for u in us.values())
    nat = sum(u["native"] for u in us.values())
    dec = sum(u["declined"] for u in us.values())
    nh = sum(u["nohandler"] for u in us.values())
    nat_ns = sum(u["native_ns"] for u in us.values())
    jit = sum(u["jit_ns"] + u["wasted_ns"] for u in us.values())
    return t, nat, dec, nh, nat_ns, jit


def compute(root):
    """-> games, rows, hot (list, best first), ng, registered texts"""
    games = load(root)
    ng = len(games)
    reg = set()
    for us0 in games.values():
        for u in us0.values():
            if u["native"]:
                reg.add(u["text"])
    rows = []
    hot = collections.OrderedDict()
    for (st, g), us0 in games.items():
        us = merge_by_text(us0)
        t, nat, dec, nh, nat_ns, jit = game_row(us)
        tot = nat_ns + jit
        share = 100.0 * jit / tot if tot else 0.0
        rows.append((st, g, t, nat, dec, nh, share, us))
        for u in us.values():
            if kind(u) == "native":
                continue
            h = hot.setdefault(u["text"], dict(text=u["text"], name=u["name"], fam=u["fam"], games=[], tasks=0, jit_ns=0, share_sum=0.0, task_share_sum=0.0,
                                               kinds=collections.Counter(), reasons=collections.Counter(), ids=set(), nohandler_tasks=0, tasks_total=0))
            jt = u["jit_ns"] + u["wasted_ns"]
            h["games"].append(g)
            h["tasks"] += u["tasks"] - u["native"]
            h["tasks_total"] += u["tasks"]
            h["nohandler_tasks"] += u["nohandler"]
            h["jit_ns"] += jt
            h["share_sum"] += 100.0 * jt / tot if tot else 0.0
            h["task_share_sum"] += 100.0 * (u["tasks"] - u["native"]) / t if t else 0.0
            h["kinds"][kind(u)] += 1
            h["reasons"].update(reasons(u))
            if u["nohandler"] and u["text"] in reg:
                h["reasons"]["no handler for this id, but its 4 KiB text is a registered build's (the id is keyed over a different ucode_size)"] += u["nohandler"]
            h["ids"].update(u["ids"])
    hl = sorted(hot.values(), key=lambda h: -h["share_sum"])
    return games, rows, hl, ng, reg


def setlabel(st):
    return "not-coded" if st.startswith("not-coded") else "test" if st.startswith("test") else "unseen"


def main():
    root = sys.argv[1]
    md = "--md" in sys.argv
    games, rows, hl, ng, reg = compute(root)
    tt = sum(r[2] for r in rows)
    tn = sum(r[3] for r in rows)
    out = []
    P = out.append
    P("%d games, %d graphics tasks offered, %d native (%.1f%%)" % (ng, tt, tn, 100.0 * tn / tt))
    P("mean per-game task share native: %.1f%%; mean share of graphics RSP time left on the JIT: %.1f%%" % (
        sum(100.0 * r[3] / r[2] for r in rows if r[2]) / ng, sum(r[6] for r in rows) / ng))
    for lb in ("test", "unseen", "not-coded"):
        rr = [r for r in rows if setlabel(r[0]) == lb]
        if rr:
            P("  %s (%d games): %d tasks, %d native; mean native share %.1f%%; mean JIT share %.1f%%" % (
                lb, len(rr), sum(r[2] for r in rr), sum(r[3] for r in rr), sum(100.0 * r[3] / r[2] for r in rr if r[2]) / len(rr), sum(r[6] for r in rr) / len(rr)))
    P("")
    if md:
        P("| set | game | gfx tasks | native | declined | no handler | graphics RSP time on the JIT | ucodes (task share, state) |")
        P("|---|---|---|---|---|---|---|---|")
    for st, g, t, nat, dec, nh, share, us in rows:
        parts = []
        for u in sorted(us.values(), key=lambda u: -u["tasks"]):
            k = kind(u)
            lbl = "%s `%s` %d%% %s" % (short(u["name"]), u["text"][:4], round(100.0 * u["tasks"] / t) if t else 0, {"native": "N", "no handler": "-", "handler, always declines": "D", "partly native": "P"}[k])
            parts.append(lbl)
        if md:
            P("| %s | %s | %d | %d | %d | %d | %.0f%% | %s |" % (setlabel(st), g, t, nat, dec, nh, share, "; ".join(parts)))
        else:
            P("%-45s %7d %7d %6d %6d %5.1f%%  %s" % (g, t, nat, dec, nh, share, "; ".join(parts)))
    P("")
    tot_r = collections.Counter()
    tot_nh = 0
    for us0 in games.values():
        for u in us0.values():
            for n, w in u["reasons"]:
                tot_r[w] += n
            tot_nh += u["nohandler"]
    P("tasks by outcome: native %d, declined by a handler %d, no handler %d (of %d)" % (tn, sum(tot_r.values()), tot_nh, tt))
    P("decline reasons (tasks, games): ")
    if md:
        P("| reason | tasks | games |")
        P("|---|---|---|")
    gcount = collections.defaultdict(set)
    for (st, g), us0 in games.items():
        for u in us0.values():
            for n, w in u["reasons"]:
                gcount[w].add(g)
    for w, n in tot_r.most_common():
        if md:
            P("| %s | %d | %d |" % (w, n, len(gcount[w])))
        else:
            P("  %6d  %2d games  %s" % (n, len(gcount[w]), w))
    P("")
    P("hot list (non-native ucodes ranked by the mean share of a game's graphics RSP time they keep on the JIT, over all %d games)" % ng)
    if md:
        P("| # | ucode (4 KiB text) | family | games | mean RSP time on JIT | mean task share | state | reasons |")
        P("|---|---|---|---|---|---|---|---|")
    for i, h in enumerate(hl, 1):
        rs = "; ".join("%s (%d)" % (w, n) for w, n in h["reasons"].most_common(3)) or "no build registered"
        ks = ", ".join("%s x%d" % (k, n) for k, n in h["kinds"].items())
        if md:
            P("| %d | %s `%s` | %s | %d | %.2f%% | %.2f%% | %s | %s |" % (i, short(h["name"]), h["text"][:8], FAM[h["fam"]], len(h["games"]), h["share_sum"] / ng, h["task_share_sum"] / ng, ks, rs))
        else:
            P("%2d %-36s %s %-6s games=%2d rsp=%5.2f%% tasks=%5.2f%%  %s | %s" % (i, short(h["name"]), h["text"][:8], FAM[h["fam"]], len(h["games"]), h["share_sum"] / ng, h["task_share_sum"] / ng, ks, rs))
    P("")
    P("games per hot ucode:")
    for h in hl:
        P("%s %s: %s" % (h["text"][:8], short(h["name"]), ", ".join(sorted(set(h["games"])))))
    print("\n".join(out))


if __name__ == "__main__":
    main()
