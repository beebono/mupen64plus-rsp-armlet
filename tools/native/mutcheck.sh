#!/bin/bash
# Mutation check of the verify machinery (docs/testing.md): builds the harness with each deliberate error of the test.dma routine
# (scripts/build.sh native --tools --nmut=N, N = 1..8) and replays routine-entry snapshots of test.dma with it; every mutation must be reported.
#   tools/native/mutcheck.sh SNAPSHOT...      (snapshots of test.dma: a verify run with ARMLET_NATIVE_ROUTINES=test.dma ARMLET_NATIVE_CAPTURE=<dir>)
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
[ $# -gt 0 ] || { echo "usage: $0 SNAPSHOT..." >&2; exit 2; }
fail=0
for n in 1 2 3 4 5 6 7 8 9 10; do
    "$R/scripts/build.sh" native --tools --nmut=$n > /dev/null 2>&1
    out=$(systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL 300 "$R/out/native-tools-nmut$n/armlet-native-replay" --quiet "$@" 2>&1 || true)
    first=$(printf '%s\n' "$out" | grep -E -A2 'MISMATCH \(' | sed -n '2,3p' | tr -s ' ' | tr '\n' ' ')
    nbad=$(printf '%s\n' "$out" | grep -c 'MISMATCH native vs JIT' || true)
    if printf '%s\n' "$out" | grep -q WATCHDOG; then first="$first (some snapshots: WATCHDOG, the JIT never arrived at the exit pc)"; nbad=$((nbad + 1)); fi
    if [ "$n" = 9 ]; then   # a change of a lane the routine declares dead must not be reported
        if [ "$nbad" -eq 0 ]; then echo "mutation 9: not reported, as the lane mask says (dead lane)"; else echo "mutation 9: REPORTED although the lane is declared dead"; fail=1; fi
    elif [ "$nbad" -gt 0 ]; then echo "mutation $n: caught in $nbad snapshots; first: $first"; else echo "mutation $n: NOT CAUGHT"; fail=1; fi
done
exit $fail
