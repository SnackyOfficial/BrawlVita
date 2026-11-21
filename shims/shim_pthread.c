/* pthread / semaphore shims.
 *
 * bionic's pthread_mutex_t / pthread_cond_t / sem_t are 4-byte structs that are statically initialised with small
 * integers (0, 0x4000 recursive, 0x8000 error-checking). The console's pthread-embedded types are pointers (also 4
 * bytes), so the slot of the Android object simply holds the real handle once it has been created.
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

void bv_sleep_delay(int kind, uint32_t us); /* forward (update 54, defined below) */

#include "../loader/elf_loader.h"
#include "../loader/log.h"
#include "shim.h"

#define BIONIC_ETIMEDOUT 110
#define STATIC_MAX 0x10000u /* values below this are bionic static initialisers, not handles */

static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;

static int map_err(int e) { return e == ETIMEDOUT ? BIONIC_ETIMEDOUT : e; }

/* ---- mutex attributes (bionic: a 4-byte int, 0 normal / 1 recursive / 2 error-checking) ---- */
static int w_pthread_mutexattr_init(int *a) { *a = 0; return 0; }
static int w_pthread_mutexattr_destroy(int *a) { (void)a; return 0; }
static int w_pthread_mutexattr_settype(int *a, int type) { *a = type; return 0; }

/* ---- mutex ---- */
static int mutex_real(volatile uint32_t *slot, pthread_mutex_t *out, int type_hint) {
    uint32_t v = *slot;
    if (v >= STATIC_MAX) {
        *out = (pthread_mutex_t)v;
        return 0;
    }
    pthread_mutex_lock(&g_init_lock);
    v = *slot;
    int r = 0;
    if (v < STATIC_MAX) {
        int type = type_hint >= 0 ? type_hint : (v == 0x4000 ? 1 : v == 0x8000 ? 2 : 0);
        pthread_mutexattr_t attr;
        pthread_mutex_t m;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, type == 1 ? PTHREAD_MUTEX_RECURSIVE : type == 2 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL);
        r = pthread_mutex_init(&m, &attr);
        pthread_mutexattr_destroy(&attr);
        if (r == 0) {
            *slot = (uint32_t)m;
            v = (uint32_t)m;
        }
    }
    pthread_mutex_unlock(&g_init_lock);
    *out = (pthread_mutex_t)v;
    return r;
}

static int w_pthread_mutex_init(uint32_t *slot, const int *attr) {
    *slot = 0;
    pthread_mutex_t m;
    return mutex_real(slot, &m, attr ? *attr : 0);
}
static int w_pthread_mutex_lock(uint32_t *slot) {
    pthread_mutex_t m;
    int r = mutex_real(slot, &m, -1);
    return r ? r : pthread_mutex_lock(&m);
}
static int w_pthread_mutex_trylock(uint32_t *slot) {
    pthread_mutex_t m;
    int r = mutex_real(slot, &m, -1);
    return r ? r : pthread_mutex_trylock(&m);
}
static int w_pthread_mutex_unlock(uint32_t *slot) {
    pthread_mutex_t m;
    int r = mutex_real(slot, &m, -1);
    return r ? r : pthread_mutex_unlock(&m);
}
static int w_pthread_mutex_destroy(uint32_t *slot) {
    if (*slot >= STATIC_MAX) {
        pthread_mutex_t m = (pthread_mutex_t)*slot;
        pthread_mutex_destroy(&m);
    }
    *slot = 0;
    return 0;
}

/* ---- condition variables ---- */
/* cycle 6 (ISSUES A2): pte's pthread_cond_destroy = 2 x sem_destroy = 2 ms of sleep; the game destroys ~100 conds/s in battle (run 79).
 * cond_pool=1 keeps destroyed conds and hands them out again (guarded by g_init_lock). */
int bv_cond_pool = 0;
#define COND_POOL 4096
static pthread_cond_t g_cpool[COND_POOL];
static unsigned g_cpool_n;
static volatile unsigned g_cpool_reused, g_cpool_kept, g_cpool_destroyed;
static int cond_real(volatile uint32_t *slot, pthread_cond_t *out) {
    uint32_t v = *slot;
    if (v >= STATIC_MAX) {
        *out = (pthread_cond_t)v;
        return 0;
    }
    pthread_mutex_lock(&g_init_lock);
    v = *slot;
    int r = 0;
    if (v < STATIC_MAX) {
        pthread_cond_t c;
        if (bv_cond_pool && g_cpool_n) { c = g_cpool[--g_cpool_n]; g_cpool_reused++; r = 0; }
        else r = pthread_cond_init(&c, NULL);
        if (r == 0) {
            *slot = (uint32_t)c;
            v = (uint32_t)c;
        }
    }
    pthread_mutex_unlock(&g_init_lock);
    *out = (pthread_cond_t)v;
    return r;
}

