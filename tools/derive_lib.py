"""Shared pieces of tools/derive_build.py: the hand-written build tables of src/hle read back from the C sources, the ucode corpus (dumps of
tools/ucode_dump_shim.c), the image model of a microcode (text + overlays) and the alignment of two images."""
import difflib
import glob
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rsp_isa as isa  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src", "hle")


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
# the C tables
# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", " ", s, flags=re.S)
    return re.sub(r"//[^\n]*", " ", s)


def split_top(s, sep=","):
    out, depth, cur, q = [], 0, [], None
    for ch in s:
        if q:
            cur.append(ch)
            if ch == q:
                q = None
            continue
        if ch in "\"'":
            q = ch
            cur.append(ch)
        elif ch in "({[":
            depth += 1
            cur.append(ch)
        elif ch in ")}]":
            depth -= 1
            cur.append(ch)
        elif ch == sep and depth == 0:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    t = "".join(cur).strip()
    if t:
        out.append(t)
    return out


def match_brace(s, i):
    """s[i] == '{': index of the matching '}'"""
    depth = 0
    for k in range(i, len(s)):
        if s[k] == "{":
            depth += 1
        elif s[k] == "}":
            depth -= 1
            if depth == 0:
                return k
    raise ValueError("unbalanced")


class CTables:
    """The structs and arrays of the hand-written build tables, as python values. Expressions are evaluated with the #defines/enums of the sources; MUT(n, ok, bad) is ok."""

    def __init__(self, srcdir=SRC):
        self.consts = {"NULL": 0, "K_EXT": 0x40}
        self.vars = {}      # name -> (type, fields dict or list)
        self.ucodes = []    # HLE_UCODE entries
        self.text = ""
        for fn in ("hle_f3dex2_int.h", "f3dex2_dmem.h", "hle.h", "hle_f3dex2.c", "hle_f3dex2_dialects.c", "hle_s2dex2.c", "f3dex.c", "f3d.c", "f3d_dmem.h", "f3dex_vtx_fx.c", "f3d_fx_int.h", "f3d_fx.h"):
            p = os.path.join(srcdir, fn)
            if os.path.exists(p):
                self.text += strip_comments(open(p).read()) + "\n"
        self._defines()
        self._enums()
        self._structs()
        self._ucodes()

    def ev(self, e):
        e = e.strip()
        m = re.fullmatch(r"MUT\((.*)\)", e, flags=re.S)
        if m:
            a = split_top(m.group(1))
            return self.ev(a[1])
        if e.startswith("&"):
            return ("ref", e[1:].strip().strip("()"))
        if e.startswith('"'):
            return e.strip('"')
        if e.startswith("{"):
            inner = e[1:e.rindex("}")]
            return [self.ev(x) for x in split_top(inner)]
        e2 = re.sub(r"\b(0x[0-9a-fA-F]+|\d+)[uU]?[lL]*\b", lambda m: m.group(1), e)
        e2 = re.sub(r"\(\s*(?:u?int\d+_t|unsigned)\s*\)", "", e2)
        names = re.findall(r"[A-Za-z_]\w*", re.sub(r"0x[0-9a-fA-F]+", "0", e2))
        env = {}
        for n in names:
            if n in self.consts:
                env[n] = self.consts[n]
            else:
                return ("sym", e)
        try:
            return eval(e2, {"__builtins__": {}}, env)
        except Exception:
            return ("sym", e)

    def _defines(self):
        for m in re.finditer(r"^\s*#define\s+(\w+)\s+(\(.*\)|0x[0-9a-fA-F]+u?|\d+u?)\s*$", self.text, flags=re.M):
            v = self.ev(m.group(2))
            if isinstance(v, int):
                self.consts[m.group(1)] = v

    def _enums(self):
        for m in re.finditer(r"enum\s*(\w*)\s*\{(.*?)\}", self.text, flags=re.S):
            cur = -1
            for item in split_top(m.group(2)):
                if "=" in item:
                    n, e = [x.strip() for x in item.split("=", 1)]
                    v = self.ev(e)
                    cur = v if isinstance(v, int) else cur + 1
                else:
                    n = item.strip()
                    cur += 1
                if re.fullmatch(r"\w+", n):
                    self.consts[n] = cur

    def _structs(self):
        for m in re.finditer(r"(?:static\s+)?const\s+(?:struct\s+(\w+)|uint\d+_t)\s+(\w+)((?:\[[^\]]*\])*)\s*=\s*\{", self.text):
            typ, name, dims = m.group(1), m.group(2), m.group(3)
            i = self.text.index("{", m.end() - 1)
            j = match_brace(self.text, i)
            body = self.text[i + 1:j]
            items = split_top(body)
            if dims or typ is None:
                self.vars[name] = (typ, [self.ev(x) for x in items])
            elif items and items[0].startswith("."):
                f = {}
                for it in items:
                    mm = re.match(r"\.(\w+)\s*=\s*(.*)$", it, flags=re.S)
                    if mm:
                        f[mm.group(1)] = self.ev(mm.group(2))
                self.vars[name] = (typ, f)
            else:
                self.vars[name] = (typ, [self.ev(x) for x in items])

    def _ucodes(self):
        for m in re.finditer(r"HLE_UCODE\((\w+)\)\s*=\s*\{", self.text):
            i = self.text.index("{", m.end() - 1)
            j = match_brace(self.text, i)
            body = self.text[i + 1:j]
            f = {}
            for it in split_top(body):
                mm = re.match(r"\.(\w+)\s*=\s*(.*)$", it, flags=re.S)
                if mm:
                    f[mm.group(1)] = mm.group(2).strip()
            if m.group(1) != "var":
                self.ucodes.append((m.group(1), f))
        # macro forms: U(var, id, name, bld), F3D_UCODE(...), F3D_UCODE_R(...)
        for m in re.finditer(r"^\s*(U|F3D_UCODE|F3D_UCODE_R)\((\w+),\s*(0x[0-9a-fA-F]+)ull,\s*\"([^\"]*)\",\s*(\w+)", self.text, flags=re.M):
            self.ucodes.append((m.group(2), {"id": m.group(3) + "ull", "name": '"%s"' % m.group(4), "fam_data": "&" + m.group(5), "_macro": m.group(1)}))

    def get(self, name):
        return self.vars[name][1]


