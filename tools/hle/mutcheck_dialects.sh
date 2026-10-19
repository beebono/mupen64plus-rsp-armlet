#!/bin/bash
# Mutation check of the dialect front-ends and kernels (F3DEXBG, S2DEX 2.x; docs/testing.md): builds the offline harness with each deliberate error (scripts/build.sh native --tools --hmut=N) and
# replays the captured task starts with it; every mutation must be reported (a mismatch or a decline that the unmutated build does not have).
#   MUTS="30 31 ..." tools/hle/mutcheck_dialects.sh SNAPSHOT...        (task snapshots: ARMLET_HLE_CAPTURE=<dir>)
#   TARGET=aarch64 RUNNER="qemu-aarch64-static -L /usr/aarch64-linux-gnu": the NEON path (the lit pass of F3DEXBG, 100 .. 120) under qemu instead of the x86_64 scalar one;
# The mutations of the sprite rectangle closed form (src/hle/s2dex2_rect.c: 30 .. 34, 150 .. 166) that the snapshots do not catch (Worms draws unscaled, unflipped sprites inside the screen) are
# run through the random test against the microcode instead (tools/hle/test_s2dex2.c; needs S2RECT_ORACLE=<rsp2c translation of overlay A>, docs/testing.md, "Regenerating the microcode oracles"): "caught by the random test".
R=$(cd "$(dirname "$0")/../.." && pwd)
[ $# -gt 0 ] || { echo "usage: MUTS='30 31 ...' $0 SNAPSHOT..." >&2; exit 2; }
fail=0
TARGET=${TARGET:-native}
RECT_MUTS="30 31 32 33 34 $(seq 150 166 | tr "\n" " ")"
for n in ${MUTS:-30 31 32 33 34 35 36 37 38 39 44 $(seq 70 93) $(seq 100 120) $(seq 150 166)}; do
    "$R/scripts/build.sh" $TARGET --tools --hmut=$n > /dev/null 2>&1
    rc=0
    out=$(systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL ${MUT_TIMEOUT:-300} $RUNNER "$R/out/$TARGET-tools-hmut$n/armlet-native-replay" --quiet "$@" 2>&1) || rc=$?
    nbad=$(printf '%s\n' "$out" | grep -c 'MISMATCH native vs JIT' || true)
    ndec=$(printf '%s\n' "$out" | grep -c 'DECLINED' || true)
    if [ "$nbad" -gt 0 ] || [ "$ndec" -gt 0 ]; then echo "mutation $n: caught in $nbad snapshots (+ $ndec declined)"
    elif [ "$rc" -ne 0 ] && ! printf '%s\n' "$out" | grep -q 'all equal'; then echo "mutation $n: caught (the replay hung or crashed, exit $rc)"
    elif [ -n "$S2RECT_ORACLE" ] && [[ " $RECT_MUTS " == *" $n "* ]] && nf=$(HOST=1 OUT=${TMPDIR:-/tmp}/mutcheck_s2 "$R/tools/hle/build_test_s2dex2.sh" "-DARMLET_HLE_MUTATE=$n" ${RECT_CASES:-20000} 2>&1 | grep -o '[0-9]* failures' | grep -v '^0 ') ; then echo "mutation $n: caught by the random test against the microcode ($nf in ${RECT_CASES:-20000} cases)"
    else echo "mutation $n: NOT CAUGHT"; fail=1; fi
done
exit $fail
