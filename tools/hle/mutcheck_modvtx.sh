#!/bin/bash
# Mutation check of the G_MODIFYVTX decline of the F3DEX 1.x front-end (mutation 471, f3dex.c; docs/testing.md, "G_MODIFYVTX outside the vertex slots"): tools/hle/mksyn_modvtx.py is the reduced task of
# the Toy Story 2 perturbed-task mismatch.
#   tools/hle/mutcheck_modvtx.sh F3DEX1_TASK.snap     (a captured graphics task of any F3DEX 1.x / F3DLX build whose vertex table is at 0x31e, e.g. Toy Story 2's F3DLX 1.23)
# Expected: the build declines the task (slot offset 0x7800); the build with the decline switched off (--hmut=471) differs from the JIT; a game's modify (slot 0, field 0x14) stays native and equal in both.
R=$(cd "$(dirname "$0")/../.." && pwd)
S=$1; [ -n "$S" ] || { echo "usage: $0 F3DEX1_TASK.snap" >&2; exit 2; }
"$R/scripts/build.sh" native --tools > /dev/null 2>&1; "$R/scripts/build.sh" native --tools --hmut=471 > /dev/null 2>&1
run() { env $(python3 "$R/tools/hle/mksyn_modvtx.py" $2) systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL 60 "$R/out/$1/armlet-native-replay" --oracle --quiet "$S" 2>&1 | tail -1; }
fail=0
a=$(run native-tools "0x7800 0x18"); b=$(run native-tools-hmut471 "0x7800 0x18"); c=$(run native-tools "0 0x14"); d=$(run native-tools-hmut471 "0 0x14")
case "$a" in *"1 declined"*) echo "fix: declined (ok)";; *) echo "fix: NOT declined: $a"; fail=1;; esac
case "$b" in *FAILED*) echo "mutation 471: caught (differs from the JIT)";; *) echo "mutation 471: NOT CAUGHT: $b"; fail=1;; esac
case "$c$d" in *"0 declined"*"all equal"*"0 declined"*"all equal"*) echo "slot 0, field 0x14: native and equal in both (ok)";; *) echo "slot 0, field 0x14: $c / $d"; fail=1;; esac
exit $fail
