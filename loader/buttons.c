#include "buttons.h"

#include <string.h>

void bvbtn_init(bvbtn_state *s, uint32_t back_mask, uint32_t quit_mask, uint32_t quit_hold_ms) {
    memset(s, 0, sizeof *s);
    s->back_mask = back_mask;
    s->quit_mask = quit_mask;
    s->quit_hold_ms = quit_hold_ms;
}

int bvbtn_update(bvbtn_state *s, uint32_t buttons, uint32_t now_ms) {
    int ev = 0;
    if (!s->primed) {
        /* buttons already held when polling starts (the one that launched the app) must not act */
        s->primed = 1;
        s->prev = buttons;
        return 0;
    }
    int back_down = s->back_mask && (buttons & s->back_mask) == s->back_mask;
    int back_was = s->back_mask && (s->prev & s->back_mask) == s->back_mask;
    if (back_down && !back_was) s->back_pressed = 1;
    if (!back_down && back_was && s->back_pressed) {
        s->back_pressed = 0;
        ev |= BVBTN_BACK;
    }
    if (!back_down && !back_was) s->back_pressed = 0;

    int quit_down = s->quit_mask && (buttons & s->quit_mask) == s->quit_mask;
    if (quit_down) {
        if (!s->quit_active) {
            s->quit_active = 1;
            s->quit_since = now_ms;
            s->quit_fired = 0;
        } else if (!s->quit_fired && now_ms - s->quit_since >= s->quit_hold_ms) {
            s->quit_fired = 1;
            ev |= BVBTN_QUIT;
        }
    } else {
        s->quit_active = 0;
    }
    s->prev = buttons;
    return ev;
}
