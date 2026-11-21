#include <string.h>
#include "texshrink.h"

static struct { unsigned id; int shift; } g_t[TS_MAX_TRACKED];
static volatile int g_lock;

static void lock(void) { while (__sync_lock_test_and_set(&g_lock, 1)) { } }
static void unlock(void) { __sync_lock_release(&g_lock); }

int ts_shift_for_budget(int width, int height, int bpp, uint64_t budget) {
    if (width <= 0 || height <= 0 || bpp <= 0) return 0;
    uint64_t bytes = (uint64_t)width * (uint64_t)height * (uint64_t)bpp;
    int shift = 0;
    while (shift < 3 && (bytes >> (2 * shift)) > budget) shift++;
    if ((bytes >> (2 * shift)) > budget) return -1; /* still too big at 1/8 per side */
    int f = 1 << shift;
    if ((width % f) || (height % f)) return -2;
    return shift;
}

int ts_shift_for(int width, int height, int bpp) {
    int s = ts_shift_for_budget(width, height, bpp, TS_LIMIT_BYTES);
    return s < 0 ? -1 : s;
}

void ts_set(unsigned id, int shift) {
    lock();
    int free_slot = -1, slot = -1;
    for (int i = 0; i < TS_MAX_TRACKED; i++) {
        if (g_t[i].shift && g_t[i].id == id) slot = i;
        if (!g_t[i].shift && free_slot < 0) free_slot = i;
    }
    if (slot < 0) slot = free_slot;
    if (slot >= 0) {
        g_t[slot].id = id;
        g_t[slot].shift = shift;
    }
    unlock();
}

int ts_get(unsigned id) {
    int r = 0;
    lock();
    for (int i = 0; i < TS_MAX_TRACKED; i++)
        if (g_t[i].shift && g_t[i].id == id) r = g_t[i].shift;
    unlock();
    return r;
}

void ts_clear(unsigned id) {
    lock();
    for (int i = 0; i < TS_MAX_TRACKED; i++)
        if (g_t[i].shift && g_t[i].id == id) g_t[i].shift = 0;
    unlock();
}

void ts_box_down_rgba(const uint8_t *src, int w, int h, int shift, uint8_t *dst) {
    int f = 1 << shift, ow = w >> shift, oh = h >> shift;
    uint32_t n = (uint32_t)f * (uint32_t)f, half = n / 2;
    for (int oy = 0; oy < oh; oy++) {
        for (int ox = 0; ox < ow; ox++) {
            uint32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
            for (int dy = 0; dy < f; dy++) {
                const uint8_t *p = src + ((size_t)(oy * f + dy) * (size_t)w + (size_t)(ox * f)) * 4;
                for (int dx = 0; dx < f; dx++, p += 4) {
                    s0 += p[0];
                    s1 += p[1];
                    s2 += p[2];
                    s3 += p[3];
                }
            }
            uint8_t *q = dst + ((size_t)oy * (size_t)ow + (size_t)ox) * 4;
            q[0] = (uint8_t)((s0 + half) / n);
            q[1] = (uint8_t)((s1 + half) / n);
            q[2] = (uint8_t)((s2 + half) / n);
            q[3] = (uint8_t)((s3 + half) / n);
        }
    }
}

static unsigned g_fbo_cur;
static struct { unsigned id; int color; int drawn, x0, y0, x1, y1; } g_fbo[TS_MAX_FBO];

void fbo_bind(unsigned id) { g_fbo_cur = id; }
unsigned fbo_current(void) { return g_fbo_cur; }

void fbo_set_color(unsigned id, int on) {
    lock();
    int slot = -1, free_slot = -1;
    for (int i = 0; i < TS_MAX_FBO; i++) {
        if (g_fbo[i].id == id && g_fbo[i].color) slot = i;
        if (!g_fbo[i].color && free_slot < 0) free_slot = i;
    }
    if (on) {
        if (slot < 0) slot = free_slot;
        if (slot >= 0) {
            g_fbo[slot].id = id;
            g_fbo[slot].color = 1;
            g_fbo[slot].drawn = 0;
        }
    } else if (slot >= 0) {
        g_fbo[slot].color = 0;
    }
    unlock();
}

int fbo_has_color(unsigned id) {
    int r = 0;
    lock();
    for (int i = 0; i < TS_MAX_FBO; i++)
        if (g_fbo[i].color && g_fbo[i].id == id) r = 1;
    unlock();
    return r;
}

void fbo_forget(unsigned id) {
    fbo_set_color(id, 0);
    if (g_fbo_cur == id) g_fbo_cur = 0;
}

int fbo_unusable(void) { return g_fbo_cur != 0 && !fbo_has_color(g_fbo_cur); }

