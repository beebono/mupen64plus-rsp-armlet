#!/bin/bash
# synth_ex2.sh REPLAY SNAPSHOT N [CASE...]: N seeds of each synthetic F3DEX2 case (tools/hle/mksyn_ex2.py) on one captured task of the build under test (it supplies the microcode and the start state);
# a seed passes when the front-end equals the JIT's fresh result and does not decline. ${PAR:-6} at a time, each in a memory-capped scope (docs/testing.md). Prints a line per case and the failing seeds.
#   RUNNER (e.g. "qemu-aarch64-static -L /usr/aarch64-linux-gnu") for an aarch64 REPLAY
R=$1; S=$2; N=$3; shift 3
cases=${*:-clip zneg tri mix misc lit wild bad}
here=$(cd "$(dirname "$0")" && pwd)
one() {
    out=$(env $(python3 "$here/mksyn_ex2.py" "$1" "$2") systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL 60 $RUNNER "$R" --oracle --quiet "$S" 2>&1 | tail -1)
    case "$out" in *"0 declined"*"all equal"*) echo "PASS $1 $2" ;; *"1 declined"*) echo "DECL $1 $2" ;; "") echo "HUNG $1 $2" ;; *) echo "FAIL $1 $2" ;; esac
}
export -f one; export R S here RUNNER PAR
fail=0
for c in $cases; do
    res=$(seq 1 "$N" | xargs -P ${PAR:-6} -I{} bash -c 'one '"$c"' {}')
    p=$(echo "$res" | grep -c PASS); f=$(echo "$res" | grep -c FAIL); d=$(echo "$res" | grep -c DECL); h=$(echo "$res" | grep -c HUNG)
    hs=""; [ "$h" -gt 0 ] && hs=", $h hung (the JIT does not finish)"
    echo "$c: $N seeds, $p equal, $f differ, $d declined$hs"
    [ "$f" -gt 0 ] && { echo "$res" | grep FAIL | sort -k3 -n | head -20 | tr '\n' ' '; echo; fail=1; }
done
exit $fail
