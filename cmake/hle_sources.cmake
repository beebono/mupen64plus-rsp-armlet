# rsp-hle translation units (paths relative to ext/mupen64plus-rsp-hle/src), as in its projects/unix/Makefile minus plugin.c
# (replaced by armlet's plugin.c / hle_glue.c) and the osal_dynamiclib_* files. Checked by check_sources.cmake.
set(ARMLET_HLE_C
    alist.c
    alist_audio.c
    alist_naudio.c
    alist_nead.c
    audio.c
    cicx105.c
    hle.c
    hvqm.c
    jpeg.c
    memory.c
    mp3.c
    musyx.c
    re2.c)