def uhex(s):
    return int(re.sub(r"[uUlL]+$", "", s.strip()), 0)


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
# the corpus
# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
def fnv(b, n):
    x = 0xcbf29ce484222325
    for i in range(0, n, 4):
        x = ((x ^ struct.unpack(">I", b[i:i + 4])[0]) * 0x100000001b3) & 0xffffffffffffffff
    return x


class Ucode:
    """One (text, data) pair of a dump directory. text: 4 KiB at IMEM 0x80 (wrapping), ext: the 8 KiB of RDRAM behind it."""

    def __init__(self, path):
        self.path = path
        base = path[:-5]
        self.stem = os.path.basename(base)
        self.text_b = open(path, "rb").read()
        self.data = open(base + ".data", "rb").read()
        self.ext = open(base + ".ext", "rb").read() if os.path.exists(base + ".ext") else b""
        self.loaded = self.stem.startswith("L")
        self.w = isa.words(self.text_b)
        self.h4k = fnv(self.text_b, 0x1000)
        self.hf80 = fnv(self.text_b, 0xf80)
        self.hf48 = fnv(self.text_b, 0xf48)
        self.games = {}
        self.banner = banner_of(self.text_b, self.data)

    def ids(self):
        # the ucode id is the FNV of the whole 4 KiB (src/gfx_ucode.c, since 2026-10-16); hf80 is the id of dumps taken before, hf48 the key of a G_LOAD_UCODE
        if self.loaded:     # (a text a G_LOAD_UCODE loads is imaged differently from a task start's: only its load key names it)
            return {self.hf48}
        return {self.h4k, self.hf80, self.hf48}


def banner_of(text, data):
    out = []
    for b in (text, data):
        for m in re.finditer(rb"[ -~]{12,}", b):
            s = m.group(0).decode()
            if "Gfx" in s or "SW Version" in s or "S2DEX" in s:
                out.append(s.strip())
    return " | ".join(dict.fromkeys(out))[:160]


