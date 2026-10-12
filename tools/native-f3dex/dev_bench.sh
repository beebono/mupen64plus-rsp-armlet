#!/bin/bash
# Runs armlet-native-replay --bench on a device under its lock, clocks pinned (rgdsplus: performance governor, restored to schedutil;
# rg35xxsp: cpu_perf.sh on/off). usage: dev_bench.sh DEV WORKDIR BENCH_N SNAP... (WORKDIR on the device, e.g. /storage/foir-dev/armlet-b)
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
DEV=$1; X=$2; N=$3; shift 3
LOCKDIR=${SESSION_LOCKDIR:-/tmp/n64-plugins-locks}; mkdir -p "$LOCKDIR"
BIN=$R/out/rg35xxsp-tools/armlet-native-replay
export DEV X N BIN R; export SNAPS="$*"
flock -w 3600 "$LOCKDIR/$DEV.lock" bash -c '
set -e
ssh -o ConnectTimeout=15 $DEV "mkdir -p $X/replay/snap"
if ssh $DEV "pgrep -x mupen64plus" > /dev/null; then echo "device busy" >&2; exit 3; fi
scp -qC $BIN $DEV:$X/replay/armlet-native-replay
tar -C $(dirname $(echo $SNAPS | cut -d" " -f1)) -cf - $(for s in $SNAPS; do basename $s; done) | ssh $DEV "tar -C $X/replay/snap -xf -"
if [ "$DEV" = rg35xxsp ]; then
    scp -q $R/../toolchains/rg35xxsp/scripts/cpu_perf.sh $DEV:$X/replay/cpu_perf.sh
    ssh -tt $DEV "sh $X/replay/cpu_perf.sh on >/dev/null 2>&1; cd $X/replay; LD_LIBRARY_PATH=/mnt/SDCARD/spruce/flip/lib:/usr/lib timeout 280 ./armlet-native-replay --bench $N snap/*.snap; sh $X/replay/cpu_perf.sh off >/dev/null 2>&1"
else
    ssh -tt $DEV "for g in /sys/devices/system/cpu/cpu[0-3]/cpufreq/scaling_governor; do echo performance > \$g; done; cd $X/replay; timeout 280 ./armlet-native-replay --bench $N snap/*.snap; for g in /sys/devices/system/cpu/cpu[0-3]/cpufreq/scaling_governor; do echo schedutil > \$g; done"
fi
ssh $DEV "rm -rf $X/replay/snap"
'
