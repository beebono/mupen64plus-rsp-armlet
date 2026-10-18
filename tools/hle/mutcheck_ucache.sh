#!/bin/bash
# The cache test (tools/hle/test_ucache.c) with each deliberate error of src/hle/ucache.c: it must pass unmutated and fail for 345 (entry checksum not checked), 346 (no bound on the
# entries), 347 (tool version not checked), 349 (the file is rewritten in place, not by rename), 442 (a permanent unlatch is not kept over a later store or a merge), 454 (the tier left out of
# the key: a loose latch read by a strict lookup), 460 (clip204 not read back from an entry). docs/testing.md, "The probe", "The loose tier".
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
build() { cc -O2 -Wall -Wextra ${1:+-DARMLET_HLE_MUTATE=$1} -I"$R/src/hle" -o "$T/t" "$R/tools/hle/test_ucache.c" "$R/src/hle/ucache.c" "$R/src/hle/derive_refs.c"; }
build ""; "$T/t" | tail -1
for m in 345 346 347 349 442 454 460; do
    build $m
    if "$T/t" >"$T/out" 2>&1; then echo "mutation $m: NOT CAUGHT"; exit 1; fi
    echo "mutation $m: caught ($(grep -c FAILED "$T/out") checks failed)"
done
