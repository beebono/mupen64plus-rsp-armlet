#!/bin/bash
# builds and runs prim_test (x86, scalar VU reference): src/hle/hle_fx.h against the exact VU instruction sequences. Optional first argument: a -DVU_MUTATE=n style
# flag set for the mutation check of the reference (see tools/hle/prim_test.c); with HLE_FX_MUTATE=n the library function n is deliberately broken.
set -e
H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/../.." && pwd)
O=${OUT:-$H/out}; mkdir -p "$O"
g++ -O2 -c -I"$R/ext/parallel-rsp" -o "$O/reciprocal.o" "$R/ext/parallel-rsp/rsp/reciprocal.cpp"
gcc -O2 -std=gnu11 -I"$H" -I"$R/src/hle" ${HLE_FX_MUTATE:+-DHLE_FX_MUTATE=$HLE_FX_MUTATE} -c "$H/prim_test.c" -o "$O/prim_test.o"
g++ -o "$O/prim_test" "$O/prim_test.o" "$O/reciprocal.o"
"$O/prim_test"
