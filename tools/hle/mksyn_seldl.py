#!/usr/bin/env python3
"""The reduced task of Kirby 64 `load` seed 904 (tools/hle/mksyn_ex2.py on devel a355089), for armlet-native-replay (HLE_PATCH / HLE_POKE, --oracle).

  tools/hle/mksyn_seldl.py [AT]     prints  HLE_PATCH=... HLE_POKE=...  for `env`: G_LOAD_UCODE of S2DEX, a G_SELECT_DL whose flags word is DMEM 0x2b0 + AT (default 0xbe: 0x36e, the
  jump table entry of opcode 0 and 0xff), a G_SELECT_DL that calls the list at RDRAM 0x0c0000 (zeros), and the load back. The microcode's own G_SELECT_DL stores the word wherever AT says: opcode 0 then
  jumps to the loop head (a no-op, 0x004) where it jumped to the sync handler (0x144) before, which the front-end classified once per build and went on emitting a no-op word per zero command.
  The front-end declines a flags word over the jump table (and the other state it caches); AT below 0x60 or above 0xd4 is a game's and stays native (MKSYN_S2TEXT / S2DATA / F2TEXT / F2DATA as in mksyn_ex2.py).
  tools/hle/mutcheck_seldl.sh runs both. Nothing of a game is in the output."""
import os
import struct
import sys

BASE = 0x300000
at = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0xbe
s2t, s2d = int(os.environ.get("MKSYN_S2TEXT", "0x3c3b0"), 0), int(os.environ.get("MKSYN_S2DATA", "0x42980"), 0)
f2t, f2d = int(os.environ.get("MKSYN_F2TEXT", "0x39e90"), 0), int(os.environ.get("MKSYN_F2DATA", "0x42170"), 0)
w = [(0xdb060000, 0),
     (0xe1000000, s2d - 0x180), (0xdd0007ff, s2t),
     (0x04000000 | at, 0xffffffff),
     (0x04000000 | 0x940c, 0xffffffff),
     (0xe1000000, f2d - 0x180), (0xdd0007ff, f2t),
     (0xdf000000, 0)]
dl = b"".join(struct.pack(">II", a, b) for a, b in w)
print("HLE_PATCH=%x:%s" % (BASE, dl.hex()), "HLE_POKE=" + ",".join("%x=%02x" % (0xff0 + i, b) for i, b in enumerate(struct.pack(">I", BASE))))
