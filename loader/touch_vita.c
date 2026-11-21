#include "touch_vita.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/touch.h>
#include <psp2/ctrl.h>
#include "vpad.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include "../jni/fake_jni.h"
#include "log.h"
#include "touch.h"

typedef void (*native_touch_fn)(void *env, uint32_t cls, int phase, int x, int y, int id);

static volatile int g_run;
static SceUID g_thread = -1;
static native_touch_fn g_fn;
static uint32_t g_cls;
static bvt_state g_state;
static volatile uint32_t g_events;
static uint32_t g_logged;

/* cycle 7 (J1): config vpad=1 -> the sticks / buttons become virtual touches (loader/vpad.c), sent from this thread */
int bv_vpad;
vpad_cfg bv_vpad_cfg;
static vpad_state g_vpad;
extern int bv_native_input;
extern volatile int bv_tv_watching;   /* update 174: Brawl TV - no player input */
extern volatile int bv_ime_open;      /* update 186: the Vita keyboard owns the touches while it is open */
extern volatile int bv_tv_quit_req, bv_tv_next_pending;   /* update 184: leave Brawl TV */
extern volatile uint32_t bv_np_buttons;
extern volatile int bv_np_lx, bv_np_ly, bv_np_rx, bv_np_ry;
volatile int bv_battle_left;      /* update 112 (J3): set by the offline server on 14109 / 14110, cleared at the next battle start */
uint32_t bv_battle_kind_addr;     /* libg global 0xc86a10 (battle kind: 0 none / server, 1 cave, 2-3 local bot battle), set by main.c */
static int in_battle(void) {
    static int last_kind;
    int kind = bv_battle_kind_addr ? *(volatile int *)(uintptr_t)bv_battle_kind_addr : 1;
    if (kind != last_kind) { if (last_kind == 0 && kind != 0) bv_battle_left = 0; last_kind = kind; }
    return kind != 0 && !bv_battle_left;
}
static void emit(void *ctx, int phase, int x, int y, int id) {
    (void)ctx;
    static const char *const names[] = {"down", "up", "move", "cancel"};
    g_events++;
    static int du;   /* update 187b (user: top-left taps do nothing): every down / up is logged (first 1500) */
    if (g_logged < 24 || bv_trace_level >= 2 || (phase <= 1 && du++ < 1500)) {
        g_logged++;
        bv_log("touch: %s id %d at %d,%d -> nOnTouchEvent(%d, %d, %d, %d)", names[phase & 3], id, x, y, phase, x, y, id);
    }
    g_fn(bv_jni_env(), g_cls, phase, x, y, id);
}

/* update 114 (tooling): tap.req script from the PC (see tools note in this file's history: notes/.../tap_run.py) */
#define BV_TAP_ID 26
static void tap_script(void) {
    SceIoStat st;
    if (sceIoGetstat(BV_DATA_DIR "/tap.req", &st) < 0) return;
    char buf[1024];
    int n = 0;
    SceUID fd = sceIoOpen(BV_DATA_DIR "/tap.req", SCE_O_RDONLY, 0);
    if (fd >= 0) { n = sceIoRead(fd, buf, sizeof buf - 1); sceIoClose(fd); }
    sceIoRemove(BV_DATA_DIR "/tap.req");
    if (n <= 0) return;
    buf[n] = 0;
    int steps = 0;
    for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        int a, b, c, d, e;
        if (sscanf(line, "wait %d", &a) == 1) { sceKernelDelayThread((a > 0 ? a : 0) * 1000); }
        else if (sscanf(line, "drag %d %d %d %d %d", &a, &b, &c, &d, &e) == 5) {
            int k, parts = 8;
            emit(NULL, 0, a, b, BV_TAP_ID);
            for (k = 1; k <= parts; k++) { sceKernelDelayThread((e > 0 ? e : 200) * 1000 / parts); emit(NULL, 2, a + (c - a) * k / parts, b + (d - b) * k / parts, BV_TAP_ID); }
            emit(NULL, 1, c, d, BV_TAP_ID);
        } else {
            int m = sscanf(line, "%d %d %d", &a, &b, &c);
            if (m < 2) continue;
            emit(NULL, 0, a, b, BV_TAP_ID);
            sceKernelDelayThread((m == 3 && c > 0 ? c : 80) * 1000);
            emit(NULL, 1, a, b, BV_TAP_ID);
        }
        steps++;
        sceKernelDelayThread(30 * 1000);
    }
    bv_log("tap.req: %d steps played", steps);
}

