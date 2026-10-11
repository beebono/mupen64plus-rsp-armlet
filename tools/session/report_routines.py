#!/usr/bin/env python3
"""report_routines.py: the routine-share tables of docs/profiling.md, from the level-3 runs of the F sessions.

  report_routines.py SESSIONDIR COST.json OUTDIR GAME...

For each game it runs ucodemap on F.<game>/<game>.cnt/prof (window = the last two marks) with the cost model (the `pooled` entry of COST.json,
costmodel.py), writes OUTDIR/<game>.map.json (read by perfmap.py and hlemodel.py) and prints Markdown: the share of the graphics JIT time per category
(cost-weighted and as plain instruction counts), the residual, and the walker validation.
"""
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ucodemap"))
import ucodemap as um

NAMES = {"banjo": "Banjo-Kazooie", "dk64": "Donkey Kong 64", "oot": "Ocarina of Time", "mk64": "Mario Kart 64 (menu)", "mk64r": "Mario Kart 64 (race)", "pm": "Paper Mario", "sm64": "Super Mario 64",
         "snap": "Pokemon Snap (zoom shot)"}
FAMS = {1: "F3D", 2: "F3DEX 1.x", 3: "F3DEX2"}
CATS = ["vertex", "triangle", "matrix", "dl", "state", "rdp", "dispatch", "dma", "rdp-submit", "task init", "unmapped"]


def run(sdir, cost, outdir, game):
    prof = os.path.join(sdir, "F." + game, game + ".cnt", "prof")
    keys = um.read_keys(prof)
    stats = um.read_stats(prof)
    labels = [l for l in stats if re.match(r"M\d+$", l)]
    m0, m1 = labels[-2], labels[-1]
    c0 = um.Counts(os.path.join(prof, "counts." + m0))
    c1 = um.Counts(os.path.join(prof, "counts." + m1))
    rspdis = os.path.join(os.path.dirname(um.__file__), "rspdis")
    sites = um.read_sites(prof, rspdis)
    out = []
    for k, ki in keys.items():
        if ki["type"] != 1:
            continue
        r = um.analyse(prof, k, ki, sites, c0, c1, dict(stats=stats, m0=m0, m1=m1, cost=cost))
        if r:
            out.append(r)
    json.dump(out, open(os.path.join(outdir, game + ".map.json"), "w"), indent=1)
    return out, m0, m1


def main():
    sdir, costf, outdir = sys.argv[1:4]
    games = sys.argv[4:]
    os.makedirs(outdir, exist_ok=True)
    cost_all = json.load(open(costf))
    cost = {k: v for k, v in cost_all["pooled"].items() if not k.startswith("_")}
    share_rows, count_rows, val_rows, own_tabs = [], [], [], []
    for g in games:
        try:
            res, m0, m1 = run(sdir, cost, outdir, g)
        except Exception as e:                         # a game without level-3 data
            print("<!-- %s: %s -->" % (g, e))
            continue
        for r in res:
            if "error" in r:
                print("<!-- %s: %s -->" % (g, r["error"]))
                continue
            cats = r["categories"]
            tc = sum(v["cost"] for v in cats.values()) or 1
            ti = r["total_instr"]
            fam = FAMS.get(r["family"], "?")
            share_rows.append("| %s | %s (%s) | %s | %.2f |" % (NAMES.get(g, g), fam, r["id"], " | ".join(("%.1f" % (100 * cats[c]["cost"] / tc)) if c in cats else "0" for c in CATS),
                                                                  100 * r["unattributed_instr"] / ti))
            count_rows.append("| %s | %s |" % (NAMES.get(g, g), " | ".join(("%.1f" % (100 * cats[c]["instr"] / ti)) if c in cats else "0" for c in CATS)))
            v = r["validation"]
            w = r["walker"]
            if "per_handler" in v:
                ph = v["per_handler"]
                bad = [h for h in ph if h["walker"] != h["entries"]]
                chk = "table in DMEM (%s), %d of %d handlers: entries == walker count" % (r["table"]["basis"], len(ph) - len(bad), len(ph))
                if bad:
                    chk += "; off: " + ", ".join("%s %d vs %d" % (h["handler"], h["entries"], h["walker"]) for h in bad[:3])
            else:
                po = [x for x in v["per_owner_matched"]]
                match = sum(1 for x in po if x["walker"] == x["entries"])
                chk = "entry counts matched to walker opcode counts for %d of %d owners (>= 0.1 %% of commands)" % (match, len(po))
            val_rows.append("| %s | %d | %d (%.2f %%) | %d | %s | %s |" % (
                NAMES.get(g, g), v["j1_count"], v["walker_cmds_all_tasks"], 100 * (v["j1_count"] / v["walker_cmds_all_tasks"] - 1) if v["walker_cmds_all_tasks"] else 0,
                v["owner_entries_total"], "%d of %d tasks clean: %d == %d" % (w["clean_tasks"], w["tasks"], v["j1_count_clean_tasks"], v["walker_cmds_clean_tasks"])
                if w["clean_tasks"] else "no clean task in the window", chk))
            t = ["", "**%s**, window %s..%s, %d instructions, ucode `%s`: %s" % (NAMES.get(g, g), m0, m1, ti, r["id"], r["name"][:70]),
                 "| owner | category | share of JIT time | instr | entries | instr per entry |", "|---|---|---|---|---|---|"]
            for o, x in list(r["owners"].items())[:12]:
                t.append("| %s | %s | %.1f %% | %.1f %% | %s | %s |" % (x["name"], x["category"], 100 * x["cost"] / tc, 100 * x["instr"] / ti, x["entries"] or "", ("%.0f" % x["instr_per_entry"]) if x["instr_per_entry"] else ""))
            own_tabs.append("\n".join(t))
    print("| game | ucode | " + " | ".join(CATS) + " | unattributed |")
    print("|---|---|" + "---|" * (len(CATS) + 1))
    print("\n".join(share_rows))
    print("\nplain instruction counts (%):\n")
    print("| game | " + " | ".join(CATS) + " |")
    print("|---|" + "---|" * len(CATS))
    print("\n".join(count_rows))
    print("\nwalker validation:\n")
    print("| game | J1 (dispatch jump) executions | walker commands, all tasks | owner entries | clean tasks | handler check |")
    print("|---|---|---|---|---|---|")
    print("\n".join(val_rows))
    print("\n".join(own_tabs))


if __name__ == "__main__":
    main()
