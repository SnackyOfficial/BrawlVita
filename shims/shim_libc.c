/* libc / bionic shims: pass-throughs that are ABI-compatible, plus the bionic-only symbols, time, dl*, system info. */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <malloc.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <wchar.h>

void bv_sleep_delay(int kind, uint32_t us); /* shims/shim_pthread.c, update 54: timed + accounted sceKernelDelayThread */
#include <wctype.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "../loader/elf_loader.h"
#include "../loader/log.h"
#include "shim.h"
#include "vsys.h"
#include <psp2/kernel/threadmgr.h>
#include <stdarg.h>

extern void *memmem(const void *, size_t, const void *, size_t);
extern size_t strnlen(const char *, size_t);
extern int vasprintf(char **, const char *, va_list);

/* ---- bionic runtime symbols ---- */
uint32_t bv_stack_chk_guard = 0xa5c3e1f7u;
static void w___stack_chk_fail(void) {
    bv_log("FATAL: stack smashing detected");
    sceKernelExitProcess(0);
}
static void w___assert2(const char *file, int line, const char *func, const char *expr) {
    bv_log("FATAL: assertion failed: %s (%s:%d, %s)", expr, file, line, func);
    sceKernelExitProcess(0);
}
static int *w___errno(void) { return __errno(); }
static int w___cxa_atexit(void (*fn)(void *), void *arg, void *dso) {
    (void)fn; (void)arg; (void)dso;
    return 0;
}
static void w___cxa_finalize(void *dso) { (void)dso; }
/* guard byte 0 = initialised, byte 1 = a thread is running the initialiser (bytes 1..3 are unused by the ARM EABI guard) */
static pthread_mutex_t g_guard_lock = PTHREAD_MUTEX_INITIALIZER;
static int w___cxa_guard_acquire(volatile char *g) {
    for (;;) {
        pthread_mutex_lock(&g_guard_lock);
        if (g[0]) {
            pthread_mutex_unlock(&g_guard_lock);
            return 0;
        }
        if (!g[1]) {
            g[1] = 1;
            pthread_mutex_unlock(&g_guard_lock);
            return 1;
        }
        pthread_mutex_unlock(&g_guard_lock);
        bv_sleep_delay(5, 1000);
    }
}
static void w___cxa_guard_release(volatile char *g) {
    pthread_mutex_lock(&g_guard_lock);
    g[0] = 1;
    g[1] = 0;
    pthread_mutex_unlock(&g_guard_lock);
}
static void w___cxa_pure_virtual(void) {
    bv_log("FATAL: pure virtual function called");
    sceKernelExitProcess(0);
}
static void w__ZdlPv(void *p) { free(p); }

static uint32_t w___gnu_Unwind_Find_exidx(uint32_t pc, int *count) {
    so_module *m = bv_module_for_pc(pc);
    if (!m || !m->exidx) {
        *count = 0;
        return 0;
    }
    *count = (int)(m->exidx_size / 8);
    return m->load + m->exidx;
}

static int w___android_log_print(int prio, const char *tag, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    bv_log("android[%d] %s: %s", prio, tag ? tag : "?", buf);
    return 0;
}
static int w___android_log_write(int prio, const char *tag, const char *text) {
    bv_log("android[%d] %s: %s", prio, tag ? tag : "?", text ? text : "");
    return 0;
}
static int w___system_property_get(const char *name, char *value) {
    BV_TRACE(1, "__system_property_get(%s)", name ? name : "?");
    value[0] = 0;
    return 0;
}

/* ---- ARM EABI memory helpers: note the swapped (n, c) order of memset ---- */
static void w___aeabi_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
static void w___aeabi_memmove(void *d, const void *s, size_t n) { memmove(d, s, n); }
static void w___aeabi_memset(void *d, size_t n, int c) { memset(d, c, n); }
static void w___aeabi_memclr(void *d, size_t n) { memset(d, 0, n); }
/* the 4/8-byte aligned variants have the same signature */
#define w___aeabi_memcpy4 w___aeabi_memcpy
#define w___aeabi_memcpy8 w___aeabi_memcpy
#define w___aeabi_memmove4 w___aeabi_memmove
#define w___aeabi_memmove8 w___aeabi_memmove
#define w___aeabi_memset4 w___aeabi_memset
#define w___aeabi_memset8 w___aeabi_memset
#define w___aeabi_memclr4 w___aeabi_memclr
#define w___aeabi_memclr8 w___aeabi_memclr