static int poll_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    SceTouchData data;
    while (g_run) {
        if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &data, 1) >= 0) {
            bvt_report rep[BVT_MAX];
            int n = 0;
            for (unsigned i = 0; i < data.reportNum && n < BVT_MAX; i++) {
                rep[n].id = data.report[i].id;
                rep[n].x = data.report[i].x;
                rep[n].y = data.report[i].y;
                n++;
            }
            {   /* update 187b: raw panel position of each new finger, and touches kept from the game */
                static int lastn, said;
                if (n > lastn && said < 1500) { said++; bv_log("touch: panel raw %d,%d (%d fingers)%s", rep[n - 1].x, rep[n - 1].y, n, bv_ime_open ? " [keyboard open: not passed]" : (bv_tv_watching && in_battle()) ? " [Brawl TV: not passed]" : ""); }
                lastn = n;
            }
            if (!bv_ime_open && (!bv_tv_watching || !in_battle())) bvt_update(&g_state, rep, n, emit, NULL);   /* update 184: menus / result screen stay usable */
            else for (int i = 0; i < n; i++) if (rep[i].x < 240 && rep[i].y < 180) bv_tv_quit_req = 1;   /* top-left corner: leave the watched match */
        }
        if (bv_vpad) {
            SceCtrlData pad;
            if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0) {
                static int was_battle = -1;
                int b = in_battle();
                if (b != was_battle) { vpad_release_all(&g_vpad, emit, NULL); bv_log("vpad: %s controls", b ? "battle" : "home screen"); was_battle = b; }
                bv_np_buttons = pad.buttons; bv_np_lx = pad.lx; bv_np_ly = pad.ly; bv_np_rx = pad.rx; bv_np_ry = pad.ry;   /* update 167 */
                if ((bv_tv_watching || bv_tv_next_pending) && (pad.buttons & SCE_CTRL_CIRCLE)) bv_tv_quit_req = 1;   /* update 184: Circle leaves Brawl TV */
                if (bv_tv_watching || bv_ime_open) { /* update 174: watching Brawl TV; update 186: the keyboard dialog has the buttons */ }
                else if (b && (!bv_native_input || bv_native_input == 3)) vpad_update(&g_vpad, pad.buttons, pad.lx, pad.ly, pad.rx, pad.ry, emit, NULL);
                else if (b && bv_native_input == 2) vpad_move_only(&g_vpad, pad.buttons, pad.lx, pad.ly, emit, NULL);   /* update 172 */
                else vpad_home(&g_vpad, pad.buttons, emit, NULL);
                /* update 144 (req. 12): the rear panel is never read: it cannot shoot (update 112's vpad_rear is removed) */
            }
        }
        { static unsigned tick; if (g_fn && !(++tick % 60)) tap_script(); }   /* update 114: about twice a second */
        sceKernelDelayThread(8 * 1000); /* about 120 Hz, the panel's sampling rate */
    }
    bvt_cancel_all(&g_state, emit, NULL);
    if (bv_vpad) vpad_release_all(&g_vpad, emit, NULL);
    return 0;
}

int bv_touch_start(uint32_t native_fn, uint32_t class_handle, int width, int height) {
    if (g_thread >= 0) return 0;
    SceTouchPanelInfo info;
    int ax0 = 0, ay0 = 0, ax1 = 1919, ay1 = 1087;
    memset(&info, 0, sizeof info);
    if (sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &info) >= 0 && info.maxAaX > info.minAaX && info.maxAaY > info.minAaY) {
        ax0 = info.minAaX;
        ay0 = info.minAaY;
        ax1 = info.maxAaX;
        ay1 = info.maxAaY;
    } else {
        bv_log("touch: sceTouchGetPanelInfo failed, assuming a 1920x1088 panel");
    }
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    if (bv_vpad) {
        sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
        sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_STOP);   /* update 144: rear panel off */
        if (width != 960) {   /* update 167: vpad layout is in 960x544 units */
            int *xs[] = {&bv_vpad_cfg.move_x, &bv_vpad_cfg.attack_x, &bv_vpad_cfg.super_x, &bv_vpad_cfg.play_x, &bv_vpad_cfg.shop_x, &bv_vpad_cfg.brawlers_x, &bv_vpad_cfg.radius, &bv_vpad_cfg.aim_radius};
            int *ys[] = {&bv_vpad_cfg.move_y, &bv_vpad_cfg.attack_y, &bv_vpad_cfg.super_y, &bv_vpad_cfg.play_y, &bv_vpad_cfg.shop_y, &bv_vpad_cfg.brawlers_y};
            for (unsigned k = 0; k < sizeof xs / sizeof xs[0]; k++) *xs[k] = *xs[k] * width / 960;
            for (unsigned k = 0; k < sizeof ys / sizeof ys[0]; k++) *ys[k] = *ys[k] * height / 544;
            if (bv_vpad_cfg.gadget_x >= 0) { bv_vpad_cfg.gadget_x = bv_vpad_cfg.gadget_x * width / 960; bv_vpad_cfg.gadget_y = bv_vpad_cfg.gadget_y * height / 544; }
        }
        vpad_init(&g_vpad, &bv_vpad_cfg);
        bv_log("vpad: Vita controls -> touches: move %d,%d attack %d,%d super %d,%d gadget %d,%d radius %d dead %d (left stick / d-pad move, right stick aim, R / Cross attack, "
               "L super, L + right stick aimed super, Square gadget)", bv_vpad_cfg.move_x, bv_vpad_cfg.move_y, bv_vpad_cfg.attack_x, bv_vpad_cfg.attack_y, bv_vpad_cfg.super_x,
               bv_vpad_cfg.super_y, bv_vpad_cfg.gadget_x, bv_vpad_cfg.gadget_y, bv_vpad_cfg.radius, bv_vpad_cfg.dead);
    }
    bvt_init(&g_state, width, height, ax0, ay0, ax1, ay1);
    g_fn = (native_touch_fn)(uintptr_t)native_fn;
    g_cls = class_handle;
    g_run = 1;
    g_thread = sceKernelCreateThread("bv_touch", poll_thread, 0x40, 256 * 1024, 0, 0, NULL);
    if (g_thread < 0) {
        bv_log("touch: cannot create the polling thread (0x%08x)", (unsigned)g_thread);
        g_run = 0;
        return -1;
    }
    sceKernelStartThread(g_thread, 0, NULL);
    bv_log("touch: front panel %d,%d..%d,%d mapped to %dx%d, polling at ~120 Hz", ax0, ay0, ax1, ay1, width, height);
    return 0;
}

void bv_touch_stop(void) {
    if (g_thread < 0) return;
    g_run = 0;
    sceKernelWaitThreadEnd(g_thread, NULL, NULL);
    sceKernelDeleteThread(g_thread);
    g_thread = -1;
}

uint32_t bv_touch_events(void) { return g_events; }
