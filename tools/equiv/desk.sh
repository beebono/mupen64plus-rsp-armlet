#!/bin/bash
# Desktop run of the equivalence harness (x86_64, offscreen, memory-capped scope). The build, plugin and run list are the same as on the devices.
#   tools/equiv/desk.sh RUN...      RUN = TAG|BUILD|ROM|KIND|LAST, ROM relative to resources/romsets (e.g. test-romset/Super Mario 64.z64)
# Env: ARMLET_SO (another armlet build, e.g. a mutated one), CORE_DIR (built mupen64plus-core projects/unix), CONSOLE (mupen64plus-ui-console binary), FOIR_SO (x86_64 foir plugin, NEON=0 build),
#      X (work dir, default test-output/equiv/desk), MEMMAX (default 2G). Needs out/native/{mupen64plus-rsp-armlet.so,ref,tools} (scripts/build.sh, build-tools.sh).
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
: "${CORE_DIR:?}" "${CONSOLE:?}" "${FOIR_SO:?}"
X=${X:-$R/test-output/equiv/desk}; N=${NATIVE_OUT:-$R/out/native}
"$R/tools/equiv/stamp.sh" check "$N/tools" || exit 1
mkdir -p "$X/plug/ref" "$X/out" "$X/cfg" "$X/home/cache"
ln -sf "${ARMLET_SO:-$N/mupen64plus-rsp-armlet.so}" "$X/plug/mupen64plus-rsp-armlet.so"
ln -sf "$N/ref/mupen64plus-rsp-hle.so" "$N/ref/mupen64plus-rsp-parallel.so" "$X/plug/ref/"
ln -sf "$N/tools/rsp-shim.so" "$N/tools/input-det.so" "$N/tools/audio-adump.so" "$X/plug/"
ln -sf "$FOIR_SO" "$X/plug/mupen64plus-rdp-foir.so"
printf '%s\n' "$@" > "$X/runs.txt"
if [ -x "$R/out/native-tools/armlet-native-replay" ]; then "$R/out/native-tools/armlet-native-replay" --masks > "$X/masks.txt"; else rm -f "$X/masks.txt"; fi
# EXTRA_MASKS="<id>:lo-hi,lo-hi;<id>:..." (hex): dead DMEM ranges of ucodes that are not in the tables (the probe's latched builds, tools/hle/probe_replay.c), appended to the masks both sides use
if [ -n "$EXTRA_MASKS" ]; then _m=$(cat "$X/masks.txt" 2>/dev/null); printf '%s%s\n' "${_m:+$_m;}" "$EXTRA_MASKS" > "$X/masks.txt"; fi
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy HOME=$X/home XDG_DATA_HOME=$X/home XDG_CONFIG_HOME=$X/home XDG_CACHE_HOME=$X/home/cache
MUPEN="$CONSOLE --corelib $CORE_DIR/libmupen64plus.so.2 --datadir $R/../mupen64plus-core-dento/data"; ROMDIR=$R/../../../resources/romsets; VIDARGS=
WRAP="systemd-run --user --scope -q -p MemoryMax=${MEMMAX:-2G} -p MemorySwapMax=0"; GAP=1
set +e
. "$R/tools/equiv/device/runner.sh"
: > "$X/out/session.info"
equiv_run_all
cat "$X/out/session.info"
