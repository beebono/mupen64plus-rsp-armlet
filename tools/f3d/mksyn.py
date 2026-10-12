#!/usr/bin/env python3
"""Synthetic F3D display lists for armlet-native-replay (HLE_PATCH / HLE_POKE): checks of the build parameters that captured games do not exercise.

  tools/f3d/mksyn.py CASE        prints  HLE_PATCH=... HLE_POKE=...  for `env`; run
      env $(tools/f3d/mksyn.py CASE) armlet-native-replay <a snapshot of the build>.snap
  The JIT runs the synthetic task in the replay world and records the result the front-end is compared with. The snapshot only supplies the microcode, its start state (the boot code runs from it) and
  the build; the display list, matrices, vertices and lights are written to free RDRAM at 0x300000 and the OSTask data_ptr is pointed at it. Nothing of a game is in the output.

  CASES  dl-depth   G_DL call after a G_MOVEWORD that makes the word at 0x110 large (finds a G_DL depth byte at the wrong address)
         culldl     G_CULLDL over vertices that only the clip ratio group flags (finds a wrong flag mask)
         popmtx     lit vertices before and after a G_POPMTX (finds a missing lights-stale flag)
         nested     calls five levels deep and a branch (the G_DL depth bookkeeping, the stack words)
"""
import struct, sys

BASE = 0x300000


class DL:
    def __init__(self):
        self.w = []

    def cmd(self, w0, w1=0):
        self.w.append((w0 & 0xffffffff, w1 & 0xffffffff))

    def bytes(self):
        return b"".join(struct.pack(">II", a, b) for a, b in self.w)


def mtx(ints, fracs=None):
    """ints: 16 values row-major; fracs: 16 values (default 0) -> 64 bytes"""
    fracs = fracs or [0] * 16
    return struct.pack(">16h", *ints) + struct.pack(">16H", *fracs)


def vtx(x, y, z, s=0, t=0, c=(255, 255, 255, 255)):
    return struct.pack(">3hH2h4B", x, y, z, 0, s, t, *c)


IDENT = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]


def hexs(addr, data):
    return "%x:%s" % (addr, data.hex())


def common(d, geom_set, lit=False):
    d.cmd(0xbc000006, 0)                       # segment 0 = 0
    d.cmd(0xbc000404, 0x00010001)              # clip ratio words (0x74): K = 1
    d.cmd(0xb6000000, 0xffffffff)              # clear geometry mode
    d.cmd(0xb7000000, geom_set)
    d.cmd(0x03800000 | 0x0f, BASE + 0x400)     # viewport


