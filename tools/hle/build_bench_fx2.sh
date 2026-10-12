#!/bin/bash
# builds bench_fx2 for the host (scalar forms only on x86; scalar + NEON on aarch64) or, with "a64", for aarch64 (static, to copy to a device): tools/hle/out/bench_fx2[.a64]
set -e
H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/../.." && pwd)
O=${OUT:-$H/out}; mkdir -p "$O"
if [ "$1" = a64 ]; then CC=aarch64-linux-gnu-gcc; CXX=aarch64-linux-gnu-g++; X=.a64; else CC=gcc; CXX=g++; X=; fi
$CXX -O2 -c -I"$R/ext/parallel-rsp" -o "$O/reciprocal$X.o" "$R/ext/parallel-rsp/rsp/reciprocal.cpp"
$CC -O3 -std=gnu11 -I"$R/src/hle" -o "$O/bench_fx2$X" "$H/bench_fx2.c" "$R/src/hle/f3dex2_fx.c" "$R/src/hle/f3dex2_fx_neon.c" "$O/reciprocal$X.o" -lstdc++ ${X:+-static}