static int w_pthread_cond_init(uint32_t *slot, const void *attr) {
    (void)attr;
    *slot = 0;
    pthread_cond_t c;
    return cond_real(slot, &c);
}
static int w_pthread_cond_destroy(uint32_t *slot) {
    if (*slot >= STATIC_MAX) {
        pthread_cond_t c = (pthread_cond_t)*slot;
        int kept = 0;
        if (bv_cond_pool) {
            pthread_mutex_lock(&g_init_lock);
            if (g_cpool_n < COND_POOL) { g_cpool[g_cpool_n++] = c; kept = 1; g_cpool_kept++; }
            pthread_mutex_unlock(&g_init_lock);
        }
        if (!kept) { pthread_cond_destroy(&c); g_cpool_destroyed++; }
    }
    *slot = 0;
    return 0;
}
static int w_pthread_cond_signal(uint32_t *slot) {
    pthread_cond_t c;
    int r = cond_real(slot, &c);
    return r ? r : pthread_cond_signal(&c);
}
static int w_pthread_cond_broadcast(uint32_t *slot) {
    pthread_cond_t c;
    int r = cond_real(slot, &c);
    return r ? r : pthread_cond_broadcast(&c);
}
static int w_pthread_cond_wait(uint32_t *cslot, uint32_t *mslot) {
    pthread_cond_t c;
    pthread_mutex_t m;
    int r = cond_real(cslot, &c);
    if (!r) r = mutex_real(mslot, &m, -1);
    return r ? r : pthread_cond_wait(&c, &m);
}
static int w_pthread_cond_timedwait(uint32_t *cslot, uint32_t *mslot, const int32_t *abs_ts) {
    pthread_cond_t c;
    pthread_mutex_t m;
    int r = cond_real(cslot, &c);
    if (!r) r = mutex_real(mslot, &m, -1);
    if (r) return r;
    struct timespec ts;
    ts.tv_sec = abs_ts[0];
    ts.tv_nsec = abs_ts[1];
    return map_err(pthread_cond_timedwait(&c, &m, &ts));
}

/* ---- pthread_once (bionic: one int, 0 = not run) ---- */
static int w_pthread_once(volatile int *once, void (*fn)(void)) {
    pthread_mutex_lock(&g_init_lock);
    int done = *once;
    if (!done) *once = 1;
    pthread_mutex_unlock(&g_init_lock);
    if (!done) {
        fn();
        *once = 2;
    } else {
        while (*once == 1) bv_sleep_delay(6, 1000); /* another thread is running the initialiser */
    }
    return 0;
}

/* ---- thread attributes (bionic layout, 24 bytes) and pthread_create ---- */
typedef struct {
    uint32_t flags; /* bit 0: detached */
    void *stack_base;
    uint32_t stack_size;
    uint32_t guard_size;
    int32_t sched_policy;
    int32_t sched_priority;
} bionic_attr;

#define DEFAULT_STACK (1024u * 1024u)
/* ---- update 54: sleep accounting (see bv_sleep_delay). Kinds: 0 usleep, 1 nanosleep, 2 sleep, 3 select, 4 poll, 5 cxa_guard, 6 pthread_once, 7 other ---- */
#define BV_SLEEP_KINDS 9
static const char *const k_sleep_names[BV_SLEEP_KINDS] = {"usleep", "nanosleep", "sleep", "select", "poll", "cxa_guard", "pthread_once", "sched_yield", "other"};
static volatile uint32_t g_sleep_us[BV_SLEEP_KINDS], g_sleep_n[BV_SLEEP_KINDS], g_sleep_game_us[BV_SLEEP_KINDS], g_sleep_game_n[BV_SLEEP_KINDS];
static volatile SceUID g_game_uid = -1;
void bv_sleep_delay(int kind, uint32_t us) {
    if (kind < 0 || kind >= BV_SLEEP_KINDS) kind = BV_SLEEP_KINDS - 1;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    sceKernelDelayThread(us);
    uint32_t d = (uint32_t)(sceKernelGetProcessTimeWide() - t0);
    __sync_fetch_and_add(&g_sleep_us[kind], d);
    __sync_fetch_and_add(&g_sleep_n[kind], 1);
    if (sceKernelGetThreadId() == g_game_uid) {
        __sync_fetch_and_add(&g_sleep_game_us[kind], d);
        __sync_fetch_and_add(&g_sleep_game_n[kind], 1);
    }
}

