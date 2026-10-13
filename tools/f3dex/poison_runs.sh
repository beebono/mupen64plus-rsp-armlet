#!/bin/bash
# poison_runs.sh TAG ROM [LAST]: the equivalence runs of one game, each in its own desk.sh session (the runner exports the environment of a run into the next one): ref, hle (native), hle +
# ARMLET_DMEM_POISON=mask, =all, =mask,ring (the RDP output ring in RDRAM overwritten too) and a verify run. Output: test-output/f3dex/poison/<TAG>/out/<TAG>.<kind>.1   Compare: tools/equiv/compare.py
R=$(cd "$(dirname "$0")/../.." && pwd)
T=$1; ROM=$2; LAST=${3:-900}
export ARMLET_SO=${ARMLET_SO:-$R/out/native-tools/mupen64plus-rsp-armlet.so} MEMMAX=2G
X=$R/test-output/f3dex/poison/$T; export X; rm -rf "$X"
$R/tools/f3dex/desk.sh "$T.ref.1|ref|$ROM|e2e|$LAST" > /dev/null 2>&1
for k in "hle|armlet-hle" "mask|armlet-hle+ARMLET_DMEM_POISON=mask" "all|armlet-hle+ARMLET_DMEM_POISON=all" "ring|armlet-hle+ARMLET_DMEM_POISON=mask,ring" "v|armlet-hle+ARMLET_HLE_VERIFY=1+ARMLET_HLE_STATS=1"; do
  $R/tools/f3dex/desk.sh "$T.${k%%|*}.1|${k#*|}|$ROM|e2e|$LAST" > /dev/null 2>&1
done
python3 $R/tools/equiv/compare.py $X/out $T.ref.1 $T.hle.1 $T.ref.1 $T.mask.1 $T.ref.1 $T.all.1 $T.ref.1 $T.ring.1 $T.ref.1 $T.v.1 2>&1 | cut -c1-200 > $X/compare.txt
