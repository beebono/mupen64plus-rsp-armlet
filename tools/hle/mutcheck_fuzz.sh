#!/bin/bash
# Mutation check of the declines and fixes that the perturbed-snapshot fuzzing found (docs/testing.md, "Fuzzing the front-ends"): builds the offline harness with each deliberate error
# (scripts/build.sh native --tools --hmut=N: the decline or fix N is switched off) and replays FILES with --oracle (and, for the ones the synthetic tasks catch, SYNTH); every mutation must make
# at least one task differ from the JIT.
#   FILES=list.txt [SYNTH="dk64.snap oot.snap"] [MUTS="217 218 ..."] tools/hle/mutcheck_fuzz.sh
#   FILES: a text file with one perturbed snapshot per line (tools/rsp2c/fuzz_snaps.py), as many as possible of those that differed from the JIT before the fixes (the old build's mismatches)
#   SYNTH: one snapshot each of Donkey Kong 64 and Ocarina of Time: tools/hle/synth_ex2.sh runs its synthetic tasks on them (the fixes of the builds' own constants, 245 and 246, and the case `bad`)
#   ROM-derived data: keep both in test-output/
R=$(cd "$(dirname "$0")/../.." && pwd)
[ -r "$FILES" ] || { echo "usage: FILES=list.txt [SYNTH=...] $0" >&2; exit 2; }
fail=0
for n in ${MUTS:-94 217 218 219 221 222 223 224 225 226 227 228 229 230 231 232 233 235 236 237 238 239 242 243 244 245 246}; do
    "$R/scripts/build.sh" native --tools --hmut=$n > /dev/null 2>&1 || { echo "mutation $n: build failed"; fail=1; continue; }
    bin="$R/out/native-tools-hmut$n/armlet-native-replay"
    bad=$(REPLAY_ARGS="--oracle" xargs -a "$FILES" -P 6 -n 40 "$R/tools/rsp2c/replay_each.sh" "$bin" 2>&1 | grep -c '^MISMATCH')
    syn=0
    for s in $SYNTH; do
        syn=$((syn + $("$R/tools/hle/synth_ex2.sh" "$bin" "$s" ${SEEDS:-200} 2>&1 | grep -o '[0-9]* differ' | awk '{t += $1} END {print t + 0}')))
    done
    if [ "$bad" -gt 0 ] || [ "$syn" -gt 0 ]; then echo "mutation $n: caught ($bad perturbed snapshots, $syn synthetic tasks differ)"
    else echo "mutation $n: NOT CAUGHT"; fail=1; fi
done
exit $fail
