#!/bin/sh
# Sourced by the flavour scripts of tools/equiv/device (rgdsplus.sh, rg35xxsp_functions.sh) and by desk.sh: the profiling session runner.
# Runs the lines of $X/runs.txt one after the other.
#   runs.txt line:  TAG|ROM|SO|FRAMES|ENV      (SO: armlet, armlet-prof, ..., ref, or an absolute path to any RSP plugin, e.g. the CFW's cxd4)
#     ROM    a file name in $ROMDIR
#     SO     armlet | armlet-prof | armlet-prof3 | armlet-shadow   (a plugin in $X/plug/<SO>.so, run behind rsp-shim.so),
#            or ref (rsp-hle + parallel-rsp as its RspFallback, $X/plug/ref/, the pairing arMLEt replaces)
#     FRAMES comma-separated VI frames (an entry may be a range a-b or a-b/step): a screenshot (FOIR_SHOT) is taken at each; the profiler snapshots its counters and the shim logs a
#            MARK line at each; the run ends at the last one (the window of a measurement is the difference of two marks)
#     ENV    VAR=value pairs joined with '+', exported for the run (ARMLET_PROF=1, ARMLET_PROF_WALK=1, ...). Handled by this script:
#            IDLE=1 (no pad input at all), HASH=1 (the shim's per-call RSP state hash stream, rsp.hash), JITDUMP=1 (parallel-rsp's code image, jit.dump), SHADOW=1 (for the --shadow build: its summary, shadow.txt), FRAMELOG=1 (foir built with -DFOIR_FRAMELOG: its per-frame record, framelog.bin; kept on a tmpfs during the run), KEEPSHOTS=1 (keep the screenshots), STATE=<file> (--savestate at startup), BIN=<path> (run that static bundle instead of $MUPEN + plugins: no shim, the screenshot files are the marks), TMO=<seconds> (the run's timeout), PERF=<seconds> (rgdsplus: perf record the process from the
#            first mark for that long; PERFG=1 adds call chains (-g); ARGS=<words> (extra mupen64plus arguments, e.g. --set for another RSP plugin); EMU=<0|1|2> (the core's --emumode, default 2); perf.data is turned into perf.script.gz: tid, ip, symbol, dso; the device's busybox has gzip, not xz)
# Needs: X (work dir: plug/ out/ cfg/ runs.txt), MUPEN (emulator command incl. core options), ROMDIR. Optional: VIDARGS, WRAP (a command
# prefix, e.g. a memory-capped scope), GAP (seconds between runs), KILLALL=1 (a device with one user: also pkill every mupen64plus).
# Plugins in $X/plug: mupen64plus-rdp-foir.so rsp-shim.so input-det.so armlet*.so

