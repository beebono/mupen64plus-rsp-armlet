#!/usr/bin/env python3
"""Synthetic Rej display lists for armlet-native-replay (HLE_PATCH / HLE_POKE, with --oracle): geometry and commands the captured games do not reach, from a seed. The frame is mksyn_ex2.py's
(the JIT runs the synthetic task in the replay world and the front-end is compared with its result; the snapshot supplies the microcode, the start state and the OSTask; the display list,
matrices and vertices are written to free RDRAM at MKSYN_BASE and the OSTask data_ptr is pointed at it); the commands are the Rej builds' (64 vertex slots of 20 bytes).

  tools/hle/mksyn_rej.py CASE SEED     prints  HLE_PATCH=... HLE_POKE=...  for `env`; tools/hle/synth_rej.sh runs many seeds

  CASES  vtx    vertices in batches of 1 .. 64 (odd and even, to the end of the 64 slots), matrices of different scales (most vertices inside the guard band, some outside: the rejected ones), fog and
                texture generation off or on, the triangles between the batches (G_TRI1, G_TRI2, G_QUAD; shade, z-buffer, texture, flat and smooth shading, culling modes), several matrices
         lit    lit vertices (1 to 7 lights, look-at vectors, texture generation linear or not, fog); the F3DLX.Rej xbus 2.08 text (Excitebike 64) only: the other builds decline them
         wild   vertices over the whole 16-bit range, matrices of large scale and tiny scale (saturation of the vertex pipeline and the triangle setup), triangles between
         misc   G_CULLDL, G_BRANCH_Z (with its G_RDPHALF_1), G_MODIFYVTX at the record fields, G_QUAD, G_POPMTX, display list calls and branches, other-mode and texture commands, G_DMA_IO
         ring   more triangles than the RDP list ring holds (XBUS texts: the ring wraps, DPC_START / DPC_END), 40 per batch
         bad    commands whose vertex numbers, indices or offsets no game writes: the front-end declines them
         load   (the xbus text of Excitebike 64) vertices and triangles, G_RDPHALF_1 + G_LOAD_UCODE of the F3DEX.NoN xbus text it loads (MKSYN_F2TEXT / MKSYN_F2DATA override the addresses), more of both, G_ENDDL
"""
import os
import random
import struct
import sys

BASE = int(os.environ.get("MKSYN_BASE", "0x300000"), 0)       # the display list; the data (viewport, matrices, vertices, ...) follows at DB, the list may be long (ring)
DB = BASE + 0x2000


class DL:
    def __init__(self):
        self.w = []

    def cmd(self, w0, w1=0):
        self.w.append((w0 & 0xffffffff, w1 & 0xffffffff))

    def bytes(self):
        return b"".join(struct.pack(">II", a, b) for a, b in self.w)


def mtx(vals):
    fx = [int(round(v * 65536)) for v in vals]
    return struct.pack(">16H", *[(f >> 16) & 0xffff for f in fx]) + struct.pack(">16H", *[f & 0xffff for f in fx])


def vtx(x, y, z, s, t, c):
    return struct.pack(">3hH2h4B", x, y, z, 0, s, t, *c)


def persp(r, scale):
    """a projection that keeps the vertices of span ~100 inside the screen most of the time: w = scale * (1 + z / 50 + 4)"""
    m = [0.0] * 16
    m[0] = m[5] = scale * r.choice([1, 2, 4])
    m[10] = scale
    m[11] = scale / r.choice([20, 50, 100])
    m[15] = scale * r.choice([4, 6, 20])
    m[12] = r.uniform(-4, 4) * scale
    m[13] = r.uniform(-4, 4) * scale
    return m


def rnd_matrix(r, scale):
    m = [0.0] * 16
    for i in range(3):
        for k in range(4):
            m[4 * i + k] = r.choice([0, 0, 1, -1, 2, 0.5, -0.5, r.uniform(-2, 2)]) * scale
    for k in range(3):
        m[12 + k] = r.uniform(-4, 4) * scale
    m[15] = r.uniform(-2, 6) * scale
    return m


