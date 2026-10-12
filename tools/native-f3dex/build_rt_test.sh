#!/bin/bash
# Builds the standalone differential test (rt_test) of src/native/f3dex.c against the fork's JIT.
#   build_rt_test.sh x86|a64|rg35 [outdir]    (rg35: the toolchains/rg35xxsp compilers; the binary runs on both devices)       needs a build of the fork's static library for that target:
#       x86:  cmake <repo>/ext/parallel-rsp -DPARALLEL_RSP_DEBUG_JIT=OFF -DPARALLEL_RSP_TESTS=OFF -DCMAKE_BUILD_TYPE=Release && make parallel-rsp lightning
#       a64:  the same with -DCMAKE_TOOLCHAIN_FILE=<repo>/ext/parallel-rsp/toolchain-aarch64-linux-gnu.cmake
#   PRSP_LIBDIR=<dir with libparallel-rsp.a liblightning.a>  NATIVE_H_DIR=<dir with native.h>  PRSP_NATIVE_H_DIR=<dir with prsp_native.h>
# Run (x86):  systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL 300 out/rt_test --ucode <dir>/<tag> --cases 2000
#      (a64): ... qemu-aarch64-static -L /usr/aarch64-linux-gnu out/rt_test ...        <tag>.text / <tag>.data: ucode text / data from ROM, not in git
set -e
T=${1:-x86}
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../.." && pwd)
O=${2:-$H/out-$T}
: "${PRSP_LIBDIR:?}"
FORK=$R/ext/parallel-rsp
NH=${NATIVE_H_DIR:-$R/src/native}
PH=${PRSP_NATIVE_H_DIR:-$FORK}
TC=$R/../toolchains/rg35xxsp/bin
case $T in x86) CC=gcc; CXX=g++; EXTRA=;; a64) CC=aarch64-linux-gnu-gcc; CXX=aarch64-linux-gnu-g++; EXTRA=;; rg35) CC=$TC/rg35-gcc; CXX=$TC/rg35-g++; EXTRA=;; esac
mkdir -p "$O"
CF="-O2 -g -std=gnu11 -DARMLET_NATIVE_TEST -I$NH -I$PH -I$R/src/native -I$R/src/hle"
$CC $CF -c "$R/src/native/f3dex.c" -o "$O/f3dex.o"
# the struct-level functions (src/hle/f3dex_*_fx.c), compiled for the host; FXFLAGS (e.g. -DF3DEX_FX_MUTATE=3) goes to them
FXOBJ=
for c in "$R"/src/hle/f3dex_*_fx.c; do [ -f "$c" ] && { $CC -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -I"$R/src/hle" $FXFLAGS -c "$c" -o "$O/$(basename "$c" .c).o"; FXOBJ="$FXOBJ $O/$(basename "$c" .c).o"; }; done
# spec routines: tools/native-f3dex/spec/*_spec.c (registered as spec.* routines) with their scenario generators spec/*_gen.inc
: > "$O/spec_gens.inc"
SPECOBJ=
for g in "$H"/spec/${SPEC_ONLY:-*}_gen.inc; do [ -f "$g" ] && echo "#include \"$g\"" >> "$O/spec_gens.inc"; done
for c in "$H"/spec/${SPEC_ONLY:-*}_spec.c; do [ -f "$c" ] && { $CC $CF -I"$H/spec" -c "$c" -o "$O/$(basename "$c" .c).o"; SPECOBJ="$SPECOBJ $O/$(basename "$c" .c).o"; }; done
$CXX -O2 -g -std=gnu++14 -DPARALLEL_RSP_LIGHTNING -DDEBUG_JIT -I"$FORK" -I"$FORK/arch/simd/rsp" -I"$FORK/lightning/include" -I"$NH" -I"$PH" -I"$H" -I"$O" \
    "$H/rt_test.cpp" "$O/f3dex.o" $SPECOBJ $FXOBJ -o "$O/rt_test" -L"$PRSP_LIBDIR" -lparallel-rsp -llightning -ldl -lpthread
echo "built $O/rt_test"
