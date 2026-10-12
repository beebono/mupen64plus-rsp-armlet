#!/bin/sh
# Sourced by the flavour scripts (rgdsplus.sh, rg35xxsp_functions.sh, ../desk.sh). Runs the lines of $X/runs.txt one after the other.
#   runs.txt line:  TAG|BUILD|ROM|KIND|LAST      ROM: a file name in $ROMDIR, or @name for $X/roms/name
#     BUILD ref (rsp-hle + parallel-rsp as RspFallback) | armlet | armlet-hle (armlet with ARMLET_GFX=hle) | armlet-off (armlet with ARMLET_RDP_COALESCE=off;
#           the other armlet builds use the config default, or ARMLET_RDP_COALESCE when the caller's environment has it (desk.sh))
#     KIND  e2e  (RSP hash stream, screenshots at 6 frames up to LAST, AI audio dump; stops after frame LAST)
#           (EQUIV_RDRAM=1 adds a hash of all RDRAM to the per-call RSPT_STATE stream, EQUIV_THREADS=<n> sets FOIR_THREADS, default 3)
#           time (stops at the first RSP call after frame LAST's screenshot; the shim logs RSP ms and CPU ms there)
# Needs: X (work dir: plug/ out/ cfg/ runs.txt), MUPEN (emulator command incl. core options), ROMDIR. Optional: VIDARGS, WRAP (a command
# prefix, e.g. a memory-capped scope), GAP (seconds between runs), KILLALL=1 (a device with one user: also pkill every mupen64plus).
# Plugins in $X/plug: mupen64plus-rdp-foir.so mupen64plus-rsp-armlet.so rsp-shim.so input-det.so audio-adump.so ref/{mupen64plus-rsp-hle,mupen64plus-rsp-parallel}.so

COALESCE0=${ARMLET_RDP_COALESCE-__unset__}      # the caller's setting (desk.sh); unset on the devices

