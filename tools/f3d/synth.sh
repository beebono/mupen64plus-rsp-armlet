#!/bin/bash
# synth.sh REPLAY SNAPDIR : the synthetic tasks (tools/f3d/mksyn.py) on one captured task of each build; a case passes when the front-end equals the JIT's fresh result and does not decline
#   SNAPDIR holds sm64/ tt/ pw/ wr/ mm/ with task snapshots (ARMLET_HLE_CAPTURE); the snapshots only supply the microcode and the start state, nothing of them is in the cases
R=$1; D=$2; here=$(cd "$(dirname "$0")" && pwd); fail=0
run() {
    snap=$(ls "$D/$1"/*.snap | head -1)
    for c in $2; do
        out=$(env $("$here/mksyn.py" $c) timeout 120 "$R" "$snap" 2>&1 | tail -1)
        case "$out" in *"0 declined"*"all equal"*) echo "PASS $1 $c" ;; *) echo "FAIL $1 $c: $out"; fail=1 ;; esac
    done
}
run sm64 "nested culldl popmtx"
run pw "nested culldl popmtx"
run tt "dl-depth nested culldl popmtx"
run mm "nested culldl popmtx"
run wr "nested"
exit $fail
