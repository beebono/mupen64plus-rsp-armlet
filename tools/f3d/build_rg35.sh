#!/bin/bash
# builds bench_fx for the rg35xxsp with the glibc 2.31 toolchain (runs on both devices): tools/f3d/build_rg35.sh -> test-output/f3d/fxn-rg35/bench_fx
set -e
cd "$(dirname "$0")"; R=$(cd ../.. && pwd); T=$R/../../../resources/toolchains/rg35xxsp/bin; O=$R/test-output/f3d/fxn-rg35; P=$R/ext/parallel-rsp
mkdir -p $O
for f in bench_fx.c $(ls $R/src/hle/f3d*_fx.c); do $T/rg35-gcc -O3 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -I$R/src/hle -I$P -c $f -o $O/$(basename $f .c).o; done
$T/rg35-g++ -O2 -std=c++14 -I$P -c $P/rsp/reciprocal.cpp -o $O/reciprocal.o
$T/rg35-g++ $O/bench_fx.o $(ls $O/f3d*_fx.o) $O/reciprocal.o -o $O/bench_fx
echo built $O/bench_fx
