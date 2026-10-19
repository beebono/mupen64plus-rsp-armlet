#!/usr/bin/env python3
"""Synthetic F3DEX2 display lists for armlet-native-replay (HLE_PATCH / HLE_POKE, with --oracle): geometry the captured games do not reach, from a seed.

  tools/hle/mksyn_ex2.py CASE SEED     prints  HLE_PATCH=... HLE_POKE=...  for `env`; run
      env $(tools/hle/mksyn_ex2.py clip 7) armlet-native-replay --oracle <a snapshot of the build>.snap
  The JIT runs the synthetic task in the replay world and the front-end is compared with its result. The snapshot supplies the microcode, the start state (the boot code runs from it)
  and the OSTask; the display list, matrices and vertices are written to free RDRAM at 0x300000 and the OSTask data_ptr is pointed at it. Nothing of a game is in the output.
  tools/hle/synth_ex2.sh runs many seeds.

  CASES  bgph    (F3DEXBG, on a lit Conker task) lit vertices in odd batches that end at the last slot, with triangles between: the phantom partner of the last pair is read from the display list's current chunk
         clip    triangles that cross the clip planes, with matrices of very different scales (short edges: the intersection's distance difference has a zero integer part), 12 per task
         zneg    vertices whose screen z is negative (the viewport's z translation 0), triangles drawn, shade / z-buffer / texture on or off
         tri     triangles of all sizes, culling modes, flat and smooth shading, textures: the triangle setup
         mix     vertices in batches of odd and even counts up to the end of the vertex buffer, the triangles between them
         misc    the other commands: G_CULLDL, G_BRANCH_Z (with its G_RDPHALF_1), G_MODIFYVTX, G_QUAD, G_SPECIAL_1 (2.04H), G_POPMTX, display list calls and branches, other-mode and texture commands
         wild    vertices with coordinates over the whole 16-bit range and matrices of large scale (the overflow and saturation paths of the vertex pipeline and the triangle setup), triangles between
         bad     commands whose vertex numbers, indices or offsets no game writes (past the 32 slots, odd, past the tables): a triangle, G_MODIFYVTX, G_CULLDL, G_BRANCH_Z, G_MOVEWORD, G_MOVEMEM; the front-ends decline them
         lit     lit vertices (1 to 5 lights, look-at vectors, texture generation linear or not, fog), the triangles between them
         mtx     matrices whose halfwords are random 16-bit values (any integer and fraction part, the saturating corners of the product): G_MTX load, multiply and push, modelview and projection, then vertices
         s2      a task that starts with S2DEX (Resident Evil 2's; MKSYN_S2DATA = its data address + 0x180): the S2DEX commands of the stage below, then G_ENDDL
         s2ring  a task that starts with S2DEX: 60 .. 119 sprite rectangles, more than the ring of an XBUS text holds (StarCraft 64's)
         load    a task that loads another microcode in the middle and back (Kirby 64's: F3DEX2 2.04H -> S2DEX 2.08 -> F3DEX2 2.04H): triangles, G_RDPHALF_1 + G_LOAD_UCODE, S2DEX commands (scissor, render mode, sprite
                 rectangles, backgrounds in 1-cycle and COPY mode, texture loads, display list selection), G_LOAD_UCODE back, triangles again. The addresses of the texts and data in RDRAM are the game's
                 (MKSYN_S2TEXT, MKSYN_S2DATA, MKSYN_F2TEXT, MKSYN_F2DATA override them; a data address is that of the data row, the one G_LOAD_UCODE copies to DMEM 0x180); the snapshot must hold those rows (any captured Kirby 64 task does)
"""
import os
import random
import struct
import sys

BASE = int(os.environ.get("MKSYN_BASE", "0x300000"), 0)       # (a game whose RDP ring lies there: MKSYN_BASE=<free RDRAM>)


class DL:
    def __init__(self):
        self.w = []

    def cmd(self, w0, w1=0):
        self.w.append((w0 & 0xffffffff, w1 & 0xffffffff))

    def bytes(self):
        return b"".join(struct.pack(">II", a, b) for a, b in self.w)


