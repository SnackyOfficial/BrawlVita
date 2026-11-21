/* EGL entry points of libg.so (Build B creates its own context on a render thread). There is no EGL on the console: the calls
 * succeed with fake handles, and vitaGL is started at the first eglGetDisplay (on the thread that will render).
 * eglSwapBuffers presents through vitaGL. UNVERIFIED ON HARDWARE.
 */
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/display.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vitaGL.h>

#include "../gfx/gl_init.h"
#include "../loader/log.h"
#include "shim.h"
#include "texshrink.h"

#define EGL_TRUE 1
#define EGL_FALSE 0
#ifndef EGL_NO_DISPLAY
#define EGL_NO_DISPLAY 0
#endif

#define EGL_ALPHA_SIZE 0x3021
#define EGL_BLUE_SIZE 0x3022
#define EGL_GREEN_SIZE 0x3023
#define EGL_RED_SIZE 0x3024
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_CONFIG_ID 0x3028
#define EGL_SAMPLES 0x3031
#define EGL_SAMPLE_BUFFERS 0x3032
#define EGL_SURFACE_TYPE 0x3033
#define EGL_NATIVE_VISUAL_ID 0x302E
#define EGL_RENDERABLE_TYPE 0x3040

volatile uint32_t bv_egl_swaps;
volatile uint32_t bv_st_swap_us, bv_st_frame_max_us; /* update 53: time inside swaps; longest frame (status line resets it) */
volatile int bv_egl_ready;
static int g_gl_started;
static int g_gl_failed;
int bv_gl_width = 960, bv_gl_height = 544;

static void boot_notice(void);
static void *w_eglGetDisplay(void *native) {
    (void)native;
    if (!g_gl_started && !g_gl_failed) {
        int r = bv_gl_init(bv_gl_width, bv_gl_height);
        if (r) {
            bv_log("EGL: graphics could not start (%d): eglGetDisplay returns EGL_NO_DISPLAY", r);
            g_gl_failed = 1;
        } else {
            g_gl_started = 1;
            boot_notice();   /* update 168b: the notice stays on screen until the game's first swap (the Supercell logo) */
        }
    }
    BV_TRACE(1, "eglGetDisplay -> %s", g_gl_started ? "fake display" : "EGL_NO_DISPLAY");
    return g_gl_started ? (void *)1 : (void *)EGL_NO_DISPLAY;
}

static int w_eglInitialize(void *dpy, int *major, int *minor) {
    (void)dpy;
    if (major) *major = 1;
    if (minor) *minor = 4;
    return EGL_TRUE;
}

static int w_eglChooseConfig(void *dpy, const int *attribs, void **configs, int size, int *num) {
    (void)dpy; (void)attribs;
    if (configs && size > 0) configs[0] = (void *)1;
    if (num) *num = 1;
    BV_TRACE(1, "eglChooseConfig -> 1 config");
    return EGL_TRUE;
}

static int w_eglGetConfigAttrib(void *dpy, void *cfg, int attr, int *value) {
    (void)dpy; (void)cfg;
    int v = 0;
    switch (attr) {
    case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE: case EGL_ALPHA_SIZE: v = 8; break;
    case EGL_DEPTH_SIZE: v = 24; break;
    case EGL_STENCIL_SIZE: v = 8; break;
    case EGL_SURFACE_TYPE: v = 4; break;       /* EGL_WINDOW_BIT */
    case EGL_RENDERABLE_TYPE: v = 0x44; break; /* ES2 | ES3 */
    case EGL_NATIVE_VISUAL_ID: case EGL_CONFIG_ID: v = 1; break;
    case EGL_SAMPLES: case EGL_SAMPLE_BUFFERS: v = 0; break;
    default: break;
    }
    if (value) *value = v;
    return EGL_TRUE;
}

static void *w_eglCreateContext(void *dpy, void *cfg, void *share, const int *attribs) {
    (void)dpy; (void)cfg; (void)share; (void)attribs;
    BV_TRACE(1, "eglCreateContext -> fake context");
    return (void *)2;
}
static void *w_eglCreateWindowSurface(void *dpy, void *cfg, void *win, const int *attribs) {
    (void)dpy; (void)cfg; (void)win; (void)attribs;
    BV_TRACE(1, "eglCreateWindowSurface -> fake surface");
    return (void *)3;
}
static int w_eglMakeCurrent(void *dpy, void *draw, void *read, void *ctx) {
    (void)dpy; (void)draw; (void)read; (void)ctx;
    bv_egl_ready = 1;
    BV_TRACE(1, "eglMakeCurrent");
    return EGL_TRUE;
}
static int w_eglSurfaceAttrib(void *dpy, void *surf, int attr, int value) {
    (void)dpy; (void)surf; (void)attr; (void)value;
    return EGL_TRUE;
}
/* Screenshots (config shot_frame=N, up to 4 lines): the screen is read back right before swap N and saved as ux0:data/BrawlVita/shot_N.tga (960x544, 32-bit,
 * rows bottom-up as GL returns them). Lets visual problems be checked from the PC (file download) instead of by photo. */
