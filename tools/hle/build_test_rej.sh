#!/bin/bash
# builds and runs test_rej (the Rej closed forms on random DMEM images; against the microcode when the rsp2c translations are given, docs/testing.md "Rej").
#   tools/hle/build_test_rej.sh [CASES [SEED]]       env: REJ_DATA=<.data dump of a Rej text> (required), REJ_VTX_ORACLE / REJ_TRI_ORACLE=<generated .inc>, EXTRA=<compiler flags, e.g. -DARMLET_HLE_MUTATE=345>
#   HOST=1: natively (x86_64, scalar only); else the aarch64 cross compiler under qemu-aarch64-static (NEON and, with SCALAR=1, the scalar forms)
set -e
H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/../.." && pwd)
O=${OUT:-$H/out/rej${HOST:+-host}-${REJ_BUILD:-xbus}}; mkdir -p "$O"
if [ -n "$HOST" ]; then CC=cc; CXX=c++; RUN=; EXTRA2=; NEON=; else CC=aarch64-linux-gnu-gcc; CXX=aarch64-linux-gnu-g++; RUN=qemu-aarch64-static; EXTRA2=-static; NEON="$R/src/hle/f3dex2_fx_neon.c"; fi
[ -n "$SCALAR" ] && EXTRA2="$EXTRA2 -DARMLET_HLE_SCALAR"
case ${REJ_BUILD:-xbus} in            # the build under test (tools/hle/gen_rej_oracles.sh has the same names)
xbus)    BP="-DREJ_VTX_ENTRY=0x4a4 -DREJ_TRI1_ENTRY=0x74c -DREJ_TRI2_ENTRY=0x750 -DREJ_HEAD=0x194 -DREJ_LAYOUT=l_lay_rej_xbus" ;;
fifo203) BP="-DREJ_VTX_ENTRY=0x4a0 -DREJ_TRI1_ENTRY=0x748 -DREJ_TRI2_ENTRY=0x74c -DREJ_HEAD=0x18c -DREJ_LAYOUT=l_lay_rej_fifo203 -DREJ_FIFO=1 -DREJ_FLUSH_A=0x250 -DREJ_FLUSH_B=0x254 -DREJ_RESUME=0x884" ;;
fifo206) BP="-DREJ_VTX_ENTRY=0x4c4 -DREJ_TRI1_ENTRY=0x740 -DREJ_TRI2_ENTRY=0x744 -DREJ_HEAD=0x18c -DREJ_LAYOUT=l_lay_rej_fifo206 -DREJ_FIFO=1 -DREJ_FLUSH_A=0x250 -DREJ_FLUSH_B=0x254 -DREJ_RESUME=0x87c" ;;
flx)     BP="-DREJ_VTX_ENTRY=0x45c -DREJ_TRI1_ENTRY=0x704 -DREJ_TRI2_ENTRY=0x708 -DREJ_HEAD=0x18c -DREJ_LAYOUT=l_lay_rej_flx -DREJ_FIFO=1 -DREJ_FLUSH_A=0x250 -DREJ_FLUSH_B=0x254 -DREJ_RESUME=0x840" ;;
*) echo "unknown REJ_BUILD" >&2; exit 2 ;;
esac
OR="$BP"
[ -n "$REJ_VTX_ORACLE" ] && OR="$OR '-DREJ_VTX_ORACLE=\"$REJ_VTX_ORACLE\"'"
[ -n "$REJ_TRI_ORACLE" ] && OR="$OR '-DREJ_TRI_ORACLE=\"$REJ_TRI_ORACLE\"'"
$CXX -O2 -c -I"$R/ext/parallel-rsp" -o "$O/reciprocal.o" "$R/ext/parallel-rsp/rsp/reciprocal.cpp"
eval $CC -O2 -std=gnu11 -I"$R/src/hle" -I"$H" $OR $EXTRA $EXTRA2 -o "$O/test_rej" "$H/test_rej.c" "$R/src/hle/rej_fx.c" "$R/src/hle/f3dex2_fx.c" $NEON "$O/reciprocal.o" -lstdc++
systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL ${TMO:-600} $RUN "$O/test_rej" ${1:-2000} ${2:-1}
