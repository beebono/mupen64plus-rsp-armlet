#!/bin/sh
# ON rg35xxsp. rg35xxsp_session.sh X : hands over from PyUI to run_exp (rg35xxsp_functions.sh), waits for $X/done, restores lastgame
# and PyUI. Adapted from the play/exp session script. PIN=1 pins the clocks (cpu_perf.sh on/off) around the runs.
X=${1:-/mnt/sdcard/n64-dev/armlet}; FL=/mnt/SDCARD/spruce/flags; MAXWAIT=${MAXWAIT:-420}; export X PIN
if pgrep -x mupen64plus > /dev/null || pgrep -f "retroarch|ra64|ra32" > /dev/null; then echo "device busy"; exit 3; fi
pgrep MainUI > /dev/null || { echo "PyUI not running"; exit 4; }
rm -f "$X/lastgame.orig" "$X/lastgame.none" "$X/done"
if [ -f "$FL/lastgame.lock" ]; then cp -p "$FL/lastgame.lock" "$X/lastgame.orig"; else touch "$X/lastgame.none"; fi
sed -e 's#^emu_name=\${0\#\*/Emu/}.*#emu_name=N64DEV#' -e 's#^emu_name=\${emu_name%%/\*}.*##' \
    -e 's#^\([[:space:]]*\)"N64")#\1"N64"|"N64DEV")#' \
    -e 's#/mnt/SDCARD/spruce/scripts/emu/lib/mupen_functions.sh#'"$X"'/rg35xxsp_functions.sh#' \
    -e 's#if \[ "\$CORE" = "mupen64plus-standalone" \]; then#if true; then#' -e 's#run_mupen_standalone#run_exp#' \
    /mnt/SDCARD/spruce/scripts/emu/standard_launch.sh > "$X/standard_launch_exp.sh"
printf '%s\n' "X=\"$X\" PIN=\"$PIN\" sh \"$X/standard_launch_exp.sh\" \"/mnt/SDCARD/Roms/N64/Super Mario 64.z64\"; touch \"$X/done\"" > /tmp/cmd_to_run.sh
echo "$(date +%T) handing over; before: gov $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor) min $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq) max $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq) temp $(cat /sys/class/thermal/thermal_zone0/temp)"
kill -TERM $(pgrep MainUI)
n=0; while [ ! -f "$X/done" ] && [ $n -lt $MAXWAIT ]; do sleep 1; n=$((n + 1)); done
echo "$(date +%T) done after ${n}s ($([ -f "$X/done" ] && echo yes || echo NO))"
[ -f "$X/done" ] || { pkill -x mupen64plus; sh "$X/cpu_perf.sh" off; }
n=0; while ! pgrep MainUI > /dev/null && [ $n -lt 60 ]; do sleep 1; n=$((n + 1)); done
echo "PyUI back: $(pgrep MainUI)"
if [ -f "$X/lastgame.orig" ]; then cp -p "$X/lastgame.orig" "$FL/lastgame.lock"; else rm -f "$FL/lastgame.lock"; fi
echo "after: gov $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor) min $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq) max $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq) online $(cat /sys/devices/system/cpu/online)"; pgrep -x mupen64plus
