#!/bin/bash
# synth.sh REPLAY SNAPDIR : the synthetic tasks (tools/f3d/mksyn.py) on one captured task of each build; a case passes when the front-end equals the JIT's fresh result and does not decline
#   SNAPDIR holds sm64/ tt/ pw/ wr/ mm/ with task snapshots (ARMLET_HLE_CAPTURE; e.g. test-output/f3dex2-204/snap has sm64/); the snapshots only supply the microcode and the start state,
#   nothing of them is in the cases. Snapshot *-$SNAP_PICK.snap (default 10) of a directory is used, else its last one: the JIT oracle does not finish the lit cases from every
#   captured start state (sm64 -0 and -9 of test-output/f3dex2-204/snap hang, -10 .. -12 run). A build without a directory is skipped.
R=$1; D=$2; here=$(cd "$(dirname "$0")" && pwd); fail=0
run() {
    [ -d "$D/$1" ] || { echo "SKIP $1 (no snapshots)"; return; }
    snap=$(ls "$D/$1"/*-${SNAP_PICK:-10}.snap 2>/dev/null || ls "$D/$1"/*.snap | tail -1)
    for c in $2; do
        out=$(env $("$here/mksyn.py" $c) timeout 300 "$R" --oracle "$snap" 2>&1 | tail -1)
        case "$out" in *"0 declined"*"all equal"*) echo "PASS $1 $c" ;; *) echo "FAIL $1 $c: $out"; fail=1 ;; esac
    done
}
decl() {     # a case the front-end must decline (the JIT runs it; the mutated build, scripts/build.sh native --tools --hmut=N, runs it and differs)
    [ -d "$D/$1" ] || { echo "SKIP $1 (no snapshots)"; return; }
    snap=$(ls "$D/$1"/*-${SNAP_PICK:-10}.snap 2>/dev/null || ls "$D/$1"/*.snap | tail -1)
    out=$(env $("$here/mksyn.py" $2) timeout 300 "$R" --oracle "$snap" 2>&1 | tail -1)
    case "$out" in *"1 declined"*) echo "PASS $1 $2 (declined)" ;; *) echo "FAIL $1 $2: $out"; fail=1 ;; esac
}
run sm64 "nested culldl popmtx mwpoints"
decl sm64 mwpoints-bad
run pw "nested culldl popmtx mwpoints"
run tt "dl-depth nested culldl popmtx mwpoints"
run mm "nested culldl popmtx mwpoints"
run wr "nested mwpoints"
exit $fail
