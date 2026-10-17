#!/bin/bash
# The cache test (tools/hle/test_ucache.c) with each deliberate error of src/hle/ucache.c: it must pass unmutated and fail for 345 (entry checksum not checked), 346 (no bound on the
# entries), 347 (tool version not checked), 349 (the file is rewritten in place, not by rename). docs/testing.md, "The probe".
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
build() { cc -O2 -Wall -Wextra ${1:+-DARMLET_HLE_MUTATE=$1} -I"$R/src/hle" -o "$T/t" "$R/tools/hle/test_ucache.c" "$R/src/hle/ucache.c" "$R/src/hle/derive_refs.c"; }
build ""; "$T/t" | tail -1
for m in 345 346 347 349; do
    build $m
    if "$T/t" >"$T/out" 2>&1; then echo "mutation $m: NOT CAUGHT"; exit 1; fi
    echo "mutation $m: caught ($(grep -c FAILED "$T/out") checks failed)"
done
