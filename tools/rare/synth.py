#!/usr/bin/env python3
"""synth.py GAME OUTDIR N SEED TEMPLATE.snap : N synthetic task snapshots of Rare's microcode (GAME = dkr | jfg), for armlet-native-replay --oracle.

The template (a captured task start, ARMLET_HLE_CAPTURE) supplies the microcode (its text and data rows), the register file and the DMEM of a task start; the display list, and everything
it names, is made up: a random scene of matrices, viewports, vertices (small, large, near the clip planes), triangles (every flag combination, flat and smooth, textured, z-buffered, culled),
RDP commands, nested lists, fog, vertex offsets, other modes, the segment table (and for Jet Force Gemini the matrix product, the base words and the texture offset table). Nothing of the template's
list is in the result. The replay runs the native front-end and the JIT from the same state, so every difference is an error of the front-end. Snapshots are ROM-derived (the text rows): OUTDIR
belongs in test-output/."""
import os
import random
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "native"))
import snaplib

HDR = "<II48sQIIIII4x"
HSIZE = struct.calcsize(HDR)
SNAP = snaplib.SNAP_SIZE
DMEM_OFF = struct.calcsize("<II I I I I I i 32h 16I 33I 256H 48H 24H h h b 3x")      # byte offset of the DMEM words in a PrspNativeSnap
OST_DATA = 0xff0                                                                    # OSTask.data_ptr in DMEM


def swap32(b):
    """the bytes of an RDRAM row as the snapshot file holds them: the host words of the RDRAM image (little-endian words of big-endian data)"""
    return b''.join(b[i:i + 4][::-1] for i in range(0, len(b), 4))


OFF = set(filter(None, os.environ.get("SYN_OFF", "").split(",")))      # features left out (for finding what makes a case unusable)


