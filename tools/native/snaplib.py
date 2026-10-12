"""snaplib.py: reads armlet routine-entry snapshot files (native_internal.h, prsp_native.h) in Python."""
import struct

SNAP_FMT = "<II I I I I I i 32h 16I 33I 256H 48H 24H h h b 3x 1024I 1024I"   # PrspNativeSnap, see prsp_native.h
SNAP_SIZE = struct.calcsize(SNAP_FMT)


class Snap:
    def __init__(self, raw):
        t = struct.unpack(SNAP_FMT, raw)
        i = 0
        self.magic, self.size, self.pc, self.has_delay_slot, self.branch_target, self.irq, self.dirty, self.timeout = t[:8]
        i = 8
        self.mfc0 = t[i:i + 32]; i += 32
        self.cr = t[i:i + 16]; i += 16
        self.sr = t[i:i + 33]; i += 33
        v = t[i:i + 256]; i += 256
        self.vr = [v[8 * k:8 * k + 8] for k in range(32)]
        f = t[i:i + 48]; i += 48
        self.flags = [f[16 * k:16 * k + 16] for k in range(3)]
        self.acc = t[i:i + 24]; i += 24
        self.div_out, self.div_in, self.dp = t[i:i + 3]; i += 3
        self.dmem = t[i:i + 1024]; i += 1024
        self.imem = t[i:i + 1024]

    def flag_bits(self, k):
        f = self.flags[k]
        return sum((1 if f[8 + b] else 0) << b | (1 if f[b] else 0) << (8 + b) for b in range(8))

    def dm8(self, a):
        w = self.dmem[(a & 0xfff) >> 2]
        return (w >> (24 - 8 * (a & 3))) & 0xff

    def dm16(self, a):
        return (self.dm8(a) << 8) | self.dm8(a + 1)

    def dm32(self, a):
        return self.dmem[(a & 0xfff) >> 2]


def read(path):
    d = open(path, "rb").read()
    hsize = struct.calcsize("<II48sQIIIII4x")
    magic, ver, name, ucode, entry, exit_, nrows, nev, flags = struct.unpack("<II48sQIIIII4x", d[:hsize])
    o = hsize
    before = Snap(d[o:o + SNAP_SIZE]); o += SNAP_SIZE
    rows = []
    for _ in range(nrows):
        a, n = struct.unpack("<II", d[o:o + 8]); o += 8
        rows.append((a, d[o:o + n])); o += n
    after = Snap(d[o:o + SNAP_SIZE]); o += SNAP_SIZE
    return dict(name=name.split(b"\0")[0].decode(), ucode=ucode, entry=entry, exit=exit_, before=before, after=after, rows=rows)
