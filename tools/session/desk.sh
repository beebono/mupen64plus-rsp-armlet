#!/bin/bash
# Desktop run of the profiling session runner (x86_64, offscreen, memory-capped scope); the same run lines as the devices.
#   tools/session/desk.sh RUN...      RUN = TAG|ROM|SO|FRAMES|ENV, ROM relative to resources/romsets (e.g. test-romset/Super Mario 64.z64)
# Env: CORE_DIR (built mupen64plus-core projects/unix), CONSOLE (mupen64plus-ui-console binary), FOIR_SO (x86_64 foir plugin, NEON=0 build),
#      X (work dir, default test-output/session/desk), MEMMAX (default 2G). Needs out/native{,-prof,-prof3}/ and out/native/tools (build.sh, build-tools.sh).
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
: "${CORE_DIR:?}" "${CONSOLE:?}" "${FOIR_SO:?}"
X=${X:-$R/test-output/session/desk}
mkdir -p "$X/plug" "$X/out" "$X/cfg" "$X/home/cache"
ln -sf "$R/out/native/mupen64plus-rsp-armlet.so" "$X/plug/armlet.so"
for v in prof prof3; do [ -f "$R/out/native-$v/mupen64plus-rsp-armlet.so" ] && ln -sf "$R/out/native-$v/mupen64plus-rsp-armlet.so" "$X/plug/armlet-$v.so"; done
"$R/tools/equiv/stamp.sh" check "$R/out/native/tools" || exit 1
ln -sf "$R/out/native/tools/rsp-shim.so" "$R/out/native/tools/input-det.so" "$X/plug/"
ln -sf "$FOIR_SO" "$X/plug/mupen64plus-rdp-foir.so"
printf '%s\n' "$@" > "$X/runs.txt"
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy HOME=$X/home XDG_DATA_HOME=$X/home XDG_CONFIG_HOME=$X/home XDG_CACHE_HOME=$X/home/cache
MUPEN="$CONSOLE --corelib $CORE_DIR/libmupen64plus.so.2 --datadir $R/../mupen64plus-core-dento/data"; ROMDIR=$R/../../../resources/romsets; VIDARGS=
WRAP="systemd-run --user --scope -q -p MemoryMax=${MEMMAX:-2G} -p MemorySwapMax=0"; GAP=1
set +e
. "$R/tools/session/runner.sh"
: > "$X/out/session.info"
equiv_run_all
cat "$X/out/session.info"