int bv_shot_frames[4];
int bv_shot_n;
static void take_shot(unsigned n) {
    if (fbo_current() != 0) {
        bv_log("screenshot %u skipped: a framebuffer object is bound at the swap", n);
        return;
    }
    const int W = bv_gl_width, H = bv_gl_height;   /* update 167: screen_res */
    uint8_t *px = (uint8_t *)malloc((size_t)W * H * 4);
    if (!px) {
        bv_log("screenshot %u: no memory", n);
        return;
    }
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (size_t i = 0; i < (size_t)W * H; i++) { uint8_t t = px[4 * i]; px[4 * i] = px[4 * i + 2]; px[4 * i + 2] = t; } /* RGBA -> BGRA */
    uint8_t hdr[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, (uint8_t)W, (uint8_t)(W >> 8), (uint8_t)H, (uint8_t)(H >> 8), 32, 8};
    char path[96];
    snprintf(path, sizeof path, BV_DATA_DIR "/shot_%u.tga", n);
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, hdr, sizeof hdr);
        sceIoWrite(fd, px, (SceSize)((size_t)W * H * 4));
        sceIoClose(fd);
        bv_log("screenshot: %s written (swap %u)", path, n);
    } else {
        bv_log("screenshot: cannot create %s (0x%08x)", path, (unsigned)fd);
    }
    free(px);
}

volatile uint32_t bv_news_btn;   /* update 146 (T18): the home NEWS button, stored by the news_off veneer (loader/main.c) */
volatile uint32_t bv_home_obj, bv_fn_str_ctor, bv_fn_str_dtor, bv_fn_child;   /* update 149: home object + game functions (loader/main.c) */
static int ptr_ok(uint32_t p) { return p && !(p & 3) && p >= 0x80000000u && p < 0xc0000000u; }
/* update 184 (run 222: after a battle bv_home_obj stayed 0 -> Brawl TV 'no home object'): the popup manager (getter libg+0x683dec, ui.md 4) keeps the
 * current screen at +0xc8 / +0xc4 (0x63dd0); one with the home's vtable is the home screen. Game thread only. */
