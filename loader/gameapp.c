#include "gameapp.h"

#include <stdio.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "../jni/fake_jni.h"
#include "../shims/shim.h"
#include "gtrace.h"
#include "hook_b.h"
#include "log.h"
#include "buttons_vita.h"
#include "touch_vita.h"

#define CLASS_HANDLE 0xC1000100u    /* any non-zero value: the natives only pass it back */
#define ASSETS_HANDLE 0xA5000001u
#define ACTIVITY_HANDLE 0xAC000001u
#define SURFACE_HANDLE 0x50000001u

#define PKG "com.supercell.brawlstars"
/* update 167: screen_res config (loader/main.c) */
int bv_screen_w = 720, bv_screen_h = 408;   /* update 175: 720x408 default (VERIFIED runs 198-208); screen_res=960 = old */
#define SCREEN_W bv_screen_w
#define SCREEN_H bv_screen_h
#define DENSITY_DPI (240 * bv_screen_w / 960)
#define PPI 220.0f

int bv_touch_enabled = 1;
int bv_buttons_enabled = 1, bv_back_cross = 0, bv_back_exits = 1;

#include <malloc.h>

#include "stats.h"
#include "../gfx/gl_init.h"

extern volatile uint32_t bv_egl_swaps;
extern volatile int bv_egl_ready;
void bv_assets_selfcheck(void);
extern int bv_gl_width, bv_gl_height;

typedef uint32_t (*fn_v)(void *env, uint32_t cls);
typedef uint32_t (*fn_i)(void *env, uint32_t cls, uint32_t a);
typedef uint32_t (*fn_create)(void *env, uint32_t cls, uint32_t assets, uint32_t data_dir, uint32_t cache_dir, uint32_t obb_path,
                              uint64_t obb_size, int w, int h, int dpi, uint32_t xdpi_bits, uint32_t ydpi_bits, int k1, int k2,
                              uint32_t activity);
typedef uint32_t (*fn_surf)(void *env, uint32_t cls, uint32_t surface, int w, int h);

static const bv_native *find(const char *name) {
    for (int i = 0; i < bv_jni_native_count(); i++) {
        const bv_native *n = bv_jni_native(i);
        if (n && !strcmp(n->name, name) && strstr(n->cls, "GameApp")) return n;
    }
    return NULL;
}

static uint32_t need(const char *name, const char *sig) {
    const bv_native *n = find(name);
    if (!n) {
        bv_log("GameApp.%s is not among the registered natives", name);
        return 0;
    }
    if (sig && strcmp(n->sig, sig))
        bv_log("GameApp.%s is registered with signature %s (expected %s): arguments may not match", name, n->sig, sig);
    return n->fn;
}

static void list_natives(void) {
    int n = bv_jni_native_count();
    bv_log("registered natives (%d):", n);
    for (int i = 0; i < n && i < 120; i++) {
        const bv_native *r = bv_jni_native(i);
        bv_log("  %s.%s %s", r->cls, r->name, r->sig);
    }
}

static void call_v(const char *name) {
    uint32_t f = need(name, "()V");
    if (!f) return;
    bv_log("calling GameApp.%s()", name);
    ((fn_v)(uintptr_t)f)(bv_jni_env(), CLASS_HANDLE);
    bv_log("  %s returned", name);
}

typedef void (*fn_ii)(void *, uint32_t, uint32_t, uint32_t);

/* The game shows its terms / notice dialogs through NativeDialogManager.ShowDialog and waits for GameApp.dialogDismissed(id, button); with nothing to
 * show, every dialog is reported as accepted (first button). Until then the game keeps its modal state, which also dims the screen behind it. */
static void report_dialogs(void) {
    int id;
    while ((id = bv_jni_take_dialog()) != 0) {
        const bv_native *dn = bv_jni_find_native("dialogDismissed");
        if (!dn) {
            bv_log("dialog %d: GameApp.dialogDismissed is not registered, cannot report it", id);
            continue;
        }
        if (strcmp(dn->sig, "(II)V")) bv_log("GameApp.dialogDismissed is registered with signature %s (expected (II)V)", dn->sig);
        bv_log("dialog %d: reported as dismissed (button 0, as if the user had pressed the first button)", id);
        ((fn_ii)(uintptr_t)dn->fn)(bv_jni_env(), CLASS_HANDLE, (uint32_t)id, 0);
        bv_log("  dialogDismissed returned");
    }
}

static void gtrace_line(const char *line) { bv_log("%s", line); }

