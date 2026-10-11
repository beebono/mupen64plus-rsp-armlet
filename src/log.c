#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "armlet.h"

static void (*g_cb)(void *ctx, int level, const char *msg);
static void *g_ctx;

/* ARMLET_LOGFILE=<path>: messages up to INFO also appended there and flushed, as stdout of a killed run loses its tail */
static void emit(int level, const char *msg)
{
    static FILE *lf;
    static int lf_tried;
    if (!lf_tried) {
        const char *p = getenv("ARMLET_LOGFILE");
        if (p && *p)
            lf = fopen(p, "a");
        lf_tried = 1;
    }
    if (lf && level <= M64MSG_INFO) {
        fprintf(lf, "%d %s\n", level, msg);
        fflush(lf);
    }
    if (g_cb)
        g_cb(g_ctx, level, msg);
}

void armlet_log_set(void (*cb)(void *, int, const char *), void *ctx)
{
    g_cb = cb;
    g_ctx = ctx;
}

void armlet_vlog_raw(int level, const char *fmt, va_list ap)
{
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    emit(level, buf);
}

void armlet_log(int level, const char *fmt, ...)
{
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "arMLEt: ");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
    emit(level, buf);
}