class Task:
    def __init__(self, game, rng):
        self.g = game
        self.r = rng
        self.rows = []                      # (address, bytes)
        self.next = 0x500000
        self.nv = 0                          # vertices loaded (slots 0 .. nv - 1)
        self.jfg = game == "jfg"
        self.vslot0 = 0
        self.base = {1: 0, 4: 0}              # Jet Force Gemini: the base words (0xbf) the addresses of matrix and vertex commands are relative to

    def alloc(self, data, align=8):
        a = (self.next + align - 1) & ~(align - 1)
        data = bytes(data) + bytes(-len(data) & 7)
        self.rows.append((a, data))     # (logical bytes; swapped when the file is written)
        self.next = a + len(data) + 64
        return a

    # ---- commands ----------------------------------------------------------------------------------
    @staticmethod
    def w(w0, w1):
        return struct.pack(">II", w0 & 0xffffffff, w1 & 0xffffffff)

    def data_cmd(self, op, b1, data, extra_low=0, w1_extra=0, pad=0):
        """a class-0 command: the DMA of len(data) bytes (+ pad) from a fresh address"""
        buf = bytes(pad) + bytes(data)
        a = self.alloc(buf + bytes(16), 8)
        ln = (len(buf) + 7) & ~7
        w0 = op << 24 | b1 << 16 | extra_low << 8 | (ln - 1)
        return self.w(w0, a + w1_extra - self.base.get(op, 0))

    def matrix(self, slot_b1, mtx_bytes, mult=None):
        return self.data_cmd(1, slot_b1, mtx_bytes)

    def rmtx(self, mode):
        r = self.r
        m = bytearray(64)
        for row in range(4):
            for col in range(4):
                if mode == 0:       # near identity with scale
                    iv = (r.choice([1, 2, 4, 0, 0, 0, -1])) if row == col else (0 if r.random() < 0.8 else r.randint(-3, 3))
                    fv = r.getrandbits(16) if r.random() < 0.5 else 0
                elif mode == 1:     # wild
                    iv = r.randint(-300, 300); fv = r.getrandbits(16)
                elif mode == 2:     # extremes
                    iv = r.choice([-32768, 32767, 0, 1, -1, 16384]); fv = r.choice([0, 0xffff, 0x8000, r.getrandbits(16)])
                elif mode == 4:     # a perspective-like matrix: unit scale, W = z + d, so that vertices of a few hundred units are about as large as the clip volume
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
                else:               # a projective-ish matrix: small entries, w column from z
                    iv = r.randint(-2, 2) if row != col else r.choice([1, 2]); fv = r.getrandbits(16)
                    if col == 3 and row != 3:
                        iv = 0 if r.random() < 0.7 else r.randint(0, 1)
                struct.pack_into(">h", m, 8 * row + 2 * col, iv)
                struct.pack_into(">H", m, 32 + 8 * row + 2 * col, fv)
        return bytes(m)

    def vertices(self, n, mode, colours=True):
        r = self.r
        out = bytearray()
        for i in range(n):
            if mode == 0:
                x, y, z = (r.randint(-100, 100) for _ in range(3))
            elif mode == 1:
                x, y, z = (r.randint(-3000, 3000) for _ in range(3))
            elif mode == 2:
                x, y, z = (r.choice([-32768, 32767, r.randint(-32768, 32767), 0]) for _ in range(3))
            elif mode == 3:
                x, y, z = (r.randint(-1200, 1200) for _ in range(3))
            else:
                x, y, z = (r.randint(-500, 500) for _ in range(3))
            out += struct.pack(">hhh", x, y, z) + bytes(r.getrandbits(8) for _ in range(4))
        return bytes(out)

    def vtx(self, n, v0, mode, append=False):
        """n vertices (1 .. 32)"""
        r = self.r
        pad = r.choice([0, 2, 4, 6])
        data = self.vertices(n, mode)
        ln = pad + len(data)
        if self.jfg:
            b1 = (max(n, 1) << 3 & 0xf8) | pad
            if n > 31:
                n = 31
            b1 = (n << 3) | pad
            ln = (pad + 10 * n + 7) & ~7
            buf = bytes(pad) + data[:10 * n]
            a = self.alloc(buf + bytes(16), 8)
            w0 = 4 << 24 | b1 << 16 | (v0 * 2) << 8 | (ln - 1)
            return self.w(w0, a + pad - self.base[4])
        b1 = ((n - 1) << 3) | pad | (1 if append else 0)
        buf = bytes(pad) + data
        a = self.alloc(buf + bytes(16), 8)
        ln = (len(buf) + 7) & ~7
        return self.w(4 << 24 | b1 << 16 | (ln - 1), a + pad)

    def tris(self, count, nvtx, flag0):
        r = self.r
        recs = bytearray()
        for i in range(count):
            fl = r.choice([0x00, 0x40, 0x00, 0x40, r.getrandbits(8)])
            idx = [r.randrange(nvtx) for _ in range(3)]
            if r.random() < 0.15:
                idx = [idx[0], idx[0], idx[1]]
            st = b"".join(struct.pack(">hh", r.choice([r.randint(-32768, 32767), r.randint(-2000, 2000), 0]), r.randint(-4000, 4000)) for _ in range(3))
            recs += bytes([fl, idx[0], idx[1], idx[2]]) + st
        a = self.alloc(bytes(recs) + bytes(16), 8)
        ln = len(recs)
        b1 = ((count - 1) << 4) | (flag0 & 1)
        return self.w(5 << 24 | b1 << 16 | (ln - 1), a)

    def block(self, n):
        r = self.r
        cmds = bytearray()
        for i in range(n):
            op = r.choice([0xe7, 0xe6, 0xe8, 0xf5, 0xf2, 0xf3, 0xfc, 0xef, 0xf7, 0xf6, 0x00, 0xe4])
            cmds += struct.pack(">II", op << 24 | r.getrandbits(24), r.getrandbits(32))
        a = self.alloc(bytes(cmds), 8)
        return self.w(7 << 24 | n << 16 | (8 * n - 1), a)