static void idle(int seconds) {
    extern void bv_thread_report(uint32_t window_ms); /* shims/shim_pthread.c */
    extern void bv_thread_register(uint32_t fn);
    bv_thread_register(0); /* this thread: shown as "loader" */
    uint32_t last_swaps = 0;
    for (int s = 1; seconds <= 0 || s <= seconds; s++) {   /* cycle 8 (I5): run_seconds <= 0 = run until the app is closed */
        sceKernelDelayThread(1000 * 1000);
        report_dialogs();
        if (s % 5 == 0) bv_thread_report(5000);
        if (bv_gtrace_enabled) bv_gtrace_report(gtrace_line); /* update 40: field getters read in the last second (gtrace.h) */
        bv_probe_report(gtrace_line);                           /* update 47: code-path probes that ran in the last second */
        /* update 44: screenshot on request. When ux0:data/BrawlVita/shot.req exists (put there from the PC), the frame 3 swaps later is saved as
         * shot_<swap>.tga (shims/shim_egl.c) and the request file is removed. */
        {
            extern int bv_shot_frames[4], bv_shot_n;
            SceIoStat st;
            /* cycle 4 (ISSUES F1): trace.req = log two whole frames of GL calls starting 3 swaps from now (like gltrace_frame, but on demand) */
            if (sceIoGetstat(BV_DATA_DIR "/trace.req", &st) >= 0) {
                extern int bv_gltrace_frame;
                sceIoRemove(BV_DATA_DIR "/trace.req");
                bv_gltrace_frame = (int)bv_egl_swaps + 3;
                bv_log("GL trace requested: frames %d and %d are logged (W[n] lines)", bv_gltrace_frame, bv_gltrace_frame + 1);
            }
            if (sceIoGetstat(BV_DATA_DIR "/shot.req", &st) >= 0) {
                sceIoRemove(BV_DATA_DIR "/shot.req");
                int slot = bv_shot_n < 4 ? bv_shot_n++ : 3;
                bv_shot_frames[slot] = (int)bv_egl_swaps + 3;
                bv_log("screenshot requested: frame %d -> ux0:data/BrawlVita/shot_%d.tga", bv_shot_frames[slot], bv_shot_frames[slot]);
            }
        }
        static uint32_t last_swaps_for_frame;
        uint32_t sw = bv_egl_swaps;
        bv_log("t+%ds: EGL swaps %u (+%u), EGL context %s, offline messages handled %u, touch events %u, back presses %u", s, (unsigned)sw, (unsigned)(sw - last_swaps),
               bv_egl_ready ? "current" : "not yet", (unsigned)bvb_hook_messages(), (unsigned)bv_touch_events(),
               (unsigned)bv_buttons_events());
        last_swaps = sw;
        {   /* update 53: GPU- or CPU-bound? time inside the swap per second, draw calls per frame, longest frame */
            extern volatile uint32_t bv_st_swap_us, bv_st_frame_max_us, bv_st_draws;
            static uint32_t last_swap_us, last_draws;
            uint32_t frames = sw - (last_swaps_for_frame ? last_swaps_for_frame : sw);
            uint32_t su = bv_st_swap_us, dr = bv_st_draws;
            if (frames > 0 && frames < 1000)
                bv_log("    frame: %u frames, swap wait %u ms of the last second, %u draws per frame, longest frame %u ms", (unsigned)frames,
                       (unsigned)((su - last_swap_us) / 1000), (unsigned)((dr - last_draws) / frames), (unsigned)(bv_st_frame_max_us / 1000));
            bv_st_frame_max_us = 0;
            last_swap_us = su; last_draws = dr; last_swaps_for_frame = sw;
        }
        { extern void bv_zones_report(void) __attribute__((weak)); if (bv_zones_report) bv_zones_report(); }   /* update 165 */
        {   /* update 144 (req. 23): where a slow frame goes - render target churn (create / destroy / reuse) and time inside vitaGL's draw calls */
            extern volatile uint32_t bv_gxm_rt_made __attribute__((weak)), bv_rtc_hits __attribute__((weak)), bv_rtc_kept __attribute__((weak)),
                bv_gxm_rt_us __attribute__((weak)), bv_st_draw_us __attribute__((weak));
            extern int bv_prof;
            static uint32_t l_made, l_hits, l_kept, l_rtus, l_dus;
            if (bv_prof && &bv_gxm_rt_made && &bv_rtc_hits && &bv_st_draw_us) {
                uint32_t m = bv_gxm_rt_made, h = bv_rtc_hits, k = bv_rtc_kept, ru = bv_gxm_rt_us, du = bv_st_draw_us;
                bv_log("    perf: render targets created %u, reused %u, kept %u, %u ms in create/destroy | %u ms inside draw calls", (unsigned)(m - l_made),
                       (unsigned)(h - l_hits), (unsigned)(k - l_kept), (unsigned)((ru - l_rtus) / 1000), (unsigned)((du - l_dus) / 1000));
                l_made = m; l_hits = h; l_kept = k; l_rtus = ru; l_dus = du;
            }
        }
        /* where does the loading time go: asset reads (I/O), ETC1 texture conversion, heap, and which asset was opened last and how long ago */
        char gpu[160] = "";
        if (bv_egl_ready) bv_gl_mem_report(gpu, sizeof gpu);
        struct mallinfo mi = mallinfo();
        uint32_t now_ms = (uint32_t)(sceKernelGetProcessTimeWide() / 1000);
        extern volatile uint32_t bv_st_tex_n, bv_st_tex_kb, bv_st_tex_us; /* shims/shim_gles.c */
        static uint32_t last_tex_n, last_etc1_n;
        extern volatile uint32_t bv_st_etc1_conv_us; /* shims/shim_gles.c */
        extern volatile uint32_t bv_st_t16_us;       /* shims/shim_gles.c, update 88 */
        if (bv_st_tex_n != last_tex_n || bv_st_etc1_n != last_etc1_n) /* only when something was uploaded in the last second */
            bv_log("    tex: %u uploads with data, %u KB in %u ms (RGBA4444 conversion %u ms) | ETC1 %u, of its %u ms %u ms converting", (unsigned)bv_st_tex_n, (unsigned)bv_st_tex_kb,
                   (unsigned)(bv_st_tex_us / 1000), (unsigned)(bv_st_t16_us / 1000), (unsigned)bv_st_etc1_n, (unsigned)(bv_st_etc1_us / 1000), (unsigned)(bv_st_etc1_conv_us / 1000));
        last_tex_n = bv_st_tex_n;
        last_etc1_n = bv_st_etc1_n;
        extern volatile uint32_t bv_st_io_calls, bv_st_io_kb_r, bv_st_io_kb_w, bv_st_io_us, bv_st_io_opens; /* shims/shim_io.c */
        static uint32_t last_io_calls;
        if (bv_st_io_calls != last_io_calls)
            bv_log("    io (game files): %u calls, %u opens, read %u KB, written %u KB, %u ms inside", (unsigned)bv_st_io_calls, (unsigned)bv_st_io_opens,
                   (unsigned)bv_st_io_kb_r, (unsigned)bv_st_io_kb_w, (unsigned)(bv_st_io_us / 1000));
        last_io_calls = bv_st_io_calls;
        bv_log("    load: assets opened %u, reads %u, %u KB in %u ms | ETC1 uploads %u, %u KB in %u ms | heap %u KB | gpu mem free/total: %s | last asset \"%s\" opened %u ms ago",
               (unsigned)bv_st_asset_opens, (unsigned)bv_st_asset_reads, (unsigned)(bv_st_asset_bytes / 1024), (unsigned)(bv_st_asset_us / 1000),
               (unsigned)bv_st_etc1_n, (unsigned)(bv_st_etc1_bytes / 1024), (unsigned)(bv_st_etc1_us / 1000), (unsigned)((unsigned)mi.uordblks / 1024), gpu,
               bv_st_last_asset, (unsigned)(now_ms - bv_st_last_asset_ms));
        extern void bv_gl_mem_check(const char *where); /* shims/shim_gles.c, update 52 */
        bv_gl_mem_check("status line");
        extern void bv_sleep_report(void); /* shims/shim_pthread.c, update 54 */
        bv_sleep_report();
        extern void bv_team_probe(void); /* loader/main.c, cycle 4 (E1) */
        bv_team_probe();
        {   /* cycle 6: stall watchdog - no frame for 3 status seconds -> print the GL ring once per stall (run 81 died inside glClear) */
            extern void bv_gl_ring_dump(const char *why);
            static uint32_t stall_last_sw, stall_n;
            static int stall_dumped;
            uint32_t swn = bv_egl_swaps;
            if (bv_egl_ready && swn > 100 && swn == stall_last_sw) {
                if (++stall_n >= 3 && !stall_dumped) { stall_dumped = 1; bv_gl_ring_dump("no frame for 3 s"); }
            } else {
                stall_n = 0;
                stall_dumped = 0;
            }
            stall_last_sw = swn;
        }
    }
}

