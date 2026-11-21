#include "buttons_vita.h"

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "../jni/fake_jni.h"
#include "buttons.h"
#include "log.h"

typedef uint32_t (*native_back_fn)(void *env, uint32_t cls);

static native_back_fn g_back;
static uint32_t g_cls;
static int g_exits;
static volatile uint32_t g_events;
static bvbtn_state g_state;

static int pad_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    SceCtrlData pad;
    for (;;) {
        if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0) {
            uint32_t now = (uint32_t)(sceKernelGetProcessTimeWide() / 1000);
            int ev = bvbtn_update(&g_state, pad.buttons, now);
            if (ev & BVBTN_QUIT) {
                bv_log("buttons: quit combo held: ending the app");
                sceKernelDelayThread(200 * 1000);
                sceKernelExitProcess(0);
            }
            if (ev & BVBTN_BACK) {
                g_events++;
                uint32_t r = g_back(bv_jni_env(), g_cls) & 0xff; /* jboolean */
                bv_log("buttons: back pressed -> backButtonPressed() = %u%s", (unsigned)r, r ? " (handled by the game)" : " (not handled)");
                if (!r && g_exits) {
                    bv_log("buttons: the game did not handle back: ending the app like Android's activity does");
                    sceKernelDelayThread(200 * 1000);
                    sceKernelExitProcess(0);
                }
            }
        }
        sceKernelDelayThread(16 * 1000);
    }
    return 0;
}

int bv_buttons_start(uint32_t back_fn, uint32_t class_handle, int back_is_cross, int back_exits) {
    static SceUID th = -1;
    if (th >= 0) return 0;
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG); /* cycle 7: analog for the virtual pad (buttons still read) */
    g_back = (native_back_fn)(uintptr_t)back_fn;
    g_cls = class_handle;
    g_exits = back_exits;
    uint32_t back_mask = back_is_cross ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
    bvbtn_init(&g_state, back_fn ? back_mask : 0, SCE_CTRL_START | SCE_CTRL_SELECT, 2000);
    th = sceKernelCreateThread("bv_buttons", pad_thread, 0x40, 256 * 1024, 0, 0, NULL);
    if (th < 0) {
        bv_log("buttons: cannot create the polling thread (0x%08x)", (unsigned)th);
        return -1;
    }
    sceKernelStartThread(th, 0, NULL);
    bv_log("buttons: %s = back%s, Start + Select held 2 s = quit", back_is_cross ? "Cross" : "Circle", back_fn ? "" : " (backButtonPressed is not registered: disabled)");
    return 0;
}

uint32_t bv_buttons_events(void) { return g_events; }