/* cycle 5 (ISSUES A2): every sceKernelDelayThread(CB) of the eboot goes through here (CMakeLists: -Wl,--wrap=...). On the game thread: time
 * and count per caller address (single writer = the game thread; the status thread only reads). */
int __real_sceKernelDelayThread(SceUInt delay);
int __real_sceKernelDelayThreadCB(SceUInt delay);
#define DELAY_SLOTS 16
static volatile uint32_t g_dl_pc[DELAY_SLOTS], g_dl_us[DELAY_SLOTS], g_dl_n[DELAY_SLOTS], g_dl_other_us, g_dl_other_n;
static void delay_note(uint32_t pc, uint32_t us) {
    for (int i = 0; i < DELAY_SLOTS; i++) {
        if (g_dl_pc[i] == pc || g_dl_pc[i] == 0) {
            if (!g_dl_pc[i]) g_dl_pc[i] = pc;
            g_dl_us[i] += us;
            g_dl_n[i]++;
            return;
        }
    }
    g_dl_other_us += us;
    g_dl_other_n++;
}
int __wrap_sceKernelDelayThread(SceUInt delay) {
    if (sceKernelGetThreadId() != g_game_uid) return __real_sceKernelDelayThread(delay);
    uint32_t pc = (uint32_t)__builtin_return_address(0);
    uint64_t t0 = sceKernelGetProcessTimeWide();
    int r = __real_sceKernelDelayThread(delay);
    delay_note(pc, (uint32_t)(sceKernelGetProcessTimeWide() - t0));
    return r;
}
int __wrap_sceKernelDelayThreadCB(SceUInt delay) {
    if (sceKernelGetThreadId() != g_game_uid) return __real_sceKernelDelayThreadCB(delay);
    uint32_t pc = (uint32_t)__builtin_return_address(0) | 1u; /* odd = the CB variant */
    uint64_t t0 = sceKernelGetProcessTimeWide();
    int r = __real_sceKernelDelayThreadCB(delay);
    delay_note(pc, (uint32_t)(sceKernelGetProcessTimeWide() - t0));
    return r;
}
static void delay_report(void) {
    static uint32_t last_us[DELAY_SLOTS], last_n[DELAY_SLOTS], last_ous, last_on;
    char line[400];
    int at = snprintf(line, sizeof line, "    delays (game thread, last second, ms/calls by caller):");
    int any = 0;
    for (int i = 0; i < DELAY_SLOTS && g_dl_pc[i]; i++) {
        uint32_t u = g_dl_us[i] - last_us[i], n = g_dl_n[i] - last_n[i];
        last_us[i] = g_dl_us[i];
        last_n[i] = g_dl_n[i];
        if (n && at < (int)sizeof line - 40) { at += snprintf(line + at, sizeof line - (size_t)at, " 0x%08x %u/%u", (unsigned)g_dl_pc[i], (unsigned)(u / 1000), (unsigned)n); any = 1; }
    }
    uint32_t ou = g_dl_other_us - last_ous, on = g_dl_other_n - last_on;
    last_ous = g_dl_other_us;
    last_on = g_dl_other_n;
    if (on && at < (int)sizeof line - 40) { at += snprintf(line + at, sizeof line - (size_t)at, " other %u/%u", (unsigned)(ou / 1000), (unsigned)on); any = 1; }
    if (any) bv_log("%s", line);
}

