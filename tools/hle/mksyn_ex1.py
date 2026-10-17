#!/usr/bin/env python3
"""Synthetic F3DEX 1.x / F3DLX display lists for armlet-native-replay (HLE_PATCH / HLE_POKE, with --oracle): tasks that load another microcode text in the middle (G_RDPHALF_1 + G_LOAD_UCODE), from a seed.

  tools/hle/mksyn_ex1.py CASE SEED     prints  HLE_PATCH=... HLE_POKE=...  for `env`; run
      env MKSYN1_A_TEXT=... $(tools/hle/mksyn_ex1.py load 7) armlet-native-replay --oracle <a snapshot of build A>.snap
  The JIT runs the synthetic task in the replay world and the front-end is compared with its result. The snapshot supplies the start state (the boot code runs from it, the microcode A is the snapshot's own)
  and the OSTask; the display list, matrices and vertices are written to free RDRAM at 0x300000 (MKSYN_BASE) and the OSTask data_ptr is pointed at it. The loaded texts and their data go to BASE + 0x20000 ..:
  files named by the environment (the microcode texts are 4096 bytes and the data 2048: the dumps of tools/ucode_dump_run.sh, test-output/derive/dumps/<set>/<game>/dump/<text id>.<data id>.{text,data}), so
  a task can load a text the snapshot's RDRAM does not hold. Nothing of a game is in the output of this script but those files' bytes (ROM-derived: use it in test-output/, never commit its output).

  MKSYN1_A_TEXT / MKSYN1_A_DATA   the text and data of the snapshot's own build (loaded again: a re-initialisation, from another address); the overlays (the .ext file next to the text) follow the text.
                                  MKSYN1_A_ADDR: the physical address of the snapshot's own text (OSTask.ucode): its text and overlays are written there too
  MKSYN1_B_TEXT / MKSYN1_B_DATA   another build of the front-end (registered, or latched by the probe)
  MKSYN1_X_TEXT / MKSYN1_X_DATA   a text no F3DEX 1.x build covers (S2DEX 1.x: SpaceStation Silicon Valley, Pokemon Puzzle League), or an F3DEX 1.x text whose IMEM tail differs (F3DEX 0.95: declined)

  CASES  stage   no load: lit and unlit vertices, triangles, matrices, state commands, display list calls (the generator's own check: native == JIT)
         load    a few stages, then loads in a sequence drawn from  A -> A, A -> B, A -> B -> A, A -> B -> B, A -> A -> B  (the data of the loaded text 2048 bytes, or fewer: the DMEM above is the old state);
                 after a load the state commands are given again, as a game does (the data image resets the matrices, segments and viewport, a short one not)
         xload   a stage, then the load of X: the front-end declines (the JIT runs the task); a seed passes when both agree and the front-end declines or is equal
         call    as load, with the loads inside a called display list (the return stack survives a data image shorter than 0x148 bytes, else the depth byte is the image's)
"""
import os
import random
import struct
import sys

BASE = int(os.environ.get("MKSYN_BASE", "0x300000"), 0)


class DL:
    def __init__(self):
        self.w = []

    def cmd(self, w0, w1=0):
        self.w.append((w0 & 0xffffffff, w1 & 0xffffffff))

    def bytes(self):
        return b"".join(struct.pack(">II", a, b) for a, b in self.w)


def mtx(vals):
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
    m[12] = r.uniform(-4, 4) * scale
    m[13] = r.uniform(-4, 4) * scale
    m[14] = r.uniform(-4, 4) * scale
    m[15] = r.uniform(-2, 6) * scale
    return m


def ident():
    return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]


def moveword(d, index, off, val):
    d.cmd(0xbc000000 | (off << 8) | index, val)


