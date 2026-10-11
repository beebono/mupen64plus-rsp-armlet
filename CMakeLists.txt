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
    src/plugin.c src/config.c src/hle_glue.c src/dispatch.c src/gfx_ucode.c src/log.c)

add_library(mupen64plus-rsp-armlet MODULE ${armlet_c} ${hle_c} ${prsp_cpp} ${prsp_extra_cpp})
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

# rsp/vu_inline.cpp holds the register-convention VU op bodies the aarch64 JIT copies into its code (x19 = CPUState). The copied
# code must not touch any integer register the JIT keeps live, so every one of them is fixed for that file (x0-x8, x16, x17 stay
# free). Source file properties are directory-scoped, which is why this lives here and not in a subdirectory. Same string as the fork.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    set_source_files_properties("${ARMLET_PRSP_DIR}/rsp/vu_inline.cpp" PROPERTIES COMPILE_FLAGS
        "-ffixed-x9 -ffixed-x10 -ffixed-x11 -ffixed-x12 -ffixed-x13 -ffixed-x14 -ffixed-x15 -ffixed-x18 -ffixed-x20 -ffixed-x21 -ffixed-x22 -ffixed-x23 -ffixed-x24 -ffixed-x25 -ffixed-x26 -ffixed-x27 -ffixed-x28")
endif()

# Only the six plugin API symbols leave the module; Lightning's _emit_* and everything else stay private.
target_link_options(mupen64plus-rsp-armlet PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/armlet_api_export.ver" -Wl,-Bsymbolic -Wl,--exclude-libs,ALL)
target_link_libraries(mupen64plus-rsp-armlet PRIVATE lightning-m64p dl m)