void bv_sem_reaper_report(void);
/* status line (loader/gameapp.c), once per second: what slept and how long since the last report; the game main thread separately */
void bv_sleep_report(void) {
    static uint32_t last_us[BV_SLEEP_KINDS], last_n[BV_SLEEP_KINDS], last_gus[BV_SLEEP_KINDS], last_gn[BV_SLEEP_KINDS];
    char line[512];
    int at = snprintf(line, sizeof line, "    sleeps (last second, ms/calls): game thread");
    int any = 0;
    for (int k = 0; k < BV_SLEEP_KINDS; k++) {
        uint32_t gu = g_sleep_game_us[k] - last_gus[k], gn = g_sleep_game_n[k] - last_gn[k];
        last_gus[k] = g_sleep_game_us[k]; last_gn[k] = g_sleep_game_n[k];
        if (gn && at < (int)sizeof line - 40) { at += snprintf(line + at, sizeof line - (size_t)at, " %s %u/%u", k_sleep_names[k], (unsigned)(gu / 1000), (unsigned)gn); any = 1; }
    }
    if (!any && at < (int)sizeof line - 10) at += snprintf(line + at, sizeof line - (size_t)at, " none");
    if (at < (int)sizeof line - 16) at += snprintf(line + at, sizeof line - (size_t)at, " | all threads");
    for (int k = 0; k < BV_SLEEP_KINDS; k++) {
        uint32_t u = g_sleep_us[k] - last_us[k], n = g_sleep_n[k] - last_n[k];
        last_us[k] = g_sleep_us[k]; last_n[k] = g_sleep_n[k];
        if (n && at < (int)sizeof line - 40) at += snprintf(line + at, sizeof line - (size_t)at, " %s %u/%u", k_sleep_names[k], (unsigned)(u / 1000), (unsigned)n);
    }
    bv_log("%s", line);
    delay_report();
    bv_sem_reaper_report();
    if (bv_cond_pool) {
        static unsigned lr, lk;
        if (g_cpool_reused != lr || g_cpool_kept != lk)
            bv_log("    cond pool: %u reused, %u kept in the last second (%u in the pool, %u destroyed because full)", g_cpool_reused - lr, g_cpool_kept - lk,
                   g_cpool_n, g_cpool_destroyed);
        lr = g_cpool_reused;
        lk = g_cpool_kept;
    }
}
int bv_game_stack_kb = 4096; /* config game_stack_kb: stack of the game's main thread libg+0x694008 (update 53); 0 = as the game asks */

static int w_pthread_attr_init(bionic_attr *a) {
    memset(a, 0, sizeof *a);
    a->stack_size = DEFAULT_STACK;
    return 0;
}
static int w_pthread_attr_destroy(bionic_attr *a) { (void)a; return 0; }
static int w_pthread_attr_setstacksize(bionic_attr *a, uint32_t sz) { a->stack_size = sz; return 0; }
static int w_pthread_attr_setdetachstate(bionic_attr *a, int st) {
    if (st) a->flags |= 1; else a->flags &= ~1u;
    return 0;
}

typedef struct {
    void *(*fn)(void *);
    void *arg;
} start_info;

/* ---- per-thread CPU accounting and core placement (update 25) ----
 * Run 9 (sound on, 2026-11-11): the same asset reads took 2.5x longer inside fread once FMOD's mixer ran, so the loading thread shares its core with FMOD's
 * threads. Every thread the game creates is registered here; bv_thread_report() logs each one's CPU use and core every few seconds.
 * Config cores=N: 0 = the threads keep the affinity they are created with (as before); 1 = FMOD's threads on core 2, the game's threads take cores 0 and 1 in
 * turn (creation order); 2 = every thread may run on all three user cores. */
int bv_thread_cores;
#define MAX_TRACKED 64
typedef struct {
    SceUID uid;
    uint32_t fn;
    uint64_t last_clocks;
} tracked;
static tracked g_thr[MAX_TRACKED];
static volatile int g_nthr;
static volatile int g_game_threads;

static void thread_place(uint32_t fn) {
    if (!bv_thread_cores) return;
    const so_module *m = bv_module_for_pc(fn & ~1u);
    int mask;
    if (bv_thread_cores == 2) mask = SCE_KERNEL_CPU_MASK_USER_ALL;
    /* update 166 (zone profile run 196: 'finish character animations' 470-660 ms/s on the game thread while the worker threads sat 'ready' on the
     * game thread's core 0): cores=3 = every created thread on cores 1+2; the game's main thread moves itself to core 0 alone at its first frame
     * (shims/shim_egl.c) */
    else if (bv_thread_cores == 3) mask = SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2;
    else if (m && !strcmp(m->name, "libfmod.so")) mask = SCE_KERNEL_CPU_MASK_USER_2;
    else mask = (__sync_fetch_and_add(&g_game_threads, 1) & 1) ? SCE_KERNEL_CPU_MASK_USER_1 : SCE_KERNEL_CPU_MASK_USER_0;
    int r = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), mask);
    bv_log("thread %s+0x%x: cpu affinity 0x%x (cores=%d) -> 0x%x", m ? m->name : "?", m ? (fn & ~1u) - m->load : fn, (unsigned)mask, bv_thread_cores, (unsigned)r);
}