/* ---- process ---- */
static void w_exit(int code) {
    bv_log("exit(%d) called by the game", code);
    sceKernelExitProcess(code);
}
static void w_abort(void) {
    bv_log("abort() called by the game");
    sceKernelExitProcess(0);
}
#define w__exit w_exit
static int w_getpid(void) { return 1000; }
static int w_raise(int sig) { BV_TRACE(1, "raise(%d)", sig); return 0; }
static int w_sigaction(int sig, const void *act, void *old) { (void)sig; (void)act; (void)old; return 0; }
static int w_sigprocmask(int how, const void *set, void *old) { (void)how; (void)set; (void)old; return 0; }
static long w_syscall(long n, ...) {
    va_list ap;
    va_start(ap, n);
    long a1 = va_arg(ap, long), a2 = va_arg(ap, long), a3 = va_arg(ap, long);
    va_end(ap);
    long ret = -1;
    int err = 0;
    if (bv_vsys_handle(n, a1, a2, a3, (long)(sceKernelGetThreadId() & 0x7fffffff), &ret, &err)) {
        BV_TRACE(1, "syscall(%ld) -> %ld (answered here)", n, ret);
        if (err) errno = err;
        return ret;
    }
    BV_TRACE(1, "syscall(%ld) -> -1", n);
    errno = ENOSYS;
    return -1;
}
static char *w_getenv(const char *name) { (void)name; return NULL; }
static void *w_popen(const char *c, const char *m) { (void)c; (void)m; return NULL; }
static int w_pclose(void *f) { (void)f; return -1; }
/* The game's own code asks for the page size with 39 (the allocator, four sites) and with 40 (the self-check, six times at the start of nOnCreate: it used to get -1
 * here); bionic's header has _SC_PAGESIZE and _SC_PAGE_SIZE next to each other. 96 / 97 = processors, 98 / 99 = pages of memory. */
static long w_sysconf(int name) {
    switch (name) {
    case 0x00: return 131072; /* _SC_ARG_MAX */
    case 0x06: return 100;    /* _SC_CLK_TCK */
    case 0x0b: return 1024;   /* _SC_OPEN_MAX */
    case 39: case 40: case 41: return 4096; /* _SC_PAGESIZE, _SC_PAGE_SIZE */
    case 96: case 97: return 4; /* _SC_NPROCESSORS_CONF / _ONLN */
    case 98: return 49152;    /* _SC_PHYS_PAGES: 192 MB */
    case 99: return 32768;    /* _SC_AVPHYS_PAGES */
    default: BV_TRACE(1, "sysconf(%d) -> -1", name); return -1;
    }
}
static int w_uname(char *u) {
    memset(u, 0, 65 * 6);
    strcpy(u, "Linux");
    strcpy(u + 65, "localhost");
    strcpy(u + 130, "3.18.0");
    strcpy(u + 195, "#1");
    strcpy(u + 260, "armv7l");
    return 0;
}
static int w_strerror_r(int e, char *buf, size_t n) {
    snprintf(buf, n, "%s", strerror(e));
    return 0;
}

/* ---- time: bionic time_t is 32 bits, struct timespec/timeval are two int32 ---- */
static int w_clock_gettime(int clk, int32_t *ts) {
    if (clk == 0) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        ts[0] = (int32_t)tv.tv_sec;
        ts[1] = (int32_t)(tv.tv_usec * 1000);
    } else {
        uint64_t us = sceKernelGetProcessTimeWide();
        ts[0] = (int32_t)(us / 1000000u);
        ts[1] = (int32_t)((us % 1000000u) * 1000u);
    }
    return 0;
}
static int w_gettimeofday(int32_t *tv, void *tz) {
    (void)tz;
    struct timeval t;
    gettimeofday(&t, NULL);
    tv[0] = (int32_t)t.tv_sec;
    tv[1] = (int32_t)t.tv_usec;
    return 0;
}
static int32_t w_time(int32_t *out) {
    int32_t n = (int32_t)time(NULL);
    if (out) *out = n;
    return n;
}
static int32_t w_clock(void) { return (int32_t)sceKernelGetProcessTimeWide(); }
static int w_nanosleep(const int32_t *req, int32_t *rem) {
    (void)rem;
    bv_sleep_delay(1, (uint32_t)(req[0] * 1000000u + req[1] / 1000u));
    return 0;
}
static unsigned w_sleep(unsigned s) {
    bv_sleep_delay(2, s * 1000000u);
    return 0;
}
static int w_usleep(unsigned us) {
    bv_sleep_delay(0, us);
    return 0;
}
static void w_tzset(void) {}
static int32_t bv_timezone;
static char *bv_tzname[2] = {"UTC", "UTC"};

