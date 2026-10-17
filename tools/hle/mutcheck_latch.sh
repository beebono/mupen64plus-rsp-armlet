#!/bin/bash
# The latch policy's test (tools/hle/test_latch.c) with each deliberate error of src/hle/latch.c: it must pass unmutated and fail for 430 (the fingerprint without the counts), 431 (repeats count
# fully), 432 (NEAR needs no more evidence), 433 (no distinct fingerprints needed), 434 (a NEAR build is suspended, not unlatched, at its first mismatch), 435 (one strike more than set), 436 (a
# suspension keeps the validated kinds and fingerprints), 437 (no sampling decay). docs/testing.md, "The background validator and the latch policy".
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
build() { cc -O2 -Wall -Wextra ${1:+-DARMLET_HLE_MUTATE=$1} -I"$R/src/hle" -o "$T/t" "$R/tools/hle/test_latch.c" "$R/src/hle/latch.c" -lm; }
build ""; "$T/t" | tail -1
for m in 430 431 432 433 434 435 436 437; do
    build $m
    if "$T/t" >"$T/out" 2>&1; then echo "mutation $m: NOT CAUGHT"; exit 1; fi
    echo "mutation $m: caught ($(grep -c FAILED "$T/out") checks failed)"
done
