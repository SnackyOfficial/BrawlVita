#include "etc1.h"

#include <string.h>

/* intensity modifiers per table codeword, indexed by (msb << 1) | lsb of the texel */
static const int k_mod[8][4] = {
    {2, 8, -2, -8}, {5, 17, -5, -17}, {9, 29, -9, -29}, {13, 42, -13, -42},
    {18, 60, -18, -60}, {24, 80, -24, -80}, {33, 106, -33, -106}, {47, 183, -47, -183},
};

static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static int clamp5(int v) { return v < 0 ? 0 : v > 31 ? 31 : v; }
static int ext4(int v) { return (v << 4) | v; }
static int ext5(int v) { return (v << 3) | (v >> 2); }

void bv_etc1_decode_block(const uint8_t b[8], uint8_t rgb[16][3]) {
    int base[2][3];
    int diff = (b[3] >> 1) & 1, flip = b[3] & 1;
    if (diff) {
        int r = b[0] >> 3, g = b[1] >> 3, bl = b[2] >> 3;
        int dr = (int8_t)((b[0] & 7) << 5) >> 5, dg = (int8_t)((b[1] & 7) << 5) >> 5, db = (int8_t)((b[2] & 7) << 5) >> 5;
        base[0][0] = ext5(r); base[0][1] = ext5(g); base[0][2] = ext5(bl);
        base[1][0] = ext5(clamp5(r + dr)); base[1][1] = ext5(clamp5(g + dg)); base[1][2] = ext5(clamp5(bl + db));
    } else {
        base[0][0] = ext4(b[0] >> 4); base[1][0] = ext4(b[0] & 15);
        base[0][1] = ext4(b[1] >> 4); base[1][1] = ext4(b[1] & 15);
        base[0][2] = ext4(b[2] >> 4); base[1][2] = ext4(b[2] & 15);
    }
    int cw[2] = {(b[3] >> 5) & 7, (b[3] >> 2) & 7};
    unsigned msb = ((unsigned)b[4] << 8) | b[5], lsb = ((unsigned)b[6] << 8) | b[7];
    /* the 4 colours of each half, computed once (update 33: was clamped per texel) */
    uint8_t pal[2][4][3];
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < 4; i++) {
            int m = k_mod[cw[s]][i];
            pal[s][i][0] = (uint8_t)clamp8(base[s][0] + m);
            pal[s][i][1] = (uint8_t)clamp8(base[s][1] + m);
            pal[s][i][2] = (uint8_t)clamp8(base[s][2] + m);
        }
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int k = x * 4 + y; /* texels are stored column by column */
            int sub = flip ? (y >= 2) : (x >= 2);
            int idx = (int)((((msb >> k) & 1) << 1) | ((lsb >> k) & 1));
            const uint8_t *c = pal[sub][idx];
            uint8_t *p = rgb[y * 4 + x];
            p[0] = c[0];
            p[1] = c[1];
            p[2] = c[2];
        }
    }
}

static unsigned pack565(int r, int g, int b) { return (unsigned)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }
static void unpack565(unsigned c, int out[3]) {
    int r = (int)(c >> 11) & 31, g = (int)(c >> 5) & 63, b = (int)c & 31;
    out[0] = (r << 3) | (r >> 2);
    out[1] = (g << 2) | (g >> 4);
    out[2] = (b << 3) | (b >> 2);
}

/* Fast encoder (update 33). Run 18 (2026-11-11): picking a brawler loads all its skins, 120 ETC1 textures (7 MB) took 7193 ms here = the 9 s freeze the
 * user saw. Same endpoints as the reference encoder below; each texel's index comes from one projection onto the colour line (multiply/compare, no division)
 * instead of 4 distance tests. tests/host_etc1_test.c checks its quality against the reference and times both. */
