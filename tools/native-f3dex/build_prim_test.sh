#!/bin/bash
# builds and runs prim_test (x86, scalar VU backend): the closed forms of docs/ucode-f3dex.md against the VU instruction sequences
set -e
H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/../.." && pwd)
mkdir -p "$H/out-x86"
g++ -O2 -c -I"$R/ext/parallel-rsp" -o "$H/out-x86/reciprocal.o" "$R/ext/parallel-rsp/rsp/reciprocal.cpp"
gcc -O2 -std=gnu11 -I"$R/src/native" -c "$H/prim_test.c" -o "$H/out-x86/prim_test.o"
g++ -o "$H/out-x86/prim_test" "$H/out-x86/prim_test.o" "$H/out-x86/reciprocal.o"
"$H/out-x86/prim_test"
