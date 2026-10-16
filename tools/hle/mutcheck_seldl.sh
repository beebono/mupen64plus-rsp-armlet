#!/bin/bash
# Mutation check of the G_SELECT_DL decline (mutation 315, hle_s2dex2.c; docs/testing.md, "G_SELECT_DL over the jump table"): tools/hle/mksyn_seldl.py is the reduced task of Kirby 64 `load` seed 904.
#   tools/hle/mutcheck_seldl.sh KIRBY_TASK.snap     (a captured Kirby 64 graphics task; MKSYN_S2TEXT etc. for another game's addresses)
# Expected: the build declines the task (flags word at 0x2b0 + 0xbe over the jump table); the build with the decline switched off (--hmut=315) differs from the JIT; a flags word of a game's range (AT 0x10)
# stays native and equal in both.
R=$(cd "$(dirname "$0")/../.." && pwd)
S=$1; [ -n "$S" ] || { echo "usage: $0 KIRBY_TASK.snap" >&2; exit 2; }
"$R/scripts/build.sh" native --tools > /dev/null 2>&1; "$R/scripts/build.sh" native --tools --hmut=315 > /dev/null 2>&1
run() { env $(python3 "$R/tools/hle/mksyn_seldl.py" "$2") systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL 60 "$R/out/$1/armlet-native-replay" --oracle --quiet "$S" 2>&1 | tail -1; }
fail=0
a=$(run native-tools 0xbe); b=$(run native-tools-hmut315 0xbe); c=$(run native-tools 0x10); d=$(run native-tools-hmut315 0x10)
case "$a" in *"1 declined"*) echo "fix: declined (ok)";; *) echo "fix: NOT declined: $a"; fail=1;; esac
case "$b" in *FAILED*) echo "mutation 315: caught (differs from the JIT)";; *) echo "mutation 315: NOT CAUGHT: $b"; fail=1;; esac
case "$c$d" in *"0 declined"*"all equal"*"0 declined"*"all equal"*) echo "AT 0x10: native and equal in both (ok)";; *) echo "AT 0x10: $c / $d"; fail=1;; esac
exit $fail
