#!/bin/bash
# Builds a plain-C test of the struct-level functions (src/hle/f3dex_*_fx.c), no JIT: tools/f3dex/<name>.c (a main()) linked with all of them.
#   build_fxn.sh <name> [host|a64|rg35] [outdir]       output: <outdir>/<name>   (default outdir tools/f3dex/out-<target>)
#   FXFLAGS="-DF3DEX_FX_MUTATE=3" (mutations), NEON follows the compiler (a64 / rg35: on; host x86_64: the scalar fall-backs)
# a64 binaries run under  qemu-aarch64-static -L /usr/aarch64-linux-gnu ;  rg35 binaries (glibc 2.17 symbols) run on both devices.
set -e
N=$1; T=${2:-host}
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../.." && pwd)
O=${3:-$H/out-$T}
case $T in
 host) CC=gcc; CXX=g++;;
 a64) CC=aarch64-linux-gnu-gcc; CXX=aarch64-linux-gnu-g++;;
 rg35) TC=$R/../toolchains/rg35xxsp/bin; CC=$TC/rg35-gcc; CXX=$TC/rg35-g++;;
esac
mkdir -p "$O/obj-$N"
OBJ=
for c in "$H/$N.c" "$R"/src/hle/f3dex_*_fx.c; do
  o="$O/obj-$N/$(basename "$c" .c).o"
  $CC -O3 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -I"$R/src/hle" -I"$H" $FXFLAGS -c "$c" -o "$o"
  OBJ="$OBJ $o"
done
$CXX -O2 -std=c++14 -I"$R/ext/parallel-rsp" -c "$R/ext/parallel-rsp/rsp/reciprocal.cpp" -o "$O/obj-$N/reciprocal.o"
$CXX $OBJ "$O/obj-$N/reciprocal.o" -o "$O/$N"
echo "built $O/$N"
