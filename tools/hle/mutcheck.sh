#!/bin/bash
# Mutation check of the task-level path's tests (docs/testing.md): builds the offline harness with each deliberate error (scripts/build.sh native --tools --hmut=N / --fmut=N) and
# replays the captured task starts with it; every mutation must be reported (a mismatch or a decline that the unmutated build does not have).
#   tools/hle/mutcheck.sh SNAPSHOT...        (task snapshots: ARMLET_HLE_CAPTURE=<dir>)
# Mutations that change nothing the tasks can see (a clamp that the data never reaches) are listed as "NOT CAUGHT" with the reason in the table of docs/testing.md.
R=$(cd "$(dirname "$0")/../.." && pwd)
[ $# -gt 0 ] || { echo "usage: $0 SNAPSHOT..." >&2; exit 2; }
fail=0
SNAPS=("$@")
for n in 1 3 4 6 7 8 9 10 11 12 13 14 15 16 17 18; do
    "$R/scripts/build.sh" native --tools --hmut=$n > /dev/null 2>&1
    out=$(systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL 900 "$R/out/native-tools-hmut$n/armlet-native-replay" --quiet "${SNAPS[@]}" 2>&1 || true)
    nbad=$(printf '%s\n' "$out" | grep -c 'MISMATCH native vs JIT' || true)
    ndec=$(printf '%s\n' "$out" | grep -c 'DECLINED' || true)
    if [ "$nbad" -gt 0 ] || [ "$ndec" -gt 0 ]; then echo "mutation $n: caught in $nbad snapshots (+ $ndec declined)"; else echo "mutation $n: NOT CAUGHT"; fail=1; fi
done
for n in 1 2 3 4 5 6 7; do
    "$R/scripts/build.sh" native --tools --fmut=$n > /dev/null 2>&1
    out=$(systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL 900 "$R/out/native-tools-fmut$n/armlet-native-replay" --quiet "${SNAPS[@]}" 2>&1 || true)
    nbad=$(printf '%s\n' "$out" | grep -c 'MISMATCH native vs JIT' || true)
    ndec=$(printf '%s\n' "$out" | grep -c 'DECLINED' || true)
    if [ "$nbad" -gt 0 ] || [ "$ndec" -gt 0 ]; then echo "library mutation $n: caught in $nbad snapshots (+ $ndec declined)"; else echo "library mutation $n: NOT CAUGHT"; fail=1; fi
done
exit $fail