def setup(d, r, geom, trans_z, tex, fog, clip_ratio=3):
    d.cmd(0xdb060000, 0)                                  # segment 0 = 0
    for off in (0x04, 0x0c):
        d.cmd(0xdb040000 | off, clip_ratio)
    for off in (0x14, 0x1c):
        d.cmd(0xdb040000 | off, (-clip_ratio) & 0xffffffff)
    d.cmd(0xdb0e0000, r.choice([0xffff, 0x7fff, 0x0100, 0x4000]))   # perspective normalisation
    if fog:
        d.cmd(0xdb080000, (r.choice([0x100, 0x200, 0x400, 0x1000]) << 16) | (r.choice([0x0, 0x1000, 0x7000, 0xf000, 0x2000])))
    d.cmd(0xd9ffffff, 0)
    d.cmd(0xd9ffffff, geom)
    d.cmd(0xdc080008, DB + 0x400)                       # viewport
    if tex:
        d.cmd(0xd7000002 | (r.randrange(8) << 8), r.choice([0x80008000, 0xffffffff, 0x20004000, 0x00010001]))


def frame(r, geom, trans_z, tex, fog):
    d = DL()
    setup(d, r, geom, trans_z, tex, fog)
    vp = struct.pack(">8h", 160 * 4, 120 * 4, 0x1ff, 0, 160 * 4, 120 * 4, trans_z, 0)
    return d, {0x400: vp}


def geom_of(r, lit=False, fog=None):
    shade = r.choice([0, 0x4, 0x4 | 0x200000, 0x4 | 0x200000])
    return shade | r.choice([0, 1]) | r.choice([0, 0, 0x400, 0x200, 0x600]) | (0x10000 if (fog if fog is not None else r.random() < 0.4) else 0) | (0x20000 if lit else 0) | 0x800000


def batch(d, data, r, vaddr, n, v0, span, lit=False, wild=False):
    vs = b""
    ctr = [r.uniform(-span, span) for _ in range(3)]
    for i in range(n):
        sp = r.choice([1, 2, 5, 20, 100]) if not wild else r.choice([100, 2000, 32768])
        x, y, z = [max(-32768, min(32767, int(c + r.uniform(-sp, sp)))) for c in ctr]
        if wild:
            x, y, z = [r.choice([-32768, -32767, 32767, 0, 1, -1]) if r.random() < 0.25 else c for c in (x, y, z)]
        if lit:
            nrm = [r.randrange(-127, 128) & 255 for _ in range(3)]
            vs += struct.pack(">3hH2h4B", x, y, z, 0, r.randrange(-1024, 1024), r.randrange(-1024, 1024), nrm[0], nrm[1], nrm[2], r.randrange(256))
        else:
            vs += vtx(x, y, z, r.randrange(-1024, 1024), r.randrange(-1024, 1024), (r.randrange(256), r.randrange(256), r.randrange(256), r.randrange(256)))
    data[vaddr] = vs
    d.cmd((0x01 << 24) | (n << 12) | ((v0 + n) << 1), DB + vaddr)


def tris(d, r, v0, n, count):
    if n < 3:
        return
    for _ in range(count):
        a, b, c = r.sample(range(v0, v0 + n), 3)
        k = r.random()
        if k < 0.3:
            d.cmd((0x06 << 24) | (a * 2 << 16) | (b * 2 << 8) | c * 2, (r.randrange(v0, v0 + n) * 2 << 16) | (r.randrange(v0, v0 + n) * 2 << 8) | r.randrange(v0, v0 + n) * 2)
        elif k < 0.4 and n >= 4:
            q = r.sample(range(v0, v0 + n), 4)
            d.cmd((0x07 << 24) | (q[0] * 2 << 16) | (q[1] * 2 << 8) | q[2] * 2, (q[0] * 2 << 16) | (q[2] * 2 << 8) | q[3] * 2)
        else:
            d.cmd((0x05 << 24) | (a * 2 << 16) | (b * 2 << 8) | c * 2)


