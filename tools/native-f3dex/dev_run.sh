#!/bin/bash
# Runs armlet-native-replay (any arguments) on a device under its lock, clocks pinned. usage: dev_run.sh DEV WORKDIR [SNAP...] -- ARGS...
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
DEV=$1; X=$2; shift 2
SN=(); while [ $# -gt 0 ] && [ "$1" != -- ]; do SN+=("$1"); shift; done; [ "$1" = -- ] && shift
ARGS="$*"
LOCKDIR=${SESSION_LOCKDIR:-/tmp/n64-plugins-locks}; mkdir -p "$LOCKDIR"
export DEV X ARGS R; export SNAPS="${SN[*]}"
flock -w 3600 "$LOCKDIR/$DEV.lock" bash -c '
set -e
ssh -o ConnectTimeout=15 $DEV "mkdir -p $X/replay/snap && rm -f $X/replay/snap/*"
if ssh $DEV "pgrep -x mupen64plus" > /dev/null; then echo "device busy" >&2; exit 3; fi
scp -qC $R/out/rg35xxsp-tools/armlet-native-replay $DEV:$X/replay/armlet-native-replay
if [ -n "$SNAPS" ]; then tar -C $(dirname $(echo $SNAPS | cut -d" " -f1)) -cf - $(for s in $SNAPS; do basename $s; done) | ssh $DEV "tar -C $X/replay/snap -xf -"; fi
if [ "$DEV" = rg35xxsp ]; then
    scp -q $R/../toolchains/rg35xxsp/scripts/cpu_perf.sh $DEV:$X/replay/cpu_perf.sh
    ssh -tt $DEV "sh $X/replay/cpu_perf.sh on >/dev/null 2>&1; cd $X/replay; LD_LIBRARY_PATH=/mnt/SDCARD/spruce/flip/lib:/usr/lib timeout 280 ./armlet-native-replay $ARGS; sh $X/replay/cpu_perf.sh off >/dev/null 2>&1"
else
    ssh -tt $DEV "for g in /sys/devices/system/cpu/cpu[0-3]/cpufreq/scaling_governor; do echo performance > \$g; done; cd $X/replay; timeout 280 ./armlet-native-replay $ARGS; for g in /sys/devices/system/cpu/cpu[0-3]/cpufreq/scaling_governor; do echo schedutil > \$g; done"
fi
ssh $DEV "rm -rf $X/replay/snap"
'
