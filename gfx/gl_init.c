#include "gl_init.h"

#include <psp2/io/stat.h>
#include <stdio.h>
#include <vitaGL.h>

#include "../loader/log.h"

/* update 87 (ISSUES H1): the pair-keyed shader cache is used only with config vgl_cache=1. vgl_bv_shader_cache exists only in our vitaGL build
 * (tools/vitagl_paircache.py, USE_CACHE_VITAGL=1); weak, so the stock library links and the switch then does nothing. */
extern int vgl_bv_shader_cache __attribute__((weak));
int bv_vgl_cache = 1; /* default since update 92 (runs 92-99, training cave included) */
int bv_fbo_scenes;   /* config fbo_scenes (0 = vitaGL default 1; update 92 experiment A5): scenes per frame of FBO render targets */

static int file_exists(const char *path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

int bv_ds_store = 1;   /* update 172; update 175: default (icons inside their frames, user pass) */
int bv_gl_init(int width, int height) {
    int ok = (width == 960 && height == 544) || (width == 720 && height == 408) || (width == 640 && height == 368);
    if (!ok) return -2;
    /* vitaGL compiles shaders at run time through SceShaccCg, which needs libshacccg.suprx from the console. */
    if (!file_exists("ur0:/data/libshacccg.suprx") && !file_exists("ur0:data/libshacccg.suprx") &&
        !file_exists("ur0:/data/external/libshacccg.suprx")) {
        bv_log("graphics: libshacccg.suprx not found in ur0:data (needed by vitaGL)");
        return -1;
    }
    sceIoMkdir(BV_DATA_DIR "/shader_cache", 0777);
    vglSetShaderCachePath(BV_DATA_DIR "/shader_cache");
    if (&vgl_bv_shader_cache) {
        vgl_bv_shader_cache = bv_vgl_cache;
        bv_log("graphics: vitaGL with the pair-keyed shader cache, cache %s (vgl_cache=%d)", bv_vgl_cache ? "ON" : "off", bv_vgl_cache);
    } else if (bv_vgl_cache) {
        bv_log("graphics: vgl_cache=1 but this build links the stock vitaGL (no cache)");
    }
    if (bv_fbo_scenes > 0) {
        int n = bv_fbo_scenes > 8 ? 8 : bv_fbo_scenes;
        vglSetupRenderTargetScenesNum(1, (uint8_t)n);
        bv_log("graphics: framebuffer render targets get %d scenes per frame (fbo_scenes)", n);
    }
    {   /* update 172 (icons fix B): keep the display depth/stencil across scenes (tools/vitagl_dsstore.py) */
        extern int vgl_bv_ds_store __attribute__((weak));
        extern int bv_ds_store;
        /* update 176: no start-up force mode / mask init (runs 209-211: with it the battle ground stayed black even after the runtime switch-off);
         * the display depth/stencil is kept only while in menus by vgl_bv_ds_force (shims/shim_egl.c) */
        if (&vgl_bv_ds_store) { vgl_bv_ds_store = 0; bv_log("graphics: ds_store=%d (display depth/stencil kept in menus only, switched per frame)", bv_ds_store); }
        else if (bv_ds_store) bv_log("graphics: ds_store=1 but this vitaGL has no vgl_bv_ds_store");
    }
    vglInitExtended(0, width, height, 0x1000000, SCE_GXM_MULTISAMPLE_NONE);
    bv_log("graphics: vitaGL started at %dx%d", width, height);
    return 0;
}

int bv_ime_frame(void);   /* gfx/ime_vita.c (update 186): 1 while the Vita keyboard dialog is up */
void bv_gl_swap(void) { vglSwapBuffers(bv_ime_frame() ? GL_TRUE : GL_FALSE); }

/* Free/total texture memory of vitaGL's pools, for the status line: a texture upload into a pool that is full is what ended the run of 2026-10-26 (memcpy to address 0
 * inside glTexSubImage2D while ui_tex.sc was being loaded). */
void bv_gl_mem_report(char *out, size_t n) {
    static const struct { int type; const char *name; } pools[] = {
        {VGL_MEM_VRAM, "vram"}, {VGL_MEM_RAM, "ram"}, {VGL_MEM_PHYCONT, "phycont"}, {VGL_MEM_BUDGET, "cdlg"},
    };
    size_t o = 0;
    out[0] = 0;
    for (unsigned i = 0; i < sizeof pools / sizeof pools[0] && o + 40 < n; i++)
        o += (size_t)snprintf(out + o, n - o, "%s%s %u/%u MB", i ? ", " : "", pools[i].name, (unsigned)(vglMemFree((vglMemType)pools[i].type) >> 20),
                              (unsigned)(vglMemTotal((vglMemType)pools[i].type) >> 20));
}