static void (*g_pre_oncreate)(void);
void bv_gameapp_set_pre_oncreate(void (*fn)(void)) { g_pre_oncreate = fn; }

int bv_gameapp_run(int level, int run_seconds) {
    if (level <= 0) return 0;
    void *env = bv_jni_env();
    list_natives();

    uint32_t create = need("createGameMain",
                           "(Landroid/content/res/AssetManager;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JIIIFFIILandroid/app/Activity;)Ljava/lang/String;");
    if (!create) return -1;
    uint32_t data_dir = bv_jni_new_string("/data/data/" PKG);
    uint32_t cache_dir = bv_jni_new_string("/data/data/" PKG "/cache");
    uint32_t obb = bv_jni_new_string("/data/data/" PKG "/cache"); /* GameApp.b(Context): the cache directory with the most free space */
    bv_assets_selfcheck();
    bv_log("calling GameApp.createGameMain(assets, dataDir, cacheDir, cacheDir, 1 GiB free, %d, %d, %d, %.0f, %.0f, 2, 0, activity)", SCREEN_W, SCREEN_H, DENSITY_DPI,
           (double)PPI, (double)PPI);
    uint32_t r = ((fn_create)(uintptr_t)create)(env, CLASS_HANDLE, ASSETS_HANDLE, data_dir, cache_dir, obb, 1024ull * 1024 * 1024, SCREEN_W, SCREEN_H, DENSITY_DPI,
                                                bv_f2u(PPI), bv_f2u(PPI), 2, 0, ACTIVITY_HANDLE);
    bv_log("  createGameMain returned 0x%08x", r);
    if (g_pre_oncreate) g_pre_oncreate();
    call_v("nOnCreate");
    if (level == 1) return 0;

    uint32_t start = need("start", "(Ljava/lang/String;)V");
    if (start) {
        bv_log("calling GameApp.start(\"\")");
        ((fn_i)(uintptr_t)start)(env, CLASS_HANDLE, bv_jni_new_string(""));
        bv_log("  start returned");
    }
    call_v("nOnStart");
    call_v("nOnResume");
    uint32_t focus = need("nOnWindowFocusChanged", "(Z)V");
    if (focus) {
        bv_log("calling GameApp.nOnWindowFocusChanged(true)");
        ((fn_i)(uintptr_t)focus)(env, CLASS_HANDLE, 1);
    }
    if (bv_buttons_enabled) {
        const bv_native *back = find("backButtonPressed");
        if (back && strcmp(back->sig, "()Z")) bv_log("GameApp.backButtonPressed is registered with signature %s (expected ()Z)", back->sig);
        bv_buttons_start(back ? back->fn : 0, CLASS_HANDLE, bv_back_cross, bv_back_exits);
    } else {
        bv_log("buttons disabled by configuration");
    }
    if (level == 2) {
        bv_log("level 2: the game runs without a surface for %d s", run_seconds);
        idle(run_seconds);
        return 0;
    }

    bv_gl_width = SCREEN_W;
    bv_gl_height = SCREEN_H;
    uint32_t created = need("nOnSurfaceCreated", "(Landroid/view/Surface;)V");
    uint32_t changed = need("nOnSurfaceChanged", "(Landroid/view/Surface;II)V");
    if (created) {
        bv_log("calling GameApp.nOnSurfaceCreated(surface)");
        ((fn_i)(uintptr_t)created)(env, CLASS_HANDLE, SURFACE_HANDLE);
        bv_log("  nOnSurfaceCreated returned");
    }
    if (changed) {
        bv_log("calling GameApp.nOnSurfaceChanged(surface, %d, %d)", SCREEN_W, SCREEN_H);
        ((fn_surf)(uintptr_t)changed)(env, CLASS_HANDLE, SURFACE_HANDLE, SCREEN_W, SCREEN_H);
        bv_log("  nOnSurfaceChanged returned");
    }
    if (bv_touch_enabled) {
        uint32_t touch = need("nOnTouchEvent", "(IIII)V");
        if (touch) bv_touch_start(touch, CLASS_HANDLE, SCREEN_W, SCREEN_H);
    } else {
        bv_log("touch input disabled by configuration");
    }
    bv_log("level 3: the game runs with a surface for %d s", run_seconds);
    idle(run_seconds);
    return 0;
}
