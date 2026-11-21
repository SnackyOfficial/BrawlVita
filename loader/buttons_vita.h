/* Polls the console's buttons: Circle (or Cross, config back=cross) = Android back, Start + Select held 2 s = quit. UNVERIFIED ON HARDWARE. */
#ifndef BV_BUTTONS_VITA_H
#define BV_BUTTONS_VITA_H

#include <stdint.h>

/* `back_fn` is the run-time address of backButtonPressed(JNIEnv*, jclass) -> jboolean. back_is_cross selects Cross instead of Circle.
 * back_exits: when the game does not handle BACK (returns false) the app ends, like Android's activity does. */
int bv_buttons_start(uint32_t back_fn, uint32_t class_handle, int back_is_cross, int back_exits);
uint32_t bv_buttons_events(void);

#endif
