#!/bin/bash
# Makes the rsp2c translations of a Rej text's G_VTX and G_TRI1 / G_TRI2 handlers that tools/hle/build_test_rej.sh checks the closed forms against (docs/testing.md "Rej"). ROM-derived: keep the outputs out of git.
#   tools/hle/gen_rej_oracles.sh BUILD DUMP_PREFIX OUTDIR
# BUILD: xbus (Excitebike 64 F3DLX.Rej xbus 2.08), fifo203 (F-Zero X F3DLX.Rej fifo 2.03), fifo206 (Resident Evil 2 F3DEX.Rej fifo 2.06), flx (F-Zero X F3DFLX.Rej fifo 2.03F); the dump prefix is <dir>/<ucodeid>.<datahash>
set -e
H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/../.." && pwd)
B=$1; P=$2; O=$3; mkdir -p "$O"
DESC=0x2e8; [ "$B" = flx ] && DESC=0x288
python3 "$H/rej_img.py" "$P" "$O/$B.be" $DESC
case $B in
xbus)    VTX=0x4a4; T1=0x74c; T2=0x750; STOPS="--stop 0x194 --stop 0x190 --stop 0x210"; TSTOPS="$STOPS"; RESUME= ;;
fifo203) VTX=0x4a0; T1=0x748; T2=0x74c; STOPS="--stop 0x18c --stop 0x188 --stop 0x208"; TSTOPS="$STOPS --stop 0x250 --stop 0x254"; RESUME="--entry 0x884" ;;
flx)     VTX=0x45c; T1=0x704; T2=0x708; STOPS="--stop 0x18c --stop 0x188 --stop 0x208"; TSTOPS="$STOPS --stop 0x250 --stop 0x254"; RESUME="--entry 0x840" ;;
fifo206) VTX=0x4c4; T1=0x740; T2=0x744; STOPS="--stop 0x18c --stop 0x188 --stop 0x208"; TSTOPS="$STOPS --stop 0x250 --stop 0x254"; RESUME="--entry 0x87c" ;;
*) echo "unknown build $B" >&2; exit 2 ;;
esac
python3 "$R/tools/rsp2c/rsp2c.py" --name rej_vtx_gen --entry $VTX $STOPS --jr-regs 31 --ext "0xfd8:rj_dma(x, r)" --ext "0xfc8:rj_wait(x, r)" --seg "$O/$B.be" 0x000 0x1000 --out "$O/${B}_vtx.inc"
python3 "$R/tools/rsp2c/rsp2c.py" --name rej_tri_gen --entry $T1 --entry $T2 $RESUME $TSTOPS --jr-regs 31 --seg "$O/$B.be" 0x000 0x1000 --out "$O/${B}_tri.inc"
echo "wrote $O/${B}_vtx.inc $O/${B}_tri.inc"
