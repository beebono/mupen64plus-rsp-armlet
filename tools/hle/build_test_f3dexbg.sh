#!/bin/bash
# builds and runs test_f3dexbg (the lit pass of F3DEXBG after the NEON and the scalar vertex forms) with the aarch64 cross compiler under qemu-aarch64-static; HOST=1: natively (scalar only).
set -e
H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/../.." && pwd)
O=${OUT:-$H/out/a64}; mkdir -p "$O"
if [ -n "$HOST" ]; then CC=cc; CXX=c++; RUN=; EXTRA=; NEON=; else CC=aarch64-linux-gnu-gcc; CXX=aarch64-linux-gnu-g++; RUN=qemu-aarch64-static; EXTRA=-static; NEON="$R/src/hle/f3dex2_fx_neon.c"; fi
$CXX -O2 -c -I"$R/ext/parallel-rsp" -o "$O/reciprocal.o" "$R/ext/parallel-rsp/rsp/reciprocal.cpp"
$CC -O3 -std=gnu11 -I"$R/src/hle" $1 -o "$O/test_f3dexbg" "$H/test_f3dexbg.c" "$R/src/hle/f3dex2_fx.c" "$R/src/hle/f3dexbg_fx.c" $NEON "$O/reciprocal.o" -lstdc++ $EXTRA
systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL 600 $RUN "$O/test_f3dexbg" ${2:-20000}