/* The area drawn into the bound colour framebuffer since it was last taken (the union of the viewports of its draw calls): the region the frame's render-to-texture
 * result occupies, which shim_gles.c may flip vertically (fbo_flip_from, the upside-down 3D brawler of 2026-11-11). */
void fbo_note_draw(int x, int y, int w, int h) {
    if (!g_fbo_cur || w <= 0 || h <= 0) return;
    lock();
    for (int i = 0; i < TS_MAX_FBO; i++)
        if (g_fbo[i].color && g_fbo[i].id == g_fbo_cur) {
            if (!g_fbo[i].drawn) {
                g_fbo[i].x0 = x; g_fbo[i].y0 = y; g_fbo[i].x1 = x + w; g_fbo[i].y1 = y + h;
                g_fbo[i].drawn = 1;
            } else {
                if (x < g_fbo[i].x0) g_fbo[i].x0 = x;
                if (y < g_fbo[i].y0) g_fbo[i].y0 = y;
                if (x + w > g_fbo[i].x1) g_fbo[i].x1 = x + w;
                if (y + h > g_fbo[i].y1) g_fbo[i].y1 = y + h;
            }
        }
    unlock();
}

int fbo_take_drawn(unsigned id, int *x, int *y, int *w, int *h) {
    int r = 0;
    lock();
    for (int i = 0; i < TS_MAX_FBO; i++)
        if (g_fbo[i].color && g_fbo[i].id == id && g_fbo[i].drawn) {
            *x = g_fbo[i].x0; *y = g_fbo[i].y0; *w = g_fbo[i].x1 - g_fbo[i].x0; *h = g_fbo[i].y1 - g_fbo[i].y0;
            g_fbo[i].drawn = 0;
            r = 1;
        }
    unlock();
    return r;
}

/* ---- 16-bit storage (texshrink.h, update 51) ---- */
static struct { unsigned id; int w, h; } g_t16[T16_MAX];
static int g_t16_n;

void t16_set(unsigned id, int w, int h) {
    if (!id) return;
    lock();
    int free_slot = -1, slot = -1;
    for (int i = 0; i < T16_MAX; i++) {
        if (g_t16[i].id == id) slot = i;
        if (!g_t16[i].id && free_slot < 0) free_slot = i;
    }
    if (slot < 0 && free_slot >= 0) { slot = free_slot; g_t16_n++; }
    if (slot >= 0) { g_t16[slot].id = id; g_t16[slot].w = w; g_t16[slot].h = h; }
    unlock();
}

int t16_get(unsigned id, int *w, int *h) {
    int r = 0;
    if (!id) return 0;
    lock();
    for (int i = 0; i < T16_MAX; i++)
        if (g_t16[i].id == id) {
            if (w) *w = g_t16[i].w;
            if (h) *h = g_t16[i].h;
            r = 1;
            break;
        }
    unlock();
    return r;
}

void t16_clear(unsigned id) {
    if (!id) return;
    lock();
    for (int i = 0; i < T16_MAX; i++)
        if (g_t16[i].id == id) { g_t16[i].id = 0; g_t16_n--; }
    unlock();
}

int t16_count(void) { return g_t16_n; }

/* 4x4 ordered-dither thresholds 0..15 (Bayer) */
static const uint8_t k_bayer4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

/* update 88 (experiment t16_lut, default 0): the same conversion from tables. g_q4[b][v] = the 4-bit value of channel v at Bayer threshold b, built
 * with exactly the formula below, so the output is bit-identical (tests/host: ts_lut_identical). 4 KB + 256 B: fits the L1 cache. */
