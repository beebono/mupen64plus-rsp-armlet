#!/bin/bash
# builds and runs test_fx2 (the NEON forms against the scalar forms) with the aarch64 cross compiler under qemu-aarch64-static. Optional first argument: extra compiler flags (e.g. -DARMLET_HLE_MUTATE=8).
set -e
H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/../.." && pwd)
O=${OUT:-$H/out/a64}; mkdir -p "$O"
CC=aarch64-linux-gnu-gcc; CXX=aarch64-linux-gnu-g++
$CXX -O2 -c -I"$R/ext/parallel-rsp" -o "$O/reciprocal.o" "$R/ext/parallel-rsp/rsp/reciprocal.cpp"
$CC -O3 -std=gnu11 -I"$R/src/hle" $1 -o "$O/test_fx2" "$H/test_fx2.c" "$R/src/hle/f3dex2_fx.c" "$R/src/hle/f3dex2_fx_neon.c" "$R/src/hle/rej_fx.c" "$O/reciprocal.o" -lstdc++ -static
systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL 600 qemu-aarch64-static "$O/test_fx2" ${2:-200000}