def case(name, seed):
    r = random.Random(seed * 7919 + len(name))
    data = {}
    if name in ("vtx", "wild", "ring", "load"):
        geom = geom_of(r)
        tex = r.choice([0, 2])
        d, data = frame(r, geom, r.choice([0x1ff, 0, 0x100]), tex, bool(geom & 0x10000))
        wild = name == "wild"
        scale = r.choice([1, 2, 4, 16, 64, 256, 1 / 256]) if wild else r.choice([1, 0.5, 1 / 8])
        data[0x500] = mtx([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])
        data[0x540] = mtx(rnd_matrix(r, scale) if (wild or r.random() < 0.3) else persp(r, scale))
        d.cmd(0xda380007, DB + 0x540)
        d.cmd(0xda380003, DB + 0x500)
        vaddr = 0x600
        nb = 4
        for b in range(nb):
            n = r.choice([3, 4, 5, 9, 16, 33, 40, 63, 64]) if r.random() < 0.5 else r.randrange(1, 65)
            v0 = r.choice([0, 64 - n, r.randrange(0, 65 - n)])
            span = r.choice([4, 10, 30, 100, 400]) if not wild else r.choice([2000, 10000, 32767])
            batch(d, data, r, vaddr, n, v0, span, wild=wild)
            vaddr += 0x500
            tris(d, r, v0, n, 40 if name == "ring" else r.randrange(2, 12))
            if name == "load" and b == 1:
                f2t, f2d = int(os.environ.get("MKSYN_F2TEXT", "0x800aa600"), 0), int(os.environ.get("MKSYN_F2DATA", "0x800d1ec0"), 0)
                d.cmd(0xe1000000, f2d)
                d.cmd(0xdd0007ff, f2t)
                setup(d, r, geom & ~0x20000, 0x1ff, tex, bool(geom & 0x10000))        # (the F3DEX2.NoN text: its own geometry, 32 slots)
                d.cmd(0xda380007, DB + 0x540)
                d.cmd(0xda380003, DB + 0x500)
                n = r.choice([3, 4, 9, 16])
                batch(d, data, r, vaddr, n, 0, 30)
                vaddr += 0x500
                tris(d, r, 0, n, 6)
                break
        d.cmd(0xdf000000, 0)
    elif name == "lit":
        gm = geom_of(r, True) | r.choice([0, 0x40000, 0x40000 | 0x80000])
        d, data = frame(r, gm, r.choice([0x1ff, 0]), 2, bool(gm & 0x10000))
        nl = r.randrange(1, 8)
        d.cmd(0xdb020000, nl * 24)                         # G_MW_NUMLIGHT
        for slot in range(nl + 3):                         # look-at X, Y, the lights, the ambient slot
            ofs = 24 * slot if slot < 2 else 0x30 + 24 * (slot - 2)
            col = bytes([r.randrange(256), r.randrange(256), r.randrange(256), 0])
            dirv = bytes([r.randrange(256), r.randrange(256), r.randrange(256), 0])
            data[0x1000 + 0x20 * slot] = col + col + dirv + b"\0" * 4 if slot >= 2 else b"\0" * 8 + bytes([0x7f, 0, 0, 0] if slot == 0 else [0, 0x7f, 0, 0]) + b"\0" * 4
            d.cmd(0xdc000000 | (1 << 19) | ((ofs // 8) << 8) | 0x0a, DB + 0x1000 + 0x20 * slot)
        data[0x500] = mtx([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])
        data[0x540] = mtx(persp(r, r.choice([1, 0.5, 1 / 8])))
        d.cmd(0xda380007, DB + 0x540)
        d.cmd(0xda380003, DB + 0x500)
        vaddr = 0x600
        for b in range(3):
            n = r.choice([3, 4, 5, 8, 11, 31, 40, 64]) if r.random() < 0.6 else r.randrange(1, 65)
            v0 = r.choice([0, 64 - n, r.randrange(0, 65 - n)])
            batch(d, data, r, vaddr, n, v0, 30, lit=True)
            vaddr += 0x500
            tris(d, r, v0, n, 4)
        d.cmd(0xdf000000, 0)
    elif name == "misc":
        geom = geom_of(r)
        d, data = frame(r, geom, r.choice([0x1ff, 0]), 2, bool(geom & 0x10000))
        data[0x500] = mtx([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])
        data[0x540] = mtx(persp(r, r.choice([1, 0.5, 1 / 8])))
        data[0x580] = mtx(rnd_matrix(r, 0.25))
        d.cmd(0xda380007, DB + 0x540)
        d.cmd(0xda380003, DB + 0x500)
        n = r.choice([6, 9, 16, 64])
        v0 = r.randrange(0, 65 - n)
        batch(d, data, r, 0x600, n, v0, 30)
        sub = DL()
        sub.cmd(0xf7000000, 0x11111111)
        sub.cmd(0xdf000000, 0)
        data[0x2800] = sub.bytes()
        sub2 = DL()
        sub2.cmd(0xf7000000, 0x22222222)
        sub2.cmd(0x05000000 | ((v0) * 2 << 16) | ((v0 + 1) * 2 << 8) | (v0 + 2) * 2)
        sub2.cmd(0xdf000000, 0)
        data[0x2900] = sub2.bytes()
        data[0x3000] = bytes(r.randrange(256) for _ in range(0x100))
        vi = lambda: r.randrange(v0, v0 + n)
        for _ in range(r.randrange(8, 20)):
            k = r.randrange(13)
            if k == 0:
                a, b2 = sorted((vi(), vi()))
                d.cmd((0x03 << 24) | (a * 2), b2 * 2)
            elif k == 1:
                d.cmd(0xe1000000, DB + 0x2900)
                d.cmd((0x04 << 24) | (vi() * 2) & 0xfff, r.choice([0, 0x10000, 0x40000000, 0xffff0000, r.randrange(1 << 32)]))
            elif k == 2:
                d.cmd((0x02 << 24) | (r.choice([0x10, 0x14, 0x18, 0x1c, 0x20]) << 16) | (vi() * 2), r.choice([0x7fff7fff, 0x00ff00ff, r.randrange(1 << 32)]))
            elif k == 3:
                q = [vi() for _ in range(4)]
                d.cmd((0x07 << 24) | (q[0] * 2 << 16) | (q[1] * 2 << 8) | q[2] * 2, (q[0] * 2 << 16) | (q[2] * 2 << 8) | q[3] * 2)
            elif k == 4:
                d.cmd(0xd6000000 | (r.choice([0x22c0, 0x22c0, 0x2300]) << 8) | r.choice([0xff, 0x7f, 0x1f]), DB + 0x3000)      # G_DMA_IO: 256 / 128 / 32 bytes to the end of the vertex area of F3DFLX.Rej (declined when it would overwrite state: the vertex slots of the other builds reach 0x910)
            elif k == 5:
                d.cmd(0xda380001, DB + 0x580)
                d.cmd(0xda380000, DB + 0x500)
                d.cmd(0xd8380002, 64)
            elif k == 6:
                d.cmd(0xde000000, DB + 0x2800)
            elif k == 7:
                d.cmd(0xde010000, DB + 0x2800)
            elif k == 8:
                d.cmd(0xe3000a01, r.choice([0, 0x30000000]))
                d.cmd(0xe2001e01, 0x00500000)
            elif k == 9:
                d.cmd(0xd7000002 | (r.randrange(8) << 8), r.choice([0x80008000, 0x40004000]))
            elif k == 10:
                d.cmd(0xd9ffffff, r.choice([0, 0x200000, 0x400, 0x200, 4, 1]))
                d.cmd(0xd9000000 | (r.randrange(1 << 24)), 0)
            elif k == 11:
                a, b2 = vi(), vi()
                d.cmd((0x03 << 24) | (min(a, b2) * 2), max(a, b2) * 2)
            else:
                d.cmd((0x05 << 24) | (vi() * 2 << 16) | (vi() * 2 << 8) | vi() * 2)
        d.cmd(0xdf000000, 0)
    elif name == "bad":
        geom = geom_of(r)
        d, data = frame(r, geom, 0x1ff, 2, False)
        data[0x500] = mtx([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])
        data[0x540] = mtx(persp(r, 0.5))
        d.cmd(0xda380007, DB + 0x540)
        d.cmd(0xda380003, DB + 0x500)
        n = 12
        batch(d, data, r, 0x600, n, 0, 30)
        sub = DL()
        sub.cmd(0xf7000000, 0x11111111)
        sub.cmd(0xdf000000, 0)
        data[0x2800] = sub.bytes()
        bad = lambda: r.choice([r.randrange(0x82, 0x100), r.randrange(0, 0x80) | 1, r.randrange(0x100, 0x1000)])
        kinds = 11
        for _ in range(1):                                          # ONE bad command per task (the seed picks the kind), so that a guard is not masked by another one
            k = seed % kinds
            if k == 7:                                              # G_DMA_IO that writes to RDRAM, into IMEM, or over the display list buffer / the vertex slots' neighbours
                d.cmd(0xd6000000 | ([0xa2c0, 0x6000, 0x1040, 0x4040, 0x2540, 0x2400, 0x2a00, 0x2640][(seed // kinds) % 8] << 8) | r.choice([0xff, 0x1f, 0x7f]), DB + 0x3000)
                continue
            if k == 8:                                              # G_VTX with an end index no game writes, or a count that does not fit it
                nn = r.choice([1, 4, 8])
                end = r.choice([2 * nn + 1, 0x82, 0xfe, 2 * nn - 2 if nn > 1 else 1, 2 * nn + 0x100])
                d.cmd((0x01 << 24) | (r.choice([0, 0x100000]) | nn << 12) | end, DB + 0x600)
                continue
            if k == 9:                                              # a geometry mode with a bit the build does not define
                d.cmd(0xd9ffffff, geom | r.choice([0x2, 0x8, 0x80, 0x1000, 0x2000, 0x4000, 0x1000000]))
                d.cmd((0x05 << 24) | (r.randrange(n) * 2 << 16) | (r.randrange(n) * 2 << 8) | r.randrange(n) * 2)
                d.cmd(0xdf000000, 0)
                continue
            if k == 10:                                             # a G_VTX after the geometry mode was set with undefined bits (the lit state)
                d.cmd(0xd9ffffff, 0x20000 | 0x40000 | 0x4)
                d.cmd((0x01 << 24) | (4 << 12) | 8, DB + 0x600)
                continue
            if k == 0:
                d.cmd((0x05 << 24) | (bad() & 0xff) << 16 | (r.randrange(n) * 2 << 8) | r.randrange(n) * 2)
            elif k == 1:
                d.cmd((0x02 << 24) | (r.choice([0x00, 0x08, 0x0c, 0x28, 0x50]) << 16) | bad(), r.randrange(1 << 32))
            elif k == 2:
                d.cmd((0x03 << 24) | bad(), r.choice([bad(), 0x16, 0x1e]))
            elif k == 3:
                d.cmd(0xe1000000, DB + 0x2800)
                d.cmd((0x04 << 24) | bad(), r.randrange(1 << 32))
            elif k == 4:
                d.cmd(0xdb000000 | (r.choice([0x10, 0x12, 0x20, 0x40, 0x04, 0x06]) << 16) | r.choice([0x41, 0x80, 0x100, 0x3e, 0x44]), r.randrange(1 << 32))
            elif k == 5:
                d.cmd((0x01 << 24) | (r.choice([0, 65, 80, 200]) << 12) | r.choice([2, 130, 0x7e, 0xfe]), DB + 0x600)
            else:
                d.cmd(0xdc000000 | (r.choice([0x08, 0x20, 0x40]) << 16) | (r.choice([0x10, 0x40, 0xfe, 0x7e]) | r.choice([0, 0x20])), DB + 0x500)
            d.cmd((0x05 << 24) | (r.randrange(n) * 2 << 16) | (r.randrange(n) * 2 << 8) | r.randrange(n) * 2)
        d.cmd(0xdf000000, 0)
    else:
        sys.exit("unknown case " + name)
    data[0] = d.bytes()
    patch = ["%x:%s" % ((DB + off) if off else BASE, b.hex()) for off, b in sorted(data.items())]
    poke = ",".join("%x=%02x" % (0xff0 + i, b) for i, b in enumerate(struct.pack(">I", BASE)))
    print("HLE_PATCH=" + ";".join(patch), "HLE_POKE=" + poke)


if len(sys.argv) != 3:
    sys.exit(__doc__)
case(sys.argv[1], int(sys.argv[2]))