def case(name):
    patch = []
    dl = DL()
    data = {}
    viewport = struct.pack(">8h", 160 * 4, 120 * 4, 0x1ff, 0, 160 * 4, 120 * 4, 0x1ff, 0)
    data[0x400] = viewport
    if name == "dl-depth":
        d = dl
        common(d, 0x4)
        d.cmd(0xbc00000e, 0x7f00ffff)                        # G_MOVEWORD index 0x0e (the perspective norm word): the word at 0x110 = 0x7f00ffff
        d.cmd(0x06000000, BASE + 0x200)                      # call the list at +0x200
        d.cmd(0xf7000000, 0x11111111)                        # marker 2 (fill color), after the call returns
        d.cmd(0xb8000000, 0)
        sub = DL()
        sub.cmd(0xf7000000, 0x22222222)                      # marker 1, inside the call
        sub.cmd(0xb8000000, 0)
        data[0x200] = sub.bytes()
    elif name == "nested":
        d = dl
        common(d, 0x4)
        for lvl in range(5):
            d.cmd(0x06000000, BASE + 0x200 + 0x100 * lvl)
            d.cmd(0xf7000000, 0x01010101 * (lvl + 1))
        d.cmd(0xb8000000, 0)
        for lvl in range(5):
            s = DL()
            s.cmd(0xf7000000, 0x0a0a0a0a * (lvl + 1))
            if lvl < 4:
                s.cmd(0x06000000, BASE + 0x200 + 0x100 * (lvl + 1))
            else:
                s.cmd(0x06010000, BASE + 0x700)              # a branch
            s.cmd(0xf7000000, 0x0b0b0b0b * (lvl + 1))
            s.cmd(0xb8000000, 0)
            data[0x200 + 0x100 * lvl] = s.bytes()
        t = DL()
        t.cmd(0xf7000000, 0x33333333)
        t.cmd(0xb8000000, 0)
        data[0x700] = t.bytes()
    elif name == "culldl":
        d = dl
        common(d, 0x4)
        d.cmd(0xbc000404, 0x00000000)              # clip ratio K = 0: the second flag group trips at x <= 0 etc.
        d.cmd(0x01000040 | (0x02 << 16), BASE + 0x500)   # G_MTX modelview load
        d.cmd(0x01010040 | (0x02 << 16), BASE + 0x540)   # projection load (W = 10)
        d.cmd(0x04000000 | (2 << 20) | 48, BASE + 0x580)  # G_VTX n=3 at v0 = 0
        d.cmd(0xbe000000, 3 * 40)                          # G_CULLDL 0 .. 3 vertices
        d.cmd(0xf7000000, 0x44444444)                      # marker: only reached when the list was not culled
        d.cmd(0xb8000000, 0)
        data[0x500] = mtx(IDENT)
        p = IDENT[:]
        p[15] = 10                                         # W = 10
        data[0x540] = mtx(p)
        data[0x580] = vtx(-5, 0, 0) + vtx(-4, 0, 0) + vtx(-3, 0, 0)
    elif name == "popmtx":
        d = dl
        common(d, 0x20000 | 0x4 | 0x200)
        d.cmd(0xbc000002, 0x80000040)               # G_MW_NUMLIGHT: one light
        d.cmd(0x03000000 | (0x86 << 16) | 0x0f, BASE + 0x600)   # light 0
        d.cmd(0x03000000 | (0x82 << 16) | 0x0f, BASE + 0x610)   # look-at Y
        d.cmd(0x03000000 | (0x84 << 16) | 0x0f, BASE + 0x620)   # look-at X
        d.cmd(0x03000000 | (0x88 << 16) | 0x0f, BASE + 0x630)   # ambient (slot after the light)
        d.cmd(0x01010040 | (0x02 << 16), BASE + 0x540)           # projection
        d.cmd(0x01000040 | (0x06 << 16), BASE + 0x500)           # modelview load + push (identity)
        d.cmd(0x01000040 | (0x02 << 16), BASE + 0x510)           # modelview load: a turned matrix
        d.cmd(0x04000000 | (2 << 20) | 48, BASE + 0x580)         # lit vertices (the lights are stale: relit with the turned matrix)
        d.cmd(0xbf000000, (0 << 24) | (0 << 16) | (10 << 8) | 20)
        d.cmd(0xbd000000, 0)                                     # G_POPMTX: modelview back to identity
        d.cmd(0x04000000 | (2 << 20) | 48, BASE + 0x580)         # the same vertices again
        d.cmd(0xbf000000, (0 << 24) | (0 << 16) | (10 << 8) | 20)
        d.cmd(0xb8000000, 0)
        data[0x500] = mtx(IDENT)
        turn = [0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
        data[0x510] = mtx(turn)
        p = IDENT[:]
        p[15] = 1
        data[0x540] = mtx(p)
        data[0x600] = bytes([200, 100, 50, 0, 200, 100, 50, 0, 0x60, 0x20, 0x40, 0, 0, 0, 0, 0])   # colour, colour copy, direction
        data[0x610] = bytes([0, 0, 0, 0, 0, 0, 0, 0, 0, 0x7f, 0, 0, 0, 0, 0, 0])
        data[0x620] = bytes([0, 0, 0, 0, 0, 0, 0, 0, 0x7f, 0, 0, 0, 0, 0, 0, 0])
        data[0x630] = bytes([30, 30, 30, 0, 30, 30, 30, 0, 0, 0, 0, 0, 0, 0, 0, 0])
        data[0x580] = vtx(50, 40, 0, c=(0x40, 0x20, 0x60, 255)) + vtx(-40, 30, 0, c=(0x10, 0x70, 0x30, 255)) + vtx(0, -60, 0, c=(0x60, 0x60, 0x10, 255))
    else:
        sys.exit("unknown case " + name)
    data[0] = dl.bytes()
    for off, b in sorted(data.items()):
        patch.append(hexs(BASE + off, b))
    poke = ",".join("%x=%02x" % (0xff0 + i, b) for i, b in enumerate(struct.pack(">I", BASE)))
    print("HLE_PATCH=" + ";".join(patch), "HLE_POKE=" + poke)


if len(sys.argv) != 2:
    sys.exit(__doc__)
case(sys.argv[1])
