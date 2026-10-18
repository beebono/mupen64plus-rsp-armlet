#!/usr/bin/env python3
"""The reduced task of the Toy Story 2 perturbed-task mismatch (fuzz_snaps seed 2, F3DLX 1.23 task 22; docs/testing.md, "G_MODIFYVTX outside the vertex slots"), for armlet-native-replay (HLE_PATCH / HLE_POKE, --oracle).

  tools/hle/mksyn_modvtx.py [W0LOW [FIELD]]     prints  HLE_PATCH=... HLE_POKE=...  for `env`: a G_VTX of 4 nonzero vertex records, a G_MODIFYVTX with the slot offset W0LOW (default 0x7800) and the record
  field FIELD (default 0x18), a G_SETOTHERMODE_L and G_ENDDL. The microcode reads the slot address at vtab + (W0LOW & 0xfff) = 0xb1e for 0x7800: in the operand buffer, where the G_VTX staged its records
  (the last halfword of the fourth), which the lean mode does not fill. The front-end read the address 0 there and stored the word (0x02000100) at DMEM 0x18, the othermode word; the microcode's store went to
  the staged halfword. The front-end declines an offset outside the 32 slots (0 .. 0x3e, even) and a field outside the 0x28-byte record (mutation 471); W0LOW 0 FIELD 0x14 is a game's and stays native and equal.
  FIELD 0xb0 makes the second command a G_BRANCH_Z of the same slot (mutation 472: the slot address read at 0xb1e gives another z, the branch goes the other way).
  tools/hle/mutcheck_modvtx.sh runs both. Nothing of a game is in the output."""
import struct
import sys

BASE = 0x300000
lo = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x7800
fld = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x18
bz = fld == 0xb0        # FIELD 0xb0: a G_BRANCH_Z on the same slot instead (mutation 472): w1 = 0 jumps to the list at BASE + 0x800 (a different othermode write) when the slot's z is not above 0
w = [(0x01020040, BASE + 0x1000)] + ([(0xb4000000, BASE + 0x800), (0xb0000000 | lo, 0)] if bz else [(0xb2000000 | (fld << 16) | lo, 0x02000100)]) + [
     (0xb9000002, 1),
     (0xb8000000, 0)]
dl = b"".join(struct.pack(">II", a, b) for a, b in w)
dl2 = b"".join(struct.pack(">II", a, b) for a, b in [(0xb9000002, 2), (0xb8000000, 0)])
vt = bytes(0x20 + i for i in range(0x40))
print("HLE_PATCH=%x:%s;%x:%s" % (BASE, dl.hex(), BASE + 0x1000, vt.hex()) + ";%x:%s" % (BASE + 0x800, dl2.hex()), "HLE_POKE=" + ",".join("%x=%02x" % (0xff0 + i, b) for i, b in enumerate(struct.pack(">I", BASE))))
