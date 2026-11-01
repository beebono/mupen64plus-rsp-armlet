#!/bin/bash
# Makes the rsp2c translations of a 1.x Rej text's G_VTX and G_TRI1 / G_TRI2 handlers that tools/hle/build_test_rej1.sh checks the closed forms against (docs/testing.md "1.x Rej"). ROM-derived: keep the outputs out of git.
#   tools/hle/gen_rej1_oracles.sh DUMP_PREFIX OUTDIR [VTX TRI2 TRI1 HEAD]
# DUMP_PREFIX: <dir>/<ucodeid>.<datahash> of tools/ucode_dump_shim.c (.text, .ext: the RDRAM behind the text, .data); the pcs default to F3DLX.Rej 1.23's. The IMEM image is the head code (the DMA table's entry at
# DMEM 0x08) and the text at 0x80 with the lighting overlay (entry 0x18) in the overlay window, as the code finds it when it lights.
set -e
H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/../.." && pwd)
P=$1; O=$2; VTX=${3:-0x3ac}; T2=${4:-0xa70}; T1=${5:-0x1c0}; HEAD=${6:-0x09c}; mkdir -p "$O"
python3 - "$P" "$O/rej1.be" <<'EOF'
import struct, sys
p, out = sys.argv[1:3]
full = open(p + '.text', 'rb').read() + open(p + '.ext', 'rb').read()
d = open(p + '.data', 'rb').read()
img = bytearray(0x1000)
for k in (0, 1, 3):                                 # the text, the head, the lighting overlay (entries at DMEM 0, 8, 0x18: RDRAM offset, length - 1, IMEM address)
    o, l, a = struct.unpack('>IHH', d[8 * k:8 * k + 8])
    img[(a & 0xfff):(a & 0xfff) + l + 1] = full[o:o + l + 1]
open(out, 'wb').write(img)
EOF
EXT=(--ext "0xfd8:rj_dma(x, r)" --ext "0xfd4:rj_dma0(x, r)" --ext "0xfc4:rj_wait(x, r)")
python3 "$R/tools/rsp2c/rsp2c.py" --name rej1_vtx_gen --entry $VTX --stop $HEAD "${EXT[@]}" --seg "$O/rej1.be" 0x000 0x1000 --out "$O/rej1_vtx.inc"
python3 "$R/tools/rsp2c/rsp2c.py" --name rej1_tri_gen --entry $T2 --entry $T1 --stop $HEAD "${EXT[@]}" --seg "$O/rej1.be" 0x000 0x1000 --out "$O/rej1_tri.inc"
echo "wrote $O/rej1_vtx.inc $O/rej1_tri.inc"
