/* Hardware buttons: turns successive snapshots of the console's button mask into the two actions the game needs.
 *  - BACK: Android's back key. GameApp.onBackPressed (read with tools/dexdis.py) runs on key-up and calls the native backButtonPressed();
 *    when that returns false the activity finishes. So BACK fires when the back button is RELEASED (after being seen pressed).
 *  - QUIT: an emergency exit, the quit combo held for a while (the PS button only opens the system menu and cannot close this app cleanly).
 * Portable C11 (no console headers): unit-tested on the PC (tests/host_buttons_test.c); loader/buttons_vita.c polls the pad.
 */
#ifndef BV_BUTTONS_H
#define BV_BUTTONS_H

#include <stdint.h>

#define BVBTN_BACK 1
#define BVBTN_QUIT 2

typedef struct bvbtn_state {
    uint32_t back_mask, quit_mask;
    uint32_t quit_hold_ms;
    uint32_t prev;
    uint32_t quit_since;
    int primed;       /* the first snapshot only sets the baseline */
    int back_pressed; /* the back button went down after the baseline */
    int quit_active, quit_fired;
} bvbtn_state;

void bvbtn_init(bvbtn_state *s, uint32_t back_mask, uint32_t quit_mask, uint32_t quit_hold_ms);
/* `buttons` is the pressed-button mask, `now_ms` a monotonic clock. Returns a mask of BVBTN_* events. */
int bvbtn_update(bvbtn_state *s, uint32_t buttons, uint32_t now_ms);

#endif