void bv_thread_register(uint32_t fn) {
    int i = __sync_fetch_and_add(&g_nthr, 1);
    if (i >= MAX_TRACKED) return;
    g_thr[i].fn = fn;
    g_thr[i].uid = sceKernelGetThreadId();
    SceKernelThreadInfo ti;
    memset(&ti, 0, sizeof ti);
    ti.size = sizeof ti;
    if (sceKernelGetThreadInfo(g_thr[i].uid, &ti) == 0) {
        g_thr[i].last_clocks = ti.runClocks;
        const so_module *m = bv_module_for_pc(fn & ~1u);
        bv_log("thread #%d %s+0x%x: uid 0x%x, priority %d, affinity 0x%x, on core %d", i, m ? m->name : (fn ? "?" : "loader"), m ? (fn & ~1u) - m->load : fn,
               (unsigned)g_thr[i].uid, (int)ti.currentPriority, (unsigned)ti.currentCpuAffinityMask, (int)ti.currentCpuId);
    }
}

/* ---- wait sampler (update 26). Run 10: after the first 30 s of loading no thread used more than 25 % of a core, so the 3+ minutes of loading are mostly
 * WAITING. Every 50 ms this samples each tracked thread's state and, when it waits, the kind of wait and the object (kernel uid); the report names the most
 * frequent ones per thread. ---- */
#define WAIT_KEYS 4
typedef struct {
    uint32_t samples, running, ready;
    uint32_t key_type[WAIT_KEYS], key_id[WAIT_KEYS], key_n[WAIT_KEYS];
    uint32_t other; /* waits that did not fit in the keys */
} wait_hist;
static wait_hist g_wh[MAX_TRACKED];
static SceUID g_sampler = -1;

static int sampler_main(SceSize args, void *argp) {
    (void)args; (void)argp;
    for (;;) {
        sceKernelDelayThread(50 * 1000);
        int n = g_nthr < MAX_TRACKED ? g_nthr : MAX_TRACKED;
        for (int i = 0; i < n; i++) {
            if (!g_thr[i].uid) continue;
            SceKernelThreadInfo ti;
            memset(&ti, 0, sizeof ti);
            ti.size = sizeof ti;
            if (sceKernelGetThreadInfo(g_thr[i].uid, &ti) != 0) continue;
            wait_hist *h = &g_wh[i];
            h->samples++;
            if (ti.status == SCE_THREAD_RUNNING) { h->running++; continue; }
            if (ti.status == SCE_THREAD_READY) { h->ready++; continue; }
            int k;
            for (k = 0; k < WAIT_KEYS; k++)
                if (h->key_n[k] && h->key_type[k] == ti.waitType && h->key_id[k] == (uint32_t)ti.waitId) break;
            if (k == WAIT_KEYS)
                for (k = 0; k < WAIT_KEYS; k++)
                    if (!h->key_n[k]) { h->key_type[k] = ti.waitType; h->key_id[k] = (uint32_t)ti.waitId; break; }
            if (k == WAIT_KEYS) h->other++;
            else h->key_n[k]++;
        }
    }
    return 0;
}

/* The name of the kernel object a thread waits on (pthread-embedded builds mutexes and condition variables on kernel objects), or "" */
static const char *obj_name(uint32_t uid, char *buf, size_t n) {
    buf[0] = 0;
    if (!uid) return buf;
    SceKernelSemaInfo si;
    memset(&si, 0, sizeof si);
    si.size = sizeof si;
    if (sceKernelGetSemaInfo((SceUID)uid, &si) == 0) { snprintf(buf, n, " sema \"%.31s\"", si.name); return buf; }
    SceKernelMutexInfo mi;
    memset(&mi, 0, sizeof mi);
    mi.size = sizeof mi;
    if (sceKernelGetMutexInfo((SceUID)uid, &mi) == 0) { snprintf(buf, n, " mutex \"%.31s\"", mi.name); return buf; }
    SceKernelEventFlagInfo ei;
    memset(&ei, 0, sizeof ei);
    ei.size = sizeof ei;
    if (sceKernelGetEventFlagInfo((SceUID)uid, &ei) == 0) { snprintf(buf, n, " eventflag \"%.31s\"", ei.name); return buf; }
    for (int i = 0; i < g_nthr && i < MAX_TRACKED; i++)
        if ((uint32_t)g_thr[i].uid == uid) { snprintf(buf, n, " thread #%d", i); return buf; }
    return buf;
}

