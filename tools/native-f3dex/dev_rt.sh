#!/bin/bash
# Runs out-rg35/rt_test on a device under its lock, clocks pinned. usage: dev_rt.sh DEV WORKDIR UCODE_DIR -- ARGS...   (ARGS may use $UC for the ucode dir)
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
DEV=$1; X=$2; UCD=$3; shift 3; [ "$1" = -- ] && shift
ARGS="$*"; ARGS=${ARGS//@UC@/$X/rt/uc}; export DEV X UCD R ARGS RT_PREFIX RT_NOJIT
LOCKDIR=${SESSION_LOCKDIR:-/tmp/n64-plugins-locks}; mkdir -p "$LOCKDIR"
flock -w 3600 "$LOCKDIR/$DEV.lock" bash -c '
set -e
ssh -o ConnectTimeout=15 $DEV "mkdir -p $X/rt/uc"
if ssh $DEV "pgrep -x mupen64plus" > /dev/null; then echo "device busy" >&2; exit 3; fi
scp -qC $R/tools/native-f3dex/out-rg35/rt_test $DEV:$X/rt/rt_test
scp -qC $UCD/*.text $UCD/*.data $DEV:$X/rt/uc/
CMD="cd $X/rt; export RT_NOJIT=$RT_NOJIT; timeout 280 $RT_PREFIX ./rt_test $ARGS"
if [ "$DEV" = rg35xxsp ]; then
    scp -q $R/../../../resources/toolchains/rg35xxsp/scripts/cpu_perf.sh $DEV:$X/rt/cpu_perf.sh
    ssh -tt $DEV "sh $X/rt/cpu_perf.sh on >/dev/null 2>&1; LD_LIBRARY_PATH=/mnt/SDCARD/spruce/flip/lib:/usr/lib sh -c \"$CMD\"; sh $X/rt/cpu_perf.sh off >/dev/null 2>&1"
else
    ssh -tt $DEV "for g in /sys/devices/system/cpu/cpu[0-3]/cpufreq/scaling_governor; do echo performance > \$g; done; sh -c \"$CMD\"; for g in /sys/devices/system/cpu/cpu[0-3]/cpufreq/scaling_governor; do echo schedutil > \$g; done"
fi
'
