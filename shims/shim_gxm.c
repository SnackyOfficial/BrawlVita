/* update 94 (ISSUES A5): link-time wraps of the sceGxm calls vitaGL makes for framebuffer render targets and scenes.
 * Evidence so far (runs 102, 104, 106): Play's arena pre-bake renders ~48 small textures through framebuffer objects inside ONE frame and the
 * game dies at FBO clear #47/48 every time; run 106: glFinish before every 8th clear returned in 2 ms (the GPU is idle and healthy) and it still
 * died at #47 -> a call fails at a fixed count. These wraps log every error return of sceGxmCreateRenderTarget / sceGxmBeginScene /
 * sceGxmEndScene / sceGxmDestroyRenderTarget and the number of live render targets, so the failing call shows in the log.
 * Always on (cheap): only counters, errors and the first calls are logged. */
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <stdint.h>

#include "../loader/log.h"

int __real_sceGxmCreateRenderTarget(const SceGxmRenderTargetParams *params, SceGxmRenderTarget **renderTarget);
int __real_sceGxmDestroyRenderTarget(SceGxmRenderTarget *renderTarget);
int __real_sceGxmBeginScene(SceGxmContext *context, unsigned int flags, const SceGxmRenderTarget *renderTarget, const SceGxmValidRegion *validRegion,
                            SceGxmSyncObject *vertexSyncObject, SceGxmSyncObject *fragmentSyncObject, const SceGxmColorSurface *colorSurface,
                            const SceGxmDepthStencilSurface *depthStencil);
int __real_sceGxmEndScene(SceGxmContext *context, const SceGxmNotification *vertexNotification, const SceGxmNotification *fragmentNotification);

volatile uint32_t bv_gxm_rt_live, bv_gxm_rt_made, bv_gxm_scenes, bv_gxm_errors;

/* update 95 (ISSUES A5, run 108): creation fails with 0x805b0027 when ~48 render targets are alive; vitaGL's queued (already replaced) ones are
 * only destroyed a few frames later. config rt_purge (default 1): on a failed creation, destroy every queued render target now
 * (vgl_bv_purge_rts_now, our vitaGL patch tools/vitagl_rtpurge.py; weak: absent in a stock vitaGL build) and try once more. */
int vgl_bv_purge_rts_now(void) __attribute__((weak));
int bv_rt_purge = 1;
static volatile uint32_t g_rt_purges;

/* update 144 (req. 5 / 6, evidence runs 154 / 159): battles on the Graveyard Showdown and Mini City maps re-create render targets all the time
 * (run 159: 1024+ sceGxmCreateRenderTarget calls, an rt_purge with a full GPU drain every 0.5-2 s; fast 3v3 runs: ~260 and 3 purges). vitaGL drops a
 * framebuffer's render target whenever a new texture is attached and makes a new one, although a render target only depends on its parameters
 * (size, scenes per frame, MSAA, flags), not on the texture. config rt_cache=1: DestroyRenderTarget keeps the target (vitaGL only destroys after its
 * purge delay or after sceGxmFinish, so the GPU is done with it) and CreateRenderTarget hands back a kept one with the same parameters. On a failed
 * creation the kept ones are destroyed first. 0 = off (default until a battle confirms it). Timing of both calls goes to the per-second line. */
int bv_rt_cache;
#define RTC_MAX 24
#define RTL_MAX 160
static struct { SceGxmRenderTarget *rt; SceGxmRenderTargetParams p; } g_rtc[RTC_MAX];   /* kept (free) */
static int g_rtc_n;
static volatile int g_rtc_lock, g_rtc_bypass;   /* spin lock; bypass = rt_purge is running: destroy for real */
static void rtc_lock(void) { while (__sync_lock_test_and_set(&g_rtc_lock, 1)) { } }
static void rtc_unlock(void) { __sync_lock_release(&g_rtc_lock); }
static struct { SceGxmRenderTarget *rt; SceGxmRenderTargetParams p; } g_rtl[RTL_MAX];   /* params of every live one we made */
volatile uint32_t bv_rtc_hits, bv_rtc_kept, bv_gxm_rt_us;   /* reuses, kept on destroy, microseconds inside create + destroy */
static int same_params(const SceGxmRenderTargetParams *a, const SceGxmRenderTargetParams *b) {
    return a->flags == b->flags && a->width == b->width && a->height == b->height && a->scenesPerFrame == b->scenesPerFrame &&
           a->multisampleMode == b->multisampleMode && a->multisampleLocations == b->multisampleLocations && a->driverMemBlock == b->driverMemBlock;
}
static void rtl_put(SceGxmRenderTarget *rt, const SceGxmRenderTargetParams *p) {
    for (int i = 0; i < RTL_MAX; i++) if (!g_rtl[i].rt) { g_rtl[i].rt = rt; g_rtl[i].p = *p; return; }
}
static const SceGxmRenderTargetParams *rtl_get(SceGxmRenderTarget *rt) {
    for (int i = 0; i < RTL_MAX; i++) if (g_rtl[i].rt == rt) return &g_rtl[i].p;
    return NULL;
}
static void rtl_drop(SceGxmRenderTarget *rt) {
    for (int i = 0; i < RTL_MAX; i++) if (g_rtl[i].rt == rt) { g_rtl[i].rt = NULL; return; }
}
static int rtc_flush(void) {   /* destroy every kept target (frees room for a new one) */
    int n = 0;
    for (int i = 0; i < g_rtc_n; i++) {
        if (__real_sceGxmDestroyRenderTarget(g_rtc[i].rt) >= 0) { __sync_sub_and_fetch(&bv_gxm_rt_live, 1); n++; }
        rtl_drop(g_rtc[i].rt);
    }
    g_rtc_n = 0;
    return n;
}

