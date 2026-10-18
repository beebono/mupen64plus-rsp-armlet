#!/bin/bash
# The ring of the S2DEX xbus build (docs/testing.md "XBUS, part 3") against deliberate errors 366 (the wrap 8 bytes late) and 367 (the wrapped pointer).
# usage: tools/hle/mutcheck_s2xbus.sh SNAP [N]   SNAP: a captured StarCraft 64 task (ARMLET_HLE_CAPTURE with ARMLET_UCODES=0000000000000001); N seeds (12)
set -u
H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/../.." && pwd)
S=${1:?snapshot}; N=${2:-12}
(cd "$R"; scripts/build.sh native --tools >/dev/null 2>&1)
echo "unmutated: $(PAR=4 "$H/synth_ex2.sh" "$R/out/native-tools/armlet-native-replay" "$S" "$N" s2ring | head -1)"
for m in 366 367; do
    (cd "$R"; scripts/build.sh native --tools --hmut=$m >/dev/null 2>&1)
    r=$(PAR=4 "$H/synth_ex2.sh" "$R/out/native-tools-hmut$m/armlet-native-replay" "$S" "$N" s2ring | head -1)
    case "$r" in *" 0 differ"*) echo "$m NOT CAUGHT ($r)";; *) echo "$m caught ($r)";; esac
done
