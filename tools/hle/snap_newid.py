#!/usr/bin/env python3
"""Rewrites the ucode id in the header of snapshot files captured before 2026-10-16, when the id was the FNV-1a-64 over ucode_size bytes instead of the whole 4 KiB
(docs/options.md, GfxHleUcodes): an old id in a header makes today's armlet-native-replay find no build for the task.

  snap_newid.py [--dry-run] FILE.snap...

The header (struct armlet_snapfile_hdr, src/native/native_internal.h) holds the id as a host-order uint64 at offset 56. Files whose id is not an old one are left alone."""
import struct
import sys

OLD_TO_NEW = {
    0x19bd8c825b9b3f72: 0x76c763bc18ad3ab3,     # Wipeout 64 (F3DLX 1.21)
    0x410cfbcf87378a9c: 0xc2aca403f282af1b,     # Jet Force Gemini
    0x5dce3244420ce715: 0x6635562b2dde31ae,     # Buck Bumble / Cruis'n World (F3DEX 2.04)
    0x72a1416f6d8445c3: 0x909f82e8f8da6456,     # Diddy Kong Racing
    0x88fa3c0aa5315997: 0x0b574ecb06d75978,     # Tetrisphere
    0xb228360be43c0447: 0xfa171b4489d45ef2,     # Worms Armageddon (S2DEX 2.08)
    0xe80e52925f70b674: 0xe48f5dc9bed2fe3d,     # Conker's Bad Fur Day (F3DEXBG)
    0xac310503c83b23ea: 0xd2d1f9f2e7dfe1ab,     # Dr. Mario 64 / RR64 (F3DEX 2.08)
    0x1de5ed1ca5f278ab: 0x047252e810e35d56,     # Pokemon Puzzle League (F3DEX 1.23)
    0x9ac3b68bf66e164f: 0x208a567e9aead5ea,     # Quest 64 (F3DEX 1.21)
}


def main():
    dry = "--dry-run" in sys.argv
    files = [a for a in sys.argv[1:] if a != "--dry-run"]
    if not files:
        sys.exit(__doc__)
    n = 0
    for p in files:
        with open(p, "r+b") as f:
            f.seek(56)
            b = f.read(8)
            if len(b) != 8:
                continue
            old = struct.unpack("<Q", b)[0]
            new = OLD_TO_NEW.get(old)
            if new is None:
                continue
            n += 1
            print("%s: %016x -> %016x%s" % (p, old, new, " (dry run)" if dry else ""))
            if not dry:
                f.seek(56)
                f.write(struct.pack("<Q", new))
    print("%d of %d files had an old id" % (n, len(files)))


if __name__ == "__main__":
    main()
