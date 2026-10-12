#!/bin/bash
# rw_run.sh TAG UCODEID: runs armlet-dmem-rw on every captured task start of the ucode in test-output/f3dex/rw (EVERY=1 captures) -> test-output/f3dex/rwout/rw/TAG/*.rw, then the union per build
R=$(cd "$(dirname "$0")/../.." && pwd)
TAG=$1; ID=$2
O=$R/test-output/f3dex/rwout; mkdir -p $O/rw/$TAG
for f in $R/test-output/f3dex/rw/hle.task-$ID-*.snap; do
  b=$(basename $f .snap)
  [ -s $O/rw/$TAG/$b.rw ] || systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL 120 $R/out/native-tools/armlet-dmem-rw -s 0x1000 -n 20000000 --final $f > $O/rw/$TAG/$b.rw 2>&1
done
(cd $O && python3 $R/tools/f3dex/dmem_rw_agg.py $TAG)
