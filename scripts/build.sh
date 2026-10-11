#!/bin/bash
# Builds mupen64plus-rsp-armlet.so from a copy of the sources (work/<name>/), so a tree that is being edited does not matter.
#   scripts/build.sh native|aarch64|rg35xxsp [--prof|--prof3] [--shadow]  -> out/<target>[-prof|-prof3][-shadow]/mupen64plus-rsp-armlet.so
#   scripts/build.sh ref native|aarch64|rg35xxsp                  -> out/<target>/ref/{mupen64plus-rsp-hle,mupen64plus-rsp-parallel}.so
# native: host compiler; aarch64: aarch64-linux-gnu-* (the rgdsplus' glibc); rg35xxsp: $RG35_TOOLCHAIN (default ../toolchains/rg35xxsp:
# glibc 2.31 sysroot, C with the host GCC 13, C++ with the host g++-12 and a dynamic libstdc++), whose output runs on both devices.
# --prof: the arMLEt profiler, levels 1-2 (-DARMLET_PROFILE, parallel-rsp built with the PRSP_HOOKS observation hooks); --prof3: the same plus
# parallel-rsp's per-instruction profiling hook (-DPRSP_PROFILE) for the level-3 site counts (4-5x slower: counts only).
# --shadow: parallel-rsp's shadow verification (-DPRSP_SHADOW). See docs/profiling.md.
# ref is the equivalence-test reference: rsp-hle (Makefile, -O3, same compiler) with RspFallback = parallel-rsp from the same submodules.
# M64P_API_INCLUDE_DIR may point at the directory holding m64p_types.h (default ../mupen64plus-core/src/api).
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
API=${M64P_API_INCLUDE_DIR:-$R/../mupen64plus-core/src/api}
RG35=${RG35_TOOLCHAIN:-$R/../toolchains/rg35xxsp}
ref=0; [ "$1" = ref ] && { ref=1; shift; }
target=$1; shift || true
prof=0; prof3=0; shadow=0
for a in "$@"; do case $a in --prof) prof=1 ;; --prof3) prof3=1 ;; --shadow) shadow=1 ;; *) echo "unknown option $a" >&2; exit 2 ;; esac; done
name=$target; [ $prof = 1 ] && name=$name-prof; [ $prof3 = 1 ] && name=$name-prof3; [ $shadow = 1 ] && name=$name-shadow
OUT=$R/out/$name; [ $ref = 1 ] && OUT=$R/out/$target/ref
W=$R/work/$name; [ $ref = 1 ] && W=$R/work/ref-$name
case $target in
native)   NM=nm; OBJDUMP=objdump; TC=(); HLE=(CC=cc) ;;
aarch64)  NM=aarch64-linux-gnu-nm; OBJDUMP=aarch64-linux-gnu-objdump
          TC=(-DCMAKE_TOOLCHAIN_FILE="$R/ext/parallel-rsp/toolchain-aarch64-linux-gnu.cmake")
          HLE=(HOST_CPU=aarch64 CROSS_COMPILE=aarch64-linux-gnu-) ;;
rg35xxsp) NM=aarch64-linux-gnu-nm; OBJDUMP=aarch64-linux-gnu-objdump
          TC=(-DCMAKE_TOOLCHAIN_FILE="$RG35/toolchain-rg35xxsp.cmake")
          HLE=(HOST_CPU=aarch64 CROSS_COMPILE=aarch64-linux-gnu- CC="$RG35/bin/rg35-gcc") ;;
*) echo "usage: $0 [ref] native|aarch64|rg35xxsp [--prof|--prof3] [--shadow]" >&2; exit 2 ;;
esac
mkdir -p "$W" "$OUT"
# the submodules' .git files point into this repo's .git, which the copy does not need
rsync -a --delete --exclude '/build*' --exclude /out --exclude /work --exclude /test-output --exclude .git "$R/" "$W/src/"

if [ $ref = 0 ]; then
    flags=()
    [ $prof = 1 ] && flags+=(-DARMLET_PROFILE=ON)
    [ $prof3 = 1 ] && flags+=(-DARMLET_PROFILE=ON -DARMLET_PRSP_PROFILE=ON)
    [ $shadow = 1 ] && flags+=(-DARMLET_PRSP_SHADOW=ON)
    cmake -S "$W/src" -B "$W/build" -G Ninja -DCMAKE_BUILD_TYPE=Release "${TC[@]}" -DM64P_API_INCLUDE_DIR="$API" "${flags[@]}" >/dev/null
    cmake --build "$W/build"
    cp "$W/build/mupen64plus-rsp-armlet.so" "$OUT/"
    libs=(mupen64plus-rsp-armlet.so)
else
    cmake -S "$W/src/ext/parallel-rsp" -B "$W/prsp" -G Ninja -DCMAKE_BUILD_TYPE=Release "${TC[@]}" \
        -DPARALLEL_RSP_M64P_PLUGIN=ON -DPARALLEL_RSP_DEBUG_JIT=OFF -DPARALLEL_RSP_TESTS=OFF -DM64P_API_INCLUDE_DIR="$API" >/dev/null
    cmake --build "$W/prsp" --target mupen64plus-rsp-parallel
    cp "$W/prsp/mupen64plus-rsp-parallel.so" "$OUT/"
    make -C "$W/src/ext/mupen64plus-rsp-hle/projects/unix" all "${HLE[@]}" OPTFLAGS=-O3 APIDIR="$API" -j"$(nproc)"
    cp "$W/src/ext/mupen64plus-rsp-hle/projects/unix/mupen64plus-rsp-hle.so" "$OUT/"
    libs=(mupen64plus-rsp-hle.so mupen64plus-rsp-parallel.so)
fi

for l in "${libs[@]}"; do
    f=$OUT/$l
    printf '%s\n  exports: %s\n' "$f" "$($NM -D --defined-only "$f" | awk '{print $3}' | sort | tr '\n' ' ')"
    glibc=$($OBJDUMP -T "$f" | grep -o 'GLIBC_[0-9.]*' | sort -V | tail -1)
    glibcxx=$($OBJDUMP -T "$f" | grep -o 'GLIBCXX_[0-9.]*' | sort -V | tail -1)
    printf '  needs: %s %s\n' "$glibc" "$glibcxx"
    if [ "$target" = rg35xxsp ] && [ "$(printf '%s\nGLIBC_2.17\n' "$glibc" | sort -V | tail -1)" != GLIBC_2.17 ]; then
        echo "  FAIL: glibc floor above 2.17" >&2; exit 1
    fi
done