/* One line per thread that had samples: running / ready share and its two most frequent waits (type, object uid, share). Cleared after each report. */
static void wait_report(void) {
    int n = g_nthr < MAX_TRACKED ? g_nthr : MAX_TRACKED;
    for (int i = 0; i < n; i++) {
        wait_hist h = g_wh[i];
        memset(&g_wh[i], 0, sizeof g_wh[i]);
        if (!h.samples) continue;
        int a = -1, b = -1;
        for (int k = 0; k < WAIT_KEYS; k++) {
            if (!h.key_n[k]) continue;
            if (a < 0 || h.key_n[k] > h.key_n[a]) { b = a; a = k; }
            else if (b < 0 || h.key_n[k] > h.key_n[b]) b = k;
        }
        char w[300] = "", nm[64];
        if (a >= 0)
            snprintf(w, sizeof w, ", waits: type 0x%x on 0x%x%s %u%%", (unsigned)h.key_type[a], (unsigned)h.key_id[a], obj_name(h.key_id[a], nm, sizeof nm),
                     (unsigned)(h.key_n[a] * 100 / h.samples));
        if (b >= 0)
            snprintf(w + strlen(w), sizeof w - strlen(w), ", type 0x%x on 0x%x%s %u%%", (unsigned)h.key_type[b], (unsigned)h.key_id[b], obj_name(h.key_id[b], nm, sizeof nm),
                     (unsigned)(h.key_n[b] * 100 / h.samples));
        if (h.other) snprintf(w + strlen(w), sizeof w - strlen(w), ", other waits %u%%", (unsigned)(h.other * 100 / h.samples));
        bv_log("  thread #%d: running %u%%, ready %u%%%s", i, (unsigned)(h.running * 100 / h.samples), (unsigned)(h.ready * 100 / h.samples), w);
    }
}

/* One line: every tracked thread that used at least 2 % of a core since the last call, busiest first is not needed: the log is read by eye. */
void bv_thread_report(uint32_t window_ms) {
    if (g_sampler < 0) {
        g_sampler = sceKernelCreateThread("bv_waits", sampler_main, 0x10000100 - 8, 0x4000, 0, 0, NULL);
        if (g_sampler >= 0) sceKernelStartThread(g_sampler, 0, NULL);
        bv_log("wait sampler thread: 0x%x", (unsigned)g_sampler);
    } else {
        wait_report();
    }
    char line[900];
    int at = snprintf(line, sizeof line, "cpu (last %u ms):", (unsigned)window_ms);
    int n = g_nthr < MAX_TRACKED ? g_nthr : MAX_TRACKED, shown = 0;
    for (int i = 0; i < n && at < (int)sizeof line - 60; i++) {
        SceKernelThreadInfo ti;
        memset(&ti, 0, sizeof ti);
        ti.size = sizeof ti;
        if (!g_thr[i].uid || sceKernelGetThreadInfo(g_thr[i].uid, &ti) != 0) continue;
        uint64_t c = ti.runClocks;
        uint64_t d = c - g_thr[i].last_clocks;
        g_thr[i].last_clocks = c;
        unsigned pct = window_ms ? (unsigned)(d / 10u / window_ms) : 0; /* runClocks are microseconds */
        if (pct < 2) continue;
        at += snprintf(line + at, sizeof line - (size_t)at, " #%d %u%% c%d", i, pct, (int)ti.lastExecutedCpuId);
        shown++;
    }
    if (shown) bv_log("%s", line);
}

static void *thread_tramp(void *p) {
    start_info si = *(start_info *)p;
    free(p);
    bv_thread_register((uint32_t)(uintptr_t)si.fn);
    {   /* update 54: remember the game main thread for the sleep accounting */
        const so_module *gm = bv_module_for_pc((uint32_t)(uintptr_t)si.fn & ~1u);
        if (gm && !strcmp(gm->name, "libg.so") && (((uint32_t)(uintptr_t)si.fn & ~1u) - gm->load) == 0x694008u) g_game_uid = sceKernelGetThreadId();
    }
    thread_place((uint32_t)(uintptr_t)si.fn);
    BV_TRACE(2, "thread start fn=%p arg=%p", (void *)si.fn, si.arg);
    void *r = si.fn(si.arg);
    BV_TRACE(2, "thread end fn=%p", (void *)si.fn);
    return r;
}