volatile uint32_t bv_fn_popup_mgr;
static uint32_t g_home_vt;
uint32_t bv_home_find(void) {
    uint32_t h = bv_home_obj;
    if (ptr_ok(h)) return h;
    if (!g_home_vt || !bv_fn_popup_mgr) return 0;
    uint32_t mgr = ((uint32_t (*)(void))(uintptr_t)bv_fn_popup_mgr)();
    if (!ptr_ok(mgr)) return 0;
    for (int k = 0; k < 2; k++) {
        uint32_t s = *(volatile uint32_t *)(uintptr_t)(mgr + (k ? 0xc4u : 0xc8u));
        if (ptr_ok(s) && *(volatile uint32_t *)(uintptr_t)s == g_home_vt) {
            bv_home_obj = s;
            bv_log("home: home screen %p found again through the popup manager (+0x%x)", (void *)(uintptr_t)s, k ? 0xc4u : 0xc8u);
            return s;
        }
    }
    return 0;
}
/* update 149 (user T18): the NEWS clip, looked up by name on the captured home object once (the game's own lookup), then hidden on every frame */
static void news_clip_rehide(void) {
    static uint32_t home, home_vt, clip, clip_vt;
    if (!bv_home_obj) { static unsigned k; if (!(++k & 15)) bv_home_find(); }   /* update 184 */
    uint32_t h = bv_home_obj;
    if (!ptr_ok(h) || !bv_fn_child || !bv_fn_str_ctor || !bv_fn_str_dtor) return;
    if (h != home) {   /* a new home screen: look the clip up again */
        home = h;
        home_vt = *(volatile uint32_t *)(uintptr_t)h;
        if (!g_home_vt) g_home_vt = home_vt;   /* update 184 */
        uint32_t str[8] = {0};
        ((void (*)(void *, const char *))(uintptr_t)bv_fn_str_ctor)(str, "button_news");
        clip = ((uint32_t (*)(uint32_t, void *))(uintptr_t)bv_fn_child)(h, str);
        ((void (*)(void *))(uintptr_t)bv_fn_str_dtor)(str);
        clip_vt = ptr_ok(clip) ? *(volatile uint32_t *)(uintptr_t)clip : 0;
        bv_log("news_off: home %p: button_news clip %p", (void *)(uintptr_t)h, (void *)(uintptr_t)clip);
    }
    if (*(volatile uint32_t *)(uintptr_t)h != home_vt) { bv_home_obj = 0; home = 0; clip = 0; return; }   /* home screen gone */
    if (!ptr_ok(clip) || *(volatile uint32_t *)(uintptr_t)clip != clip_vt) return;
    ((volatile uint8_t *)(uintptr_t)clip)[4] = 0;   /* visible (the byte settings_clean clears, VERIFIED run 139) */
}
void bv_input_probe_tick(int in_battle) __attribute__((weak));
void bv_tv_tick(void) __attribute__((weak));
void bv_native_tick(void) __attribute__((weak));
void bv_homecard_tick(void) __attribute__((weak));
extern uint32_t bv_battle_kind_addr __attribute__((weak));
static void news_rehide(void) {
    news_clip_rehide();
    if (bv_tv_tick) bv_tv_tick();   /* update 163: Brawl TV start on the game thread */
    if (bv_native_tick) bv_native_tick();   /* update 167: native controls */
    if (bv_homecard_tick) bv_homecard_tick();   /* update 167: home card refresh */
    if (bv_input_probe_tick && &bv_battle_kind_addr && bv_battle_kind_addr) bv_input_probe_tick(*(volatile int *)(uintptr_t)bv_battle_kind_addr != 0);   /* update 162 */
    static uint32_t btn, vt;
    uint32_t b = bv_news_btn;
    if (!b || (b & 3) || b < 0x80000000u || b >= 0xc0000000u) return;
    if (b != btn) { btn = b; vt = *(volatile uint32_t *)(uintptr_t)b; bv_log("news_off: home NEWS button %p remembered (vtable %08x)", (void *)(uintptr_t)b, (unsigned)vt); }
    if (*(volatile uint32_t *)(uintptr_t)b != vt) { bv_news_btn = 0; btn = 0; return; }   /* the object is gone (another screen): forget it */
    volatile uint8_t *o = (volatile uint8_t *)(uintptr_t)b;
    o[4] = 0;
    o[0x34] = 0;
}
/* update 168: boot loading notice (user: show it on the boot screen, after the vitaGL screen). Drawn with scissored clears (no shader, no
 * texture) on the default framebuffer right before the swap, until the home screen object exists. Our own 5x7 font, only the glyphs used. */
int bv_loading_notice = 1;
static const struct { char c; uint8_t r[7]; } k_glyph[] = {
    {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}}, {'D', {30, 17, 17, 17, 17, 17, 30}}, {'E', {31, 16, 16, 30, 16, 16, 31}},
    {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}}, {'I', {14, 4, 4, 4, 4, 4, 14}}, {'L', {16, 16, 16, 16, 16, 16, 31}},
    {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 25, 21, 19, 17, 17, 17}}, {'O', {14, 17, 17, 17, 17, 17, 14}}, {'S', {15, 16, 16, 14, 1, 1, 30}},
    {'T', {31, 4, 4, 4, 4, 4, 4}}, {'K', {17, 18, 20, 24, 20, 18, 17}}, {'R', {30, 17, 17, 30, 20, 18, 17}}, {'U', {17, 17, 17, 17, 17, 17, 14}}, {'W', {17, 17, 17, 21, 21, 21, 10}}, {'P', {30, 17, 17, 30, 16, 16, 16}},
    {'1', {4, 12, 4, 4, 4, 4, 14}}, {'2', {14, 17, 1, 2, 4, 8, 31}}, {'-', {0, 0, 0, 14, 0, 0, 0}}, {'.', {0, 0, 0, 0, 0, 12, 12}},
};
#define BV_NOTICE_TEXT "LOADING THE GAME - THIS TAKES ABOUT A MINUTE"
/* update 170: the same notice from the first second, before vitaGL exists: a 960x544 32-bit framebuffer in CDRAM handed to sceDisplay.
 * vitaGL replaces the display buffer when it starts (eglGetDisplay -> bv_gl_init); the block is freed after boot_notice's swap. */
