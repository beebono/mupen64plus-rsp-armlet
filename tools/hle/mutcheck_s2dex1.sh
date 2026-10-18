#!/bin/bash
# Mutation check of the S2DEX 1.06 / 1.07 front-end and kernels (docs/ucode-s2dex1.md, docs/testing.md): builds the offline harness with each deliberate error (scripts/build.sh native --tools --hmut=N) and replays
# the captured task starts with it; every mutation must be reported (a mismatch or a decline that the unmutated build does not have).
#   MUTS="520 521 ..." tools/hle/mutcheck_s2dex1.sh DIR...        (a directory of task snapshots per game, ARMLET_HLE_CAPTURE=<dir>: test-output/s2dex1/{bangaioh,mtc,ppl,yoshi}; a file "patch" in it is the HLE_PATCH of its snapshots:
#   the RDRAM rows of a text that a G_LOAD_UCODE loads lack the bytes the front-end hashes and compares, docs/testing.md)
# The mutations of the kernels (src/hle/s2dex2_rect.c, s2dex2_bg.c and s2dex1_bgcopy.c: 500 .. 512, and the ones of the 2.x numbering that the 1.x forms share) that the snapshots do not catch (the games draw few flipped, scaled or
# cut sprites) are run through the random test against the microcode instead (tools/hle/test_s2dex1.c; needs tools/hle/oracle/s1_*_gen.inc, docs/testing.md, "Regenerating the microcode oracles"): "caught by the random test".
# The guards of the front-end (521 .. 523, 529, 532, 533: a game's values are inside them) are exercised by perturbed copies of the snapshots (tools/rsp2c/fuzz_snaps.py, FUZZ=<dir of them>, replayed with --oracle).
R=$(cd "$(dirname "$0")/../.." && pwd)
[ $# -gt 0 ] || { echo "usage: MUTS='520 521 ...' $0 DIR..." >&2; exit 2; }
fail=0
KERN_MUTS="500 501 502 503 504 510 511 512 513 514 515 516 517 $(seq 540 561 | grep -v 547 | tr '\n' ' ')"
for n in ${MUTS:-520 521 522 523 524 525 526 527 528 529 530 531 532 533 $KERN_MUTS}; do
    "$R/scripts/build.sh" native --tools --hmut=$n > /dev/null 2>&1
    rc=0
    out=
    for d in "$@"; do
        out+=$(HLE_PATCH=$(cat "$d/patch" 2>/dev/null) systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL ${MUT_TIMEOUT:-300} "$R/out/native-tools-hmut$n/armlet-native-replay" --quiet "$d"/*.snap 2>&1) || rc=$?
        out+=$'\n'
    done
    nbad=$(printf '%s\n' "$out" | grep -c 'MISMATCH native vs JIT' || true)
    ndec=$(printf '%s\n' "$out" | grep -c 'DECLINED' || true)
    if [ "$nbad" -gt 0 ] || [ "$ndec" -gt 0 ]; then echo "mutation $n: caught in $nbad snapshots (+ $ndec declined)"; continue; fi
    if [ "$rc" -ne 0 ] && ! printf '%s\n' "$out" | grep -q 'all equal'; then echo "mutation $n: caught (the replay hung or crashed, exit $rc)"; continue; fi
    if [[ " $KERN_MUTS " == *" $n "* ]] && nf=$(HOST=1 OUT=${TMPDIR:-/tmp}/mutcheck_s1 "$R/tools/hle/build_test_s2dex1.sh" "-DARMLET_HLE_MUTATE=$n" ${KERN_CASES:-300000} 2>&1 | grep -o '[0-9]* failures' | grep -v '^0 '); then
        echo "mutation $n: caught by the random test against the microcode ($nf in ${KERN_CASES:-300000} cases)"; continue
    fi
    if [ -n "$FUZZ" ] && ls "$FUZZ"/*.snap > /dev/null 2>&1; then
        out=$(REPLAY_ARGS="--oracle" REPLAY_TIMEOUT=${FUZZ_TIMEOUT:-20} "$R/tools/rsp2c/replay_each.sh" "$R/out/native-tools-hmut$n/armlet-native-replay" "$FUZZ"/*.snap 2>&1)
        base=$(REPLAY_ARGS="--oracle" REPLAY_TIMEOUT=${FUZZ_TIMEOUT:-20} "$R/tools/rsp2c/replay_each.sh" "$R/out/native-tools/armlet-native-replay" "$FUZZ"/*.snap 2>&1 | tail -1)
        if [ "$(printf '%s\n' "$out" | tail -1)" != "$base" ]; then echo "mutation $n: caught by the perturbed snapshots ($(printf '%s\n' "$out" | tail -1) against $base)"; continue; fi
    fi
    echo "mutation $n: NOT CAUGHT"; fail=1
done
exit $fail