static int w_pthread_create(pthread_t *t, const bionic_attr *a, void *(*fn)(void *), void *arg) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    uint32_t stack = a && a->stack_size ? a->stack_size : DEFAULT_STACK;
    if (stack < 256 * 1024) stack = 256 * 1024;
    /* update 53: the game's main thread (libg+0x694008: logic + all GL calls, so vitaGL's runtime shader compiler runs on it) overflowed its 1 MB stack:
     * crash dumps of run 34 (inside glLinkProgram, before home) and run 40 (battle) have its sp at 0x8a2005f0 / 0x8a2004f0, below the bottom of its
     * 1 MB stack (run 32: same thread at sp 0x8a300920 near the top); other battle crashes left unfinished .tmp dumps. Config game_stack_kb (default 4096). */
    {
        const so_module *m = bv_module_for_pc((uint32_t)(uintptr_t)fn & ~1u);
        if (m && !strcmp(m->name, "libg.so") && (((uint32_t)(uintptr_t)fn & ~1u) - m->load) == 0x694008u && bv_game_stack_kb > 0 &&
            stack < (uint32_t)bv_game_stack_kb * 1024u) {
            bv_log("pthread_create: game main thread libg+0x694008 asked for %u KB of stack: given %d KB (vitaGL compiles shaders on it)", (unsigned)(stack >> 10),
                   bv_game_stack_kb);
            stack = (uint32_t)bv_game_stack_kb * 1024u;
        }
    }
    pthread_attr_setstacksize(&attr, stack);
    if (a && (a->flags & 1)) pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    start_info *si = (start_info *)malloc(sizeof *si);
    if (!si) return EAGAIN;
    si->fn = fn;
    si->arg = arg;
    int r = pthread_create(t, &attr, thread_tramp, si);
    pthread_attr_destroy(&attr);
    if (r) free(si);
    BV_TRACE(1, "pthread_create(fn=%p stack=%u) -> %d", (void *)fn, (unsigned)stack, r);
    return r;
}

static int w_pthread_setschedparam(pthread_t t, int policy, const void *p) { (void)t; (void)policy; (void)p; return 0; }
static int w_pthread_getschedparam(pthread_t t, int *policy, int *prio) {
    (void)t;
    if (policy) *policy = 0;
    if (prio) *prio = 0;
    return 0;
}
static int w_pthread_setname_np(pthread_t t, const char *name) { (void)t; (void)name; return 0; }
/* cycle 5 (ISSUES A2): newlib's sched_yield is pte_osThreadSleep(1) = 1 ms. Same default, now counted (kind 7); config yield_us = experiment. */
int bv_yield_us = 1000;
static int w_sched_yield(void) { bv_sleep_delay(7, bv_yield_us < 0 ? 1000u : (uint32_t)bv_yield_us); return 0; }