def scene(t, depth=0):
    r = t.r
    out = []
    jfg = t.jfg
    # geometry mode, persp, texture, other modes, fog, offsets
    gm_set = 0
    for bit, p in ((0x1, 0.5), (0x2, 0.5), (0x4, 0.6), (0x200, 0.5), (0x2000, 0.4), (0x1000, 0.3), (0x10000, 0.3)):
        if r.random() < p:
            gm_set |= bit
    out.append(t.w(0xb6 << 24, 0xffffffff))
    out.append(t.w(0xb7 << 24, gm_set))
    out.append(t.w(0xb4 << 24, r.choice([0xffff, 0x7fff, r.getrandbits(16), 0x8000, 0x4000])))
    out.append(t.w(0xbb << 24 | r.getrandbits(8) << 16 | r.getrandbits(8) << 8 | r.getrandbits(1), r.getrandbits(32)))
    t.late_texture = t.w(0xbb << 24 | r.getrandbits(8) << 16 | r.getrandbits(8) << 8 | r.getrandbits(1), r.getrandbits(32))
    if r.random() < 0.5:
        out.append(t.w(0xba << 24 | 4 << 8 | 4, r.getrandbits(32) & 0xf0))
        out.append(t.w(0xb9 << 24 | 0 << 8 | 31, r.getrandbits(32)))
    # the clip ratio K (word at 0x74), z clamp byte etc. are left; fog, offset enable and vectors
    if r.random() < 0.5:
        out.append(t.w(0xbc << 24 | 0 << 8 | 8, (r.choice([0x100, 0x200, 0x80, r.getrandbits(16)]) << 16) | r.choice([0, 0x10, 0xffe0, r.getrandbits(16)])))
    off_on = r.random() < 0.4
    out.append(t.w(0xbc << 24 | 0 << 8 | 2, 1 if off_on else 0))                          # (moveword index 2: the word at 0x144, the vertex offset enable)
    if off_on or r.random() < 0.3:
        for k in range(4):                                                                  # the offset vector in slot 0's record: integer words, fraction words
            out.append(t.w(0xbc << 24 | 4 * k << 8 | 0xc, r.choice([0, r.getrandbits(32), (r.randint(-50, 50) & 0xffff) << 16 | r.randint(-50, 50) & 0xffff])))
    if r.random() < 0.3:
        out.append(t.w(0xbc << 24 | 4 << 8 | 6, 0x100000))                                 # segment 1
    if jfg:
        b0, b1 = r.choice([0, 0x1000, 0x20000]), r.choice([0, 0x100, 0x30000, 0xfff00000])
        if "bases" in OFF:
            b0 = b1 = 0
        out.append(t.w(0xbf << 24 | b0, b1))
        t.base = {1: b0, 4: b1}
    # viewport (op 3, b1 0x80)
    vp = struct.pack(">hhhh", r.choice([640, 160 * 4, r.randint(1, 1000)]), r.choice([480, 120 * 4, r.randint(1, 1000)]), r.choice([0x1ff, 0x3ff, r.getrandbits(15)]), 0) + \
        struct.pack(">hhhh", r.choice([640, 160 * 4, r.randint(-1000, 1000)]), r.choice([480, 120 * 4, r.randint(-1000, 1000)]), r.choice([0x1ff, 0, r.getrandbits(15)]), 0)
    out.append(t.data_cmd(3, 0x80, vp[:16]))
    nmat = r.randint(1, 3 if not jfg else 4)
    cur = 0
    for k in range(nmat):
        mode = r.choice([4, 4, 4, 4, 0, 1, 2, 3])
        m = t.rmtx(mode)
        if not jfg:
            slot = r.choice([0, 0x40, 0x80])
            out.append(t.matrix(slot, m))
        else:
            if k == 0 or r.random() < 0.5:
                out.append(t.matrix(r.randrange(5), m))               # (slots 5 .. 7 would overlap the vertex records)
            else:
                out.append(t.matrix((0x80 if "mul" not in OFF else 0) | r.randrange(8) << 3 | r.randrange(8), m))
        # vertices and triangles under this matrix
        nv = r.randint(1, 24)
        vm = r.choice([3, 3, 3, 1, 0, 2]) if mode != 4 else r.choice([4, 4, 4, 4, 3, 1, 0])
        if jfg:
            v0 = r.randint(0, 12)
            nv = min(nv, 24 - v0)
            out.append(t.vtx(nv, v0, vm))
            base = v0
        else:
            out.append(t.vtx(nv, 0, vm))
            base = 0
        # a second load that appends / moves
        if r.random() < 0.3:
            n2 = r.randint(1, 12)
            if jfg:
                v2 = r.randint(0, 12)
                out.append(t.vtx(n2, v2, vm))
                lo, hi = min(base, v2), max(base + nv, v2 + n2)
            else:
                n2 = min(n2, 24 - nv)
                out.append(t.vtx(max(n2, 1), 0, vm, append=True))
                hi = nv + max(n2, 1)
                lo = 0
        else:
            lo, hi = base, base + nv
        if hi - lo < 1:
            continue
        for q in range(r.randint(1, 4)):
            nt = r.randint(1, 10)
            tri = t.tris(nt, hi - lo, r.getrandbits(1))
            # the vertex indices are relative to the loaded range: patch the command's data so they are absolute slots
            out.append(tri)
            t.patch_tris = getattr(t, "patch_tris", []) + [(len(t.rows) - 1, lo, hi - lo)]
        if r.random() < 0.5:
            out.append(t.block(r.randint(1, 8)))
        if r.random() < 0.4:                                                    # (the texture command after the triangles: only the last one is seen in the end state)
            out.append(t.late_texture)
    # raw RDP commands
    for q in range(r.randint(0, 6)):
        op = r.choice([0xe7, 0xe6, 0xe8, 0xf5, 0xf2, 0xf3, 0xfc, 0xef, 0xf7, 0xf6, 0xff, 0xfe, 0xfd, 0xe4, 0xe5, 0xf0, 0xfa, 0xf9, 0xf8])
        out.append(t.w(op << 24 | r.getrandbits(24), r.choice([r.getrandbits(32), 0x02000100 + r.getrandbits(12) * 8])))
        if op in (0xe4, 0xe5):
            out.append(t.w(0xb3 << 24, r.getrandbits(32)))
            out.append(t.w(0xb2 << 24, r.getrandbits(32)))
    if jfg and r.random() < 0.5 and "tex" not in OFF:
        tab = b"".join(struct.pack(">H", r.randrange(0, 0x1000) & ~1) for _ in range(32))
        out.append(t.data_cmd(2, 0, tab))
        out.append(t.w(0xfd << 24 | r.getrandbits(24), 0x01000000 + r.getrandbits(12) * 8))
        out.append(t.w(0xfd << 24 | r.getrandbits(24), 0x100000 + r.getrandbits(12) * 8))
    return out


