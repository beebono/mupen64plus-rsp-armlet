#!/bin/bash
# builds and runs test_s2dex2 (the S2DEX 2.x closed forms against the microcode: the sprite rectangle, with S2RECT_ORACLE=<rsp2c translation of overlay A> and / or S2RECT_VU=1 (the instruction-level kernel
# in tools/hle/oracle/); the background routine, with S2BG_ORACLE=<generated file>; docs/testing.md, "Regenerating the microcode oracles") with the aarch64 cross compiler under qemu-aarch64-static; HOST=1: natively.
# S2BGC_ORACLE=<generated file> checks G_BG_COPY (tools/rsp2c/rsp2c.py of Kirby 64's S2DEX, docs/testing.md).
# The build under test: S2OUT=792 S2SYNC=1 (Ocarina of Time's S2DEX 2.05 [Safe]: 792-byte output buffers, a pipe sync in front of every flush check), S2RECT_ENTRY / S2RECT_DONE / S2RECT_CLIP and S2BG_ENTRY / S2BG_END
# (the pcs the rectangle and background oracles are entered and left at, when they are not those of Worms' build).
# First argument: extra compiler flags for the rectangle closed form (-DARMLET_HLE_MUTATE=n for its mutation check; BGFLAGS=-DARMLET_HLE_MUTATE=n for the background routine's); second: the number of cases.
set -e
H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/../.." && pwd)
O=${OUT:-$H/out/a64}; mkdir -p "$O"
if [ -n "$HOST" ]; then CC=cc; CXX=c++; RUN=; EXTRA=; else CC=aarch64-linux-gnu-gcc; CXX=aarch64-linux-gnu-g++; RUN=qemu-aarch64-static; EXTRA=-static; fi
$CXX -O2 -c -I"$R/ext/parallel-rsp" -o "$O/reciprocal.o" "$R/ext/parallel-rsp/rsp/reciprocal.cpp"
$CC -O3 -std=gnu11 -DARMLET_NATIVE -I"$R/src/hle" $1 -c -o "$O/s2dex2_rect.o" "$R/src/hle/s2dex2_rect.c"
EXTRAOBJ=
if [ -n "$S2RECT_VU" ]; then
    $CC -O2 -std=gnu11 -DARMLET_NATIVE -I"$R/src/hle" -I"$H/oracle" -c -o "$O/s2dex2_rect_vu.o" "$H/oracle/s2dex2_rect_vu.c"; EXTRAOBJ="$O/s2dex2_rect_vu.o"
fi
$CC -O3 -std=gnu11 -DARMLET_NATIVE -I"$R/src/hle" $BGFLAGS -c -o "$O/s2dex2_bg.o" "$R/src/hle/s2dex2_bg.c"
$CC -O2 -std=gnu11 -I"$R/src/hle" -I"$H" ${S2BG_ORACLE:+-DS2BG_ORACLE=\"$S2BG_ORACLE\"} ${S2BGC_ORACLE:+-DS2BGC_ORACLE=\"$S2BGC_ORACLE\"} ${S2BGC_ENTRY:+-DS2BGC_ENTRY=$S2BGC_ENTRY -DS2BGC_END=$S2BGC_END} ${S2RECT_ORACLE:+-DS2RECT_ORACLE=\"$S2RECT_ORACLE\"} ${S2RECT_VU:+-DS2RECT_VU=1} ${S2OUT:+-DS2_OUT=$S2OUT} ${S2SYNC:+-DS2_SYNC=$S2SYNC} ${S2RECT_ENTRY:+-DS2RECT_ENTRY=$S2RECT_ENTRY -DS2RECT_DONE=$S2RECT_DONE -DS2RECT_CLIP=$S2RECT_CLIP} ${S2BG_ENTRY:+-DS2BG_ENTRY=$S2BG_ENTRY -DS2BG_END=$S2BG_END} -c -o "$O/test_s2dex2.o" "$H/test_s2dex2.c"
$CXX -o "$O/test_s2dex2" "$O/test_s2dex2.o" "$O/s2dex2_rect.o" $EXTRAOBJ "$O/s2dex2_bg.o" "$O/reciprocal.o" $EXTRA
systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL 900 $RUN "$O/test_s2dex2" ${2:-20000}
