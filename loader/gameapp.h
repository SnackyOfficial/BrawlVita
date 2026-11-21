/* Drives Build B's native side the way Android's GameApp activity does (docs/engine_boot_flow.md), by the natives that
 * JNI_OnLoad registered. The call order and arguments come from the Java side of the APK (tools/dexdis.py on classes.dex):
 *   onCreate: createGameMain(assets, dataDir, cacheDir, obbPath, obbSize, width, height, dpi, xdpi, ydpi, 2, 0, activity), nOnCreate
 *   onStart:  start(intentString), nOnStart       onResume: nOnResume       focus: nOnWindowFocusChanged(true)
 *   surface:  nOnSurfaceCreated(surface), nOnSurfaceChanged(surface, width, height)   (the render thread is the game's own)
 * UNVERIFIED ON HARDWARE.
 */
#ifndef BV_GAMEAPP_H
#define BV_GAMEAPP_H

#include "elf_loader.h"

/* level 1 = createGameMain + nOnCreate; 2 = + start / nOnStart / nOnResume / focus (no surface); 3 = + surface (graphics).
 * After level 2 and 3 the game runs on its own threads for `run_seconds`. Returns 0 on success, negative on failure. */
int bv_gameapp_run(int level, int run_seconds);

/* called once, right before GameApp.nOnCreate (the loader uses it to write a snapshot of libg's data segment) */
void bv_gameapp_set_pre_oncreate(void (*fn)(void));

/* config touch=1 (default): at level 3 the front touch panel is polled and sent to nOnTouchEvent */
extern int bv_touch_enabled;
/* config buttons=1 (default), back=circle|cross (back_cross=1), back_exits=1: from level 2 on the pad is polled; the back button calls backButtonPressed()
 * and, if the game does not handle it, ends the app like Android's activity does; Start + Select held 2 s always ends it */
extern int bv_buttons_enabled, bv_back_cross, bv_back_exits;

#endif