int __wrap_sceGxmCreateRenderTarget(const SceGxmRenderTargetParams *params, SceGxmRenderTarget **renderTarget) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    if (bv_rt_cache && params && params->driverMemBlock == (SceUID)-1) {   /* vitaGL passes -1 (driver memory allocated by the system) */
        rtc_lock();
        for (int i = 0; i < g_rtc_n; i++)
            if (same_params(&g_rtc[i].p, params)) {
                *renderTarget = g_rtc[i].rt;
                g_rtc[i] = g_rtc[--g_rtc_n];
                rtc_unlock();
                uint32_t k = __sync_add_and_fetch(&bv_rtc_hits, 1);
                if (k <= 8 || !(k & (k - 1))) bv_log("gxm: rt_cache: %ux%u render target reused (#%u, %d kept)", (unsigned)params->width, (unsigned)params->height, (unsigned)k, g_rtc_n);
                __sync_add_and_fetch(&bv_gxm_rt_us, (uint32_t)(sceKernelGetProcessTimeWide() - t0));
                return 0;
            }
        rtc_unlock();
    }
    int r = __real_sceGxmCreateRenderTarget(params, renderTarget);
    if (r < 0 && bv_rt_cache && g_rtc_n > 0) {   /* the kept ones count against the ~48 limit: give them back first */
        rtc_lock();
        int fl = rtc_flush();
        rtc_unlock();
        r = __real_sceGxmCreateRenderTarget(params, renderTarget);
        bv_log("gxm: rt_cache: creation failed, %d kept render targets destroyed, retry -> 0x%08x", fl, (unsigned)r);
    }
    if (r < 0 && bv_rt_purge && vgl_bv_purge_rts_now) {
        uint32_t live = bv_gxm_rt_live;
        g_rtc_bypass = 1;   /* update 144: these must really be destroyed */
        int freed = vgl_bv_purge_rts_now(); /* its sceGxmDestroyRenderTarget calls go through our wrap: live drops */
        g_rtc_bypass = 0;
        int r2 = __real_sceGxmCreateRenderTarget(params, renderTarget);
        uint32_t k = __sync_add_and_fetch(&g_rt_purges, 1);
        if (k <= 20 || !(k & (k - 1)))
            bv_log("gxm: CreateRenderTarget failed 0x%08x with %u live: %d queued render targets destroyed now (rt_purge), retry -> 0x%08x, live %u (#%u)", (unsigned)r,
                   (unsigned)live, freed, (unsigned)r2, (unsigned)bv_gxm_rt_live, (unsigned)k);
        r = r2;
    }
    uint32_t n = __sync_add_and_fetch(&bv_gxm_rt_made, 1);
    if (r >= 0) __sync_add_and_fetch(&bv_gxm_rt_live, 1);
    if (r >= 0 && bv_rt_cache && params) { rtc_lock(); rtl_put(*renderTarget, params); rtc_unlock(); }
    __sync_add_and_fetch(&bv_gxm_rt_us, (uint32_t)(sceKernelGetProcessTimeWide() - t0));
    if (r < 0 || n <= 6 || (n >= 40 && n <= 64) || !(n & (n - 1))) {
        if (r < 0) __sync_add_and_fetch(&bv_gxm_errors, 1);
        bv_log("gxm: CreateRenderTarget #%u %ux%u scenes/frame %u -> 0x%08x (%p), live %u", (unsigned)n, params ? (unsigned)params->width : 0,
               params ? (unsigned)params->height : 0, params ? (unsigned)params->scenesPerFrame : 0, (unsigned)r, renderTarget ? (void *)*renderTarget : NULL,
               (unsigned)bv_gxm_rt_live);
    }
    return r;
}

