#!/bin/bash
# One game for the native-coverage recount (docs/ucode-coverage.md): headless x86_64, GfxMode=hle, the per-ucode counters of src/ustats.c.
#   ucode_coverage_run.sh ROM OUTDIR [SECS]        -> OUTDIR/ustats.txt  (tools/ucode_coverage.py reads <set>/<game>/ustats.txt)
# Env: CORE_DIR (built mupen64plus-core projects/unix), CONSOLE (mupen64plus-ui-console), FOIR_SO (x86_64 foir, NEON=0 build), INPUT_SO (tools/equiv's input-det.so), ARMLET_SO (the module).
# Run six at a time at most, inside the memory-capped scope that this script makes (CLAUDE.md, "Desktop safety").
set -e
ROM=$1; O=$2; SECS=${3:-57}
mkdir -p "$O/home/cache" "$O/cfg"
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy HOME="$O/home" XDG_DATA_HOME="$O/home" XDG_CONFIG_HOME="$O/home" XDG_CACHE_HOME="$O/home/cache"
export FOIR_THREADS=2 ARMLET_GFX=hle ARMLET_HLE_STATS=1 ARMLET_UCODE_STATS="$O/ustats.txt"
systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL "$SECS" \
  "$CONSOLE" --corelib "$CORE_DIR/libmupen64plus.so.2" --datadir "$CORE_DIR/../../data" --configdir "$O/cfg" --plugindir "$(dirname "$ARMLET_SO")" \
  --gfx "$FOIR_SO" --rsp "$ARMLET_SO" --audio dummy --input "$INPUT_SO" --noosd --set "Core[RandomizeInterrupt]=False" --emumode 2 "$ROM" > "$O/log" 2>&1 || true
