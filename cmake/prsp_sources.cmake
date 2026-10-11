# parallel-rsp translation units (paths relative to ext/parallel-rsp). Kept in step with the fork's PARALLEL_RSP_SOURCES by
# check_sources.cmake; the plugin entry (m64p_plugin_entry.cpp), the debug JIT and the test drivers are not used.
set(ARMLET_PRSP_CPP
    rsp/vfunctions.cpp
    rsp_jit.cpp
    jit_allocator.cpp
    rsp_disasm.cpp
    rsp/ls.cpp
    rsp/reciprocal.cpp
    rsp/cp0.cpp
    rsp/cp2.cpp
    shadow.cpp
    rsp/vu_inline.cpp
    arch/simd/rsp/rsp_core.cpp)
set(ARMLET_PRSP_EXTRA_CPP parallel.cpp)

set(ARMLET_LIGHTNING_C
    lightning/lib/jit_disasm.c
    lightning/lib/jit_memory.c
    lightning/lib/jit_names.c
    lightning/lib/jit_note.c
    lightning/lib/jit_print.c
    lightning/lib/jit_size.c
    lightning/lib/lightning.c)
