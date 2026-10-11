"""profdata.py: loaders for the files of a profiling run (docs/profiling.md): <run>/rspt, frames, prof/{info,keys,stats,tasks,map,sites,counts.*}.

A run directory is what tools/session/runner.sh leaves per TAG (fetched to test-output/session/<dev>/<label>/<TAG>).
"""
import collections
import os
import re


def read_marks(run):
    """MARK lines of the shim: [(calls, rsp_ms, emu_thread_cpu_ms, process_cpu_ms)] in list order"""
    r = []
    p = os.path.join(run, "rspt")
    if os.path.exists(p):
        for l in open(p):
            if l.startswith("MARK"):
                f = l.split()
                r.append((int(f[1]), float(f[2]), float(f[3]), float(f[4])))
    return r


def read_frames(run):
    return [int(x) for x in open(os.path.join(run, "frames")).read().strip().split(",")]


def read_info(prof):
    d = {}
    for l in open(os.path.join(prof, "info")):
        p = l.split()
        if len(p) >= 2 and p[0] != "mark":
            d[p[0]] = float(p[1])
    return d


def read_keys(prof):
    keys = {}
    for l in open(os.path.join(prof, "keys")):
        m = re.match(r"K (\d+) (\d+) ([0-9a-f]+) (\d+) \| ?(.*)", l.strip())
        if m:
            keys[int(m.group(1))] = dict(type=int(m.group(2)), id=m.group(3), family=int(m.group(4)), name=m.group(5))
    return keys


def read_stats(prof):
    """label -> {key index: {'calls','tasks','resumes','t_total', counter..., 'rdp_ops', 'walk', 'walk_clean', 'ops', 'cops', 'cls'}}"""
    S = collections.OrderedDict()
    cur = None
    for l in open(os.path.join(prof, "stats")):
        p = l.split()
        if not p:
            continue
        if p[0] == "S":
            cur = p[1]
            S[cur] = {"_ns": float(p[2]), "_wall": float(p[3])}
            continue
        if cur is None:
            continue
        k = int(p[1])
        e = S[cur].setdefault(k, {})
        if p[0] == "k":
            e.update(calls=int(p[2]), tasks=int(p[3]), resumes=int(p[4]), t_total=float(p[5]))
        elif p[0] == "c":
            for t in p[2:]:
                a, b = t.split("=")
                e[a] = float(b)
        elif p[0] in ("w", "u"):
            d = {t.split("=")[0]: int(t.split("=")[1]) for t in p[2:]}
            e["walk" if p[0] == "w" else "walk_clean"] = d
        elif p[0] in ("o", "q", "r"):
            d = {}
            for t in p[2:]:
                a, b = t.split(":")
                d[int(a, 16)] = int(b)
            e[{"o": "ops", "q": "cops", "r": "rdp_ops"}[p[0]]] = d
        elif p[0] == "x":
            e["cls"] = {t.split("=")[0]: int(t.split("=")[1]) for t in p[2:]}
        elif p[0] == "g":                       # callback histogram: g key has_triangle n:ns per size bin
            t = int(p[2])
            nn = e.setdefault("cbh_n", {})
            ns = e.setdefault("cbh_ns", {})
            for b, tok in enumerate(p[3:]):
                n_, ns_ = tok.split(":")
                nn[t * 16 + b] = int(n_)
                ns[t * 16 + b] = float(ns_)
        elif p[0] == "d":                       # callback duration histogram: d key size_bin has_triangle dur_bin:n ... (dur_bin b: ticks in [2^(b-1), 2^b), 0: no tick)
            dd = e.setdefault("cbd", {})
            for tok in p[4:]:
                j, n_ = tok.split(":")
                dd[(int(p[2]) * 2 + int(p[3])) * 32 + int(j)] = int(n_)
        elif p[0] == "h":
            e["vhist"] = {int(t.split(":")[0]): int(t.split(":")[1]) for t in p[2:]}
    return S


def window(S, m0, m1, key):
    """difference of every counter of a key between two snapshots (an absent key in m0 counts as 0)"""
    a, b = S[m0].get(key, {}), S[m1].get(key, {})
    d = {}
    for k, v in b.items():
        if isinstance(v, (int, float)):
            d[k] = v - a.get(k, 0)
        elif isinstance(v, dict):
            w = a.get(k, {})
            d[k] = {x: y - w.get(x, 0) for x, y in v.items()}
    return d


TASK_COLS = None


def read_tasks(prof):
    """task log rows as dicts: key nth phase fresh yield t_total_ns ... (see tasklog.cpp), w_* (None when not walked), dyn, class counts"""
    rows = []
    cols = None
    for l in open(os.path.join(prof, "tasks")):
        if l.startswith("#"):
            parts = l[1:].split("|")
            c1 = parts[0].split()
            c2 = parts[1].split()
            c3 = parts[2].split()
            cols = (c1, c2, c3)
            continue
        if not l.endswith("\n"):
            continue            # a cut last line
        parts = l.split("|")
        if len(parts) != 3 or cols is None:
            continue
        r = {}
        for names, vals in zip(cols, parts):
            v = vals.split()
            if len(v) != len(names):
                r = None
                break
            for n, x in zip(names, v):
                r[n] = None if x == "-" else float(x)
        if r:
            rows.append(r)
    return rows


def gfx_keys(keys):
    return [k for k, v in keys.items() if v["type"] == 1]
