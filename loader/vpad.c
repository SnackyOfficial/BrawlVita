/* Vita physical controls -> virtual touches (cycle 7, ISSUES J1). See vpad.h. */
#include "vpad.h"

#include <string.h>

void vpad_default(vpad_cfg *c) {
    c->move_x = 90;
    c->move_y = 430;
    c->attack_x = 815;
    c->attack_y = 330;
    c->super_x = 655;
    c->super_y = 395;
    c->gadget_x = -1;
    c->gadget_y = -1;
    c->radius = 100;      /* update 146 (user T4): a real thumb drag, not a held button */
    c->aim_radius = 110;
    c->dead = 24;
    c->play_x = 828; c->play_y = 490; c->shop_x = 52; c->shop_y = 252; c->brawlers_x = 52; c->brawlers_y = 320;   /* home buttons (run 120 screenshot) */
}

void vpad_init(vpad_state *s, const vpad_cfg *c) {
    memset(s, 0, sizeof *s);
    s->c = *c;
}

/* stick value 0..255 -> -127..127 with dead zone (returns 0 inside it) */
static int axis(int v, int dead) {
    int d = v - 128;
    if (d > 127) d = 127;
    if (d < -127) d = -127;
    return (d > -dead && d < dead) ? 0 : d;
}

/* one finger that follows a 2D direction around a centre: down when the direction leaves the dead zone, move while it changes, up when it returns */
static void finger(int *down, int *px, int *py, int id, int cx, int cy, int dx, int dy, int radius, vpad_emit_fn emit, void *ctx) {
    int active = dx || dy;
    int x = cx + dx * radius / 127, y = cy + dy * radius / 127;
    if (active && !*down) {
        emit(ctx, 0, cx, cy, id);   /* the finger lands on the joystick centre */
        *down = 1;
        *px = cx;
        *py = cy;
        return;                     /* update 146: first a still touch, the drag starts with the next poll */
    }
    if (active && (x != *px || y != *py)) {
        /* update 146 (user T4): slide towards the target, at most 24 px per poll (~3000 px/s), so the game sees a drag out of the centre */
        int ddx = x - *px, ddy = y - *py, m = ddx < 0 ? -ddx : ddx, my = ddy < 0 ? -ddy : ddy;
        if (my > m) m = my;
        if (m > 24) { x = *px + ddx * 24 / m; y = *py + ddy * 24 / m; }
        emit(ctx, 2, x, y, id);
        *px = x;
        *py = y;
    }
    if (!active && *down) {
        emit(ctx, 1, *px, *py, id);
        *down = 0;
    }
}

void vpad_update(vpad_state *s, uint32_t b, int lx, int ly, int rx, int ry, vpad_emit_fn emit, void *ctx) {
    const vpad_cfg *c = &s->c;
    /* movement: left stick, the d-pad overrides it */
    int mx = axis(lx, c->dead), my = axis(ly, c->dead);
    if (b & (VPAD_BTN_UP | VPAD_BTN_DOWN | VPAD_BTN_LEFT | VPAD_BTN_RIGHT)) {
        mx = (b & VPAD_BTN_RIGHT) ? 127 : (b & VPAD_BTN_LEFT) ? -127 : 0;
        my = (b & VPAD_BTN_DOWN) ? 127 : (b & VPAD_BTN_UP) ? -127 : 0;
    }
    finger(&s->move_down, &s->move_x, &s->move_y, VPAD_ID_MOVE, c->move_x, c->move_y, mx, my, c->radius, emit, ctx);

    /* aiming: right stick drives the attack joystick, or the super while L is held (an aimed super) */
    int ax = axis(rx, c->dead), ay = axis(ry, c->dead);
    int want_super = (b & VPAD_BTN_L) != 0;
    if (s->aim_down && s->aim_super != want_super && (ax || ay)) {
        /* L pressed / released while aiming: lift the finger first (cancel = up at the centre: no shot) */
        emit(ctx, 1, s->aim_super ? c->super_x : c->attack_x, s->aim_super ? c->super_y : c->attack_y, VPAD_ID_AIM);
        s->aim_down = 0;
    }
    if (!s->aim_down) s->aim_super = want_super;
    finger(&s->aim_down, &s->aim_x, &s->aim_y, VPAD_ID_AIM, s->aim_super ? c->super_x : c->attack_x, s->aim_super ? c->super_y : c->attack_y, ax, ay, c->aim_radius > 0 ? c->aim_radius : c->radius, emit,
           ctx);

    /* quick attack: R or Cross (tap on the attack joystick) */
    int tap = (b & (VPAD_BTN_R | VPAD_BTN_CROSS)) != 0;
    if (tap && !s->tap_down) {
        emit(ctx, 0, c->attack_x, c->attack_y, VPAD_ID_TAP);
        s->tap_down = 1;
    } else if (!tap && s->tap_down) {
        emit(ctx, 1, c->attack_x, c->attack_y, VPAD_ID_TAP);
        s->tap_down = 0;
    }

    /* quick super: L alone (no right stick) taps the super button on release of L */
    int l = (b & VPAD_BTN_L) != 0;
    if (l && !s->l_prev && !(ax || ay) && !s->aim_down) {
        s->super_tap = 1;                 /* L pressed with the right stick centred */
    } else if (s->super_tap && (ax || ay)) {
        s->super_tap = 0;                 /* became an aimed super */
    } else if (!l && s->super_tap) {
        emit(ctx, 0, c->super_x, c->super_y, VPAD_ID_GADGET + 1);
        emit(ctx, 1, c->super_x, c->super_y, VPAD_ID_GADGET + 1);
        s->super_tap = 0;
    }
    s->l_prev = l;

    /* gadget: Square */
    if (c->gadget_x >= 0) {
        int g = (b & VPAD_BTN_SQUARE) != 0;
        if (g && !s->gadget_down) {
            emit(ctx, 0, c->gadget_x, c->gadget_y, VPAD_ID_GADGET);
            s->gadget_down = 1;
        } else if (!g && s->gadget_down) {
            emit(ctx, 1, c->gadget_x, c->gadget_y, VPAD_ID_GADGET);
            s->gadget_down = 0;
        }
    }
}

