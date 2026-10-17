cmake_minimum_required(VERSION 3.16)
project(mupen64plus-rsp-armlet VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
# Keep -O3 and generic armv8-a: no -mcpu / -march, no -flto (it does not mix with the -ffixed-* file below).
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(M64P_API_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mupen64plus-core/src/api" CACHE PATH
    "Directory with m64p_types.h / m64p_plugin.h")
option(ARMLET_PRSP_PROFILE "parallel-rsp dynamic instruction-mix profiling (-DPRSP_PROFILE)" OFF)
option(ARMLET_PROFILE "the profiler (src/prof, docs/profiling.md): per-task stats, the JIT address map; with ARMLET_PRSP_PROFILE also per-site counts" OFF)
option(ARMLET_NATIVE "native microcode routines (src/native; needs the fork's prsp_native.h hook, -DPRSP_NATIVE); inert unless GfxNative is on" ON)
set(ARMLET_NATIVE_MUTATE 0 CACHE STRING "test builds only: a deliberate error in the test.dma routine (src/native/test_dma.c), to show that verify catches it")
option(ARMLET_RDP_DEFER "coalescing of the RDP list calls (needs the fork's prsp_rdp_defer.h hooks, -DPRSP_RDP_DEFER); OFF builds the module without it" ON)
set(ARMLET_RDP_MUTATE 0 CACHE STRING "test builds only: drop safeguard N of the RDP-list coalescing (src/rdp_defer.c)")
set(ARMLET_HLE_MUTATE 0 CACHE STRING "test builds only: a deliberate error N in the task-level native path (MUT(n, ...) in src/hle/f3dex2_fx.c and hle_f3dex2.c; tools/hle/mutcheck.sh)")
option(ARMLET_HLE_SCALAR "the task-level native path (src/hle) without NEON on aarch64: the scalar reference forms (-U__ARM_NEON)" OFF)
set(ARMLET_F3D_MUTATE 0 CACHE STRING "test builds only: a deliberate error N in the F3D family's struct-level functions (F3D_FX_MUTATE in src/hle/f3d_*_fx.c; the Rare front-end uses them)")
set(ARMLET_HLE_FX_MUTATE 0 CACHE STRING "test builds only: a deliberate error N in the shared fixed-point library (HLE_FX_MUT in src/hle/hle_fx.h)")
option(ARMLET_PRSP_SHADOW "parallel-rsp shadow verification: every task is replayed on a second JIT (-DPRSP_SHADOW)" OFF)

set(ARMLET_PRSP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ext/parallel-rsp")
set(ARMLET_HLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ext/mupen64plus-rsp-hle")
foreach(d "${ARMLET_PRSP_DIR}/CMakeLists.txt" "${ARMLET_HLE_DIR}/src/hle.c")
    if(NOT EXISTS "${d}")
        message(FATAL_ERROR "${d} is missing: run 'git submodule update --init' (non-recursive)")
    endif()
endforeach()
if(NOT EXISTS "${M64P_API_INCLUDE_DIR}/m64p_types.h")
    message(FATAL_ERROR "m64p_types.h not found in M64P_API_INCLUDE_DIR=${M64P_API_INCLUDE_DIR}")
endif()

include(cmake/prsp_sources.cmake)
include(cmake/hle_sources.cmake)
include(cmake/check_sources.cmake)

# --- GNU Lightning (the fork's copy), static and private to the module
list(TRANSFORM ARMLET_LIGHTNING_C PREPEND "${ARMLET_PRSP_DIR}/")
add_library(lightning-m64p STATIC ${ARMLET_LIGHTNING_C})
target_include_directories(lightning-m64p PUBLIC "${ARMLET_PRSP_DIR}/lightning/include")
target_compile_definitions(lightning-m64p PUBLIC HAVE_MMAP=1)
set_target_properties(lightning-m64p PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)

# --- the module
list(TRANSFORM ARMLET_PRSP_CPP PREPEND "${ARMLET_PRSP_DIR}/" OUTPUT_VARIABLE prsp_cpp)
list(TRANSFORM ARMLET_PRSP_EXTRA_CPP PREPEND "${ARMLET_PRSP_DIR}/" OUTPUT_VARIABLE prsp_extra_cpp)
list(TRANSFORM ARMLET_HLE_C PREPEND "${ARMLET_HLE_DIR}/src/" OUTPUT_VARIABLE hle_c)
set(armlet_c
    src/plugin.c src/config.c src/hle_glue.c src/dispatch.c src/ustats.c src/gfx_ucode.c src/log.c src/rdp_defer.c)
file(GLOB armlet_hle_src CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/hle/*.c")     # the task-level native path (src/hle)
file(GLOB armlet_hle_cpp CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/hle/*.cpp")
list(APPEND armlet_c ${armlet_hle_src})

if(ARMLET_NATIVE)
    file(GLOB armlet_native_c CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/native/*.c")
    file(GLOB armlet_native_cpp CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/native/*.cpp")
    list(APPEND armlet_c ${armlet_native_c})
endif()

set(armlet_cpp ${armlet_native_cpp} ${armlet_hle_cpp})
if(ARMLET_PROFILE)
    list(APPEND armlet_c src/prof/prof_glue.c)
    list(APPEND armlet_cpp src/prof/prof.cpp src/prof/rdp.cpp src/prof/walker.cpp src/prof/tasklog.cpp src/prof/map.cpp src/prof/counts.cpp src/prof/ucode.cpp)
endif()

add_library(mupen64plus-rsp-armlet MODULE ${armlet_c} ${armlet_cpp} ${hle_c} ${prsp_cpp} ${prsp_extra_cpp})
set_target_properties(mupen64plus-rsp-armlet PROPERTIES
    PREFIX ""
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    LINKER_LANGUAGE CXX
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/armlet_api_export.ver")

# The include directories are per language: rsp-hle has headers named memory.h / common.h / audio.h that must not shadow
# anything for the C++ side (and parallel-rsp's directories are of no use to C).
target_include_directories(mupen64plus-rsp-armlet PRIVATE
    "${M64P_API_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "$<$<COMPILE_LANGUAGE:C>:${ARMLET_HLE_DIR}/src>"
    "$<$<COMPILE_LANGUAGE:CXX>:${ARMLET_PRSP_DIR}>"
    "$<$<COMPILE_LANGUAGE:CXX>:${ARMLET_PRSP_DIR}/arch/simd/rsp>")

# parallel-rsp: PARALLEL_INTEGRATION selects real SP DMA / RDP list handling in cp0.cpp, the release JIT has no DEBUG_JIT.
target_compile_definitions(mupen64plus-rsp-armlet PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:PARALLEL_INTEGRATION;M64P_PLUGIN_API;PARALLEL_RSP_LIGHTNING>")
if(ARMLET_RDP_DEFER)
    target_compile_definitions(mupen64plus-rsp-armlet PRIVATE ARMLET_RDP_DEFER "$<$<COMPILE_LANGUAGE:CXX>:PRSP_RDP_DEFER>")
endif()
if(ARMLET_NATIVE_MUTATE)
    set_source_files_properties(src/native/test_dma.c PROPERTIES COMPILE_DEFINITIONS ARMLET_NATIVE_MUTATE=${ARMLET_NATIVE_MUTATE})
endif()
if(ARMLET_NATIVE)
    target_compile_definitions(mupen64plus-rsp-armlet PRIVATE ARMLET_NATIVE "$<$<COMPILE_LANGUAGE:CXX>:PRSP_NATIVE>")
    target_include_directories(mupen64plus-rsp-armlet PRIVATE "$<$<COMPILE_LANGUAGE:C>:${ARMLET_PRSP_DIR}>")
endif()
if(ARMLET_RDP_MUTATE)
    target_compile_definitions(mupen64plus-rsp-armlet PRIVATE ARMLET_RDP_MUTATE=${ARMLET_RDP_MUTATE})
endif()
if(ARMLET_HLE_MUTATE)
    target_compile_definitions(mupen64plus-rsp-armlet PRIVATE ARMLET_HLE_MUTATE=${ARMLET_HLE_MUTATE})
endif()
if(ARMLET_HLE_FX_MUTATE)
    target_compile_definitions(mupen64plus-rsp-armlet PRIVATE HLE_FX_MUTATE=${ARMLET_HLE_FX_MUTATE})
endif()
if(ARMLET_F3D_MUTATE)
    target_compile_definitions(mupen64plus-rsp-armlet PRIVATE F3D_FX_MUTATE=${ARMLET_F3D_MUTATE})
endif()
if(ARMLET_PROFILE)
    # PRSP_HOOKS turns on the observation hooks of the fork's prsp_hooks.h; the profiler's C++ files see the same definition
    target_compile_definitions(mupen64plus-rsp-armlet PRIVATE ARMLET_PROFILE "$<$<COMPILE_LANGUAGE:CXX>:PRSP_HOOKS>")
    target_include_directories(mupen64plus-rsp-armlet PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/src/prof>")
endif()
if(ARMLET_PRSP_PROFILE)
    target_compile_definitions(mupen64plus-rsp-armlet PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:PRSP_PROFILE>")
endif()
if(ARMLET_PRSP_SHADOW)
    target_compile_definitions(mupen64plus-rsp-armlet PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:PRSP_SHADOW>")
endif()
target_compile_options(mupen64plus-rsp-armlet PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wno-missing-field-initializers;-Wno-empty-body;-ffast-math;-Wno-unused-parameter>"
    "$<$<COMPILE_LANGUAGE:C>:-Wall>")
# rsp-hle is built as upstream does (-ffast-math, -Wall); armlet's own code is held to -Wextra.
set_source_files_properties(${hle_c} PROPERTIES COMPILE_OPTIONS "-ffast-math")
set_source_files_properties(${armlet_c} PROPERTIES COMPILE_OPTIONS "-Wextra;-Wno-unused-parameter")
if(ARMLET_HLE_SCALAR)
    set_property(SOURCE ${armlet_hle_src} APPEND PROPERTY COMPILE_OPTIONS "-U__ARM_NEON")
endif()

# rsp/vu_inline.cpp holds the register-convention VU op bodies the aarch64 JIT copies into its code (x19 = CPUState). The copied
# code must not touch any integer register the JIT keeps live, so every one of them is fixed for that file (x0-x8, x16, x17 stay
# free). Source file properties are directory-scoped, which is why this lives here and not in a subdirectory. Same string as the fork.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    set_source_files_properties("${ARMLET_PRSP_DIR}/rsp/vu_inline.cpp" PROPERTIES COMPILE_FLAGS
        "-ffixed-x9 -ffixed-x10 -ffixed-x11 -ffixed-x12 -ffixed-x13 -ffixed-x14 -ffixed-x15 -ffixed-x18 -ffixed-x20 -ffixed-x21 -ffixed-x22 -ffixed-x23 -ffixed-x24 -ffixed-x25 -ffixed-x26 -ffixed-x27 -ffixed-x28")
endif()

# Offline harness for native routines (tools/native/replay.c): the same sources, linked as a program
option(ARMLET_NATIVE_TOOLS "build the offline native-routine harness armlet-native-replay (needs ARMLET_NATIVE)" OFF)
if(ARMLET_NATIVE_TOOLS AND ARMLET_NATIVE)
    add_executable(armlet-native-replay tools/native/replay.c src/ustats.c src/gfx_ucode.c src/log.c src/rdp_defer.c ${armlet_hle_src} ${armlet_hle_cpp} ${armlet_native_c} ${armlet_native_cpp} ${prsp_cpp} ${prsp_extra_cpp})
    set_target_properties(armlet-native-replay PROPERTIES LINKER_LANGUAGE CXX)
    target_include_directories(armlet-native-replay PRIVATE
        "${M64P_API_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "$<$<COMPILE_LANGUAGE:C>:${ARMLET_HLE_DIR}/src>" "$<$<COMPILE_LANGUAGE:C>:${ARMLET_PRSP_DIR}>"
        "$<$<COMPILE_LANGUAGE:CXX>:${ARMLET_PRSP_DIR}>" "$<$<COMPILE_LANGUAGE:CXX>:${ARMLET_PRSP_DIR}/arch/simd/rsp>")
    target_compile_definitions(armlet-native-replay PRIVATE ARMLET_NATIVE "$<$<COMPILE_LANGUAGE:CXX>:PRSP_NATIVE;PARALLEL_INTEGRATION;M64P_PLUGIN_API;PARALLEL_RSP_LIGHTNING>")
    if(ARMLET_HLE_MUTATE)
        target_compile_definitions(armlet-native-replay PRIVATE ARMLET_HLE_MUTATE=${ARMLET_HLE_MUTATE})
    endif()
    if(ARMLET_HLE_FX_MUTATE)
        target_compile_definitions(armlet-native-replay PRIVATE HLE_FX_MUTATE=${ARMLET_HLE_FX_MUTATE})
    endif()
    if(ARMLET_F3D_MUTATE)
        target_compile_definitions(armlet-native-replay PRIVATE F3D_FX_MUTATE=${ARMLET_F3D_MUTATE})
    endif()
    target_compile_options(armlet-native-replay PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wno-missing-field-initializers;-Wno-empty-body;-ffast-math;-Wno-unused-parameter;-Wno-parentheses>" "$<$<COMPILE_LANGUAGE:C>:-Wall>")
    target_link_libraries(armlet-native-replay PRIVATE lightning-m64p dl m)

    # the probe on dumps of ucode texts, without a game (tools/hle/probe_replay.c): derivation, canned tasks, JIT and front-end in a private world
    add_executable(armlet-probe-replay tools/hle/probe_replay.c src/ustats.c src/gfx_ucode.c src/log.c src/rdp_defer.c ${armlet_hle_src} ${armlet_hle_cpp} ${armlet_native_c} ${armlet_native_cpp} ${prsp_cpp} ${prsp_extra_cpp})
    set_target_properties(armlet-probe-replay PROPERTIES LINKER_LANGUAGE CXX)
    target_include_directories(armlet-probe-replay PRIVATE
        "${M64P_API_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "$<$<COMPILE_LANGUAGE:C>:${ARMLET_HLE_DIR}/src>" "$<$<COMPILE_LANGUAGE:C>:${ARMLET_PRSP_DIR}>"
        "$<$<COMPILE_LANGUAGE:CXX>:${ARMLET_PRSP_DIR}>" "$<$<COMPILE_LANGUAGE:CXX>:${ARMLET_PRSP_DIR}/arch/simd/rsp>")
    target_compile_definitions(armlet-probe-replay PRIVATE ARMLET_NATIVE "$<$<COMPILE_LANGUAGE:CXX>:PRSP_NATIVE;PARALLEL_INTEGRATION;M64P_PLUGIN_API;PARALLEL_RSP_LIGHTNING>")
    if(ARMLET_HLE_MUTATE)
        target_compile_definitions(armlet-probe-replay PRIVATE ARMLET_HLE_MUTATE=${ARMLET_HLE_MUTATE})
    endif()
    if(ARMLET_HLE_FX_MUTATE)
        target_compile_definitions(armlet-probe-replay PRIVATE HLE_FX_MUTATE=${ARMLET_HLE_FX_MUTATE})
    endif()
    if(ARMLET_F3D_MUTATE)
        target_compile_definitions(armlet-probe-replay PRIVATE F3D_FX_MUTATE=${ARMLET_F3D_MUTATE})
    endif()
    target_compile_options(armlet-probe-replay PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wno-missing-field-initializers;-Wno-empty-body;-ffast-math;-Wno-unused-parameter;-Wno-parentheses>" "$<$<COMPILE_LANGUAGE:C>:-Wall>")
    target_link_libraries(armlet-probe-replay PRIVATE lightning-m64p dl m)

    # instruction-level tracer of a snapshot (tools/native/trace.cpp), C++ only
    add_executable(armlet-native-trace tools/native/trace.cpp ${prsp_cpp} ${prsp_extra_cpp})
    set_target_properties(armlet-native-trace PROPERTIES LINKER_LANGUAGE CXX)
    target_include_directories(armlet-native-trace PRIVATE "${M64P_API_INCLUDE_DIR}" "${ARMLET_PRSP_DIR}" "${ARMLET_PRSP_DIR}/arch/simd/rsp")
    target_compile_definitions(armlet-native-trace PRIVATE PRSP_NATIVE PARALLEL_INTEGRATION M64P_PLUGIN_API PARALLEL_RSP_LIGHTNING)
    target_compile_options(armlet-native-trace PRIVATE -Wall -Wno-missing-field-initializers -Wno-empty-body -ffast-math -Wno-unused-parameter -Wno-parentheses)
    target_link_libraries(armlet-native-trace PRIVATE lightning-m64p dl m)
endif()

# Only the six plugin API symbols leave the module; Lightning's _emit_* and everything else stay private.
target_link_options(mupen64plus-rsp-armlet PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/armlet_api_export.ver" -Wl,-Bsymbolic -Wl,--exclude-libs,ALL)
target_link_libraries(mupen64plus-rsp-armlet PRIVATE lightning-m64p dl m)
