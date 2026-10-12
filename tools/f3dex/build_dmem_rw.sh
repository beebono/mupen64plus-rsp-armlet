#!/bin/bash
# Builds armlet-dmem-rw (tools/f3dex/dmem_rw.cpp: trace.cpp with a byte-granular DMEM read/write footprint) from the work copy of scripts/build.sh native --tools -> out/native-tools/armlet-dmem-rw
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
W=$R/work/native-tools
[ -d "$W/build" ] || { echo "run scripts/build.sh native --tools first" >&2; exit 2; }
cp "$R/tools/f3dex/dmem_rw.cpp" "$W/src/tools/native/trace.cpp"
cmake --build "$W/build" --target armlet-native-trace
cp "$W/build/armlet-native-trace" "$R/out/native-tools/armlet-dmem-rw"
cp "$R/tools/native/trace.cpp" "$W/src/tools/native/trace.cpp"
echo "built $R/out/native-tools/armlet-dmem-rw"