class Corpus:
    def __init__(self, dumpdir):
        self.by_stem = {}
        for sub in sorted(glob.glob(os.path.join(dumpdir, "*", "*"))):
            d = os.path.join(sub, "dump")
            game = os.path.basename(sub)
            tasks = {}
            seq = os.path.join(d, "seq.txt")
            if os.path.exists(seq):
                for ln in open(seq):
                    f = ln.split()
                    if len(f) >= 2:
                        tasks[f[1]] = tasks.get(f[1], 0) + 1
            for p in sorted(glob.glob(os.path.join(d, "*.text"))):
                stem = os.path.basename(p)[:-5]
                u = self.by_stem.get(stem)
                if u is None:
                    u = self.by_stem[stem] = Ucode(p)
                if stem in tasks:
                    u.games[game] = tasks[stem]
                else:
                    u.games.setdefault(game, 0)
        self.items = list(self.by_stem.values())
        self.by_id = {}
        for u in self.items:
            for h in u.ids():
                self.by_id.setdefault(h, []).append(u)

    def find_id(self, i, prefer_data=None):
        c = self.by_id.get(i, [])
        if not c:
            return None
        # the text with the most tasks (the first data image of a text is not always the pristine one: the dump has one per distinct data)
        return sorted(c, key=lambda u: -sum(u.games.values()))[0]

    def distinct_texts(self):
        """one Ucode per distinct 4 KiB text (the data images differ in DMEM state): the one with the most tasks"""
        m = {}
        for u in self.items:
            m.setdefault(u.h4k, []).append(u)
        return [sorted(v, key=lambda u: (-sum(u.games.values()), u.stem))[0] for v in m.values()]


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
# images and alignment
# ---------------------------------------------------------------------------------------------------------------------------------------------------------------
class Image:
    """The code of a microcode: the 4 KiB text (word i at IMEM (0x80 + 4 i) & 0xfff) followed by the overlay segments whose bytes lie behind the text (RDRAM text + off, off >= 0x1000).
    seg[i] = (segment index) for every word; pc[i] = its IMEM address."""

    def __init__(self, uc, overlays=()):
        self.uc = uc
        self.w = list(uc.w)
        self.pc = [(0x80 + 4 * i) & 0xfff for i in range(len(self.w))]
        self.seg = [0] * len(self.w)
        self.segs = [("text", 0x80, 0, len(self.w))]       # name, imem base, first word, n words
        for (off, imem, ln) in overlays:
            if off < 0x1000 or off - 0x1000 + ln > len(uc.ext):
                continue
            first = len(self.w)
            ws = isa.words(uc.ext[off - 0x1000:off - 0x1000 + ln])
            self.w += ws
            self.pc += [imem + 4 * k for k in range(len(ws))]
            self.seg += [len(self.segs)] * len(ws)
            self.segs.append(("ovl@%03x+%x" % (imem, off), imem, first, len(ws)))
        self.n = len(self.w)
        self.active = None          # segment indices that code can enter (None: all); set once the layout says which descriptors are overlays of this build

    def idx_text(self, pc):
        return ((pc - 0x80) & 0xfff) // 4

    def resolve(self, pc, cur_seg=0):
        """word indices an IMEM address can mean from code in segment cur_seg: its own segment first, else the text, else the overlays covering it"""
        pc &= 0xfff
        out = []
        s = self.segs[cur_seg]
        if cur_seg and s[1] <= pc < s[1] + 4 * s[3]:
            return [s[2] + (pc - s[1]) // 4]
        out.append(self.idx_text(pc))
        for k in range(1, len(self.segs)):
            s = self.segs[k]
            if self.active is not None and k not in self.active:
                continue
            if s[1] <= pc < s[1] + 4 * s[3]:
                out.append(s[2] + (pc - s[1]) // 4)
        return out


def find_descriptors(data):
    """overlay descriptors in a data segment: (text offset, length register << 16 | 0x1000 | imem). -> [(off, imem, len)]"""
    out = []
    for a in range(0, min(len(data), 0xe00) - 4, 4):
        o, b = struct.unpack(">II", data[a:a + 8])
        if (b & 0xf000) == 0x1000 and 0x1f <= (b >> 16) <= 0x7ff and o <= 0x3000 and (o & 7) == 0:
            imem = b & 0xfff
            if 0x100 <= imem <= 0x700 and (imem & 7) == 0:
                out.append((o, imem, (b >> 16) + 1))
    return out


def wkey(w):
    d = isa.decode(w)
    c = d["cls"]
    if c == isa.C_J:
        return (d["name"],)
    if c == isa.C_BR:
        return (d["name"], d.get("rs"), d.get("rt"))
    if c == isa.C_MEM:
        return (d["name"], d["rs"], d["rt"])
    if c == isa.C_VMEM:
        return (d["name"], d["rs"], d["vt"], d["elem"], d["sub"])
    if c == isa.C_ALUI:
        return (d["name"], d["rs"], d["rt"])
    return (w,)


def align_keys(ka, kb, minblock=3):
    """Greedy block alignment that tolerates moved blocks: SequenceMatcher on everything, then again on the leftovers of both sides (their relative order kept) until nothing new of
    at least minblock words matches. -> {index in a: index in b}"""
    pairs = {}
    ia = list(range(len(ka)))
    ib = list(range(len(kb)))
    first = True
    while ia and ib:
        sm = difflib.SequenceMatcher(None, [ka[i] for i in ia], [kb[j] for j in ib], autojunk=False)
        got = 0
        for blk in sm.get_matching_blocks():
            if blk.size < (1 if first else minblock):
                continue
            for k in range(blk.size):
                pairs[ia[blk.a + k]] = ib[blk.b + k]
            got += blk.size
        first = False
        if not got:
            break
        ua, ub = set(pairs), set(pairs.values())
        ia = [i for i in range(len(ka)) if i not in ua]
        ib = [j for j in range(len(kb)) if j not in ub]
    return pairs