static SceUID g_early_fb = -1;
void bv_early_notice(void) {
    if (!bv_loading_notice) return;
    extern int bv_screen_w, bv_screen_h;   /* update 181: same mode and layout as the vitaGL notice (no jump when vitaGL takes over) */
    const int W = bv_screen_w, H = bv_screen_h, P = W == 960 ? 1024 : W == 720 ? 768 : 640;
    g_early_fb = sceKernelAllocMemBlock("bv_notice_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 0x240000, NULL);   /* 1024 x 544 x 4 = 2.13 MB, rounded up to 256 KB (update 170 crashed with 2 MB) */
    if (g_early_fb < 0) { bv_log("early notice: no CDRAM block (0x%08x)", (unsigned)g_early_fb); return; }
    uint32_t *fb = NULL;
    sceKernelGetMemBlockBase(g_early_fb, (void **)&fb);
    memset(fb, 0, (size_t)P * H * 4);
    const char *txt = BV_NOTICE_TEXT;
    int n = (int)strlen(txt), px = W >= 900 ? 3 : 2, cw = 6 * px, x0 = (W - n * cw) / 2, y0 = H / 2 - 4 * px;
    y0 = H - y0 - 7 * px;   /* GL draws bottom-up, the CPU buffer top-down */
    for (int i = 0; i < n; i++) {
        const uint8_t *g = NULL;
        for (unsigned k = 0; k < sizeof k_glyph / sizeof k_glyph[0]; k++) if (k_glyph[k].c == txt[i]) { g = k_glyph[k].r; break; }
        if (!g) continue;
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (g[row] & (16 >> col))
                    for (int yy = 0; yy < px; yy++)
                        for (int xx = 0; xx < px; xx++) fb[(y0 + row * px + yy) * P + x0 + i * cw + col * px + xx] = 0xffffffffu;
    }
    SceDisplayFrameBuf d;
    memset(&d, 0, sizeof d);
    d.size = sizeof d; d.base = fb; d.pitch = P; d.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8; d.width = W; d.height = H;
    int r = sceDisplaySetFrameBuf(&d, SCE_DISPLAY_SETBUF_NEXTFRAME);
    bv_log("early notice: shown before vitaGL (0x%08x)", (unsigned)r);
}
static void notice_rect(int x, int y, int w, int h) { glScissor(x, y, w, h); glClear(GL_COLOR_BUFFER_BIT); }
/* update 168b (user): only in the black gap between vitaGL starting and the game's first frame (run 200: 14 s -> 43 s) - drawn ONCE on the
 * game thread right after vitaGL starts and presented with one swap; the display keeps it until the game swaps its first frame. */
static void boot_notice_draw(void) {
    glViewport(0, 0, bv_gl_width, bv_gl_height);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    const char *txt = BV_NOTICE_TEXT;
    int n = (int)strlen(txt), px = bv_gl_width >= 900 ? 3 : 2, cw = 6 * px, W = n * cw, x0 = (bv_gl_width - W) / 2, y0 = bv_gl_height / 2 - 4 * px;
    glEnable(GL_SCISSOR_TEST);
    glClearColor(0.f, 0.f, 0.f, 0.6f);
    notice_rect(x0 - 10, y0 - 8, W + 20, 7 * px + 16);
    glClearColor(1.f, 1.f, 1.f, 1.f);
    for (int i = 0; i < n; i++) {
        const uint8_t *g = NULL;
        for (unsigned k = 0; k < sizeof k_glyph / sizeof k_glyph[0]; k++) if (k_glyph[k].c == txt[i]) { g = k_glyph[k].r; break; }
        if (!g) continue;
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (g[row] & (16 >> col)) notice_rect(x0 + i * cw + col * px, y0 + (6 - row) * px, px, px);
    }
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.f, 0.f, 0.f, 1.f);
}
static void boot_notice(void) {   /* update 181: drawn into every display buffer (vitaGL triple-buffers), so no buffer flashes stale content */
    if (!bv_loading_notice) return;
    for (int k = 0; k < 3; k++) { boot_notice_draw(); bv_gl_swap(); }
    { extern volatile int bv_notice_wipe; bv_notice_wipe = 1; }   /* update 184: the game's first frames do not clear: shim_gles wipes them */
    if (g_early_fb >= 0) { sceKernelFreeMemBlock(g_early_fb); g_early_fb = -1; }   /* vitaGL owns the display now */
    bv_log("boot notice shown (until the game's first frame)");
}
extern int bv_thread_cores __attribute__((weak));
static int w_eglSwapBuffers(void *dpy, void *surf) {
    (void)dpy; (void)surf;
    bv_egl_swaps++;
    if (bv_egl_swaps == 1 && &bv_thread_cores && bv_thread_cores == 3) {   /* update 166: the frame thread gets core 0 to itself */
        int r = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
        bv_log("cores=3: game frame thread 0x%x on core 0 alone -> 0x%x", (unsigned)sceKernelGetThreadId(), (unsigned)r);
    }
    news_rehide();
    {   /* update 174: display depth/stencil kept only outside battles (ds_store=1; run 205: black battle ground) */
        extern void vgl_bv_ds_force(int on) __attribute__((weak));
        extern int bv_ds_store;
        extern uint32_t bv_battle_kind_addr __attribute__((weak));
        static int last = -1;
        int in_battle = &bv_battle_kind_addr && bv_battle_kind_addr && *(volatile int *)(uintptr_t)bv_battle_kind_addr != 0;
        int want = bv_ds_store && !in_battle;
        if (vgl_bv_ds_force && want != last) { vgl_bv_ds_force(want); if (last != -1 || want) bv_log("ds_store: display depth/stencil %s", want ? "kept (menus)" : "not kept (battle)"); last = want; }
    }
    for (int i = 0; i < bv_shot_n && i < 4; i++)
        if ((uint32_t)bv_shot_frames[i] == bv_egl_swaps) take_shot((unsigned)bv_egl_swaps);
    if (bv_egl_swaps <= 3 || !(bv_egl_swaps & (bv_egl_swaps - 1))) bv_log("EGL: swap #%u", (unsigned)bv_egl_swaps);
    /* update 53: time inside the swap (vitaGL: GPU finish / vsync wait) vs the rest of the frame (game + our wrappers on the CPU): says whether a slow
     * battle is GPU- or CPU-bound. Shown on the status line as "frame:". */
    uint64_t t0 = sceKernelGetProcessTimeWide();
    bv_gl_swap();
    if (bv_egl_swaps == 1 && bv_loading_notice) {
        /* update 184b (runs 225-229: the frames read back with the notice although they were cleared with GL first): the display buffers themselves
         * are zeroed once the game's first frame is out (vitaGL globals: gxm_color_surfaces_addr / gxm_display_buffer_count / DISPLAY_STRIDE x HEIGHT) */
        extern void *gxm_color_surfaces_addr[5] __attribute__((weak));
        extern uint8_t gxm_display_buffer_count __attribute__((weak));
        extern int DISPLAY_STRIDE __attribute__((weak)), DISPLAY_HEIGHT __attribute__((weak));
        if (&gxm_display_buffer_count && &DISPLAY_STRIDE && &DISPLAY_HEIGHT && gxm_color_surfaces_addr) {
            glFinish();
            int nb = gxm_display_buffer_count > 5 ? 5 : gxm_display_buffer_count;
            for (int k = 0; k < nb; k++)
                if (gxm_color_surfaces_addr[k]) memset(gxm_color_surfaces_addr[k], 0, (size_t)DISPLAY_STRIDE * (size_t)DISPLAY_HEIGHT * 4u);
            bv_log("boot notice: %d display buffers zeroed after the game's first frame (%d x %d)", nb, DISPLAY_STRIDE, DISPLAY_HEIGHT);
        }
    }
    uint64_t t1 = sceKernelGetProcessTimeWide();
    static uint64_t last_end;
    __sync_fetch_and_add(&bv_st_swap_us, (uint32_t)(t1 - t0));
    if (last_end) {
        uint32_t frame = (uint32_t)(t1 - last_end);
        if (frame > bv_st_frame_max_us) bv_st_frame_max_us = frame;
    }
    last_end = t1;
    return EGL_TRUE;
}

const bv_shim bv_shims_egl[] = {
    SHIM(eglGetDisplay), SHIM(eglInitialize), SHIM(eglChooseConfig), SHIM(eglGetConfigAttrib), SHIM(eglCreateContext),
    SHIM(eglCreateWindowSurface), SHIM(eglMakeCurrent), SHIM(eglSurfaceAttrib), SHIM(eglSwapBuffers),
    SHIM_END};