def build(game, rng, template):
    d = bytearray(open(template, "rb").read())
    magic, ver, name, ucode, entry, exit_, nrows, nev, flags = struct.unpack(HDR, bytes(d[:HSIZE]))
    t = Task(game, rng)
    t.patch_tris = []
    cmds = []
    for s in range(rng.randint(1, 4)):
        sub = scene(t)
        if rng.random() < 0.25 and len(sub) > 2:                     # a nested list: the scene as a called list
            a = t.alloc(b"".join(sub) + t.w(0xb8 << 24, 0), 8)
            cmds.append(t.w(6 << 24 | (rng.choice([0, 0, 1]) << 16) | 7, a))
        else:
            cmds += sub
    cmds.append(t.w(0xb8 << 24, 0))
    # triangle indices: make them absolute slots (the vertex range of the load they follow)
    for (ri, lo, cnt) in t.patch_tris:
        a, buf = t.rows[ri]
        buf = bytearray(buf)
        for off in range(0, len(buf) - 16 if len(buf) > 16 else len(buf), 16):
            for k in (1, 2, 3):
                if off + k < len(buf):
                    buf[off + k] = lo + buf[off + k] % cnt if rng.random() < 0.995 else buf[off + k]
        t.rows[ri] = (a, bytes(buf))
    lst = t.alloc(b"".join(cmds) + bytes(512), 8)
    # file: header, before (DMEM word at OST_DATA = the list), rows + ours, after, events
    o = HSIZE
    before = bytearray(d[o:o + SNAP])
    struct.pack_into("<I", before, DMEM_OFF + OST_DATA, lst)
    o += SNAP
    rows = bytearray()
    ro = o
    for _ in range(nrows):
        a, n = struct.unpack("<II", d[ro:ro + 8])
        rows += d[ro:ro + 8 + n]
        ro += 8 + n
    rest = d[ro:]
    text = os.environ.get("SYN_TEXT")                        # the whole microcode text as the game's RDRAM holds it (overlays included): a template task that did not use the clipper has none of it
    if text:
        ub = struct.unpack_from("<I", before, DMEM_OFF + 0xfd0)[0] & 0x7ffff8        # OSTask.ucode
        buf = open(text, "rb").read()
        rows += struct.pack("<II", ub, len(buf)) + swap32(buf)
        nrows += 1
    for a, buf in t.rows:
        rows += struct.pack("<II", a, len(buf)) + swap32(buf)
    hdr = struct.pack(HDR, magic, ver, name, ucode, entry, exit_, nrows + len(t.rows), nev, flags)
    return hdr + bytes(before) + bytes(rows) + bytes(rest)


def main():
    game, outdir, n, seed, tmpl = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
    os.makedirs(outdir, exist_ok=True)
    for i in range(n):
        rng = random.Random(seed * 100003 + i)
        open(os.path.join(outdir, "syn.%s.%d.snap" % (game, i)), "wb").write(build(game, rng, tmpl))


if __name__ == "__main__":
    main()
