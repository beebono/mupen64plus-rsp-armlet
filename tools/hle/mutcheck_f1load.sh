#!/bin/bash
# Mutation check of the F3DEX 1.x front-end's G_LOAD_UCODE (src/hle/f3dex.c, docs/testing.md "F3DEX 1.x G_LOAD_UCODE"): builds the offline harness with each deliberate error (scripts/build.sh native --tools --hmut=N)
# and runs the synthetic load tasks (tools/hle/mksyn_ex1.py; the MKSYN1_* environment names the texts and data, see there) on one captured task of the build under test; a mutation is caught when more seeds
# differ from the JIT, or are declined, than with the unmutated build.
#   MUTS="460 461 ..." SEEDS=300 tools/hle/mutcheck_f1load.sh SNAPSHOT [CASE...]      (default cases: load xload)
# 463 (the probe is not told of the loaded text) and 466 (the decline memory) are not seen by seeds: 463 by a game run with ARMLET_PROBE (the probe's log), 466 by replaying a game's tasks in one process
# (the counts of "an earlier task with these pointers loaded another ucode": SpaceStation Silicon Valley / Pokemon Puzzle League snapshots, docs/testing.md).
R=$(cd "$(dirname "$0")/../.." && pwd)
[ $# -gt 0 ] || { echo "usage: MUTS='460 ...' SEEDS=n $0 SNAPSHOT [CASE...]" >&2; exit 2; }
snap=$1; shift
cases=${*:-load xload}
count() {   # BIN -> "differ declined"
    PAR=${PAR:-3} "$R/tools/hle/synth_ex1.sh" "$1" "$snap" ${SEEDS:-300} $cases | awk '/seeds,/{for(i=1;i<=NF;i++){if($i=="differ,"||$i=="differ")d+=$(i-1); if($i=="declined"||$i=="declined,")c+=$(i-1)}} END{print d+0, c+0}'
}
"$R/scripts/build.sh" native --tools > /dev/null 2>&1
read d0 c0 <<< "$(count "$R/out/native-tools/armlet-native-replay")"
echo "unmutated: $d0 differ, $c0 declined"
fail=0
for n in ${MUTS:-460 461 462 464 465 467 468 470}; do
    "$R/scripts/build.sh" native --tools --hmut=$n > /dev/null 2>&1
    read d c <<< "$(count "$R/out/native-tools-hmut$n/armlet-native-replay")"
    if [ "$d" -gt "$d0" ] || [ "$c" -gt "$c0" ]; then echo "mutation $n: caught ($d differ, $c declined)"; else echo "mutation $n: NOT CAUGHT ($d differ, $c declined)"; fail=1; fi
done
exit $fail
