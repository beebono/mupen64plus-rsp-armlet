#!/bin/bash
# Survey runner: one ROM, headless, the udump shim in front of parallel-rsp (LLE) with foir (x86_64 scalar build).
#   ucode_dump_run.sh ROM OUTDIR SECS
# Env: UD_SO (shim), CORE_DIR, CONSOLE, FOIR_SO, PRSP_SO, INPUT_SO, DATADIR. Run inside a memory-capped scope (see CLAUDE.md).
set -e
ROM=$1; OUT=$2; SECS=${3:-45}
mkdir -p "$OUT/home/cache" "$OUT/cfg" "$OUT/dump"
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy HOME=$OUT/home XDG_DATA_HOME=$OUT/home XDG_CONFIG_HOME=$OUT/home XDG_CACHE_HOME=$OUT/home/cache
export UDUMP_TARGET=$PRSP_SO UDUMP_DIR=$OUT/dump FOIR_THREADS=2
systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL "$SECS" \
  "$CONSOLE" --corelib "$CORE_DIR/libmupen64plus.so.2" --datadir "$CORE_DIR/../../data" --configdir "$OUT/cfg" --plugindir "$(dirname "$UD_SO")" \
  --gfx "$FOIR_SO" --rsp "$UD_SO" --audio dummy --input "$INPUT_SO" --noosd --set "Core[RandomizeInterrupt]=False" --emumode 2 "$ROM" > "$OUT/log" 2>&1 || true