/* ---- semaphores: bionic sem_t is 4 bytes, ours a pointer, so the slot holds the handle ---- */
static int w_sem_init(sem_t *s, int pshared, unsigned v) { return sem_init(s, pshared, v); }
static int w_sem_post(sem_t *s) { return sem_post(s); }
static int w_sem_wait(sem_t *s) { return sem_wait(s); }
int __real_sem_destroy(sem_t *s);
/* cycle 5 (ISSUES A2): pte's sem_destroy sleeps 1 ms; libg destroys semaphores often. sem_reaper=1 moves the destroy to a background thread. */
int bv_sem_reaper = 0;
#define REAP_RING 1024
static sem_t g_reap[REAP_RING];
static volatile unsigned g_reap_head, g_reap_tail, g_reap_done, g_reap_direct;
static pthread_mutex_t g_reap_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_reap_thr;
static volatile int g_reap_started;
static void *reaper_main(void *arg) {
    (void)arg;
    for (;;) {
        sem_t s = NULL;
        pthread_mutex_lock(&g_reap_lock);
        if (g_reap_tail != g_reap_head) { s = g_reap[g_reap_tail % REAP_RING]; g_reap_tail++; }
        pthread_mutex_unlock(&g_reap_lock);
        if (s) { __real_sem_destroy(&s); g_reap_done++; }
        else __real_sceKernelDelayThread(2000);
    }
    return NULL;
}
/* game-thread callers of sem_destroy (run 74: the 1 ms sleeps do not come through w_sem_destroy) */
static volatile uint32_t g_sd_pc[8], g_sd_n[8], g_sd_other;
static void sd_note(uint32_t pc) {
    for (int i = 0; i < 8; i++) {
        if (g_sd_pc[i] == pc || g_sd_pc[i] == 0) { if (!g_sd_pc[i]) g_sd_pc[i] = pc; g_sd_n[i]++; return; }
    }
    g_sd_other++;
}
static int w_sem_destroy(sem_t *s) { return sem_destroy(s); }   /* -> __wrap_sem_destroy */
int __wrap_sem_destroy(sem_t *s) {
    if (sceKernelGetThreadId() == g_game_uid) sd_note((uint32_t)__builtin_return_address(0));
    if (!bv_sem_reaper || !s || !*s) return __real_sem_destroy(s);
    if (!g_reap_started) {
        pthread_mutex_lock(&g_reap_lock);
        if (!g_reap_started && pthread_create(&g_reap_thr, NULL, reaper_main, NULL) == 0) {
            g_reap_started = 1;
            bv_log("sem reaper: background thread started (sem_destroy no longer sleeps on the caller's thread)");
        }
        pthread_mutex_unlock(&g_reap_lock);
        if (!g_reap_started) return __real_sem_destroy(s);
    }
    int queued = 0;
    pthread_mutex_lock(&g_reap_lock);
    if (g_reap_head - g_reap_tail < REAP_RING) { g_reap[g_reap_head % REAP_RING] = *s; g_reap_head++; queued = 1; }
    pthread_mutex_unlock(&g_reap_lock);
    if (!queued) { g_reap_direct++; return __real_sem_destroy(s); }
    *s = NULL;
    return 0;
}
void bv_sem_reaper_report(void) {
    static unsigned last_q, last_d;
    static uint32_t last_sd[8];
    char line[300];
    int at = snprintf(line, sizeof line, "    sem_destroy (game thread, last second, calls by caller):");
    int any = 0;
    for (int i = 0; i < 8 && g_sd_pc[i]; i++) {
        uint32_t n = g_sd_n[i] - last_sd[i];
        last_sd[i] = g_sd_n[i];
        if (n && at < (int)sizeof line - 24) { at += snprintf(line + at, sizeof line - (size_t)at, " 0x%08x %u", (unsigned)g_sd_pc[i], (unsigned)n); any = 1; }
    }
    if (any) bv_log("%s", line);
    if (!g_reap_started) return;
    unsigned q = g_reap_head, d = g_reap_done;
    if (q != last_q) bv_log("    sem reaper: %u queued, %u destroyed in the last second (%u pending, %u direct because full)", q - last_q, d - last_d, q - g_reap_tail,
                           g_reap_direct);
    last_q = q;
    last_d = d;
}

const bv_shim bv_shims_pthread[] = {
    SHIM(pthread_mutexattr_init), SHIM(pthread_mutexattr_destroy), SHIM(pthread_mutexattr_settype),
    SHIM(pthread_mutex_init), SHIM(pthread_mutex_lock), SHIM(pthread_mutex_trylock), SHIM(pthread_mutex_unlock),
    SHIM(pthread_mutex_destroy),
    SHIM(pthread_cond_init), SHIM(pthread_cond_destroy), SHIM(pthread_cond_signal), SHIM(pthread_cond_broadcast),
    SHIM(pthread_cond_wait), SHIM(pthread_cond_timedwait),
    SHIM(pthread_once),
    SHIM(pthread_attr_init), SHIM(pthread_attr_destroy), SHIM(pthread_attr_setstacksize),
    SHIM(pthread_attr_setdetachstate), SHIM(pthread_create),
    SHIM(pthread_setschedparam), SHIM(pthread_getschedparam), SHIM(pthread_setname_np), SHIM(sched_yield),
    SHIM(sem_init), SHIM(sem_post), SHIM(sem_wait), SHIM(sem_destroy),
    /* same layout on both sides */
    SHIM_PASS(pthread_self), SHIM_PASS(pthread_equal), SHIM_PASS(pthread_join), SHIM_PASS(pthread_detach),
    SHIM_PASS(pthread_exit), SHIM_PASS(pthread_key_create), SHIM_PASS(pthread_key_delete),
    SHIM_PASS(pthread_getspecific), SHIM_PASS(pthread_setspecific),
    SHIM_END};
