#!/usr/bin/env python3
"""synth_indy.py OUTDIR N SEED TEMPLATE.snap : N synthetic task snapshots of the Indiana Jones microcode, for armlet-native-replay --oracle (docs/native-custom.md, docs/testing.md).

The template (a captured task start, ARMLET_HLE_CAPTURE) supplies the microcode's loader and the register file; the display list, and everything it names, is made up: chunks of 32 commands
(0x108 bytes each, linked) with a random scene of matrices, viewports, fog, offset vectors, vertex loads (from a DMA address, from the stream, by index), colour tables (32-bit, 5-6-5, scaled, lit by
ambient / directional / point lights), triangles and quads (every colour and texture word variant, environment mapped coordinates, skip counters), near-clipping triangles, other modes, texture
rectangles, nested and branched lists, a deferred list, small FIFOs that wrap. The overlays (clipper, lighting, environment map, indexed fetch) come from INDY_TEXT (the 0x3000 bytes of the text as
the game's RDRAM holds it: ARMLET_UCODES=1 with HLE_CUSTOM_DUMP of an earlier build, or any dump of RDRAM at OSTask.ucode). The replay runs the native front-end and the JIT from the same state, so
every difference between them is an error of the front-end. Snapshots are ROM-derived: OUTDIR belongs in test-output/."""
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import synth                                           # the snapshot layout (HDR, DMEM_OFF, swap32)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "native"))
import snaplib

HDR, HSIZE, SNAP, DMEM_OFF, swap32 = synth.HDR, synth.HSIZE, synth.SNAP, synth.DMEM_OFF, synth.swap32
OST_DATA, OST_OUT, OST_OUT_END = 0xff0, 0xfe8, 0xfec
CMD_SLOTS = 32
OFF = set(filter(None, os.environ.get("SYN_OFF", "").split(",")))      # features left out


def w(w0, w1):
    return struct.pack(">II", w0 & 0xffffffff, w1 & 0xffffffff)


class Task:
    def __init__(self, rng):
        self.r = rng
        self.rows = []
        self.next = 0x500000
        self.cmds = []                       # list of commands, each a list of 8-byte slots
        self.nv = 0
        self.ncol = 0

    def alloc(self, data, align=8):
        a = (self.next + align - 1) & ~(align - 1)
        data = bytes(data) + bytes(-len(data) & 7)
        self.rows.append((a, data))
        self.next = a + len(data) + 64
        return a

    def add(self, *slots):
        self.cmds.append(list(slots))

    # ---- commands --------------------------------------------------------------------------------
    def load(self, dst, data, typ=0, bit22=0, bit23=0, count=0, stream=False):
        """op 1: the DMA of len(data) bytes to DMEM dst (from a fresh address, or from the stream), then the type"""
        data = bytes(data) + bytes(-len(data) & 7)
        n = len(data)
        a = 0 if stream else self.alloc(data + bytes(16), 8)
        w0 = 1 << 24 | bit23 << 23 | bit22 << 22 | typ << 20 | (dst & 0xfff) << 8 | (n - 1)
        self.add(w(w0, count << 24 | a))
        return a

    def moveword(self, dst, value):
        self.add(w(0xbc << 24 | dst & 0xfff, value))

    def geom(self, clear, setb):
        self.add(w(0xb6 << 24, clear))
        self.add(w(0xb7 << 24, setb))

    def rmtx(self, mode):
        r = self.r
        m = bytearray(64)
        for row in range(4):
            for col in range(4):
                if mode == 0:
                    iv = r.choice([1, 2, 4, 0, 0, 0, -1]) if row == col else (0 if r.random() < 0.8 else r.randint(-3, 3))
                    fv = r.getrandbits(16) if r.random() < 0.5 else 0
                elif mode == 1:
                    iv = r.randint(-300, 300); fv = r.getrandbits(16)
                elif mode == 2:
                    iv = r.choice([-32768, 32767, 0, 1, -1, 16384]); fv = r.choice([0, 0xffff, 0x8000, r.getrandbits(16)])
                else:                       # perspective-like: unit scale, W = z + d
                    iv = 1 if row == col and col < 3 else 0
                    fv = 0
                    if col == 3 and row == 2:
                        iv = 1
                    if col == 3 and row == 3:
                        iv = r.randint(300, 3000)
                    if row == 3 and col < 3 and r.random() < 0.3:
                        iv = r.randint(-50, 50)
                    if r.random() < 0.15 and row != 3:
                        fv = r.getrandbits(16)
                struct.pack_into(">h", m, 8 * row + 2 * col, iv)
                struct.pack_into(">H", m, 32 + 8 * row + 2 * col, fv)
        return bytes(m)

    def positions(self, n, mode):
        r = self.r
        out = bytearray()
        for i in range(n):
            if mode == 0:
                p = [r.randint(-100, 100) for _ in range(3)]
            elif mode == 1:
                p = [r.randint(-3000, 3000) for _ in range(3)]
            elif mode == 2:
                p = [r.choice([-32768, 32767, r.randint(-32768, 32767), 0]) for _ in range(3)]
            else:
                p = [r.randint(-900, 900) for _ in range(3)]
            out += struct.pack(">hhh", *p) + struct.pack(">H", r.getrandbits(16))
        return bytes(out)