class Gen:
    """one task: the display list, the RDRAM rows, the running counters"""

    def __init__(self, r):
        self.r = r
        self.d = DL()
        self.data = {}
        self.vaddr = 0x6000
        self.maddr = 0x4000
        self.laddr = 0x2400
        self.paddr = 0x2000
        self.lx = os.environ.get("MKSYN1_LX") == "1"

    def setup(self, geom, lit=False):
        r, d = self.r, self.d
        moveword(d, 6, 0, 0)                                    # segment 0 = 0
        for off, v in ((0x04, 3), (0x0c, 3), (0x14, -3), (0x1c, -3)):      # the clip ratios: x, y
            moveword(d, 4, off, v)
        d.cmd(0xb6000000, 0xffffffff)
        d.cmd(0xb7000000, geom)
        tz = r.choice([0x1ff, 0, 0x100])
        vp = struct.pack(">8h", 160 * 4, 120 * 4, 0x1ff, 0, 160 * 4, 120 * 4, tz, 0)
        self.data[self.paddr] = vp
        d.cmd(0x0380000f, BASE + self.paddr)                    # G_MOVEMEM viewport (16 bytes)
        self.paddr += 0x10
        if r.random() < 0.5:
            d.cmd(0xbb000000 | r.choice([0x0001, 0x0101, 0x0801, 0x0000]), r.choice([0xffffffff, 0x80008000, 0x20004000]))
        if lit:
            nl = r.randrange(1, 5)
            moveword(d, 2, 0, 0x80000000 + (nl + 1) * 32)       # G_MW_NUMLIGHT
            for slot in range(nl + 2):                          # look-at Y, look-at X (idx 0x82, 0x84), the lights from 0x86, the ambient behind them
                idx = 0x82 + 2 * slot if slot < 2 else 0x86 + 2 * (slot - 2)
                col = bytes([r.randrange(256), r.randrange(256), r.randrange(256), 0])
                dirv = bytes([r.randrange(256), r.randrange(256), r.randrange(256), 0])
                self.data[self.laddr] = (col + col + dirv + b"\0" * 4) if slot >= 2 else b"\0" * 8 + bytes([0x7f, 0, 0, 0] if slot == 0 else [0, 0x7f, 0, 0]) + b"\0" * 4
                d.cmd(0x03000000 | (idx << 16) | 0x0f, BASE + self.laddr)
                self.laddr += 0x20

    def matrices(self, scale):
        r, d = self.r, self.d
        self.data[self.maddr] = mtx(ident())
        self.data[self.maddr + 0x40] = mtx(rnd_matrix(r, scale))
        d.cmd(0x01030000 | 0x3f, BASE + self.maddr + 0x40)      # projection, load
        d.cmd(0x01020000 | 0x3f, BASE + self.maddr)             # modelview, load
        self.maddr += 0x80

    def batch(self, lit):
        r, d = self.r, self.d
        n = r.choice([3, 3, 4, 5, 6, 9, 11])
        v0 = r.randrange(0, 32 - n + 1)
        vs = b""
        for _ in range(n):
            if lit:
                nrm = [r.randrange(-127, 128) for _ in range(3)]
                vs += struct.pack(">3hH2h4B", r.randint(-200, 200), r.randint(-200, 200), r.randint(-200, 200), 0, r.randrange(-1024, 1024), r.randrange(-1024, 1024),
                                  nrm[0] & 255, nrm[1] & 255, nrm[2] & 255, r.randrange(256))
            else:
                sp = r.choice([2, 5, 20, 100])
                vs += vtx(r.randint(-sp, sp) * r.choice([1, 2]), r.randint(-sp, sp), r.randint(-sp, sp), r.randrange(-1024, 1024), r.randrange(-1024, 1024),
                          (r.randrange(256), r.randrange(256), r.randrange(256), r.randrange(256)))
        self.data[self.vaddr] = vs
        d.cmd(0x04000000 | (v0 * 2 << 16) | (n << 10) | (16 * n - 1), BASE + self.vaddr)
        self.vaddr += 0x200
        for _ in range(r.randrange(2, 8)):
            a, b, c = r.sample(range(v0, v0 + n), 3)
            if r.random() < 0.3:
                a2, b2, c2 = r.sample(range(v0, v0 + n), 3)
                d.cmd(0xb1000000 | (a2 * 2 << 16) | (b2 * 2 << 8) | c2 * 2, (a * 2 << 16) | (b * 2 << 8) | c * 2)
            else:
                d.cmd(0xbf000000, (a * 2 << 16) | (b * 2 << 8) | c * 2)

    def rdp(self):
        r, d = self.r, self.d
        k = r.randrange(5)
        if k == 0:
            d.cmd(0xf7000000, r.randrange(1 << 32))
        elif k == 1:
            d.cmd(0xe7000000, 0)
        elif k == 2:
            d.cmd(r.choice([0xb9, 0xba]) << 24 | (r.randrange(32) << 8) | r.randrange(1, 16), r.randrange(1 << 32))   # G_SETOTHERMODE_L / _H
        elif k == 3:
            d.cmd(0xfc000000 | r.randrange(1 << 24), r.randrange(1 << 32))
        else:
            d.cmd(0xed000000 | (r.randrange(0, 40) << 12) | r.randrange(0, 40) * 4, ((1280 - r.randrange(0, 40) * 4) << 12) | (960 - r.randrange(0, 40) * 4))

    def stage(self, geom_extra=0):
        """the state commands of a game after (re)initialisation, then vertices and triangles"""
        r, d = self.r, self.d
        lit = r.random() < 0.3
        geom = r.choice([0, 0x4, 0x4 | 0x200]) | r.choice([0, 1]) | r.choice([0, 0, 0x1000, 0x2000]) | (0x20000 if lit else 0) | (0x800000 if not self.lx or r.random() < 0.9 else 0) | geom_extra
        self.setup(geom, lit)
        self.matrices(r.choice([1, 0.5, 1 / 8]))
        for _ in range(r.randrange(1, 4)):
            self.batch(lit)
            if r.random() < 0.4:
                self.rdp()