/* update 172 (native_input=2): only the movement finger (left stick / d-pad) - the game's own joystick code moves the brawler and draws the stick,
 * exactly like a thumb on a phone; attack / super / gadget are sent as native input items by the loader. */
void vpad_move_only(vpad_state *s, uint32_t b, int lx, int ly, vpad_emit_fn emit, void *ctx) {
    const vpad_cfg *c = &s->c;
    int mx = axis(lx, c->dead), my = axis(ly, c->dead);
    if (b & (VPAD_BTN_UP | VPAD_BTN_DOWN | VPAD_BTN_LEFT | VPAD_BTN_RIGHT)) {
        mx = (b & VPAD_BTN_RIGHT) ? 127 : (b & VPAD_BTN_LEFT) ? -127 : 0;
        my = (b & VPAD_BTN_DOWN) ? 127 : (b & VPAD_BTN_UP) ? -127 : 0;
    }
    finger(&s->move_down, &s->move_x, &s->move_y, VPAD_ID_MOVE, c->move_x, c->move_y, mx, my, c->radius, emit, ctx);
}
void vpad_home(vpad_state *s, uint32_t b, vpad_emit_fn emit, void *ctx) {
    static const uint32_t keys[3] = {VPAD_BTN_CROSS, VPAD_BTN_SQUARE, VPAD_BTN_TRIANGLE};
    const int xs[3] = {s->c.play_x, s->c.shop_x, s->c.brawlers_x}, ys[3] = {s->c.play_y, s->c.shop_y, s->c.brawlers_y};
    for (int i = 0; i < 3; i++) {
        int now = (b & keys[i]) != 0, was = (s->home_prev & keys[i]) != 0;
        if (now && !was) emit(ctx, 0, xs[i], ys[i], VPAD_ID_TAP);
        if (!now && was) emit(ctx, 1, xs[i], ys[i], VPAD_ID_TAP);
    }
    s->home_prev = b & (VPAD_BTN_CROSS | VPAD_BTN_SQUARE | VPAD_BTN_TRIANGLE);
}

void vpad_release_all(vpad_state *s, vpad_emit_fn emit, void *ctx) {
    if (s->move_down) emit(ctx, 1, s->move_x, s->move_y, VPAD_ID_MOVE);
    if (s->aim_down) emit(ctx, 1, s->aim_x, s->aim_y, VPAD_ID_AIM);
    if (s->tap_down) emit(ctx, 1, s->c.attack_x, s->c.attack_y, VPAD_ID_TAP);
    if (s->gadget_down) emit(ctx, 1, s->c.gadget_x, s->c.gadget_y, VPAD_ID_GADGET);
    s->move_down = s->aim_down = s->tap_down = s->gadget_down = s->super_tap = 0;
}
