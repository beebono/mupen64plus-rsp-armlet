#!/bin/bash
# Mutation check of the build switching (G_LOAD_UCODE between builds of the F3DEX2 front-end: hle_f3dex2.c, hle_s2dex2.c), of G_BG_COPY (s2dex2_bg.c) and of the declines found by fuzzing the
# switching tasks (docs/testing.md, "Build switching"): builds the offline harness with each deliberate error (scripts/build.sh native --tools --hmut=N) and replays the snapshots with it; a
# mutation is caught when more snapshots mismatch the JIT's result, or more are declined, than with the unmutated build (REPLAY_ARGS=--oracle: the JIT is run from the same state, for perturbed
# snapshots and synthetic tasks), or the replay hangs. The G_BG_COPY mutations that the snapshots do not catch (Kirby 64 draws one background in COPY mode) go to the random test against the
# microcode (tools/hle/test_s2dex2.c, S2BGC_ORACLE=<rsp2c translation>, docs/testing.md).
#   MUTS="250 251 ..." [REPLAY_ARGS=--oracle] [S2BGC_ORACLE=...] tools/hle/mutcheck_switch.sh SNAPSHOT...
R=$(cd "$(dirname "$0")/../.." && pwd)
[ $# -gt 0 ] || { echo "usage: MUTS='250 251 ...' $0 SNAPSHOT..." >&2; exit 2; }
fail=0
run_set() {   # BIN -> "mismatches declined hung" over the snapshots, 6 batches at a time
    printf '%s\n' "$@" | tail -n +2 | xargs -P 6 -n 25 systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL ${MUT_TIMEOUT:-300} "$1" --quiet $REPLAY_ARGS 2>&1 \
        | awk '/MISMATCH native vs JIT/{b++} /DECLINED/{d++} END{print b+0, d+0}'
}
"$R/scripts/build.sh" native --tools > /dev/null 2>&1
read b0 d0 <<< "$(run_set "$R/out/native-tools/armlet-native-replay" _ "$@")"
echo "unmutated: $b0 mismatches, $d0 declined"
for n in ${MUTS:-$(seq 240 259) $(seq 260 299)}; do
    "$R/scripts/build.sh" native --tools --hmut=$n > /dev/null 2>&1
    read b d <<< "$(run_set "$R/out/native-tools-hmut$n/armlet-native-replay" _ "$@")"
    if [ "$b" -gt "$b0" ] || [ "$d" -gt "$d0" ]; then echo "mutation $n: caught ($b mismatches, $d declined)"
    elif [ -n "$S2BGC_ORACLE" ] && [ "$n" -ge 260 ] && nf=$(HOST=1 OUT=${TMPDIR:-/tmp}/mutcheck_s2bgc BGFLAGS="-DARMLET_HLE_MUTATE=$n" "$R/tools/hle/build_test_s2dex2.sh" "" ${BGC_CASES:-60000} 2>&1 | grep -o '[0-9]* failures' | grep -v '^0 ') ; then echo "mutation $n: caught by the random test against the microcode ($nf in ${BGC_CASES:-60000} cases)"
    else echo "mutation $n: NOT CAUGHT ($b mismatches, $d declined)"; fail=1; fi
done
exit $fail
