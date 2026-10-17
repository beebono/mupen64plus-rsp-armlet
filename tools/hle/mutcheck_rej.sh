#!/bin/bash
# Deliberate errors in the Rej code (docs/testing.md, "Rej"): each must be caught. Three kinds:
#   closed forms (rej_fx.c, rej_dmem.h): test_rej against the rsp2c translations of the microcode (HOST=1, scalar) on a build that exercises it; a failure means caught
#   NEON forms (f3dex2_fx_neon.c, FX2_NEON_MUTATE): the same test under qemu-aarch64, NEON
#   the front-end (hle_rej.c): a native-tools build (--hmut=N); armlet-native-replay of the captured snapshots and of synthetic tasks (--oracle); a mismatch, a decline the unmutated build does not
#   have, or a hung JIT counts as caught
# usage: tools/hle/mutcheck_rej.sh [n...]  (env: ORACLES=<dir with xbus_vtx.inc ... flx_tri.inc>, DUMPS=<derive dump dir>, SNAPS=<dir with exb/ snapshots>)
set -u
H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/../.." && pwd)
: "${ORACLES:?}" "${DUMPS:?}" "${SNAPS:?}"
data() { case $1 in xbus) echo "$DUMPS/unseen-romset/Excitebike 64/dump/f67036e9d2a701c3.0e64072c5fa95f8a.data";; fifo203) echo "$DUMPS/test-romset/F-Zero X/dump/7de803fdbd8fb8b0.c66b1666ca4ac3d1.data";; fifo206) echo "$DUMPS/unseen-romset/Resident Evil 2/dump/95129dc7d307dd43.6a88f656c2bb936f.data";; flx) echo "$DUMPS/test-romset/F-Zero X/dump/c750be26762c28b0.3468240547911617.data";; esac; }
ht() {   # ht BUILD CASES [qemu] [extra compiler flags]
    ( cd "$R"; export REJ_DATA="$(data $1)" REJ_BUILD=$1 REJ_VTX_ORACLE=$ORACLES/$1_vtx.inc REJ_TRI_ORACLE=$ORACLES/$1_tri.inc EXTRA="${4:-}"; if [ -z "${3:-}" ]; then export HOST=1; else unset HOST; fi; tools/hle/build_test_rej.sh $2 2>&1 | tail -1 ); }
caught_h() { case "$1" in *" 0 failures"*) return 1;; *failures*) return 0;; *) return 0;; esac; }
all="376 375 373 374 377 378 379 380 381 383 384 396 360 382 388 389 390 361 366 367 368 369 370 371 385 386 387 391 392 393 394 395 362 363 364"
for MUTN in ${@:-$all}; do
    case $MUTN in
    377|378|379|380|381|383|384|360|361|366|367|382|388|389|390) b=xbus; res=$(ht $b 3000 "" "-DARMLET_HLE_MUTATE=$MUTN"); caught_h "$res" && echo "$MUTN caught ($b: ${res##*: })" || echo "$MUTN NOT CAUGHT ($b)";;
    373|374) b=fifo203; res=$(ht $b 3000 "" "-DARMLET_HLE_MUTATE=$MUTN"); caught_h "$res" && echo "$MUTN caught ($b: ${res##*: })" || echo "$MUTN NOT CAUGHT ($b)";;
    376) b=xbus; res=$(ht $b 3000 "" "-DARMLET_HLE_MUTATE=$MUTN"); caught_h "$res" && echo "$MUTN caught ($b: ${res##*: })" || echo "$MUTN NOT CAUGHT ($b)";;
    396) b=flx; res=$(ht $b 3000 "" "-DARMLET_HLE_MUTATE=$MUTN"); caught_h "$res" && echo "$MUTN caught ($b: ${res##*: })" || echo "$MUTN NOT CAUGHT ($b)";;
    368|369|370|371) b=xbus; res=$(ht $b 3000 qemu "-DFX2_NEON_MUTATE=$MUTN"); caught_h "$res" && echo "$MUTN caught (NEON, $b: ${res##*: })" || echo "$MUTN NOT CAUGHT (NEON)";;
    *)  # the front-end: a native-tools build with the mutation; the replays of the captured snapshots and of synthetic tasks (--oracle) must differ from the unmutated build's (a mismatch, a decline that the unmutated build
        # does not have, or one that it does and the mutated does not)
        (cd "$R"; scripts/build.sh native --tools --hmut=$MUTN >/dev/null 2>&1)
        rp=$R/out/native-tools-hmut$MUTN/armlet-native-replay; base=$R/out/native-tools/armlet-native-replay
        run() { timeout 300 systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 "$@" 2>&1 | tail -2 | sed 's/([0-9.]* us)//; s#.*snap: ##; s#.*DECLINED#DECLINED#' | tr '\n' ' '; }       # (the decline reason too: a guard that another one backs up still changes it)
        r1=$(run $rp $SNAPS/exb/*.snap); r0=$(run $base $SNAPS/exb/*.snap)
        n2=0
        for c in misc bad; do for sd in $(seq 1 $([ $c = bad ] && echo 40 || echo 16)); do
            e=$(export $(python3 "$H/mksyn_rej.py" $c $sd); run $rp --oracle --quiet $SNAPS/exb/hle.task-8b0a3e3240821457-0.snap)
            e0=$(export $(python3 "$H/mksyn_rej.py" $c $sd); run $base --oracle --quiet $SNAPS/exb/hle.task-8b0a3e3240821457-0.snap)
            [ "$e" = "$e0" ] || n2=$((n2+1))
        done; done
        if [ "$r1" != "$r0" ] || [ $n2 -gt 0 ]; then echo "$MUTN caught (replay: $r1; synthetic: $n2 of 56 differ)"; else echo "$MUTN NOT CAUGHT (replay: $r1)"; fi;;
    esac
done
