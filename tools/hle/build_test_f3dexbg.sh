#!/bin/bash
# builds and runs test_f3dexbg (the lit pass of F3DEXBG after the NEON and the scalar vertex forms; the NEON lit pass against the scalar one) with the aarch64 cross compiler under qemu-aarch64-static; HOST=1: natively (scalar only).
#   build_test_f3dexbg.sh [CFLAGS (-DFBG_NEON_MUTATE=n: a deliberate error in the NEON lit pass; -DARMLET_HLE_MUTATE=n: in the scalar one too)] [vertex cases] [lit cases] [seed]
set -e
H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/../.." && pwd)
O=${OUT:-$H/out/a64}; mkdir -p "$O"
if [ -n "$HOST" ]; then CC=cc; CXX=c++; RUN=; EXTRA=; NEON=; else CC=aarch64-linux-gnu-gcc; CXX=aarch64-linux-gnu-g++; RUN=qemu-aarch64-static; EXTRA=-static; NEON="$R/src/hle/f3dex2_fx_neon.c $R/src/hle/rej_fx.c"; fi
$CXX -O2 -c -I"$R/ext/parallel-rsp" -o "$O/reciprocal.o" "$R/ext/parallel-rsp/rsp/reciprocal.cpp"
# the reference of the lit-pass comparison: the scalar pass of an unmutated copy of f3dexbg_fx.c, its symbols renamed (so a -DARMLET_HLE_MUTATE=n build, which mutates the scalar pass and the NEON one alike, is still
# compared with the right answer)
$CC -O3 -std=gnu11 -I"$R/src/hle" -Dfbg_lit_pass_scalar=fbg_ref_lit_pass_scalar -Dfbg_lit_pass=fbg_ref_lit_pass -Dfbg_clip_frac=fbg_ref_clip_frac -c -o "$O/f3dexbg_ref.o" "$R/src/hle/f3dexbg_fx.c"
$CC -O3 -std=gnu11 -I"$R/src/hle" $1 -o "$O/test_f3dexbg" "$H/test_f3dexbg.c" "$R/src/hle/f3dex2_fx.c" "$R/src/hle/f3dexbg_fx.c" "$O/f3dexbg_ref.o" $NEON "$O/reciprocal.o" -lstdc++ $EXTRA
systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL 600 $RUN "$O/test_f3dexbg" ${2:-20000} ${3:-200000} ${4:-0}