int ts_lut = 1; /* default since update 89 (verified: host test + runs 95/96); config t16_lut=0 = the formula loop */
static uint8_t g_q4[16][256], g_a4[256];
static volatile int g_q4_ready;
static void q4_init(void) {
    for (int b = 0; b < 16; b++)
        for (int v = 0; v < 256; v++) {
            unsigned t = (unsigned)b * 16u + 8u, q = ((v * 15u + t) * 32897u) >> 23;
            g_q4[b][v] = (uint8_t)(q > 15 ? 15 : q);
        }
    for (int v = 0; v < 256; v++) {
        unsigned a = ((v * 15u + 127u) * 32897u) >> 23;
        g_a4[v] = (uint8_t)(a > 15 ? 15 : a);
    }
    g_q4_ready = 1;
}
static void rgba8_to_4444_lut(const uint8_t *src, int w, int h, int x0, int y0, uint16_t *dst) {
    if (!g_q4_ready) q4_init();
    for (int y = 0; y < h; y++) {
        const uint8_t *s = src + (size_t)y * (size_t)w * 4;
        uint16_t *d = dst + (size_t)y * (size_t)w;
        const uint8_t *row = k_bayer4 + (((y0 + y) & 3) << 2);
        const uint8_t *t0 = g_q4[row[(x0 + 0) & 3]], *t1 = g_q4[row[(x0 + 1) & 3]], *t2 = g_q4[row[(x0 + 2) & 3]], *t3 = g_q4[row[(x0 + 3) & 3]];
        int x = 0;
        for (; x + 4 <= w; x += 4, s += 16) {   /* all four pixels are read before any is written (in-place use: d may start at s) */
            uint32_t p0, p1, p2, p3;
            memcpy(&p0, s, 4);
            memcpy(&p1, s + 4, 4);
            memcpy(&p2, s + 8, 4);
            memcpy(&p3, s + 12, 4);
            d[x] = (uint16_t)((t0[p0 & 255] << 12) | (t0[(p0 >> 8) & 255] << 8) | (t0[(p0 >> 16) & 255] << 4) | g_a4[p0 >> 24]);
            d[x + 1] = (uint16_t)((t1[p1 & 255] << 12) | (t1[(p1 >> 8) & 255] << 8) | (t1[(p1 >> 16) & 255] << 4) | g_a4[p1 >> 24]);
            d[x + 2] = (uint16_t)((t2[p2 & 255] << 12) | (t2[(p2 >> 8) & 255] << 8) | (t2[(p2 >> 16) & 255] << 4) | g_a4[p2 >> 24]);
            d[x + 3] = (uint16_t)((t3[p3 & 255] << 12) | (t3[(p3 >> 8) & 255] << 8) | (t3[(p3 >> 16) & 255] << 4) | g_a4[p3 >> 24]);
        }
        for (; x < w; x++, s += 4) {
            const uint8_t *t = g_q4[row[(x0 + x) & 3]];
            d[x] = (uint16_t)((t[s[0]] << 12) | (t[s[1]] << 8) | (t[s[2]] << 4) | g_a4[s[3]]);
        }
    }
}

void ts_rgba8_to_4444(const uint8_t *src, int w, int h, int x0, int y0, uint16_t *dst) {
    if (ts_lut) {
        rgba8_to_4444_lut(src, w, h, x0, y0, dst);
        return;
    }
    for (int y = 0; y < h; y++) {
        const uint8_t *s = src + (size_t)y * (size_t)w * 4;
        uint16_t *d = dst + (size_t)y * (size_t)w;
        const uint8_t *row = k_bayer4 + (((y0 + y) & 3) << 2);
        for (int x = 0; x < w; x++, s += 4) {
            unsigned t = row[(x0 + x) & 3] * 16u + 8u;            /* 8..248: below 255, so 0 stays 0 and 255 stays 15; the mean colour is kept */
            /* x / 255 as (x * 32897) >> 23: exact for x < 66299 (here x <= 4073); the Vita's Cortex-A9 has no divide instruction */
            unsigned r = ((s[0] * 15u + t) * 32897u) >> 23, g = ((s[1] * 15u + t) * 32897u) >> 23, b = ((s[2] * 15u + t) * 32897u) >> 23;
            unsigned a = ((s[3] * 15u + 127u) * 32897u) >> 23;    /* alpha rounded: no noise on edges */
            if (r > 15) r = 15;
            if (g > 15) g = 15;
            if (b > 15) b = 15;
            d[x] = (uint16_t)((r << 12) | (g << 8) | (b << 4) | a);
        }
    }
}

/* ---- render targets (texshrink.h, update 52) ---- */
/* update 135 (A14 root cause, run 152): 256 was too small - the Showdown pre-bake keeps more render-target textures alive than that, new ones were not
 * marked, glGenerateMipmap ran on them (tripwire ring: I tex 16x16, A attach, M mipmap with the RAM pool changing) and vitaGL's RAM pool broke (A1). */
#define RT_MAX 4096
static unsigned g_rt[RT_MAX];
volatile unsigned bv_rt_full; /* times rt_mark found no free slot */

void rt_mark(unsigned id) {
    if (!id) return;
    lock();
    int free_slot = -1, have = 0;
    for (int i = 0; i < RT_MAX; i++) {
        if (g_rt[i] == id) have = 1;
        if (!g_rt[i] && free_slot < 0) free_slot = i;
    }
    if (!have && free_slot >= 0) g_rt[free_slot] = id;
    else if (!have) bv_rt_full++;
    unlock();
}

int rt_is(unsigned id) {
    int r = 0;
    if (!id) return 0;
    lock();
    for (int i = 0; i < RT_MAX; i++)
        if (g_rt[i] == id) { r = 1; break; }
    unlock();
    return r;
}

void rt_clear(unsigned id) {
    if (!id) return;
    lock();
    for (int i = 0; i < RT_MAX; i++)
        if (g_rt[i] == id) g_rt[i] = 0;
    unlock();
}
