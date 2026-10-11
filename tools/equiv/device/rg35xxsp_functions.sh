#!/bin/sh
# ON rg35xxsp, sourced by the spruce launcher copy (rg35xxsp_session.sh replaces run_mupen_standalone with run_exp).
# Runs $X/runs.txt with spruce's standalone mupen64plus, private HOME / XDG / configdir under $X; nothing of spruce is modified.
X=${X:-/mnt/sdcard/n64-dev/armlet}
. "$X/runner.sh"
run_exp() {
    S=/mnt/SDCARD/Emu/N64/mupen64plus
    rm -rf "$X/home"; mkdir -p "$X/home/cache" "$X/out" "$X/cfg"
    touch "$X/marker"
    export HOME=$X/home XDG_CONFIG_HOME=$X/home XDG_DATA_HOME=$X/home XDG_CACHE_HOME=$X/home/cache SDL_AUDIODRIVER=dummy
    export LD_LIBRARY_PATH="$S:/mnt/SDCARD/App/PyUI/dll-mali:/mnt/SDCARD/spruce/flip/lib:/usr/lib:/usr/lib/aarch64-linux-gnu:$LD_LIBRARY_PATH"
    MUPEN="$S/mupen64plus --corelib $S/libmupen64plus.so.2 --datadir $S"; ROMDIR=/mnt/SDCARD/Roms/N64; VIDARGS="--fullscreen --resolution 640x480"
    GAP=3; KILLALL=1
    echo "launcher env: gov $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor) freq $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq) online $(cat /sys/devices/system/cpu/online)" > "$X/out/session.info"
    [ -n "$PIN" ] && sh "$X/cpu_perf.sh" on >> "$X/out/session.info" 2>&1
    equiv_run_all
    [ -n "$PIN" ] && sh "$X/cpu_perf.sh" off >> "$X/out/session.info" 2>&1
    { echo "outside-writes check (newer than marker, outside $X):"
      find /mnt/SDCARD/Emu/N64 /mnt/SDCARD/Roms/N64 /mnt/SDCARD/Saves /mnt/SDCARD/App/PyUI /root -xdev -newer "$X/marker" -not -path "$X/*" 2>/dev/null | head -20
      echo "(end of list)"; } >> "$X/out/session.info"
}
