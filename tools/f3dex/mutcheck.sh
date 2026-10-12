#!/bin/bash
# mutcheck.sh <spec prefix, e.g. fxtri> <routine, e.g. spec.f3dex.fxtri> <build, e.g. banjo121> <n...>: rebuilds rt_test with FXFLAGS=-DF3DEX_FX_MUTATE=n (private output dir) and prints the BAD count of the
# differential test for each mutation (every mutation must show BAD > 0; "0 BAD" means the tests do not see that mutation).
H=$(cd "$(dirname "$0")" && pwd); R=$H/../..
. "${ARMLET_ENV:?shell file that defines the build helpers (bt, R, B)}"
SPECP=$1; ROUTINE=$2; BUILD=$3; shift 3
for n in "$@"; do
  B=$SP/mut_${SPECP}_$n; mkdir -p $B
  ( cd $R/tools/native-f3dex; FXFLAGS="-DF3DEX_FX_MUTATE=$n" SPEC_ONLY=$SPECP bt ${TARGET:-x86} $B >/dev/null 2>&1 ) || { echo "mut $n: build failed"; continue; }
  out=$(systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL 300 $B/rt_test --ucode $UC/$BUILD --routine $ROUTINE --cases ${CASES:-10000} 2>&1 | grep -E "cases:" | head -1)
  echo "mut $n ($BUILD): $out"
done
