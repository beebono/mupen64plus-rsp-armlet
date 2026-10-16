#!/bin/bash
# replay_each.sh REPLAY SNAPSHOT...: one armlet-native-replay run per snapshot with a time limit (a perturbed display list can make the JIT loop for ever); prints the summary
#   REPLAY_TIMEOUT (default 20 s), DECLINE_LOG (file that collects the decline reasons)
# Each run is a process group of its own (job control), and the group is killed when the run ends and when this script is interrupted: `armlet-native-replay --oracle` forks the JIT into a child
# per snapshot, which a replay that `timeout` killed could leave spinning (and holding its binary: "Text file busy" at the next build).
R=$1; shift
set -m
cur=
trap '[ -n "$cur" ] && kill -KILL -- -$cur 2>/dev/null; exit 130' INT TERM HUP
tmp=$(mktemp); trap 'rm -f "$tmp"' EXIT
eq=0; bad=0; dec=0; hang=0
for f in "$@"; do
    systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL ${REPLAY_TIMEOUT:-20} "$R" --quiet $REPLAY_ARGS "$f" >"$tmp" 2>&1 </dev/null &
    cur=$!
    { wait $cur; rc=$?; } 2>/dev/null
    kill -KILL -- -$cur 2>/dev/null          # whatever the run left in its group (the JIT child)
    cur=
    out=$(cat "$tmp")
    if [ $rc -eq 137 ] || [ $rc -eq 124 ]; then hang=$((hang+1)); echo "HANG $f"; continue; fi
    case "$out" in
    *"MISMATCH native vs JIT"*) bad=$((bad+1)); echo "MISMATCH $f";;
    *DECLINED*) dec=$((dec+1)); echo "$out" | grep DECLINED | sed 's/.*DECLINED//' | head -1 >> ${DECLINE_LOG:-/dev/null};;
    *"native == JIT"*) eq=$((eq+1));;
    *) echo "OTHER $f: $out" | head -3;;
    esac
done
echo "equal $eq, mismatch $bad, declined $dec, hung $hang"
