/* Polls the front touch panel and feeds GameApp.nOnTouchEvent (see touch.h for the event mapping). UNVERIFIED ON HARDWARE. */
#ifndef BV_TOUCH_VITA_H
#define BV_TOUCH_VITA_H

#include <stdint.h>

/* `native_fn` is the run-time address of nOnTouchEvent(JNIEnv*, jclass, int phase, int x, int y, int id); width/height the game surface.
 * Starts the polling thread. Returns 0, or a negative value if the panel could not be set up. */
int bv_touch_start(uint32_t native_fn, uint32_t class_handle, int width, int height);
void bv_touch_stop(void);
uint32_t bv_touch_events(void);

#endif
