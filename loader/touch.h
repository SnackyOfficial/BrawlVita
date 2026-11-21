/* Touch input: turns successive snapshots of the console's touch panel into the events Build B's Java side sends to
 * GameApp.nOnTouchEvent(phase, x, y, pointerId) (GL2JNISurfaceView.onTouchEvent, read with tools/dexdis.py):
 *   phase 0 = pointer down, 1 = pointer up, 2 = move (one event for EVERY active pointer whenever any of them moved), 3 = cancel.
 * Coordinates are pixels of the game surface. Portable C11 (no console headers): unit-tested on the PC (tests/host_touch_test.c);
 * loader/touch_vita.c polls the panel and calls the native.
 */
#ifndef BV_TOUCH_H
#define BV_TOUCH_H

#include <stdint.h>

#define BVT_MAX 6 /* the front panel reports up to 6 contacts */

enum { BVT_DOWN = 0, BVT_UP = 1, BVT_MOVE = 2, BVT_CANCEL = 3 };

typedef struct bvt_report {
    int id;  /* contact id, stable while the finger stays down */
    int x, y; /* panel units */
} bvt_report;

typedef void (*bvt_emit_fn)(void *ctx, int phase, int x, int y, int id);

typedef struct bvt_state {
    int width, height;             /* game surface */
    int ax0, ay0, ax1, ay1;        /* panel active area (inclusive) */
    int n;
    struct { int id, slot, x, y; } cur[BVT_MAX]; /* contacts currently down: panel id, pointer id sent to the game, position in game pixels */
    uint32_t events;
} bvt_state;

void bvt_init(bvt_state *s, int width, int height, int ax0, int ay0, int ax1, int ay1);
/* Compares the new snapshot with the previous one and emits down / up / move events. Order: ups, downs, moves.
 * The pointer id sent to the game is the lowest free slot 0..5 (Android recycles small ids; the panel's contact ids keep counting up). */
void bvt_update(bvt_state *s, const bvt_report *r, int count, bvt_emit_fn emit, void *ctx);
/* Releases everything that is still down as a cancel (pointer ids are reported with x = y = 0, like Android's ACTION_CANCEL). */
void bvt_cancel_all(bvt_state *s, bvt_emit_fn emit, void *ctx);

#endif