def mtx(vals):
    """vals: 16 values (floats, row-major) -> the 64-byte N64 matrix: 16 integer halfwords, then 16 fraction halfwords"""
    fx = [int(round(v * 65536)) for v in vals]
    ints = [(f >> 16) & 0xffff for f in fx]
    fracs = [f & 0xffff for f in fx]
    return struct.pack(">16H", *ints) + struct.pack(">16H", *fracs)


def vtx(x, y, z, s, t, c):
    return struct.pack(">3hH2h4B", x, y, z, 0, s, t, *c)


def rnd_matrix(r, scale):
    m = [0.0] * 16
    for i in range(3):
        for k in range(4):
            m[4 * i + k] = r.choice([0, 0, 1, -1, 2, 0.5, -0.5, r.uniform(-2, 2)]) * scale
    m[3 * 4 + 0] = r.uniform(-4, 4) * scale
    m[3 * 4 + 1] = r.uniform(-4, 4) * scale
    m[3 * 4 + 2] = r.uniform(-4, 4) * scale
    m[3 * 4 + 3] = r.uniform(-2, 6) * scale
    return m


def setup(d, r, geom, trans_z, clip_ratio=3):
    d.cmd(0xdb060000, 0)                                  # segment 0 = 0
    for off in (0x04, 0x0c):                              # clip ratio: x, y
        d.cmd(0xdb040000 | off, clip_ratio)
    for off in (0x14, 0x1c):
        d.cmd(0xdb040000 | off, (-clip_ratio) & 0xffffffff)
    d.cmd(0xdb0e0000, r.choice([0xffff, 0x7fff, 0x0100, 0x4000]))   # perspective normalisation
    d.cmd(0xd9ffffff, 0)                                  # clear the geometry mode
    d.cmd(0xd9ffffff, geom)
    d.cmd(0xdc080008, BASE + 0x400)                       # viewport


def common(r, geom, trans_z, tex):
    d = DL()
    setup(d, r, geom, trans_z)
    vp = struct.pack(">8h", 160 * 4, 120 * 4, 0x1ff, 0, 160 * 4, 120 * 4, trans_z, 0)
    if tex:
        d.cmd(0xd7000002 | (r.randrange(8) << 8), r.choice([0x80008000, 0xffffffff, 0x20004000, 0x00010001]))
    return d, {0x400: vp}


def tame_sprite(r):
    """a sprite on the screen with scales a game uses (the closed form takes these)"""
    return struct.pack(">hHHHhHHHHHBBBB", r.randint(0, 1200), r.choice([0x400, 0x400, 0x200, 0x800]), r.randrange(8, 64) * 32, 0, r.randint(0, 900), r.choice([0x400, 0x400, 0x200, 0x800]), r.randrange(8, 64) * 32, 0,
                       r.randrange(0, 40), r.randrange(0, 0x200), r.randrange(0, 5), r.randrange(0, 4), r.randrange(0, 16), 0)


def sprite(r):
    return struct.pack(">hHHHhHHHHHBBBB", r.randint(-40, 1300), r.choice([0x400, 0x400, 0x200, 0x800, r.randrange(0x100, 0x1000)]), r.randrange(8, 64) * 32, 0,
                       r.randint(-40, 980), r.choice([0x400, 0x400, 0x200, 0x800, r.randrange(0x100, 0x1000)]), r.randrange(8, 64) * 32, 0,
                       r.randrange(0, 40), r.randrange(0, 0x200), r.randrange(0, 5), r.randrange(0, 4), r.randrange(0, 16), r.choice([0, 0, 1, 0x10, 0x11]))

def bg(r, copy):
    sc = 0x400 if not copy else r.choice([0x400, 0x400, 0x53, r.randrange(0x400)])
    iw = r.choice([0x500, 0x2c0, 0x500, r.randrange(0x40, 0xc00)])
    ih = r.choice([0xc0, 0x88, r.randrange(0x20, 0x380)])
    return struct.pack(">8HIHBBBBBBHHiHH", r.choice([0, 0, 0, r.randrange(0, 0x400)]), iw, r.choice([0xa4, 0x1c, 0, r.randrange(0, 0x300)]), r.choice([iw, iw, r.randrange(0x40, 0xc00)]),
                       r.choice([0, 0, r.randrange(0, 0x300)]), ih, r.choice([0x2e8, 0x2d8, 0, r.randrange(0, 0x300)]), r.choice([ih, ih, r.randrange(0x20, 0x380)]),
                       0x00100000 + r.randrange(0x4000) * 8, r.choice([0xfff4, 0xfff4, 0x0033, 0]), r.randrange(0, 5), r.randrange(0, 4), r.randrange(0, 2), r.randrange(0, 16),
                       0, r.choice([0, 0, 1]), sc, sc, r.choice([0, 0x17, r.randrange(0, 0x100)]), r.randrange(0, 0x100), r.randrange(0, 0x200))