equiv_run() {
    TAG=$1; BUILD=$2; ROM=$3; KIND=$4; LAST=$5
    # BUILD may carry environment for the run: armlet+VAR=value+VAR=value (e.g. armlet+ARMLET_NATIVE=verify+ARMLET_NATIVE_ABORT=1)
    EXTRA=; case $BUILD in *+*) EXTRA=${BUILD#*+}; BUILD=${BUILD%%+*} ;; esac
    P=$X/plug; OUT=$X/out/$TAG
    rm -rf "$OUT"; mkdir -p "$OUT"; echo "$LAST" > "$OUT/last"
    # every run starts from the same state: no save file (a game that finds one boots differently) and a fresh config
    rm -rf "$X/cfg" "$X/home/mupen64plus" "$X/home/.local"; mkdir -p "$X/cfg"
    RSPARGS=
    case $BUILD in
    ref) export RSPT_TARGET=$P/ref/mupen64plus-rsp-hle.so
         RSPARGS="--set rsp-hle[DisplayListToGraphicsPlugin]=False --set rsp-hle[AudioListToAudioPlugin]=False --set rsp-hle[RspFallback]=$P/ref/mupen64plus-rsp-parallel.so" ;;
    armlet|armlet-hle|armlet-off) export RSPT_TARGET=$P/mupen64plus-rsp-armlet.so ;;
    *) echo "$TAG: unknown build $BUILD" >> "$X/out/session.info"; return ;;
    esac
    # DMEM ranges the task-level native path declares dead (tools/equiv/rsp_shim.c): the host stages $X/masks.txt, the reference runs use it too
    if [ -s "$X/masks.txt" ]; then RSPT_MASKS=$(cat "$X/masks.txt"); export RSPT_MASKS; else unset RSPT_MASKS; fi
    unset ARMLET_GFX RSPT_HASH RSPT_STATE RSPT_MARK ADUMP_FILE ARMLET_NATIVE ARMLET_NATIVE_ROUTINES ARMLET_NATIVE_ABORT ARMLET_NATIVE_STATS \
          ARMLET_NATIVE_POISON ARMLET_NATIVE_CAPTURE ARMLET_NATIVE_CAPTURE_MAX ARMLET_NATIVE_CAPTURE_EVERY
    if [ -n "$EXTRA" ]; then
        _oifs=$IFS; IFS=+
        for _kv in $EXTRA; do export "$_kv"; done
        IFS=$_oifs
    fi
    [ "$BUILD" = armlet-hle ] && export ARMLET_GFX=hle
    # the runs of one session share this shell: armlet-off must not leak into the next run
    if [ "$BUILD" = armlet-off ]; then export ARMLET_RDP_COALESCE=off
    elif [ "$COALESCE0" = __unset__ ]; then unset ARMLET_RDP_COALESCE
    else export ARMLET_RDP_COALESCE=$COALESCE0; fi
    export ARMLET_LOGFILE=$OUT/armlet.log FOIR_THREADS=${EQUIV_THREADS:-3} FOIR_SHOT_DIR=$OUT FOIR_LOGFILE=$OUT/foir.log RSPT_LOG=$OUT/rspt
    case $ROM in @*) ROMPATH=$X/roms/${ROM#@} ;; *) ROMPATH=$ROMDIR/$ROM ;; esac
    LASTF=$OUT/frame_$(printf %05d "$LAST").ppm
    if [ "$KIND" = e2e ]; then
        S=$((LAST / 6))
        export FOIR_SHOT_FRAMES=$S,$((S * 2)),$((S * 3)),$((S * 4)),$((S * 5)),$LAST
        export RSPT_HASH=$OUT/rsp.hash RSPT_STATE=$OUT/rsp.state ADUMP_FILE=$OUT/audio.raw
        RSPT_RDRAM=${EQUIV_RDRAM:-0}; export RSPT_RDRAM
        AUD=$P/audio-adump.so; TMO=$((LAST / 40 + 60))
    else
        export FOIR_SHOT_FRAMES=$LAST RSPT_MARK=$LASTF
        AUD=dummy; TMO=$((LAST / 30 + 60))
    fi
    {
        echo "== $TAG $BUILD $KIND $(date +%T) temp $(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null) freq $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null) gov $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null) load $(cut -d' ' -f1 /proc/loadavg)"
    } >> "$X/out/session.info"
    # shellcheck disable=SC2086
    $WRAP timeout -s KILL "$TMO" $MUPEN --configdir "$X/cfg" --plugindir "$P" --gfx "$P/mupen64plus-rdp-foir.so" --rsp "$P/rsp-shim.so" $RSPARGS \
        --audio "$AUD" --input "$P/input-det.so" --noosd --set "Core[RandomizeInterrupt]=False" --emumode 2 $VIDARGS "$ROMPATH" > "$OUT/log" 2>&1 &
    MP=$!
    i=0; status=timeout
    while [ $i -lt $((TMO * 2)) ]; do
        sleep 0.5; i=$((i + 1))
        if [ "$KIND" = e2e ]; then
            [ -f "$LASTF" ] && { sleep 1; status=ok; break; }
        else
            grep -q '^MARK' "$OUT/rspt" 2>/dev/null && { status=ok; break; }
        fi
        kill -0 $MP 2>/dev/null || { status=exited; break; }
    done
    echo "$status after $((i / 2))s" > "$OUT/status"
    pkill -KILL -P $MP 2>/dev/null; kill -KILL $MP 2>/dev/null
    [ -n "$KILLALL" ] && pkill -KILL -x mupen64plus 2>/dev/null
    wait $MP 2>/dev/null
    (
        cd "$OUT" || exit
        md5sum frame_*.ppm > shots.md5 2>/dev/null
        rm -f frame_*.ppm
        [ -f rsp.hash ] && { wc -l < rsp.hash > rsp.hash.lines; md5sum rsp.hash > rsp.hash.md5; }
        [ -f rsp.state ] && md5sum rsp.state > rsp.state.md5
        [ -f audio.raw ] && wc -c < audio.raw > audio.bytes
    )
    echo "   $TAG: $(cat "$OUT/status") end temp $(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null) freq $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null)" >> "$X/out/session.info"
    sleep "${GAP:-2}"
}

equiv_run_all() {
    mkdir -p "$X/out" "$X/cfg"
    if [ -n "$KILLALL" ] && pgrep -x mupen64plus > /dev/null; then echo "device busy: mupen64plus running" >> "$X/out/session.info"; return 3; fi
    while IFS='|' read -r t b r k l; do
        [ -n "$t" ] || continue
        equiv_run "$t" "$b" "$r" "$k" "$l"
    done < "$X/runs.txt"
}