def vertex_block(t, n, mode, how):
    """vertices into the records 0 .. n - 1: how 0 = DMA, 1 = from the stream, 2 = by index"""
    r = t.r
    if how == 2:
        # the table of 6-byte positions at DMEM[0x154] (an RDRAM address), the 16-bit indices at DMEM 0x170
        tab = t.positions(24, mode)
        tab6 = b"".join(tab[8 * i:8 * i + 6] for i in range(24)) + bytes(16)
        base = t.alloc(tab6, 8)
        t.moveword(0x154, base)
        idx = b"".join(struct.pack(">H", r.randrange(24)) for _ in range(n)) + bytes(16)
        t.load(0x170, idx[:2 * n + (-2 * n & 7)], typ=1, bit22=1, count=n)
        return
    data = t.positions(n, mode)
    if how == 1:
        chunk = t.alloc(bytes(8) + data + bytes(256), 8)
        t.add(w(2 << 24 | 8 * n, chunk))
        t.load(0x170, data, typ=1, count=n, stream=True)
    else:
        t.load(0x170, data, typ=1, count=n)


def colour_block(t, nent, kind):
    """a colour table of nent entries: kind 0 = scaled copy, 1 = 5-6-5 scaled, 2 = lit"""
    r = t.r
    if kind == 1:
        ent = b"".join(struct.pack(">H", r.getrandbits(16)) for _ in range(nent))
    else:
        ent = bytes(r.getrandbits(8) for _ in range(4 * nent))
    ent += bytes(-len(ent) & 15)
    t.moveword(0x158, r.getrandbits(32) | 0x80808080 if r.random() < 0.5 else r.getrandbits(32))
    t.moveword(0x15c, r.getrandbits(32) | 0x80808080 if r.random() < 0.5 else r.getrandbits(32))
    if kind == 2:
        nl = r.randint(0, 4)
        lights = bytearray()
        for k in range(nl):
            typ = r.choice([0, 1, 1, 2])
            d0 = struct.pack(">hhhh", *[r.randint(-8000, 8000) for _ in range(3)], 0)
            d1 = struct.pack(">hhhh", *[r.randint(-8000, 8000) for _ in range(3)], 0)
            col = bytes([r.getrandbits(8) for _ in range(3)]) + bytes([typ])
            att = struct.pack(">hH", r.randint(0, 3), r.getrandbits(16))
            lights += d0 + d1 + col + att
        if nl:
            t.load(0xb00, bytes(lights) + bytes(-len(lights) & 7), typ=0)
        t.add(w(1 << 24 | 3 << 20, nl << 24))                                              # the byte at 0x58b: the number of lights
        nrm = bytes(r.getrandbits(8) for _ in range(8 * ((nent + 1) // 2) + 8))
        t.load(0x380, nrm, typ=0)
        t.moveword(0x5b0, r.choice([0, r.getrandbits(32)]))
        t.moveword(0x5b4, r.choice([0, r.getrandbits(32)]))
    else:
        t.add(w(1 << 24 | 3 << 20, 0xff << 24))                                             # a negative byte at 0x58b: the scaled copy
    t.load(0x480, ent, typ=2, bit23=1 if kind == 1 else 0, count=nent | (0x80 if kind != 2 else 0))


def tri_block(t, nv, nent, fog, ntri, clip):
    r = t.r
    for _ in range(ntri):
        quad = r.random() < 0.35
        w0 = (0xb4 if quad else 0xbf) << 24
        tex = quad or r.random() < 0.6
        env = tex and r.random() < 0.2 and "env" not in OFF
        w0 |= (512 if tex else 0) | (2048 if env else 0) | r.choice([0, 0, 1, 2, 3])
        a = [0x600 + 40 * r.randrange(nv) for _ in range(4)]
        if r.random() < 0.1:
            a[1] = a[0]
        ci = [4 * r.randrange(max(nent, 1)) for _ in range(4)]
        s0 = w(w0, a[0] << 16 | a[1])
        s1 = w(ci[0] << 24 | ci[1] << 16 | ci[2] << 8 | ci[3], a[2] << 16 | (a[3] if quad else 0))
        slots = [s0, s1]
        if tex:
            if env:
                words = [r.randrange(0, 64) & 0xf8 for _ in range(4)]
                slots += [w(words[0], words[1]), w(words[2], words[3])]
            else:
                slots += [w(r.getrandbits(32), r.getrandbits(32)), w(r.getrandbits(32), r.getrandbits(32))]
        t.add(*slots)
        if r.random() < 0.3:
            t.add(w(r.choice([0xe6, 0xe7, 0xf5, 0xfc, 0xef, 0xf2]) << 24 | r.getrandbits(24), r.getrandbits(32)))


def scene(t):
    r = t.r
    g = 0x6 if r.random() < 0.92 else 0               # (the game draws shaded, textured triangles; the others decline by design)
    for bit, p in ((0x1, 0.5), (0x200, 0.5), (0x2000, 0.4), (0x10000, 0.25), (0x1000, 0.03), (0x800000, 0.5)):
        if r.random() < p:
            g |= bit
    fog = bool(g & 0x10000)
    clip = r.random() < 0.5 and not fog
    t.geom(0xffffffff, g)
    t.add(w(0xbb << 24 | r.getrandbits(8) << 8 | (2 if g & 2 else 0), r.getrandbits(32)))
    if r.random() < 0.6:
        t.add(w(0xba << 24 | 3 << 16 | 4 << 8 | 4, r.getrandbits(32) & 0xf0))
        t.add(w(0xb9 << 24 | r.getrandbits(1) << 23 | 0 << 16 | 31, r.getrandbits(32)))
    vp = [r.choice([896, 640, r.randint(1, 1000)]), r.choice([832, 480, r.randint(1, 1000)]), r.choice([0x1ff, 0x3ff, r.getrandbits(15)]), 0]
    tr = [r.choice([896, 640, r.randint(1, 1000)]), r.choice([832, 480, r.randint(1, 1000)]), r.choice([0x1ff, 0, r.getrandbits(15)]), 0]
    t.moveword(0x130, vp[0] << 16 | vp[1]); t.moveword(0x134, vp[2] << 16 | vp[3])
    t.moveword(0x138, tr[0] << 16 | tr[1]); t.moveword(0x13c, tr[2] << 16 | tr[3])
    t.moveword(0x14c, r.choice([0x670, 0x400, 0x7fff, 0x100, r.getrandbits(16)]))
    if r.random() < 0.6:
        t.moveword(0x128, (r.randint(-60, 60) & 0xffff) << 16 | r.randint(-60, 60) & 0xffff)
        t.moveword(0x12c, (r.randint(-60, 60) & 0xffff) << 16)
    else:
        t.moveword(0x128, 0); t.moveword(0x12c, 0)
    if fog:
        t.moveword(0x160, r.choice([0x13872ba0, r.getrandbits(32)]))
        t.moveword(0x164, r.choice([0xec79d460, r.getrandbits(32)]))
        t.moveword(0x168, r.choice([0x00ff0000, 0x01000000, r.getrandbits(32)]))
    mode = 3 if fog else r.choice([3, 3, 3, 3, 0, 1, 2])           # (fog and the clipper do not mix: the clipper declines; fog scenes stay inside the volume)
    t.load(0x5c0, t.rmtx(mode))
    if mode in (3,) and r.random() < 0.4 and "env" not in OFF:
        t.load(0xe40, t.rmtx(0), typ=0)
        t.moveword(0xf0, r.getrandbits(32)); t.moveword(0xf4, r.getrandbits(32))
        t.moveword(0xec, r.choice([0x00400040, r.getrandbits(32)]))
    n = r.randint(3, 24)
    how = r.choice([0, 0, 0, 1, 2]) if "stream" not in OFF else 0
    vertex_block(t, n, 0 if fog else r.choice([3, 3, 3, 1, 0, 2]) if mode != 3 else r.choice([3, 3, 3, 3, 1, 0]), how)
    nent = r.randint(2, 12)
    colour_block(t, nent, r.choice([0, 0, 1, 2]))
    if r.random() < 0.3 and "defer" not in OFF:
        # a deferred list: it runs after the next triangle's decision (and at the next flush)
        sub = [w(r.choice([0xe6, 0xf5, 0xef, 0xfc, 0xf2, 0xf3]) << 24 | r.getrandbits(24), r.getrandbits(32)) for _ in range(r.randint(1, 4))]
        sub.append(w(0xba << 24 | 3 << 16 | 4 << 8 | 4, r.getrandbits(32) & 0xf0))
        if r.random() < 0.3:                                        # (not what the game does: the list changes a vertex that the triangle is built from)
            sub.insert(0, w(0xbc << 24 | (0x600 + 40 * r.randrange(n) + 24 + 4 * r.randrange(2)), r.getrandbits(32)))
        sub.append(w(0xb8 << 24, 0xffffffff))
        sa = t.alloc(bytes(8) + b"".join(sub) + bytes(256), 8)
        t.moveword(0x58c, sa)
    tri_block(t, n, nent, fog, r.randint(1, 8), clip)
    if r.random() < 0.4:                                            # the texture command after the triangles, and a vertex load that no triangle follows
        t.add(w(0xbb << 24 | r.getrandbits(8) << 8 | r.getrandbits(2), r.getrandbits(32)))
    if r.random() < 0.3:
        vertex_block(t, r.randint(1, 6), 3, 0)
    for _ in range(r.randint(0, 5)):
        op = r.choice([0xe7, 0xe6, 0xe8, 0xf5, 0xf2, 0xf3, 0xfc, 0xef, 0xf7, 0xf6, 0xff, 0xfd, 0xe4, 0xe5, 0xf0, 0xfa])
        if op in (0xe4, 0xe5):
            t.add(w(op << 24 | r.getrandbits(24), r.getrandbits(32)), w(r.choice([0xe1, 0xf1]) << 24, r.getrandbits(32)))
        else:
            t.add(w(op << 24 | r.getrandbits(24), r.getrandbits(32)))


def pack_chunks(t, cmds):
    """the commands in 0x108-byte chunks (8 bytes of link, 32 slots); a command that does not fit ends the chunk with a refill"""
    chunks = []
    cur = []
    for c in cmds:
        if len(cur) + len(c) > CMD_SLOTS:
            if len(cur) < CMD_SLOTS:
                cur.append(w(0xb5 << 24, 0))
            chunks.append(cur)
            cur = []
        cur += c
        if len(cur) == CMD_SLOTS:
            chunks.append(cur)
            cur = []
    if cur:
        chunks.append(cur)
    return chunks


def build(rng, template, text):
    d = bytearray(open(template, "rb").read())
    magic, ver, name, ucode, entry, exit_, nrows, nev, flags = struct.unpack(HDR, bytes(d[:HSIZE]))
    t = Task(rng)
    for _ in range(rng.randint(1, 3)):
        scene(t)
    cmds = list(t.cmds)
    # a nested and a branched list: the last scene as a called list
    if rng.random() < 0.3 and len(cmds) > 4:
        k = rng.randint(2, len(cmds) - 1)
        sub, cmds = cmds[:k], cmds[k:]
        sc = pack_chunks(t, sub + [[w(0xb8 << 24, 0)]])
        addrs = [t.alloc(bytes(264), 8) for _ in sc]
        for i, ch in enumerate(sc):
            link = addrs[i + 1] if i + 1 < len(sc) else 0
            t.rows[t.rows.index(next(x for x in t.rows if x[0] == addrs[i]))] = (addrs[i], struct.pack(">II", link, 0) + b"".join(ch) + bytes(256 - 8 * len(ch)))
        cmds.insert(0, [w(6 << 24, addrs[0])])
    cmds.append([w(0xb8 << 24, 0)])
    chunks = pack_chunks(t, cmds)
    addrs = [t.alloc(bytes(264), 8) for _ in chunks]
    for i, ch in enumerate(chunks):
        link = addrs[i + 1] if i + 1 < len(chunks) else 0
        a0 = addrs[i]
        t.rows[next(j for j, x in enumerate(t.rows) if x[0] == a0)] = (a0, struct.pack(">II", link, 0) + b"".join(ch) + bytes(256 - 8 * len(ch)))
    o = HSIZE
    before = bytearray(d[o:o + SNAP])
    struct.pack_into("<I", before, DMEM_OFF + OST_DATA, addrs[0])
    if rng.random() < 0.6:                                          # a small FIFO: the output wraps
        start = struct.unpack_from("<I", before, DMEM_OFF + OST_OUT)[0]
        struct.pack_into("<I", before, DMEM_OFF + OST_OUT_END, start + 8 * rng.randint(0x10, 0x60))
    o += SNAP
    rows = bytearray()
    ro = o
    for _ in range(nrows):
        a, n = struct.unpack("<II", d[ro:ro + 8])
        rows += d[ro:ro + 8 + n]
        ro += 8 + n
    rest = d[ro:]
    if text:
        ub = struct.unpack_from("<I", before, DMEM_OFF + 0xfd0)[0] & 0x7ffff8
        buf = open(text, "rb").read()
        rows += struct.pack("<II", ub, len(buf)) + swap32(buf)
        nrows += 1
    for a, buf in t.rows:
        rows += struct.pack("<II", a, len(buf)) + swap32(buf)
    hdr = struct.pack(HDR, magic, ver, name, ucode, entry, exit_, nrows + len(t.rows), nev, flags)
    return hdr + bytes(before) + bytes(rows) + bytes(rest)


def main():
    outdir, n, seed, tmpl = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    text = os.environ.get("INDY_TEXT")
    os.makedirs(outdir, exist_ok=True)
    for i in range(n):
        rng = random.Random(seed * 100003 + i)
        open(os.path.join(outdir, "syn.indy.%d.snap" % i), "wb").write(build(rng, tmpl, text))


if __name__ == "__main__":
    main()