def txtr(r):
    sid = r.randrange(1 << 16)
    return struct.pack(">IIIIII", 0x00001033, 0x00100000 + r.randrange(0x1000) * 8, r.randrange(0x7ff), 0x100, sid, r.choice([0, sid, 0xffffffff]))      # (the load bits and their mask: a mask that covers the bits makes the second load of the object a no-op)


def s2_stage(d, r, data, s2d):
    d.cmd(0xed000000 | (r.randrange(0, 40) << 14) | r.randrange(0, 40) * 4, ((1280 - r.randrange(0, 40) * 4) << 12) | (960 - r.randrange(0, 40) * 4))
    d.cmd(0x0b000000 | r.choice([0, 8]), r.randrange(1 << 8) | (r.randrange(0, 4) << 3) | (r.randrange(0, 8) << 4))
    o = 0x2000
    if r.random() < 0.15:                                       # G_SELECT_DL twice: the first leaves a byte (the low byte of its w0) that the second takes for the index of the flags word at 0x2b0 + index, which can be an entry
        at = r.choice([0xc0, 0xc2, 0xc4, 0xbe, 0xd0, 0x80, 0xff, 0x20])        # of the jump table (0x370 ..): rewritten (with zeros here), the commands after it run another handler in the microcode (the front-end declines)
        d.cmd(0x04000000 | at, r.randrange(1 << 32))
        d.cmd(0x04000031, 0xffffffff)                           # (a call of the list at 0x310000: segment 0, the offset's high byte)
        data[0x10000] = struct.pack(">II", 0x01000000, BASE + 0x2f00) + sprite(r)[:0] + struct.pack(">II", 0x00000000, 0) + struct.pack(">II", 0xdf000000, 0)
        data[0x2f00] = tame_sprite(r)
    for _ in range(r.randrange(2, 7)):
        k = r.randrange(8)
        if k == 7 and r.random() < 0.75:                         # (LDTX_SPRITE / LDTX_RECT_R decline the whole task: rarely)
            k = 6
        if k in (0, 1):
            data[o] = sprite(r)
            d.cmd(0x01000000, BASE + o)
        elif k == 2:
            data[o] = bg(r, False)
            d.cmd(0x09000000, BASE + o)
        elif k == 3:
            data[o] = bg(r, True)
            d.cmd(0x0a000000, BASE + o)
        elif k == 4:
            data[o] = txtr(r)
            d.cmd(0x05000017, BASE + o)
            if r.random() < 0.4:                                # the same object again: its bits are loaded already, nothing is written
                d.cmd(0x05000017, BASE + o)
        elif k == 6:                                            # LDTX_RECT: the texture object, then the sprite behind it (48 bytes)
            data[o] = txtr(r) + (tame_sprite(r) if r.random() < 0.6 else sprite(r))
            d.cmd(0x0700002f | r.choice([0, 0x100]), BASE + o)
        elif k == 7:                                            # LDTX_SPRITE and the rotated LDTX_RECT_R (the front-end declines them)
            data[o] = txtr(r) + sprite(r)
            d.cmd(r.choice([0x06, 0x08]) << 24 | 0x2f, BASE + o)
        elif k == 5 and r.random() < 0.15:                 # an opcode nobody defined, whose jump table entry is 0x0fff (the data row is patched): the build declines it (the marker of a missing handler is 0xffff)
            op = r.choice([0x30, 0x20, 0x4a, 0x10])
            data[s2d + 0x250 + 2 * (op - 0x30) - BASE] = b"\x0f\xff"
            d.cmd(op << 24 | r.randrange(1 << 24), r.randrange(1 << 32))
        else:
            d.cmd(0x04000000 | r.randrange(0x10000), r.randrange(1 << 32))
            d.cmd(0xe6000000, 0)
        o += 0x80


