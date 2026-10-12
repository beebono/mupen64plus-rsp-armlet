#!/bin/sh
# Builds and runs the exactness test of src/native/f3dex_vu.h (tools/native-f3dex/vu_test.cpp) against the fork's helpers
# (ext/parallel-rsp: rsp/vfunctions.cpp, ls.cpp, cp2.cpp, reciprocal.cpp, arch/simd/rsp/rsp_core.cpp, compiled in).
#
#   tools/native-f3dex/build_vu_test.sh MODE [vu_test args]
#
#   MODE  x86   native x86_64, scalar backend (the fork helpers use SSE2)
#         a64   aarch64 under qemu-aarch64-static, NEON backend   (aarch64-linux-gnu-g++, sysroot /usr/aarch64-linux-gnu)
#         a64s  aarch64 under qemu, scalar backend (-DVU_FORCE_SCALAR)
#   vu_test args: -n TRIALS per op and E (default 20000)  -s SEED  -o OP[,OP...] (e.g. VMADN,LDV,recip,MFC2,store)  -v
#   env:  VU_OUT=dir   build tree (default /tmp/vu_test)
#         MUT=n        build with -DVU_MUTATE=n (deliberate bug number n in the library; the run must then FAIL)
#         CXXEXTRA     extra compiler flags (e.g. -msse4.1 selects the fork's SSE4.1 code paths on x86)
#         OPT          optimisation level (default -O2; results are the same at -O0, which was run too)
#         TIMEOUT      seconds for the run (default 3600). The run is memory capped (systemd-run, 2G) when systemd-run exists.
# Code-generation check (registers stay in NEON registers): aarch64-linux-gnu-gcc -O3 -std=gnu11 -S -o - tools/native-f3dex/vu_asm_example.c
# Exit status: 0 = all trials equal, 1 = mismatches, 2 = build/usage error.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$here/../..
fork=$root/ext/parallel-rsp
mode=${1:?usage: build_vu_test.sh x86|a64|a64s [vu_test args]}
shift
case $mode in
x86)  CXX=g++;                  arch=x86; RUN=""; DEFS="" ;;
a64)  CXX=aarch64-linux-gnu-g++; arch=a64; RUN="qemu-aarch64-static -L /usr/aarch64-linux-gnu"; DEFS="" ;;
a64s) CXX=aarch64-linux-gnu-g++; arch=a64; RUN="qemu-aarch64-static -L /usr/aarch64-linux-gnu"; DEFS="-DVU_FORCE_SCALAR" ;;
*) echo "unknown mode $mode" >&2; exit 2 ;;
esac
out=${VU_OUT:-/tmp/vu_test}
fk="$out/fork-$arch${CXXEXTRA:+-$(printf %s "$CXXEXTRA$OPT" | cksum | cut -d" " -f1)}"
mkdir -p "$fk"
FLAGS="${OPT:--O2} -std=gnu++17 -w -I$fork -I$fork/arch/simd/rsp $CXXEXTRA"

for f in rsp/vfunctions rsp/ls rsp/cp2 rsp/reciprocal arch/simd/rsp/rsp_core; do
    o="$fk/$(basename $f).o"
    [ "$o" -nt "$fork/$f.cpp" ] || $CXX $FLAGS -c "$fork/$f.cpp" -o "$o"
done
exe="$out/vu_test-$mode${MUT:+-mut$MUT}"
$CXX $FLAGS $DEFS ${MUT:+-DVU_MUTATE=$MUT} "$here/vu_test.cpp" "$fk"/*.o -o "$exe"

cap=""
command -v systemd-run >/dev/null 2>&1 && cap="systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0"
exec $cap timeout -s KILL "${TIMEOUT:-3600}" $RUN "$exe" "$@"