typedef struct {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
} bionic_tm;
static void from_tm(bionic_tm *o, const struct tm *t) {
    memset(o, 0, sizeof *o);
    memcpy(o, t, 9 * sizeof(int));
    o->tm_zone = "UTC";
}
static void *w_localtime_r(const int32_t *t, bionic_tm *out) {
    time_t tt = *t;
    struct tm r;
    if (!localtime_r(&tt, &r)) return NULL;
    from_tm(out, &r);
    return out;
}
static bionic_tm g_tm;
static void *w_gmtime(const int32_t *t) {
    time_t tt = *t;
    struct tm r;
    if (!gmtime_r(&tt, &r)) return NULL;
    from_tm(&g_tm, &r);
    return &g_tm;
}
static size_t w_strftime(char *buf, size_t max, const char *fmt, const bionic_tm *t) {
    struct tm r;
    memset(&r, 0, sizeof r);
    memcpy(&r, t, 9 * sizeof(int));
    return strftime(buf, max, fmt, &r);
}

/* ---- wide printf on bionic FILE streams ---- */
extern uint8_t bv_sF[];
static int w_fwprintf(void *f, const wchar_t *fmt, ...) {
    (void)f; (void)fmt;
    return 0;
}

/* ---- dl* ---- */
static const char *g_dlerror;
static const char *const k_system_libs[] = {"libc.so", "libm.so", "libdl.so", "liblog.so", "libGLESv2.so", "libGLESv3.so",
                                            "libEGL.so", "libandroid.so", "libstdc++.so", "libOpenSLES.so", NULL};
static void *w_dlopen(const char *name, int flags) {
    (void)flags;
    if (!name) return (void *)1;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    so_module *m = bv_module_by_name(base);
    if (m) return m;
    for (int i = 0; k_system_libs[i]; i++)
        if (!strcmp(base, k_system_libs[i])) return (void *)(uintptr_t)(2 + i);
    BV_TRACE(1, "dlopen(%s) -> not found", name);
    g_dlerror = "library not found";
    return NULL;
}
static void *w_dlsym(void *h, const char *sym) {
    uint32_t a = 0;
    if ((uintptr_t)h >= 0x1000) a = so_find_symbol((so_module *)h, sym);
    if (!a) a = bv_shim_lookup(sym);
    {
        static int n;
        if (n < 24) {
            n++;
            BV_TRACE(1, "dlsym(%p, %s) -> 0x%08x", h, sym, (unsigned)a);
        }
    }
    if (!a) {
        BV_TRACE(1, "dlsym(%s) -> not found", sym);
        g_dlerror = "symbol not found";
    }
    return (void *)(uintptr_t)a;
}
static int w_dlclose(void *h) { (void)h; return 0; }
static const char *w_dlerror(void) {
    const char *e = g_dlerror;
    g_dlerror = NULL;
    return e;
}
typedef struct {
    const char *fname;
    void *fbase;
    const char *sname;
    void *saddr;
} dl_info_t;
static int w_dladdr(const void *addr, dl_info_t *info) {
    static int n;
    so_module *m = bv_module_for_pc((uint32_t)(uintptr_t)addr);
    if (n < 24) {
        n++;
        BV_TRACE(1, "dladdr(%p) -> %s", addr, m ? m->name : "not in a game module");
    }
    if (!m) return 0;
    info->fname = m->name;
    info->fbase = (void *)(uintptr_t)m->load;
    info->sname = NULL;
    info->saddr = NULL;
    return 1;
}