def case(name, seed):
    r = random.Random(seed * 7919 + len(name))
    data = {}
    if name in ("clip", "zneg", "tri", "mix", "wild", "bgph"):
        shade = r.choice([0, 0x4, 0x4 | 0x200000])
        zb = r.choice([0, 1])
        tex = r.choice([0, 2])
        cull = r.choice([0, 0, 0x400, 0x200, 0x600])
        geom = shade | zb | cull | 0x800000 | (0x20000 if name == "bgph" else 0)   # (the texture on byte is set by G_TEXTURE below; bit 1 of the geometry mode means nothing to F3DEX2)
        if len(sys.argv) > 3:
            print('INFO', name, seed, 'shade %x zb %d tex %d cull %x' % (shade, zb, tex, cull), file=sys.stderr)
        trans_z = 0 if name == "zneg" else r.choice([0x1ff, 0, 0x100])
        d, data = common(r, geom, trans_z, tex)
        if name == "bgph":                                  # F3DEXBG's lights: an odd count, 48-byte slots (G_MOVEMEM index 10 from offset 0x0c, 6 x 8 bytes apart), the look-at slots and the ambient one included
            nl = r.choice([1, 3, 5])
            d.cmd(0xdb020000, nl * 48)
            for slot in range(nl + 2):
                blob = bytearray(48)                        # (raw direction at +0, colours at +24 and +28; the point-light fields stay zero. Axis directions: random ones differ from the microcode by one in a colour byte in ~7 % of the tasks (found with this case; not the phantom)
                for k in (24, 25, 26, 28, 29, 30):
                    blob[k] = r.randrange(256)
                blob[r.randrange(3)] = r.choice([0x7f, 0x81])
                data[0x1000 + 0x40 * slot] = bytes(blob)
                d.cmd(0xdc280000 | ((0x0c + 6 * slot) << 8) | 0x0a, BASE + 0x1000 + 0x40 * slot)
        # matrices: modelview (loaded, not pushed), projection (loaded)
        if name == "clip":
            scale = r.choice([1, 1, 0.5, 0.25, 1 / 16, 1 / 64, 1 / 256, 1 / 1024])
        elif name == "zneg":
            scale = r.choice([1, 0.25])
        elif name == "wild":
            scale = r.choice([1, 2, 4, 16, 64, 256])
        else:
            scale = r.choice([1, 0.5, 1 / 8])
        mv = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
        if name == "zneg":                                 # w = 1 + 0.5 z: points with z in (-1, 0) are in front of the near plane at negative screen z
            pj = [scale, 0, 0, 0, 0, scale, 0, 0, 0, 0, scale, 0.5 * scale, 0, 0, 0, 1 * scale]
            pj[11] = 0.5 * scale
        else:
            pj = rnd_matrix(r, scale)
        data[0x500] = mtx(mv)
        data[0x540] = mtx(pj)
        d.cmd(0xda380007, BASE + 0x540)                    # projection, load
        d.cmd(0xda380003, BASE + 0x500)                    # modelview, load
        nbatch = 3 if name not in ("mix", "bgph") else 4
        vaddr = 0x600
        for b in range(nbatch):
            n = r.choice([3, 3, 4, 5, 6, 9, 11, 16]) if name not in ("mix", "bgph") else r.randrange(1, 12)
            v0 = r.randrange(0, 32 - n + 1) if name not in ("mix", "bgph") else (32 - n if r.random() < 0.5 else r.randrange(0, 32 - n + 1))
            if name == "bgph":                              # odd counts that end at the last slot: the pair's phantom partner is the first bytes of the input buffer
                n = r.choice([1, 3, 5, 7, 9, 11, 17, 31]) if r.random() < 0.8 else n
                v0 = 32 - n
            span = r.choice([4, 10, 30, 100, 400]) if name not in ("zneg", "wild") else r.choice([0.5, 1.0]) if name == "zneg" else r.choice([2000, 10000, 32767])
            ctr = [r.uniform(-span, span) for _ in range(3)]
            vs = b""
            for i in range(n):
                if name == "zneg":
                    x, y, z = r.randint(-8, 8), r.randint(-8, 8), -1 if False else 0
                    p = [r.uniform(-8, 8), r.uniform(-8, 8), r.uniform(-1.4, 0.8)]
                    # z is an integer in the vertex: scale up the whole matrix instead
                    x, y, z = int(p[0]), int(p[1]), r.choice([-1, 0, 0, 1])
                else:
                    sp = r.choice([1, 2, 5, 20, 100]) if name != "wild" else r.choice([100, 2000, 32768])
                    x, y, z = [max(-32768, min(32767, int(c + r.uniform(-sp, sp)))) for c in ctr]
                    if name == "wild":                      # some coordinates at the ends of the 16-bit range
                        x, y, z = [r.choice([-32768, -32767, 32767, 0, 1, -1]) if r.random() < 0.25 else c for c in (x, y, z)]
                vs += vtx(x, y, z, r.randrange(-1024, 1024), r.randrange(-1024, 1024), (r.randrange(256), r.randrange(256), r.randrange(256), r.randrange(256)))
            data[vaddr] = vs
            d.cmd((0x01 << 24) | (n << 12) | ((v0 + n) << 1), BASE + vaddr)
            vaddr += 0x100
            for _ in range(r.randrange(2, 12) if name != "clip" else 12):
                a, b2, c = r.sample(range(v0, v0 + n), 3) if n >= 3 else (v0, v0, v0)
                if n < 3:
                    continue
                if r.random() < 0.3:
                    d.cmd((0x06 << 24) | (a * 2 << 16) | (b2 * 2 << 8) | c * 2, (r.randrange(v0, v0 + n) * 2 << 16) | (r.randrange(v0, v0 + n) * 2 << 8) | r.randrange(v0, v0 + n) * 2)
                else:
                    d.cmd((0x05 << 24) | (a * 2 << 16) | (b2 * 2 << 8) | c * 2)
        d.cmd(0xdf000000, 0)
    elif name == "misc":
        geom = 0x4 | 0x200000 | 1 | 0x800000
        d, data = common(r, geom, r.choice([0x1ff, 0]), 2)
        data[0x500] = mtx([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])
        data[0x540] = mtx(rnd_matrix(r, r.choice([1, 0.5, 1 / 8])))
        data[0x580] = mtx(rnd_matrix(r, 0.25))
        d.cmd(0xda380007, BASE + 0x540)
        d.cmd(0xda380003, BASE + 0x500)
        n = r.choice([6, 9, 16])
        v0 = r.randrange(0, 32 - n + 1)
        vs = b"".join(vtx(r.randint(-30, 30), r.randint(-30, 30), r.randint(-30, 30), r.randrange(-512, 512), r.randrange(-512, 512),
                          (r.randrange(256), r.randrange(256), r.randrange(256), r.randrange(256))) for _ in range(n))
        data[0x600] = vs
        d.cmd((0x01 << 24) | (n << 12) | ((v0 + n) << 1), BASE + 0x600)
        sub = DL()
        sub.cmd(0xf7000000, 0x11111111)
        sub.cmd(0xdf000000, 0)
        data[0x800] = sub.bytes()
        sub2 = DL()
        sub2.cmd(0xf7000000, 0x22222222)
        sub2.cmd(0x05000000 | ((v0) * 2 << 16) | ((v0 + 1) * 2 << 8) | (v0 + 2) * 2)
        sub2.cmd(0xdf000000, 0)
        data[0x900] = sub2.bytes()
        vi = lambda: r.randrange(v0, v0 + n)
        for _ in range(r.randrange(8, 20)):
            k = r.randrange(12)
            if k == 0:
                a, b2 = sorted((vi(), vi()))
                d.cmd((0x03 << 24) | (a * 2), b2 * 2)
            elif k == 1:
                d.cmd(0xe1000000, BASE + 0x900)
                d.cmd((0x04 << 24) | ((vi() * 5) << 12) | (vi() * 2) & 0xfff, r.choice([0, 0x10000, 0x40000000, 0xffff0000, r.randrange(1 << 32)]))
            elif k == 2:
                d.cmd((0x02 << 24) | (r.choice([0x10, 0x14, 0x18, 0x1c, 0x00, 0x04, 0x08]) << 16) | (vi() * 2), r.randrange(1 << 32))
            elif k == 3:
                q = [vi() for _ in range(4)]
                d.cmd((0x07 << 24) | (q[0] * 2 << 16) | (q[1] * 2 << 8) | q[2] * 2, (q[0] * 2 << 16) | (q[2] * 2 << 8) | q[3] * 2)
            elif k == 4:
                d.cmd(0xd5000000 | r.choice([0, 1, 0xff]), 0)
            elif k == 5:
                d.cmd(0xda380001, BASE + 0x580)         # modelview multiply, no push
                d.cmd(0xda380000, BASE + 0x500)         # push, multiply
                d.cmd(0xd8380002, 64)                   # pop
            elif k == 6:
                d.cmd(0xde000000, BASE + 0x800)         # call
            elif k == 7:
                d.cmd(0xde010000, BASE + 0x800)         # branch (ends the list: the rest of the commands is skipped)
            elif k == 8:
                d.cmd(0xe3000a01, r.choice([0, 0x30000000]))
                d.cmd(0xe2001e01, 0x00500000)
            elif k == 9:
                d.cmd(0xd7000002 | (r.randrange(8) << 8), r.choice([0x80008000, 0x40004000]))
            elif k == 10:
                d.cmd(0xd9ffffff, r.choice([0, 0x200000, 0x400, 0x200, 4, 1]))
                d.cmd(0xd9000000 | (r.randrange(1 << 24)), 0)
            else:
                d.cmd((0x05 << 24) | (vi() * 2 << 16) | (vi() * 2 << 8) | vi() * 2)
        d.cmd(0xdf000000, 0)
    elif name == "bad":
        geom = 0x4 | 0x200000 | 1 | 0x800000
        d, data = common(r, geom, 0x1ff, 2)
        data[0x500] = mtx([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])
        data[0x540] = mtx(rnd_matrix(r, 0.5))
        d.cmd(0xda380007, BASE + 0x540)
        d.cmd(0xda380003, BASE + 0x500)
        n = 12
        data[0x600] = b"".join(vtx(r.randint(-30, 30), r.randint(-30, 30), r.randint(-30, 30), r.randrange(-512, 512), r.randrange(-512, 512),
                                   (r.randrange(256), r.randrange(256), r.randrange(256), r.randrange(256))) for _ in range(n))
        d.cmd((0x01 << 24) | (n << 12) | (n << 1), BASE + 0x600)
        sub = DL()
        sub.cmd(0xf7000000, 0x11111111)
        sub.cmd(0xdf000000, 0)
        data[0x800] = sub.bytes()
        bad = lambda: r.choice([r.randrange(0x40, 0x100), r.randrange(0, 0x40) | 1, r.randrange(0x100, 0x1000)])
        for _ in range(r.randrange(2, 6)):
            k = r.randrange(6)
            if k == 0:
                d.cmd((0x05 << 24) | (bad() & 0xff) << 16 | (r.randrange(n) * 2 << 8) | r.randrange(n) * 2)
            elif k == 1:
                d.cmd((0x02 << 24) | (r.choice([0x10, 0x14, 0x18, 0x1c, 0x28, 0x50]) << 16) | bad(), r.randrange(1 << 32))
            elif k == 2:
                d.cmd((0x03 << 24) | bad(), r.choice([bad(), 0x16, 0x1e]))
            elif k == 3:
                d.cmd(0xe1000000, BASE + 0x800)
                d.cmd((0x04 << 24) | bad(), r.randrange(1 << 32))
            elif k == 4:
                d.cmd(0xdb000000 | (r.choice([0x10, 0x12, 0x20, 0x40, 0x04, 0x06]) << 16) | r.choice([0x41, 0x80, 0x100, 0x3e, 0x44]), r.randrange(1 << 32))
            else:
                d.cmd(0xdc000000 | (r.choice([0x08, 0x20, 0x40]) << 16) | (r.choice([0x10, 0x40, 0xfe, 0x7e]) | r.choice([0, 0x20])), BASE + 0x500)
            d.cmd((0x05 << 24) | (r.randrange(n) * 2 << 16) | (r.randrange(n) * 2 << 8) | r.randrange(n) * 2)
        d.cmd(0xdf000000, 0)
    elif name == "lit":
        shade = 0x4 | 0x200000
        gm = shade | 1 | 0x20000 | r.choice([0, 0x40000, 0x40000 | 0x80000]) | r.choice([0, 0x10000]) | 0x800000
        d, data = common(r, gm, r.choice([0x1ff, 0]), 2)
        nl = r.randrange(1, 6)
        if len(sys.argv) > 3:
            print('INFO lit', seed, 'texgen %d linear %d fog %d lights %d' % (bool(gm & 0x40000), bool(gm & 0x80000), bool(gm & 0x10000), nl), file=sys.stderr)
        d.cmd(0xdb020000, nl * 24)                         # G_MW_NUMLIGHT
        for slot in range(nl + 3):                         # look-at X, Y, the lights, the ambient slot
            ofs = 24 * slot if slot < 2 else 0x30 + 24 * (slot - 2)
            if slot >= 2 and slot - 2 == nl:
                ofs = 0x30 + 24 * nl                        # (the ambient slot follows the last light)
            col = bytes([r.randrange(256), r.randrange(256), r.randrange(256), 0])
            dirv = bytes([r.randrange(256), r.randrange(256), r.randrange(256), 0])
            blob = col + col + dirv + b"\0" * 4
            data[0x1000 + 0x20 * slot] = blob if slot >= 2 else b"\0" * 8 + bytes([0x7f, 0, 0, 0] if slot == 0 else [0, 0x7f, 0, 0]) + b"\0" * 4
            d.cmd(0xdc000000 | (1 << 19) | ((ofs // 8) << 8) | 0x0a, BASE + 0x1000 + 0x20 * slot)
        scale = r.choice([1, 0.5, 1 / 8])
        data[0x500] = mtx([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])
        data[0x540] = mtx(rnd_matrix(r, scale))
        d.cmd(0xda380007, BASE + 0x540)
        d.cmd(0xda380003, BASE + 0x500)
        vaddr = 0x600
        for b in range(2):
            n = r.choice([3, 4, 5, 8, 11])
            v0 = r.randrange(0, 32 - n + 1)
            vs = b""
            for i in range(n):
                nrm = [r.randrange(-127, 128) for _ in range(3)]
                vs += struct.pack(">3hH2h4B", r.randint(-200, 200), r.randint(-200, 200), r.randint(-200, 200), 0, r.randrange(-1024, 1024), r.randrange(-1024, 1024),
                                  nrm[0] & 255, nrm[1] & 255, nrm[2] & 255, r.randrange(256))
            data[vaddr] = vs
            d.cmd((0x01 << 24) | (n << 12) | ((v0 + n) << 1), BASE + vaddr)
            vaddr += 0x100
            for _ in range(4):
                a, b2, c = r.sample(range(v0, v0 + n), 3)
                d.cmd((0x05 << 24) | (a * 2 << 16) | (b2 * 2 << 8) | c * 2)
        d.cmd(0xdf000000, 0)
    elif name == "mtx":
        geom = r.choice([0, 0x4]) | r.choice([0, 1]) | 0x800000
        d, data = common(r, geom, r.choice([0x1ff, 0]), 0)
        for k in range(r.randrange(2, 6)):
            vals = bytes(r.randrange(256) for _ in range(64))
            if r.random() < 0.5:                           # integer halfwords from a small set, fractions random
                vals = struct.pack(">16H", *[r.choice([0, 1, 0xffff, 0x8000, 0x7fff, r.randrange(1 << 16)]) for _ in range(16)]) + vals[32:]
            data[0x500 + 0x40 * k] = vals
            d.cmd(0xda380000 | r.choice([0, 1, 2, 3, 4, 5, 6, 7]), BASE + 0x500 + 0x40 * k)
        n = 4
        data[0x600] = b"".join(vtx(r.randint(-40, 40), r.randint(-40, 40), r.randint(-40, 40), r.randrange(-512, 512), r.randrange(-512, 512), (1, 2, 3, 4)) for _ in range(n))
        d.cmd((0x01 << 24) | (n << 12) | (n << 1), BASE + 0x600)
        d.cmd((0x05 << 24) | (0 << 16) | (2 << 8) | 4)
        d.cmd(0xdf000000, 0)
    elif name == "load":
        import os
        s2t, s2d = int(os.environ.get("MKSYN_S2TEXT", "0x3c3b0"), 0), int(os.environ.get("MKSYN_S2DATA", "0x42980"), 0)
        f2t, f2d = int(os.environ.get("MKSYN_F2TEXT", "0x39e90"), 0), int(os.environ.get("MKSYN_F2DATA", "0x42170"), 0)
        geom = r.choice([0, 0x4]) | r.choice([0, 1]) | 0x800000
        d, data = common(r, geom, r.choice([0x1ff, 0]), r.choice([0, 2]))
        data[0x500] = mtx([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])
        data[0x540] = mtx(rnd_matrix(r, r.choice([1, 0.5, 1 / 8])))
        d.cmd(0xda380007, BASE + 0x540)
        d.cmd(0xda380003, BASE + 0x500)
        vaddr = [0x600]

        def f2_stage(d):
            for _ in range(r.randrange(1, 3)):
                n = r.choice([3, 4, 5, 6, 9])
                v0 = r.randrange(0, 32 - n + 1)
                data[vaddr[0]] = b"".join(vtx(r.randint(-40, 40), r.randint(-40, 40), r.randint(-40, 40), r.randrange(-512, 512), r.randrange(-512, 512),
                                              (r.randrange(256), r.randrange(256), r.randrange(256), r.randrange(256))) for _ in range(n))
                d.cmd((0x01 << 24) | (n << 12) | ((v0 + n) << 1), BASE + vaddr[0])
                vaddr[0] += 0x100
                for _ in range(r.randrange(1, 5)):
                    a, b2, c = r.sample(range(v0, v0 + n), 3)
                    d.cmd((0x05 << 24) | (a * 2 << 16) | (b2 * 2 << 8) | c * 2)

        f2_stage(d)
        d.cmd(0xe1000000, s2d - 0x180)
        d.cmd(0xdd0007ff, s2t)
        s2_stage(d, r, data, s2d)
        d.cmd(0xe1000000, f2d - 0x180)
        d.cmd(0xdd0007ff, f2t)
        setup(d, r, geom, 0x1ff)
        data[0x700] = mtx([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])
        d.cmd(0xda380007, BASE + 0x540)
        d.cmd(0xda380003, BASE + 0x700)
        f2_stage(d)
        d.cmd(0xdf000000, 0)
    elif name == "s2ring":
        d = DL()                                                # 60 .. 119 sprite rectangles (about 30 bytes of RDP list each): more than the DMEM ring of an XBUS S2DEX text holds (ring wrap, DPC_START / DPC_END)
        d.cmd(0xed000000 | (r.randrange(0, 40) << 14) | r.randrange(0, 40) * 4, ((1280 - r.randrange(0, 40) * 4) << 12) | (960 - r.randrange(0, 40) * 4))
        for i in range(60 + r.randrange(60)):
            data[0x2000 + 0x40 * i] = tame_sprite(r) if r.random() < 0.8 else sprite(r)
            d.cmd(0x01000000, BASE + 0x2000 + 0x40 * i)
        d.cmd(0xdf000000, 0)
    elif name == "s2":
        import os
        s2d = int(os.environ.get("MKSYN_S2DATA", "0xbb800"), 0)          # a task that starts with S2DEX (Resident Evil 2's): the data row at DMEM 0x180 is the task's data + 0x180
        d = DL()
        s2_stage(d, r, data, s2d)
        d.cmd(0xdf000000, 0)
    else:
        sys.exit("unknown case " + name)
    data[0] = d.bytes()
    patch = ["%x:%s" % (BASE + off, b.hex()) for off, b in sorted(data.items())]
    poke = ",".join("%x=%02x" % (0xff0 + i, b) for i, b in enumerate(struct.pack(">I", BASE)))
    print("HLE_PATCH=" + ";".join(patch), "HLE_POKE=" + poke)


if len(sys.argv) not in (3, 4):
    sys.exit(__doc__)
case(sys.argv[1], int(sys.argv[2]))
