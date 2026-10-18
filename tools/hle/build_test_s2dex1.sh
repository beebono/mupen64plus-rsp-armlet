#!/bin/bash
# builds and runs test_s2dex1 (the S2DEX 1.06 / 1.07 closed forms against the microcode: the rsp2c translations tools/hle/oracle/s1_{rect,bg1cyc,bgcopy}_gen.inc, docs/testing.md, "Regenerating the microcode oracles")
# with the aarch64 cross compiler under qemu-aarch64-static; HOST=1: natively. First argument: extra compiler flags for the closed forms (-DARMLET_HLE_MUTATE=n for the mutation check); second: the number of cases.
set -e
H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/../.." && pwd)
O=${OUT:-$H/out/a64-s1}; mkdir -p "$O"
if [ -n "$HOST" ]; then CC=cc; CXX=c++; RUN=; EXTRA=; else CC=aarch64-linux-gnu-gcc; CXX=aarch64-linux-gnu-g++; RUN=qemu-aarch64-static; EXTRA=-static; fi
$CXX -O2 -c -I"$R/ext/parallel-rsp" -o "$O/reciprocal.o" "$R/ext/parallel-rsp/rsp/reciprocal.cpp"
for f in s2dex1_rect s2dex1_bg s2dex1_bgcopy s2dex1_sprite; do
    $CC -O3 -std=gnu11 -DARMLET_NATIVE -I"$R/src/hle" $1 -c -o "$O/$f.o" "$R/src/hle/$f.c"
done
$CC -O2 -std=gnu11 -I"$R/src/hle" -I"$H" -c -o "$O/test_s2dex1.o" "$H/test_s2dex1.c"
$CXX -o "$O/test_s2dex1" "$O/test_s2dex1.o" "$O/s2dex1_rect.o" "$O/s2dex1_bg.o" "$O/s2dex1_bgcopy.o" "$O/s2dex1_sprite.o" "$O/reciprocal.o" $EXTRA
systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL 900 $RUN "$O/test_s2dex1" ${2:-20000}
