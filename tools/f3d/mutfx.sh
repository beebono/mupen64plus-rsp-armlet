#!/bin/bash
# mutfx.sh MODE N...: builds test_fx with F3D_FX_MUTATE=n (private object dirs) and checks that MODE's test fails for each n
# env: F3D_UCODE (the ucode image prefix), F3D_BUILD (pw: Pilotwings), COUNT (cases, default 3000)
cd "$(dirname "$0")"; R=$PWD/../..
mode=$1; shift
for n in "$@"; do
  make OBJDIR=$R/test-output/f3d/objM$n BIN=$R/test-output/f3dM$n FXFLAGS=-DF3D_FX_MUTATE=$n $R/test-output/f3dM$n/test_fx >/dev/null 2>&1 || { echo "mut $n: build failed"; continue; }
  out=$(systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL 300 $R/test-output/f3dM$n/test_fx $mode -n ${COUNT:-3000} -s 1 2>&1 | tail -1)
  echo "mut $n: $out"
done
