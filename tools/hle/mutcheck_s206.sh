#!/bin/bash
# Mutation check of the "Safe" S2DEX build (Ocarina of Time's G_LOAD_UCODE'd S2DEX 2.05) and of Resident Evil 2's S2DEX 2.06 start (docs/testing.md, "S2DEX 2.05 [Safe] and 2.06"): builds the offline harness with each deliberate
# error (scripts/build.sh native --tools --hmut=N) and runs the captured tasks (SNAPS) and the synthetic ones (SYNTH) with it; a mutation is caught when more snapshots mismatch the JIT's result or are declined, or more
# synthetic seeds differ or decline, than with the unmutated build.
#   MUTS="300 301 ..." SNAPS="file.snap ..." SYNTH="snapshot:case:seeds[:VAR=val,VAR=val] ..." [REPLAY_ARGS=--oracle] tools/hle/mutcheck_s206.sh
R=$(cd "$(dirname "$0")/../.." && pwd)
: "${SNAPS:?}" "${SYNTH:?}"
run_set() {   # BIN -> "mismatches declined fails sdeclined"
    local bin=$1 b d f sd line spec snap case n envs
    read b d <<< "$(printf '%s\n' $SNAPS | xargs -P 3 -n 25 systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL ${MUT_TIMEOUT:-300} "$bin" --quiet $REPLAY_ARGS 2>&1 \
        | awk '/MISMATCH native vs JIT/{b++} /DECLINED/{d++} END{print b+0, d+0}')"
    f=0; sd=0
    for spec in $SYNTH; do
        IFS=: read snap case n envs <<< "$spec"
        line=$(env ${envs//,/ } PAR=3 "$R/tools/hle/synth_ex2.sh" "$bin" "$snap" "$n" "$case" 2>&1 | grep "seeds,")
        f=$((f + $(echo "$line" | sed 's/.*equal, \([0-9]*\) differ.*/\1/'))); sd=$((sd + $(echo "$line" | sed 's/.* \([0-9]*\) declined.*/\1/')))
    done
    echo "$b $d $f $sd"
}
"$R/scripts/build.sh" native --tools > /dev/null 2>&1
read b0 d0 f0 s0 <<< "$(run_set "$R/out/native-tools/armlet-native-replay")"
echo "unmutated: $b0 mismatches, $d0 declined; synthetic: $f0 differ, $s0 declined"
fail=0
for n in ${MUTS:-300 301 302 303 304 305 308 309}; do
    "$R/scripts/build.sh" native --tools --hmut=$n > /dev/null 2>&1
    read b d f s <<< "$(run_set "$R/out/native-tools-hmut$n/armlet-native-replay")"
    if [ "$b" -gt "$b0" ] || [ "$d" -gt "$d0" ] || [ "$f" -gt "$f0" ] || [ "$s" -gt "$s0" ]; then echo "mutation $n: caught ($b mismatches, $d declined; synthetic $f differ, $s declined)"
    else echo "mutation $n: NOT CAUGHT ($b mismatches, $d declined; synthetic $f differ, $s declined)"; fail=1; fi
done
exit $fail
