#!/bin/bash
# replay_each.sh REPLAY SNAPSHOT...: one armlet-native-replay run per snapshot with a time limit (a perturbed display list can make the JIT loop for ever); prints the summary
#   REPLAY_TIMEOUT (default 20 s), DECLINE_LOG (file that collects the decline reasons)
R=$1; shift
eq=0; bad=0; dec=0; hang=0
for f in "$@"; do
    out=$(systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL ${REPLAY_TIMEOUT:-20} "$R" --quiet $REPLAY_ARGS "$f" 2>&1); rc=$?
    if [ $rc -eq 137 ] || [ $rc -eq 124 ]; then hang=$((hang+1)); echo "HANG $f"; continue; fi
    case "$out" in
    *"MISMATCH native vs JIT"*) bad=$((bad+1)); echo "MISMATCH $f";;
    *DECLINED*) dec=$((dec+1)); echo "$out" | grep DECLINED | sed 's/.*DECLINED//' | head -1 >> ${DECLINE_LOG:-/dev/null};;
    *"native == JIT"*) eq=$((eq+1));;
    *) echo "OTHER $f: $out" | head -3;;
    esac
done
echo "equal $eq, mismatch $bad, declined $dec, hung $hang"
