#include "touch.h"

#include <string.h>

void bvt_init(bvt_state *s, int width, int height, int ax0, int ay0, int ax1, int ay1) {
    memset(s, 0, sizeof *s);
    s->width = width;
    s->height = height;
    s->ax0 = ax0;
    s->ay0 = ay0;
    s->ax1 = ax1 > ax0 ? ax1 : ax0 + 1;
    s->ay1 = ay1 > ay0 ? ay1 : ay0 + 1;
}

static int scale(int v, int lo, int hi, int size) {
    long span = (long)hi - lo + 1;
    long p = ((long)v - lo) * size / span;
    if (p < 0) p = 0;
    if (p >= size) p = size - 1;
    return (int)p;
}

static int find(const bvt_state *s, int id) {
    for (int i = 0; i < s->n; i++)
        if (s->cur[i].id == id) return i;
    return -1;
}

void bvt_update(bvt_state *s, const bvt_report *r, int count, bvt_emit_fn emit, void *ctx) {
    if (count > BVT_MAX) count = BVT_MAX;
    int px[BVT_MAX], py[BVT_MAX];
    for (int i = 0; i < count; i++) {
        px[i] = scale(r[i].x, s->ax0, s->ax1, s->width);
        py[i] = scale(r[i].y, s->ay0, s->ay1, s->height);
    }
    /* ups: contacts that are no longer reported (last known position, like MotionEvent's action pointer) */
    for (int i = 0; i < s->n;) {
        int still = 0;
        for (int k = 0; k < count; k++)
            if (r[k].id == s->cur[i].id) still = 1;
        if (still) {
            i++;
            continue;
        }
        emit(ctx, BVT_UP, s->cur[i].x, s->cur[i].y, s->cur[i].slot);
        s->events++;
        memmove(&s->cur[i], &s->cur[i + 1], (size_t)(s->n - i - 1) * sizeof s->cur[0]);
        s->n--;
    }
    /* downs and position changes */
    int moved = 0;
    for (int k = 0; k < count; k++) {
        int i = find(s, r[k].id);
        if (i < 0) {
            if (s->n >= BVT_MAX) continue;
            i = s->n++;
            int slot = 0;
            for (;; slot++) {
                int used = 0;
                for (int j = 0; j < i; j++)
                    if (s->cur[j].slot == slot) used = 1;
                if (!used) break;
            }
            s->cur[i].id = r[k].id;
            s->cur[i].slot = slot;
            s->cur[i].x = px[k];
            s->cur[i].y = py[k];
            emit(ctx, BVT_DOWN, px[k], py[k], slot);
            s->events++;
        } else if (s->cur[i].x != px[k] || s->cur[i].y != py[k]) {
            s->cur[i].x = px[k];
            s->cur[i].y = py[k];
            moved = 1;
        }
    }
    if (moved) {
        /* Android reports every pointer of the gesture in one MOVE event; the game gets one native call per pointer */
        for (int i = 0; i < s->n; i++) {
            emit(ctx, BVT_MOVE, s->cur[i].x, s->cur[i].y, s->cur[i].slot);
            s->events++;
        }
    }
}

void bvt_cancel_all(bvt_state *s, bvt_emit_fn emit, void *ctx) {
    if (s->n) {
        emit(ctx, BVT_CANCEL, 0, 0, 0);
        s->events++;
    }
    s->n = 0;
}
