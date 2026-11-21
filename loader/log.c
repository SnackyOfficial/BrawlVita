#include "log.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <string.h>

int bv_trace_level = 1;

static SceUID g_fd = -1;
static SceUID g_mutex = -1;

void bv_log_init(const char *path) {
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(BV_DATA_DIR, 0777);
    /* update 53: keep the logs of the last three runs (log_1.txt = the previous run). Run 39: the user restarted the game after the battle crash and
     * the battle's log.txt was overwritten (the crash dump was an unfinished .tmp), so the crash had no evidence at all. */
    {
        char a[160], b[160];
        size_t L = strlen(path);
        if (L > 4 && L < sizeof a - 8 && !strcmp(path + L - 4, ".txt")) {
            snprintf(a, sizeof a, "%.*s_3.txt", (int)(L - 4), path);
            sceIoRemove(a);
            for (int k = 2; k >= 1; k--) {
                snprintf(a, sizeof a, "%.*s_%d.txt", (int)(L - 4), path, k);
                snprintf(b, sizeof b, "%.*s_%d.txt", (int)(L - 4), path, k + 1);
                sceIoRename(a, b);
            }
            snprintf(b, sizeof b, "%.*s_1.txt", (int)(L - 4), path);
            sceIoRename(path, b);
        }
    }
    g_fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    g_mutex = sceKernelCreateMutex("bv_log", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
}

void bv_logv(const char *fmt, va_list ap) {
    char buf[1024];
    uint64_t t = sceKernelGetProcessTimeWide();
    int n = snprintf(buf, sizeof buf, "[%4u.%03u] ", (unsigned)(t / 1000000u), (unsigned)((t / 1000u) % 1000u));
    if (n < 0) return;
    int m = vsnprintf(buf + n, sizeof buf - (size_t)n - 2, fmt, ap);
    if (m < 0) return;
    n += m;
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    if (g_mutex >= 0) sceKernelLockMutex(g_mutex, 1, NULL);
    if (g_fd >= 0) sceIoWrite(g_fd, buf, (SceSize)n);
    if (g_mutex >= 0) sceKernelUnlockMutex(g_mutex, 1);
}

void bv_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    bv_logv(fmt, ap);
    va_end(ap);
}
