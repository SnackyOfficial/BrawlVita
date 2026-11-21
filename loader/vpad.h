/* Vita physical controls -> virtual touches (cycle 7, ISSUES J1). Pure logic (host-tested in tests/host_vpad_test.c); loader/touch_vita.c feeds it
 * the pad state from its polling thread so every touch event reaches the game from one thread.
 * Default layout from the battle HUD (run 110 screenshot, 960x544): movement joystick bottom left, attack joystick right, super button left of it.
 *   left stick / d-pad  -> movement finger held on the movement joystick, dragged in the stick direction
 *   right stick         -> attack finger on the attack joystick (aim by dragging, release fires); while L is held it drags the SUPER instead
 *   R or Cross          -> quick attack (tap on the attack joystick = auto-aim)
 *   L                   -> super (tap = auto-aim super; hold L + right stick = aimed super)
 *   Square              -> gadget (only if a gadget position is configured) */
#ifndef BV_VPAD_H
#define BV_VPAD_H

#include <stdint.h>

#define VPAD_BTN_UP 0x0010u
#define VPAD_BTN_RIGHT 0x0020u
#define VPAD_BTN_DOWN 0x0040u
#define VPAD_BTN_LEFT 0x0080u
#define VPAD_BTN_L 0x0100u
#define VPAD_BTN_R 0x0200u
#define VPAD_BTN_CROSS 0x4000u
#define VPAD_BTN_SQUARE 0x8000u
#define VPAD_BTN_TRIANGLE 0x1000u

typedef struct vpad_cfg {
    int move_x, move_y, attack_x, attack_y, super_x, super_y, gadget_x, gadget_y; /* gadget_x < 0 = no gadget button */
    int radius;   /* drag distance in pixels at full stick (movement) */
    int aim_radius; /* update 112: drag distance for aiming */
    int play_x, play_y, shop_x, shop_y, brawlers_x, brawlers_y; /* update 112 (J3): home screen buttons */
    int dead;     /* stick dead zone, 0..127 */
} vpad_cfg;

typedef struct vpad_state {
    vpad_cfg c;
    int move_down, aim_down, aim_super, tap_down, super_tap, gadget_down, l_prev;
    uint32_t home_prev; /* update 112: home buttons pressed last poll */
    /* update 144: rear-panel fields removed */
    int move_x, move_y, aim_x, aim_y;
} vpad_state;

/* virtual pointer ids (the panel uses small ids; these never collide) */
#define VPAD_ID_MOVE 20
#define VPAD_ID_AIM 21
#define VPAD_ID_TAP 22
#define VPAD_ID_GADGET 23

typedef void (*vpad_emit_fn)(void *ctx, int phase, int x, int y, int id); /* phase 0 down, 1 up, 2 move (nOnTouchEvent) */

void vpad_default(vpad_cfg *c);
void vpad_init(vpad_state *s, const vpad_cfg *c);
/* buttons = SceCtrlData.buttons, sticks 0..255 (128 = centre) */
void vpad_update(vpad_state *s, uint32_t buttons, int lx, int ly, int rx, int ry, vpad_emit_fn emit, void *ctx);
void vpad_move_only(vpad_state *s, uint32_t buttons, int lx, int ly, vpad_emit_fn emit, void *ctx);   /* update 172 */
/* update 112 (J3): home screen mode: Cross / Square / Triangle tap PLAY / SHOP / BRAWLERS; nothing else */
void vpad_home(vpad_state *s, uint32_t buttons, vpad_emit_fn emit, void *ctx);
/* update 112 (J4): rear panel finger (active, x / y normalised -127..127 from the panel centre) -> aim drag + fire on release */
/* update 144 (req. 12): vpad_rear removed (rear touch must never shoot) */
/* releases every virtual finger */
void vpad_release_all(vpad_state *s, vpad_emit_fn emit, void *ctx);

#endif
