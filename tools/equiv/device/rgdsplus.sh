#!/bin/sh
# ON rgdsplus (ROCKNIX). rgdsplus.sh X : pins the performance governor, runs $X/runs.txt, restores schedutil. Private HOME / XDG dirs
# (HOME is /storage there, where saves of the CFW's own mupen64plus live) and configdir under $X.
X=$1
. /etc/profile > /dev/null 2>&1
export XDG_RUNTIME_DIR=/var/run/0-runtime-dir WAYLAND_DISPLAY=wayland-1 SDL_VIDEODRIVER=wayland SDL_AUDIODRIVER=dummy
mkdir -p "$X/home/cache"
export HOME=$X/home XDG_DATA_HOME=$X/home XDG_CONFIG_HOME=$X/home XDG_CACHE_HOME=$X/home/cache
MUPEN="/usr/local/bin/mupen64plus --datadir /usr/local/share/mupen64plus"; ROMDIR=/storage/roms/n64; VIDARGS=
KILLALL=1
. "$X/runner.sh"
pin() {
    G=schedutil; [ "$1" = on ] && G=performance
    for g in /sys/devices/system/cpu/cpu[0-3]/cpufreq/scaling_governor; do echo $G > $g; done
    echo "pin $1: gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor) cur=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)"
}
mkdir -p "$X/out"; touch "$X/marker"
if [ -n "$PIN" ]; then pin on >> "$X/out/session.info"; fi
equiv_run_all
if [ -n "$PIN" ]; then pin off >> "$X/out/session.info"; fi
pkill -x mupen64plus 2>/dev/null
{ echo "outside-writes check (newer than marker, outside $X):"
  find /storage/.local /storage/.config /storage/roms /storage/.cache -xdev -newer "$X/marker" -not -path "$X/*" 2>/dev/null | head -20
  echo "(end of list)"; } >> "$X/out/session.info"