def load(g, text_addr, data_addr, size, kseg):
    d = g.d
    t = text_addr | (0x80000000 if kseg else 0)
    da = data_addr | (0x80000000 if kseg else 0)
    d.cmd(0xb4000000, da)                                       # G_RDPHALF_1: the data
    d.cmd(0xaf000000 | size, t)                                 # G_LOAD_UCODE: the text; size - 1 bytes of data to DMEM 0


def case(name, seed):
    r = random.Random(seed * 7919 + len(name))
    g = Gen(r)
    env = os.environ
    rows = {}
    TA, DA, TB, DB, TX, DX = 0x20000, 0x24000, 0x26000, 0x2a000, 0x2c000, 0x30000
    for key, off, n in (("A_TEXT", TA, 4096), ("A_DATA", DA, 2048), ("B_TEXT", TB, 4096), ("B_DATA", DB, 2048), ("X_TEXT", TX, 4096), ("X_DATA", DX, 2048)):
        if env.get("MKSYN1_" + key):
            rows[off] = open(env["MKSYN1_" + key], "rb").read()[:n]
            if key.endswith("TEXT") and os.path.exists(env["MKSYN1_" + key][:-4] + "ext"):       # the overlays follow the text (the code the DMA table of the data points at: ucode + 0x1008 ..)
                rows[off + 0x1000] = open(env["MKSYN1_" + key][:-4] + "ext", "rb").read()
    if env.get("MKSYN1_A_ADDR") and "MKSYN1_A_TEXT" in env:        # the snapshot's own text and overlays (physical address): a task that never lit a vertex has no overlay code in its RDRAM for the JIT to run
        a = int(env["MKSYN1_A_ADDR"], 0)
        rows[a - BASE] = rows[TA]
        if TA + 0x1000 in rows:
            rows[a + 0x1000 - BASE] = rows[TA + 0x1000]
    def size():
        if env.get("MKSYN1_SIZE"):
            return int(env["MKSYN1_SIZE"], 0)
        return r.choice([0x7ff] * 10 + [0x3ff, 0x1ff, 0x5ff, 0x27f, 0x7f])
    kseg = r.random() < 0.5
    sizes = []
    if name == "stage":
        g.stage()
        g.d.cmd(0xb8000000, 0)
    elif name in ("load", "call"):
        seq = r.choice(["A", "B", "BA", "BB", "AB", "AA", "B"])
        if env.get("MKSYN1_SEQ"):
            seq = env["MKSYN1_SEQ"]
        outer = g.d
        if env.get('MKSYN1_INFO'):
            print('INFO', name, seed, seq, file=sys.stderr)
        if name == "call":
            sub = DL()
            g.d = sub
        g.stage()
        for ch in seq:
            sz = size()
            sizes.append(sz)
            if ch == "A":
                load(g, BASE + TA, BASE + DA, sz, kseg)
            else:
                load(g, BASE + TB, BASE + DB, sz, kseg)
            g.stage()
        if env.get('MKSYN1_INFO'):
            print('SIZES', [hex(x) for x in sizes], 'kseg', kseg, file=sys.stderr)
        g.d.cmd(0xb8000000, 0)
        if name == "call":
            g.data[0x10000] = sub.bytes()
            outer.cmd(0x06000000, BASE + 0x10000)                # a call
            outer.cmd(0xb8000000, 0)
            g.d = outer
            g.d.w = outer.w
    elif name == "xload":
        g.stage()
        load(g, BASE + TX, BASE + DX, 0x7ff, kseg)
        g.stage()
        g.d.cmd(0xb8000000, 0)
    else:
        sys.exit("unknown case " + name)
    assert len(g.d.bytes()) <= 0x2000 or name == 'call'
    g.data[0] = g.d.bytes()
    if env.get("MKSYN1_FUZZ"):                                  # perturbed lists (fuzz_snaps.py's idea on the synthetic task): bytes of the list, the vertices and the matrices flipped, command words swapped
        fr = random.Random(seed * 31 + 7)
        dl = bytearray(g.data[0])
        for _ in range(fr.randrange(1, 6)):
            k = fr.randrange(3)
            if k == 0:
                dl[fr.randrange(len(dl))] = fr.randrange(256)
            elif k == 1:
                a, b = fr.randrange(len(dl) // 8) * 8, fr.randrange(len(dl) // 8) * 8
                dl[a:a + 8] = dl[b:b + 8]
            else:
                key = fr.choice([x for x in g.data if x])
                row = bytearray(g.data[key])
                row[fr.randrange(len(row))] = fr.randrange(256)
                g.data[key] = bytes(row)
        g.data[0] = bytes(dl)
    patch = ["%x:%s" % (BASE + off, b.hex()) for off, b in sorted(g.data.items())] + ["%x:%s" % (BASE + off, b.hex()) for off, b in sorted(rows.items())]
    poke = ",".join("%x=%02x" % (0xff0 + i, b) for i, b in enumerate(struct.pack(">I", BASE)))
    print("HLE_PATCH=" + ";".join(patch), "HLE_POKE=" + poke)


if len(sys.argv) != 3:
    sys.exit(__doc__)
case(sys.argv[1], int(sys.argv[2]))