session_run() {
    TAG=$1; ROM=$2; SO=$3; FRAMES=$4; ENVS=$5
    P=$X/plug; OUT=$X/out/$TAG
    rm -rf "$OUT"; mkdir -p "$OUT"; echo "$FRAMES" > "$OUT/frames"
    rm -rf "$X/cfg" "$X/home/mupen64plus" "$X/home/.local"; mkdir -p "$X/cfg"
    unset ARMLET_GFX ARMLET_PROF ARMLET_PROF_OUT ARMLET_PROF_MARKS ARMLET_PROF_WALK ARMLET_PROF_TASKLOG ARMLET_PROF_PARSE ARMLET_PROF_CB \
          ARMLET_RDP_COALESCE ARMLET_RDP_COALESCE_BYTES ARMLET_RDP_STATS FOIR_FRAMELOG RSPT_STATE \
          RSPT_HASH RSPT_MARK ADUMP_FILE INP_DET_IDLE PRSP_DUMP_JIT PRSP_NO_IMEM_SKIP PRSP_NO_DIV_INLINE PRSP_NO_VU_FAST
    PERF= ; TMO= ; KEEPSHOTS= ; FLOG= ; STATE= ; BIN= ; PERFG= ; EXTRA= ; EMU=2
    OLDIFS=$IFS; IFS='+'
    for kv in $ENVS; do
        case $kv in
        PERF=*) PERF=${kv#PERF=} ;;
        TMO=*) TMO=${kv#TMO=} ;;
        IDLE=1) export INP_DET_IDLE=1 ;;
        HASH=1) export RSPT_HASH=$OUT/rsp.hash ;;
        JITDUMP=1) export PRSP_DUMP_JIT=$OUT/jit.dump ;;
        SHADOW=1) export PRSP_SHADOW_OUT=$OUT/shadow.txt ;;
        KEEPSHOTS=1) KEEPSHOTS=1 ;;
        STATE=*) STATE=${kv#STATE=} ;;
        BIN=*) BIN=${kv#BIN=} ;;
        PERFG=1) PERFG=-g ;;
        ARGS=*) EXTRA=${kv#ARGS=} ;;
        EMU=*) EMU=${kv#EMU=} ;;
        FRAMELOG=1) FLOG=${TMPDIR:-/tmp}/armlet-framelog.$$; export FOIR_FRAMELOG=$FLOG ;;
        ?*=*) export "$kv" ;;
        esac
    done
    IFS=$OLDIFS
    RSPARGS=
    if [ "$SO" = ref ]; then
        export RSPT_TARGET=$P/ref/mupen64plus-rsp-hle.so
        RSPARGS="--set rsp-hle[DisplayListToGraphicsPlugin]=False --set rsp-hle[AudioListToAudioPlugin]=False --set rsp-hle[RspFallback]=$P/ref/mupen64plus-rsp-parallel.so"
    elif [ "${SO#/}" != "$SO" ]; then
        export RSPT_TARGET=$SO                    # an absolute path: any RSP plugin (e.g. the CFW's cxd4)
    else
        export RSPT_TARGET=$P/$SO.so
    fi
    export ARMLET_LOGFILE=$OUT/armlet.log FOIR_THREADS=3 FOIR_SHOT_DIR=$OUT FOIR_LOGFILE=$OUT/foir.log RSPT_LOG=$OUT/rspt
    export FOIR_SHOT_FRAMES=$FRAMES
    MARKS=; LASTF=; NMARKS=0
    OLDIFS=$IFS; IFS=','
    for f in $FRAMES; do
        case $f in                    # a-b[/step] (foir's range form): the marks are its frames
            *-*) a=${f%%-*}; b=${f#*-}; st=1; case $b in */*) st=${b#*/}; b=${b%%/*} ;; esac
                 f=$(seq -s ',' "$a" "$st" "$b") ;;
        esac
        for k in $f; do LASTF=$OUT/frame_$(printf %05d "$k").ppm; MARKS=$MARKS${MARKS:+,}$LASTF; NMARKS=$((NMARKS + 1)); done
    done
    IFS=$OLDIFS
    export RSPT_MARK=$MARKS
    case $SO in armlet-prof*) export ARMLET_PROF_OUT=$OUT/prof ARMLET_PROF_MARKS=$MARKS ;; esac
    LAST=${FRAMES##*,}; LAST=${LAST##*-}; LAST=${LAST%%/*}
    [ -n "$TMO" ] || TMO=$((LAST / 30 + 60))
    {
        echo "== $TAG $SO $ROM frames $FRAMES env '$ENVS' $(date +%T) temp $(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null) freq $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null) gov $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null) load $(cut -d' ' -f1 /proc/loadavg)"
    } >> "$X/out/session.info"
    # shellcheck disable=SC2086
    if [ -n "$BIN" ]; then
        # a static bundle (its own core and plugins; no shim, so the marks are the screenshot files)
        $WRAP timeout -s KILL "$TMO" $BIN --configdir "$X/cfg" --noosd --set "Core[RandomizeInterrupt]=False" --emumode $EMU $VIDARGS \
            ${STATE:+--savestate "$STATE"} "$ROMDIR/$ROM" > "$OUT/log" 2>&1 &
    else
    $WRAP timeout -s KILL "$TMO" $MUPEN --configdir "$X/cfg" --plugindir "$P" --gfx "$P/mupen64plus-rdp-foir.so" --rsp "$P/rsp-shim.so" $RSPARGS $EXTRA \
        --audio dummy --input "$P/input-det.so" --noosd --set "Core[RandomizeInterrupt]=False" --emumode $EMU $VIDARGS ${STATE:+--savestate "$STATE"} "$ROMDIR/$ROM" > "$OUT/log" 2>&1 &
    fi
    MP=$!
    i=0; status=timeout; perf_done=
    while [ $i -lt $((TMO * 2)) ]; do
        sleep 0.5; i=$((i + 1))
        if [ -n "$BIN" ]; then n=$(ls "$OUT"/frame_*.ppm 2>/dev/null | wc -l); else n=$(grep -c '^MARK' "$OUT/rspt" 2>/dev/null); fi
        if [ -n "$PERF" ] && [ -z "$perf_done" ] && [ "${n:-0}" -ge 1 ]; then
            perf_done=1
            PID=$(pgrep -x mupen64plus | head -1)
            if [ -n "$PID" ]; then
                cp /proc/$PID/maps "$OUT/maps" 2>/dev/null
                perf record -q -F 997 $PERFG -p "$PID" -o "$OUT/perf.data" -- sleep "$PERF" > "$OUT/perf.out" 2>&1
                cp /proc/$PID/maps "$OUT/maps2" 2>/dev/null
                status=perf
                break
            fi
        fi
        [ "${n:-0}" -ge "$NMARKS" ] && { sleep 1; status=ok; break; }
        kill -0 $MP 2>/dev/null || { status=exited; break; }
    done
    echo "$status after $((i / 2))s" > "$OUT/status"
    pkill -KILL -P $MP 2>/dev/null; kill -KILL $MP 2>/dev/null
    [ -n "$KILLALL" ] && pkill -KILL -x mupen64plus 2>/dev/null
    wait $MP 2>/dev/null
    (
        cd "$OUT" || exit
        [ -n "$FLOG" ] && [ -f "$FLOG" ] && mv "$FLOG" framelog.bin
        md5sum frame_*.ppm > shots.md5 2>/dev/null
        [ -n "$KEEPSHOTS" ] || rm -f frame_*.ppm
        if [ -f perf.data ]; then
            perf script --no-inline -i perf.data -F tid,ip,sym,dso 2>/dev/null | gzip -1 > perf.script.gz        # (--no-inline: without it perf script needs addr2line for the chains, which the device lacks) with PERFG: one line per frame of the call chain, a blank line after each sample
            [ -s perf.script.gz ] && rm -f perf.data
        fi
    )
    echo "   $TAG: $(cat "$OUT/status") end temp $(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null) freq $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null) out $(du -sk "$OUT" | cut -f1) KB" >> "$X/out/session.info"
    sleep "${GAP:-2}"
}

# the name the flavour scripts of tools/equiv/device call
equiv_run_all() {
    mkdir -p "$X/out" "$X/cfg"
    if [ -n "$KILLALL" ] && pgrep -x mupen64plus > /dev/null; then echo "device busy: mupen64plus running" >> "$X/out/session.info"; return 3; fi
    while IFS='|' read -r t r s f e; do
        [ -n "$t" ] || continue
        session_run "$t" "$r" "$s" "$f" "$e"
    done < "$X/runs.txt"
}