void bv_dxt1_encode_block(const uint8_t rgb[16][3], uint8_t out[8]) {
    int lo0 = 255, lo1 = 255, lo2 = 255, hi0 = 0, hi1 = 0, hi2 = 0;
    for (int i = 0; i < 16; i++) {
        int r = rgb[i][0], g = rgb[i][1], b = rgb[i][2];
        if (r < lo0) lo0 = r;
        if (r > hi0) hi0 = r;
        if (g < lo1) lo1 = g;
        if (g > hi1) hi1 = g;
        if (b < lo2) lo2 = b;
        if (b > hi2) hi2 = b;
    }
    int in0 = (hi0 - lo0) >> 4, in1 = (hi1 - lo1) >> 4, in2 = (hi2 - lo2) >> 4;
    lo0 += in0; hi0 -= in0;
    lo1 += in1; hi1 -= in1;
    lo2 += in2; hi2 -= in2;
    unsigned c0 = pack565(hi0, hi1, hi2), c1 = pack565(lo0, lo1, lo2);
    if (c0 == c1) {
        if (c1 > 0) c1--;
        else c0++;
    }
    if (c0 < c1) { unsigned t = c0; c0 = c1; c1 = t; }
    int p0[3], p1[3];
    unpack565(c0, p0);
    unpack565(c1, p1);
    int d0 = p0[0] - p1[0], d1 = p0[1] - p1[1], d2 = p0[2] - p1[2];
    int len2 = d0 * d0 + d1 * d1 + d2 * d2;
    uint32_t bits = 0;
    if (len2 > 0) {
        /* t = dot / len2 along p1 -> p0; palette order: index 1 = p1 (t 0), 3 = 1/3, 2 = 2/3, 0 = p0 (t 1). Boundaries at 1/6, 1/2, 5/6. */
        const int t1 = len2, t3 = 3 * len2, t5 = 5 * len2;
        for (int i = 0; i < 16; i++) {
            int s = 6 * ((rgb[i][0] - p1[0]) * d0 + (rgb[i][1] - p1[1]) * d1 + (rgb[i][2] - p1[2]) * d2);
            uint32_t idx = s < t1 ? 1u : s < t3 ? 3u : s < t5 ? 2u : 0u;
            bits |= idx << (2 * i);
        }
    }
    out[0] = (uint8_t)c0; out[1] = (uint8_t)(c0 >> 8);
    out[2] = (uint8_t)c1; out[3] = (uint8_t)(c1 >> 8);
    out[4] = (uint8_t)bits; out[5] = (uint8_t)(bits >> 8); out[6] = (uint8_t)(bits >> 16); out[7] = (uint8_t)(bits >> 24);
}

/* The original encoder (up to update 32): nearest of the 4 palette colours per texel. Kept as the reference for the host test. */
void bv_dxt1_encode_block_ref(const uint8_t rgb[16][3], uint8_t out[8]) {
    int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 3; c++) {
            if (rgb[i][c] < lo[c]) lo[c] = rgb[i][c];
            if (rgb[i][c] > hi[c]) hi[c] = rgb[i][c];
        }
    /* pull the endpoints in a little: the extremes of a block are rarely worth the extra error on the other texels */
    for (int c = 0; c < 3; c++) {
        int inset = (hi[c] - lo[c]) >> 4;
        lo[c] += inset;
        hi[c] -= inset;
    }
    unsigned c0 = pack565(hi[0], hi[1], hi[2]), c1 = pack565(lo[0], lo[1], lo[2]);
    if (c0 == c1) {
        /* flat block: both endpoints equal -> every index 0 (4-colour mode needs c0 > c1, so nudge c1 down if possible) */
        if (c1 > 0) c1--;
        else c0++;
    }
    if (c0 < c1) { unsigned t = c0; c0 = c1; c1 = t; }
    int p[4][3];
    unpack565(c0, p[0]);
    unpack565(c1, p[1]);
    for (int c = 0; c < 3; c++) {
        p[2][c] = (2 * p[0][c] + p[1][c]) / 3;
        p[3][c] = (p[0][c] + 2 * p[1][c]) / 3;
    }
    uint32_t bits = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, be = 1 << 30;
        for (int k = 0; k < 4; k++) {
            int e = 0;
            for (int c = 0; c < 3; c++) {
                int d = (int)rgb[i][c] - p[k][c];
                e += d * d;
            }
            if (e < be) { be = e; best = k; }
        }
        bits |= (uint32_t)best << (2 * i);
    }
    out[0] = (uint8_t)c0; out[1] = (uint8_t)(c0 >> 8);
    out[2] = (uint8_t)c1; out[3] = (uint8_t)(c1 >> 8);
    out[4] = (uint8_t)bits; out[5] = (uint8_t)(bits >> 8); out[6] = (uint8_t)(bits >> 16); out[7] = (uint8_t)(bits >> 24);
}

void bv_dxt1_decode_block(const uint8_t b[8], uint8_t rgb[16][3]) {
    unsigned c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
    int p[4][3];
    unpack565(c0, p[0]);
    unpack565(c1, p[1]);
    for (int c = 0; c < 3; c++) {
        if (c0 > c1) {
            p[2][c] = (2 * p[0][c] + p[1][c]) / 3;
            p[3][c] = (p[0][c] + 2 * p[1][c]) / 3;
        } else {
            p[2][c] = (p[0][c] + p[1][c]) / 2;
            p[3][c] = 0;
        }
    }
    uint32_t bits = (uint32_t)b[4] | ((uint32_t)b[5] << 8) | ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 3; c++) rgb[i][c] = (uint8_t)p[(bits >> (2 * i)) & 3][c];
}

void bv_etc1_to_dxt1(const uint8_t *etc1, uint8_t *dxt1, size_t nblocks) {
    uint8_t px[16][3];
    for (size_t i = 0; i < nblocks; i++) {
        bv_etc1_decode_block(etc1 + 8 * i, px);
        bv_dxt1_encode_block(px, dxt1 + 8 * i);
    }
}
