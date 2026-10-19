#!/bin/bash
# Device session (under the device's lock, clocks pinned, <= ~5 min): the task-level F3DEX front-end against the captured JIT results, and its per-function timing.
#   tools/f3dex/dev_replay.sh DEV WORKDIR MODE [STRIDE]        DEV rgdsplus|rg35xxsp, WORKDIR on the device (e.g. /storage/foir-dev/armlet-b, /mnt/sdcard/n64-dev/armlet-b)
#   MODE eq     replay of every STRIDE-th captured task (default 1) per ucode: native == JIT?
#        bench  F3DEX_PROF=1, the NEON forms and the scalar forms (F3DEX_SCALAR=1), --bench ${BENCHN:-3} over every STRIDE-th task (default 4): per-function ns, per-task native vs JIT times
# The binary is the rg35xxsp toolchain's armlet-native-replay (glibc 2.17 symbols: runs on both devices). Snapshots (ROM-derived) are removed from the device afterwards.
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
DEV=$1; X=$2; MODE=$3; STRIDE=${4:-$([ "$MODE" = bench ] && echo 4 || echo 1)}
LOCKDIR=${SESSION_LOCKDIR:-/tmp/n64-plugins-locks}; mkdir -p "$LOCKDIR"
BIN=${BIN:-$R/out/rg35xxsp-tools/armlet-native-replay}
LIST=$(ls $R/test-output/f3dex/snap/hle.task-*.snap | awk -v s=$STRIDE 'NR % s == 1 || s == 1')
IDS=${IDS:-"208a567e9aead5ea fd5a31ccf6d40b62 7502cfb7098eeffb 34dad67335a54cda"}
export DEV X MODE BIN R LIST IDS BENCHN
flock -w 3600 "$LOCKDIR/$DEV.lock" bash -c '
set -e
ssh -o ConnectTimeout=15 $DEV "mkdir -p $X/replay/snap && rm -f $X/replay/snap/*"
if ssh $DEV "pgrep -x mupen64plus" > /dev/null; then echo "device busy" >&2; exit 3; fi
scp -qC $BIN $DEV:$X/replay/armlet-native-replay
tar -C $R/test-output/f3dex/snap -cf - $(for s in $LIST; do basename $s; done) | ssh $DEV "tar -C $X/replay/snap -xf -"
cat > /tmp/dev_replay_run.sh <<EOS
cd $X/replay
run() { timeout 70 "\$@"; }
case $MODE in
eq) for id in $IDS; do echo "== \$id"; run ./armlet-native-replay --quiet snap/hle.task-\$id-*.snap 2>&1 | tail -3; done ;;
bench) for id in $IDS; do
         for mode in NEON scalar; do
           echo "== \$id \$mode"
           if [ \$mode = scalar ]; then export F3DEX_SCALAR=1; else unset F3DEX_SCALAR; fi
           F3DEX_PROF=1 run ./armlet-native-replay --quiet --bench ${BENCHN:-3} snap/hle.task-\$id-*.snap > out.tmp 2>&1
           grep "F3DEXP " out.tmp; awk "/bench: front-end/{a+=\\\$3; b+=\\\$6; n++} END{printf \"bench total over %d tasks: front-end %.0f us, JIT %.0f us (%.2fx)\\n\", n, a, b, b/a}" out.tmp; grep snapshots out.tmp | tail -1
         done
       done ;;
esac
EOS
scp -q /tmp/dev_replay_run.sh $DEV:$X/replay/run.sh
if [ "$DEV" = rg35xxsp ]; then
    scp -q $R/../../../resources/toolchains/rg35xxsp/scripts/cpu_perf.sh $DEV:$X/replay/cpu_perf.sh
    ssh -tt $DEV "sh $X/replay/cpu_perf.sh on >/dev/null 2>&1; LD_LIBRARY_PATH=/mnt/SDCARD/spruce/flip/lib:/usr/lib sh $X/replay/run.sh > $X/replay/out.txt 2>&1; sh $X/replay/cpu_perf.sh off >/dev/null 2>&1"
else
    ssh -tt $DEV "for g in /sys/devices/system/cpu/cpu[0-3]/cpufreq/scaling_governor; do echo performance > \$g; done; sh $X/replay/run.sh > $X/replay/out.txt 2>&1; for g in /sys/devices/system/cpu/cpu[0-3]/cpufreq/scaling_governor; do echo schedutil > \$g; done"
fi
ssh $DEV "cat $X/replay/out.txt; rm -rf $X/replay/snap"
'