int __wrap_sceGxmDestroyRenderTarget(SceGxmRenderTarget *renderTarget) {
    if (bv_rt_cache && renderTarget) {
        rtc_lock();
        const SceGxmRenderTargetParams *p = rtl_get(renderTarget);
        if (p && !g_rtc_bypass && g_rtc_n < RTC_MAX) {   /* keep it for the next target of the same size (vitaGL is done with it here) */
            g_rtc[g_rtc_n].rt = renderTarget;
            g_rtc[g_rtc_n].p = *p;
            g_rtc_n++;
            rtc_unlock();
            __sync_add_and_fetch(&bv_rtc_kept, 1);
            return 0;
        }
        rtl_drop(renderTarget);
        rtc_unlock();
    }
    uint64_t t0 = sceKernelGetProcessTimeWide();
    int r = __real_sceGxmDestroyRenderTarget(renderTarget);
    __sync_add_and_fetch(&bv_gxm_rt_us, (uint32_t)(sceKernelGetProcessTimeWide() - t0));
    if (r >= 0) __sync_sub_and_fetch(&bv_gxm_rt_live, 1);
    else {
        __sync_add_and_fetch(&bv_gxm_errors, 1);
        bv_log("gxm: DestroyRenderTarget(%p) -> 0x%08x", (void *)renderTarget, (unsigned)r);
    }
    return r;
}

/* update 113 (ISSUES A14, Showdown crash, run 128): the Showdown arena pre-bake runs inside ONE 57 s frame; vitaGL keeps every replaced buffer /
 * texture block on its purge list until the next swap, so the pools drained to 0 MB and the RAM pool bookkeeping broke (tools/vitagl_gcnow.py has
 * the log evidence). config gc_now=N (default 512): before a scene starts (no scene is open here), when this frame has queued >= N blocks AND the
 * pools are low (RAM + VRAM free < gc_low_kb, default 16384) or the queue is near its 16384 limit, run vitaGL's garbage collector now
 * (vgl_bv_gc_now, our vitaGL patch; weak: absent in a stock build). 0 = off. Normal frames queue far fewer blocks, so it only acts in long frames. */
int vgl_bv_gc_now(void) __attribute__((weak));
extern int frame_elem_purge_idx __attribute__((weak));
#include <vitaGL.h>
int bv_gc_now = 512, bv_gc_low_kb = 16384;
volatile uint32_t bv_gc_runs, bv_gc_freed;

static void gc_check(void) {
    if (bv_gc_now <= 0 || !vgl_bv_gc_now || !&frame_elem_purge_idx) return;
    int q = frame_elem_purge_idx;
    if (q < bv_gc_now) return;
    size_t fr = 0;
    if (q < 12000) {
        fr = (vglMemFree(VGL_MEM_RAM) + vglMemFree(VGL_MEM_VRAM)) >> 10;
        if (fr >= (size_t)bv_gc_low_kb) return;
    }
    int n = vgl_bv_gc_now();
    uint32_t k = __sync_add_and_fetch(&bv_gc_runs, 1);
    __sync_add_and_fetch(&bv_gc_freed, (uint32_t)n);
    if (k <= 20 || !(k & (k - 1)))
        bv_log("gxm: gc_now #%u: %d queued blocks in this frame, pools %u KB free -> %d freed now, RAM %u KB / VRAM %u KB free after", (unsigned)k, q, (unsigned)fr, n,
               (unsigned)(vglMemFree(VGL_MEM_RAM) >> 10), (unsigned)(vglMemFree(VGL_MEM_VRAM) >> 10));
}

int __wrap_sceGxmBeginScene(SceGxmContext *context, unsigned int flags, const SceGxmRenderTarget *renderTarget, const SceGxmValidRegion *validRegion,
                            SceGxmSyncObject *vertexSyncObject, SceGxmSyncObject *fragmentSyncObject, const SceGxmColorSurface *colorSurface,
                            const SceGxmDepthStencilSurface *depthStencil) {
    gc_check();
    if (!renderTarget) bv_log("gxm: BeginScene with a NULL render target (scene %u) - render target creation must have failed", (unsigned)bv_gxm_scenes + 1);
    int r = __real_sceGxmBeginScene(context, flags, renderTarget, validRegion, vertexSyncObject, fragmentSyncObject, colorSurface, depthStencil);
    uint32_t n = __sync_add_and_fetch(&bv_gxm_scenes, 1);
    if (r < 0) {
        uint32_t e = __sync_add_and_fetch(&bv_gxm_errors, 1);
        if (e <= 40) bv_log("gxm: BeginScene #%u (target %p, depth %p) -> 0x%08x", (unsigned)n, (const void *)renderTarget, (const void *)depthStencil, (unsigned)r);
    }
    return r;
}

int __wrap_sceGxmEndScene(SceGxmContext *context, const SceGxmNotification *vertexNotification, const SceGxmNotification *fragmentNotification) {
    int r = __real_sceGxmEndScene(context, vertexNotification, fragmentNotification);
    if (r < 0) {
        uint32_t e = __sync_add_and_fetch(&bv_gxm_errors, 1);
        if (e <= 40) bv_log("gxm: EndScene (after scene %u) -> 0x%08x", (unsigned)bv_gxm_scenes, (unsigned)r);
    }
    return r;
}