const bv_shim bv_shims_libc[] = {
    {"__stack_chk_guard", (const void *)&bv_stack_chk_guard},
    {"timezone", (const void *)&bv_timezone},
    {"tzname", (const void *)bv_tzname},
    SHIM(__stack_chk_fail), SHIM(__assert2), SHIM(__errno), SHIM(__cxa_atexit), SHIM(__cxa_finalize),
    SHIM(__cxa_guard_acquire), SHIM(__cxa_guard_release), SHIM(__cxa_pure_virtual), SHIM(_ZdlPv),
    SHIM(__gnu_Unwind_Find_exidx), SHIM(__android_log_print), SHIM(__android_log_write), SHIM(__system_property_get),
    SHIM(__aeabi_memcpy), SHIM(__aeabi_memcpy4), SHIM(__aeabi_memcpy8), SHIM(__aeabi_memmove), SHIM(__aeabi_memmove4),
    SHIM(__aeabi_memmove8), SHIM(__aeabi_memset), SHIM(__aeabi_memset4), SHIM(__aeabi_memset8), SHIM(__aeabi_memclr),
    SHIM(__aeabi_memclr4), SHIM(__aeabi_memclr8),
    SHIM(exit), SHIM(_exit), SHIM(abort), SHIM(getpid), SHIM(raise), SHIM(sigaction), SHIM(sigprocmask), SHIM(syscall),
    SHIM(getenv), SHIM(popen), SHIM(pclose), SHIM(sysconf), SHIM(uname), SHIM(strerror_r),
    SHIM(clock_gettime), SHIM(gettimeofday), SHIM(time), SHIM(clock), SHIM(nanosleep), SHIM(sleep), SHIM(usleep), SHIM(tzset),
    SHIM(localtime_r), SHIM(gmtime), SHIM(strftime), SHIM(fwprintf),
    SHIM(dlopen), SHIM(dlsym), SHIM(dlclose), SHIM(dlerror), SHIM(dladdr),
    /* ABI-compatible with newlib */
    SHIM_PASS(memchr), SHIM_PASS(memcmp), SHIM_PASS(memcpy), SHIM_PASS(memmove), SHIM_PASS(memset), SHIM_PASS(memmem),
    SHIM_PASS(strcasecmp), SHIM_PASS(strcat), SHIM_PASS(strchr), SHIM_PASS(strcmp), SHIM_PASS(strcoll),
    SHIM_PASS(strcpy), SHIM_PASS(strdup), SHIM_PASS(strlen), SHIM_PASS(strncasecmp), SHIM_PASS(strncat),
    SHIM_PASS(strncmp), SHIM_PASS(strncpy), SHIM_PASS(strnlen), SHIM_PASS(strrchr), SHIM_PASS(strstr),
    SHIM_PASS(strxfrm), SHIM_PASS(strerror), SHIM_PASS(strtol), SHIM_PASS(strtoll), SHIM_PASS(strtoul),
    SHIM_PASS(strtoull), SHIM_PASS(strtoimax), SHIM_PASS(strtoumax), SHIM_PASS(atoi), SHIM_PASS(atoll),
    SHIM_PASS(bsearch), SHIM_PASS(qsort), SHIM_PASS(btowc), SHIM_PASS(wctob), SHIM_PASS(mbrlen), SHIM_PASS(mbstowcs),
    SHIM_PASS(wcscoll), SHIM_PASS(wcsxfrm), SHIM_PASS(isblank), SHIM_PASS(islower), SHIM_PASS(isspace),
    SHIM_PASS(isupper), SHIM_PASS(isxdigit), SHIM_PASS(tolower), SHIM_PASS(toupper), SHIM_PASS(iswalpha),
    SHIM_PASS(iswcntrl), SHIM_PASS(iswdigit), SHIM_PASS(iswlower), SHIM_PASS(iswprint), SHIM_PASS(iswpunct),
    SHIM_PASS(iswspace), SHIM_PASS(iswupper), SHIM_PASS(iswxdigit), SHIM_PASS(towlower), SHIM_PASS(towupper),
    SHIM_PASS(malloc), SHIM_PASS(calloc), SHIM_PASS(realloc), SHIM_PASS(free), SHIM_PASS(memalign),
    SHIM_PASS(srand48), SHIM_PASS(lrand48), SHIM_PASS(snprintf), SHIM_PASS(sprintf), SHIM_PASS(sscanf),
    SHIM_PASS(vsnprintf), SHIM_PASS(vsscanf), SHIM_PASS(vasprintf), SHIM_PASS(setjmp), SHIM_PASS(longjmp),
    SHIM_END};
