#!/bin/bash
# HOST. One profiling session on a device under its lock: deploy (md5-checked), run, fetch to test-output/session/<dev>/, clear the device's out/.
#   tools/session/host.sh DEV LABEL RUN...      RUN = TAG|ROM|SO|FRAMES|ENV (see runner.sh); LABEL names the session in session.<LABEL>.info
# Env: SESSION_X (the device's work dir, required), SESSION_FLAVOUR (rgdsplus | rg35xxsp, default DEV), PIN=1 (pin the clocks and restore them),
#      SESSION_FOIR (the foir .so, default ../toolchains/rg35xxsp/out/mupen64plus-rdp-foir.so, which runs on both devices),
#      SESSION_WAIT (seconds the session may take, default 300), SESSION_LOCKDIR (default /tmp/n64-plugins-locks),
#      SESSION_KEEP (a glob, relative to out/, of files to leave on the device; default none: everything is fetched and removed).
# Keep a session to about 5 minutes. Needs scripts/build.sh rg35xxsp [--prof|--prof3] and tools/equiv/build-tools.sh rg35xxsp first; the builds
# in out/rg35xxsp{,-prof,-prof3,-shadow} are deployed as armlet.so, armlet-prof.so, armlet-prof3.so, armlet-shadow.so (whichever exist). Nothing of this is committed
# with host names: DEV is an ssh alias.
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
DEV=$1; LABEL=$2; shift 2
: "${SESSION_X:?set SESSION_X to the work dir on the device}"
FLAVOUR=${SESSION_FLAVOUR:-$DEV}; WAIT=${SESSION_WAIT:-300}; LOCKDIR=${SESSION_LOCKDIR:-/tmp/n64-plugins-locks}
FOIR=${SESSION_FOIR:-$R/../toolchains/rg35xxsp/out/mupen64plus-rdp-foir.so}
RG35=${RG35_TOOLCHAIN:-$R/../toolchains/rg35xxsp}
L=$R/test-output/session/$DEV; mkdir -p "$L" "$LOCKDIR"
export R DEV LABEL SESSION_X FLAVOUR WAIT FOIR RG35 L PIN
export RUNS; RUNS=$(printf '%s\n' "$@")
flock -w 3600 "$LOCKDIR/$DEV.lock" bash -c '
set -e
X=$SESSION_X
ssh -o ConnectTimeout=15 "$DEV" "mkdir -p $X/plug $X/out"
if ssh "$DEV" "pgrep -x mupen64plus" > /dev/null; then echo "device busy: mupen64plus running" >&2; exit 3; fi
sf() { m=$(md5sum < "$1" | cut -d" " -f1); r=$(ssh -n "$DEV" "test -f $2 && md5sum < $2" 2>/dev/null | cut -d" " -f1); [ "$m" = "$r" ] || scp -qC "$1" "$DEV:$2"; }
sf "$FOIR" $X/plug/mupen64plus-rdp-foir.so
for v in "" -prof -prof3 -shadow; do
    f=$R/out/rg35xxsp$v/mupen64plus-rsp-armlet.so
    [ -f "$f" ] && sf "$f" $X/plug/armlet$v.so
done
for t in rsp-shim input-det; do sf $R/out/rg35xxsp/tools/$t.so $X/plug/$t.so; done
for f in rgdsplus.sh rg35xxsp_session.sh rg35xxsp_functions.sh; do sf $R/tools/equiv/device/$f $X/$f; done
sf $R/tools/session/runner.sh $X/runner.sh
[ "$FLAVOUR" = rg35xxsp ] && sf $RG35/scripts/cpu_perf.sh $X/cpu_perf.sh
ssh "$DEV" "rm -rf $X/out/* $X/done; cat > $X/runs.txt" <<< "$RUNS"
if [ "$FLAVOUR" = rg35xxsp ]; then
    ssh "$DEV" "MAXWAIT=$WAIT PIN=$PIN timeout $((WAIT + 120)) sh $X/rg35xxsp_session.sh $X" > "$L/session.$LABEL.out" 2>&1 || true
else
    ssh "$DEV" "PIN=$PIN timeout $((WAIT + 60)) sh $X/rgdsplus.sh $X" > "$L/session.$LABEL.out" 2>&1 || true
fi
ssh "$DEV" "pgrep -x mupen64plus" >> "$L/session.$LABEL.out" 2>&1 || true
mkdir -p "$L/$LABEL"
scp -rqC "$DEV:$X/out/*" "$L/$LABEL/"
mv "$L/$LABEL/session.info" "$L/session.$LABEL.info"
ssh "$DEV" "rm -rf $X/out/*"
cat "$L/session.$LABEL.out"
'
