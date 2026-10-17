#!/bin/bash
# Mutation check of the Rare and Indiana Jones front-ends and of the F3D functions they use (docs/native-custom.md): builds the offline harness with each deliberate error (scripts/build.sh native --tools
# --hmut=N: MUT(n, ...) in src/hle/hle_rare.c and hle_indy.c; --gmut=N: F3D_FX_MUTATE in src/hle/f3d_*_fx.c) and replays
#   rare-cap   captured Diddy Kong Racing / Jet Force Gemini task starts, against the JIT's recorded results,
#   indy-cap   captured Indiana Jones task starts (recorded results),
#   syn-*      synthetic task starts (tools/rare/synth.py: dkr, jfg; tools/rare/synth_indy.py: indy), against a JIT run from the same state (--oracle).
# A mutation is caught when some task differs from the JIT (MISMATCH) or declines that the unmutated build ran (a decline of a task the unmutated build declines is not a catch). Every mutation must be caught.
#   tools/rare/mutcheck.sh CAPTURED_RARE_DIR CAPTURED_INDY_DIR SYN_DKR_DIR SYN_JFG_DIR SYN_INDY_DIR
# Env: HMUTS / GMUTS select mutations (default: 40..60 and 170..201 / 27..41 and 43..46), N (synthetic snapshots per game, default 300), JOBS (parallel mutations, default 4), CHUNK (default 40),
#      X (work dir, default test-output/mut).
R=$(cd "$(dirname "$0")/../.." && pwd)
X=${X:-$R/test-output/mut}
RB=$R/out/native-tools/armlet-native-replay
scope() { systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL "$@"; }

sets() {    # prints "name mode file" lines
    local n=${N:-300} i
    for f in "$CAP"/*909f82e8*.snap "$CAP"/*c2aca403*.snap; do [ -f "$f" ] && echo "rare-cap rec $f"; done
    for f in "$CAPI"/*.snap; do [ -f "$f" ] && echo "indy-cap rec $f"; done
    for i in $(seq 0 $((n - 1))); do
        [ -f "$SD/syn.dkr.$i.snap" ] && echo "syn-dkr ora $SD/syn.dkr.$i.snap"
        [ -f "$SJ/syn.jfg.$i.snap" ] && echo "syn-jfg ora $SJ/syn.jfg.$i.snap"
        [ -f "$SI/syn.indy.$i.snap" ] && echo "syn-indy ora $SI/syn.indy.$i.snap"
    done
}

# run_set BIN OUTFILE: replays every snapshot of sets() with BIN, one line per snapshot: "set file verdict" (verdict: EQ, MISMATCH, DECLINED)
run_set() {
    local bin=$1 out=$2 stop=$3 mode name file chunk=() cname="" cmode=""
    : > "$out"
    flush_chunk() {
        [ ${#chunk[@]} -gt 0 ] || return 0
        local o
        o=$(scope 300 "$bin" --quiet ${cmode:+--oracle} "${chunk[@]}" 2>&1 | grep -v '^arMLEt' | grep -E 'native == JIT|MISMATCH|DECLINED' | sed -E 's/^([^:]+): .*(native == JIT|MISMATCH native vs JIT|DECLINED).*/\1 \2/; s/ native == JIT/ EQ/; s/ MISMATCH native vs JIT/ MISMATCH/')
        while read -r f v; do echo "$cname $f $v" >> "$out"; done <<< "$o"
        local f; for f in "${chunk[@]}"; do grep -q "^$cname $f " "$out" || echo "$cname $f CRASH" >> "$out"; done      # (a build that crashes or hangs on a task has no verdict for it)
        chunk=()
    }
    while read -r name mode file; do
        if [ "$name" != "$cname" ] || [ ${#chunk[@]} -ge ${CHUNK:-40} ]; then
            flush_chunk
            if [ -n "$stop" ] && grep -q ' MISMATCH$\| CRASH$' "$out"; then return 0; fi
        fi
        cname=$name; cmode=; [ "$mode" = ora ] && cmode=1
        chunk+=("$file")
    done < <(sets)
    flush_chunk
}

one() {     # one KIND N
    local kind=$1 n=$2 name=native-tools-${1}mut$2 out nbad=0 ndec=0 sum
    "$R/scripts/build.sh" native --tools --${kind}mut=$n > /dev/null 2>&1 || { echo "mutation --${kind}mut=$n: BUILD FAILED"; return 1; }
    run_set "$R/out/$name/armlet-native-replay" "$X/m.$kind$n.txt" 1
    nbad=$(grep -c ' MISMATCH$\| CRASH$' "$X/m.$kind$n.txt")
    ndec=$(awk 'NR==FNR { if ($3 == "DECLINED") b[$1" "$2] = 1; next } $3 == "DECLINED" && !(($1" "$2) in b) { c++ } END { print c + 0 }' "$X/base.txt" "$X/m.$kind$n.txt")
    sum=$(awk '$3 == "MISMATCH" { c[$1]++ } END { for (k in c) printf "%s:%d ", k, c[k] }' "$X/m.$kind$n.txt")
    if [ "$nbad" -gt 0 ] || [ "$ndec" -gt 0 ]; then echo "mutation --${kind}mut=$n: caught ($nbad mismatches ${sum}, $ndec new declines)"; else echo "mutation --${kind}mut=$n: NOT CAUGHT"; return 1; fi
}

if [ "$1" = --one ]; then shift; CAP=$1 CAPI=$2 SD=$3 SJ=$4 SI=$5; shift 5; one "$@"; exit $?; fi
CAP=$1 CAPI=$2 SD=$3 SJ=$4 SI=$5
[ -d "$CAP" ] && [ -d "$CAPI" ] && [ -d "$SD" ] && [ -d "$SJ" ] || { echo "usage: $0 CAPTURED_RARE_DIR CAPTURED_INDY_DIR SYN_DKR_DIR SYN_JFG_DIR [SYN_INDY_DIR]" >&2; exit 2; }
mkdir -p "$X"
"$R/scripts/build.sh" native --tools > /dev/null 2>&1 || { echo "build failed"; exit 1; }
run_set "$RB" "$X/base.txt" ""
echo "baseline: $(grep -c ' EQ$' "$X/base.txt") equal, $(grep -c ' MISMATCH$' "$X/base.txt") mismatching, $(grep -c ' DECLINED$' "$X/base.txt") declined"
[ "$(grep -c ' MISMATCH$' "$X/base.txt")" = 0 ] || { echo "the unmutated build mismatches"; exit 1; }
export R X N CHUNK
{ for n in ${HMUTS:-$(seq 40 60) $(seq 170 201)}; do echo "h $n"; done; for n in ${GMUTS:-$(seq 27 41) $(seq 43 46)}; do echo "g $n"; done; } |
    xargs -P "${JOBS:-4}" -L1 bash -c '"$0" --one "'"$CAP"'" "'"$CAPI"'" "'"$SD"'" "'"$SJ"'" "'"$SI"'" "$@"' "$0" | sort -V | tee "$X/summary.txt"
! grep -q 'NOT CAUGHT\|FAILED' "$X/summary.txt"
