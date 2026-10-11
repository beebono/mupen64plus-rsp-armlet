#!/bin/bash
# HOST. One session on a device under its lock: deploy (md5-checked), run, fetch to test-output/equiv/<dev>/, clear the device's out/.
#   tools/equiv/host.sh DEV LABEL RUN...      RUN = TAG|BUILD|ROM|KIND|LAST (see device/runner.sh); LABEL names the session in session.<LABEL>.info
# Env: EQUIV_X (the device's work dir, required), EQUIV_FLAVOUR (rgdsplus | rg35xxsp, default DEV), PIN=1 (pin the clocks and restore them),
#      EQUIV_FOIR (the foir .so, default ../toolchains/rg35xxsp/out/mupen64plus-rdp-foir.so, which runs on both devices),
#      EQUIV_ROMS (newline-separated local ROM files copied to <X>/roms/ for runs that name them as @file; never committed, remove them afterwards),
#      EQUIV_LOCKDIR (default /tmp/n64-plugins-locks), EQUIV_WAIT (seconds the session may take, default 300).
# Keep a session to about 5 minutes. Needs scripts/build.sh and tools/equiv/build-tools.sh for rg35xxsp (and ref) first.
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
DEV=$1; LABEL=$2; shift 2
: "${EQUIV_X:?set EQUIV_X to the work dir on the device}"
FLAVOUR=${EQUIV_FLAVOUR:-$DEV}; WAIT=${EQUIV_WAIT:-300}; LOCKDIR=${EQUIV_LOCKDIR:-/tmp/n64-plugins-locks}
FOIR=${EQUIV_FOIR:-$R/../toolchains/rg35xxsp/out/mupen64plus-rdp-foir.so}
RG35=${RG35_TOOLCHAIN:-$R/../toolchains/rg35xxsp}
L=$R/test-output/equiv/$DEV; mkdir -p "$L" "$LOCKDIR"
export R DEV LABEL EQUIV_X FLAVOUR WAIT FOIR RG35 L PIN EQUIV_ROMS
export RUNS; RUNS=$(printf '%s\n' "$@")
flock -w 3600 "$LOCKDIR/$DEV.lock" bash -c '
set -e
X=$EQUIV_X; O=$R/out/rg35xxsp
ssh -o ConnectTimeout=15 "$DEV" "mkdir -p $X/plug/ref $X/out"
if ssh "$DEV" "pgrep -x mupen64plus" > /dev/null; then echo "device busy: mupen64plus running" >&2; exit 3; fi
sf() { m=$(md5sum < "$1" | cut -d" " -f1); r=$(ssh -n "$DEV" "test -f $2 && md5sum < $2" 2>/dev/null | cut -d" " -f1); [ "$m" = "$r" ] || scp -qC "$1" "$DEV:$2"; }
sf "$FOIR" $X/plug/mupen64plus-rdp-foir.so
sf $O/mupen64plus-rsp-armlet.so $X/plug/mupen64plus-rsp-armlet.so
sf $O/ref/mupen64plus-rsp-hle.so $X/plug/ref/mupen64plus-rsp-hle.so
sf $O/ref/mupen64plus-rsp-parallel.so $X/plug/ref/mupen64plus-rsp-parallel.so
for t in rsp-shim input-det audio-adump; do sf $O/tools/$t.so $X/plug/$t.so; done
for f in $(cd $R/tools/equiv/device && ls runner.sh $FLAVOUR*.sh); do sf $R/tools/equiv/device/$f $X/$f; done
[ "$FLAVOUR" = rg35xxsp ] && sf $RG35/scripts/cpu_perf.sh $X/cpu_perf.sh
if [ -n "$EQUIV_ROMS" ]; then
    ssh "$DEV" "mkdir -p $X/roms"
    while IFS= read -r rom; do [ -n "$rom" ] && sf "$rom" "$X/roms/$(basename "$rom" | tr " " "_")"; done <<< "$EQUIV_ROMS"
fi
ssh "$DEV" "rm -rf $X/out/* $X/done; cat > $X/runs.txt" <<< "$RUNS"
if [ "$FLAVOUR" = rg35xxsp ]; then
    ssh "$DEV" "MAXWAIT=$WAIT PIN=$PIN timeout $((WAIT + 120)) sh $X/rg35xxsp_session.sh $X" > "$L/session.$LABEL.out" 2>&1 || true
else
    ssh "$DEV" "PIN=$PIN timeout $((WAIT + 60)) sh $X/rgdsplus.sh $X" > "$L/session.$LABEL.out" 2>&1 || true
fi
ssh "$DEV" "pgrep -x mupen64plus" >> "$L/session.$LABEL.out" 2>&1 || true
scp -rqC "$DEV:$X/out/*" "$L/"
mv "$L/session.info" "$L/session.$LABEL.info"
ssh "$DEV" "rm -rf $X/out/*"
cat "$L/session.$LABEL.out"
'
