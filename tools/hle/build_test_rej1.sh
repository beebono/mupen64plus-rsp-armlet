#!/bin/bash
# builds and runs test_rej1 (the 1.x Rej closed forms on random DMEM images; against the microcode when the rsp2c translations are given, docs/testing.md "1.x Rej").
#   tools/hle/build_test_rej1.sh [CASES [SEED]]      env: REJ_DATA=<.data dump of the text> (required), REJ1_BUILD=123 (F3DLX.Rej 1.23) | 121 (1.21) | 123a (the earlier 1.23), REJ1_VTX_ORACLE / REJ1_TRI_ORACLE=<generated .inc> (tools/hle/gen_rej1_oracles.sh), EXTRA=<compiler flags>
#   HOST=1: natively (x86_64, scalar); else the aarch64 cross compiler under qemu-aarch64-static (with SCALAR=1 the scalar forms)
set -e
H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/../.." && pwd)
O=${OUT:-$H/out/rej1${HOST:+-host}}; mkdir -p "$O"
if [ -n "$HOST" ]; then CC=cc; CXX=c++; RUN=; EXTRA2=; NEON=; else CC=aarch64-linux-gnu-gcc; CXX=aarch64-linux-gnu-g++; RUN=qemu-aarch64-static; EXTRA2=-static; NEON="$R/src/hle/f3dex2_fx_neon.c"; fi
[ -n "$SCALAR" ] && EXTRA2="$EXTRA2 -DARMLET_HLE_SCALAR"
case ${REJ1_BUILD:-123} in            # the build under test (its pcs; tools/hle/gen_rej1_oracles.sh takes the same)
123) OR= ;;
121) OR="-DRJ=rej1_121 -DREJ1_VTX_ENTRY=0x3b4 -DREJ1_TRI2_ENTRY=0xa78 -DREJ1_TRI1_ENTRY=0x1c8 -DREJ1_HEAD=0x09c" ;;
123a) OR="-DRJ=rej1_123a -DREJ1_VTX_ENTRY=0x3b4 -DREJ1_TRI2_ENTRY=0xa78 -DREJ1_TRI1_ENTRY=0x1c8 -DREJ1_HEAD=0x09c" ;;
*) echo "unknown REJ1_BUILD" >&2; exit 2 ;;
esac
[ -n "$REJ1_VTX_ORACLE" ] && OR="$OR '-DREJ1_VTX_ORACLE=\"$REJ1_VTX_ORACLE\"'"
[ -n "$REJ1_TRI_ORACLE" ] && OR="$OR '-DREJ1_TRI_ORACLE=\"$REJ1_TRI_ORACLE\"'"
$CXX -O2 -c -I"$R/ext/parallel-rsp" -o "$O/reciprocal.o" "$R/ext/parallel-rsp/rsp/reciprocal.cpp"
eval $CC -O2 -std=gnu11 -I"$R/src/hle" -I"$H" $OR $EXTRA $EXTRA2 -o "$O/test_rej1" "$H/test_rej1.c" "$R/src/hle/rej1_fx.c" "$R/src/hle/rej_fx.c" "$R/src/hle/f3dex2_fx.c" \
    "$R/src/hle/f3dex_vtx_fx.c" "$R/src/hle/f3dex_mtx_fx.c" $NEON "$O/reciprocal.o" -lstdc++
systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL ${TMO:-600} $RUN "$O/test_rej1" ${1:-2000} ${2:-1}
