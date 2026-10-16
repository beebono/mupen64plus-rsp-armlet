#!/bin/bash
# Mutation check of the F3DEX2 2.04 / 2.04H differences (docs/ucode-f3dex2-dialects.md, section 5; docs/testing.md): builds the offline harness with each deliberate error (scripts/build.sh native --tools
# --hmut=N, N = 210 .. 216) and runs (a) the replay of the captured task starts, (b) the synthetic tasks of tools/hle/mksyn_ex2.py against the JIT; every mutation must be reported by one of them.
#   SNAPS="dir/*.snap ..." SYNTH="base.snap ..." [SEEDS=300] tools/hle/mutcheck_204.sh
#   SNAPS: captured task snapshots (ARMLET_HLE_CAPTURE) of the builds; SYNTH: one snapshot per build to run the synthetic cases on (a task that used the clipping and the lighting overlays: they supply the code)
R=$(cd "$(dirname "$0")/../.." && pwd)
[ -n "$SNAPS" ] && [ -n "$SYNTH" ] || { echo "usage: SNAPS='...' SYNTH='...' $0" >&2; exit 2; }
fail=0
for n in ${MUTS:-210 211 212 213 214 215 216}; do
    "$R/scripts/build.sh" native --tools --hmut=$n > /dev/null 2>&1
    bin="$R/out/native-tools-hmut$n/armlet-native-replay"
    nrep=$(systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL 600 "$bin" --quiet $SNAPS 2>&1 | grep -c 'MISMATCH native vs JIT')
    nsyn=0
    for s in $SYNTH; do
        nsyn=$((nsyn + $("$R/tools/hle/synth_ex2.sh" "$bin" "$s" ${SEEDS:-300} 2>&1 | grep -o '[0-9]* differ' | awk '{t += $1} END {print t + 0}')))
    done
    if [ "$nrep" -gt 0 ] || [ "$nsyn" -gt 0 ]; then echo "mutation $n: caught ($nrep captured tasks, $nsyn synthetic tasks differ)"
    else echo "mutation $n: NOT CAUGHT"; fail=1; fi
done
exit $fail
