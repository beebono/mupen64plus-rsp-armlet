#!/bin/bash
# replay_all.sh [ids...]: the task-level family against the captured JIT results (test-output/f3dex/snap), summary per ucode
R=$(cd "$(dirname "$0")/../.." && pwd)
IDS=${*:-"208a567e9aead5ea fd5a31ccf6d40b62 7502cfb7098eeffb 34dad67335a54cda 047252e810e35d56 5f33f7c2be924de0"}
for id in $IDS; do
  ls $R/test-output/f3dex/snap/hle.task-$id-*.snap >/dev/null 2>&1 || continue
  echo "== $id"
  systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL 600 $R/out/native-tools/armlet-native-replay $REPLAY_ARGS $R/test-output/f3dex/snap/hle.task-$id-*.snap 2>&1 | grep -v "native == JIT\|task-level native graphics" | sort | uniq -c | sort -rn | head -${TOP:-12}
done
