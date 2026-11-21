#include "bytestream.h"

#include <stdlib.h>
#include <string.h>

/* Host tests build this file with -DBS_TRACE: every top-level primitive is reported (tests/host_bmsg_dump.c), nested calls are not. */
#ifdef BS_TRACE
extern void bs_trace(const char *what, long a, long b);
static int g_depth;
#define TR(w, a, b) do { if (!g_depth) bs_trace((w), (long)(a), (long)(b)); } while (0)
#define ENTER() g_depth++
#define LEAVE() g_depth--
#else
#define TR(w, a, b) ((void)0)
#define ENTER() ((void)0)
#define LEAVE() ((void)0)
#endif

void bs_init(bv_bs *s) { memset(s, 0, sizeof *s); }

void bs_free(bv_bs *s) {
    free(s->data);
    memset(s, 0, sizeof *s);
}

static void need(bv_bs *s, size_t n) {
    if (s->failed || s->len + n <= s->cap) return;
    size_t cap = s->cap ? s->cap : 256;
    while (cap < s->len + n) cap *= 2;
    uint8_t *p = (uint8_t *)realloc(s->data, cap);
    if (!p) {
        s->failed = 1;
        return;
    }
    s->data = p;
    s->cap = cap;
}

void bs_byte(bv_bs *s, unsigned v) {
    TR("UInt", v & 255u, 0);
    need(s, 1);
    if (!s->failed) s->data[s->len++] = (uint8_t)v;
}

void bs_bool(bv_bs *s, int v) {
    TR("Bool", v, 0);
    ENTER();
    bs_byte(s, v ? 1u : 0u);
    LEAVE();
}

void bs_int(bv_bs *s, int32_t v) {
    TR("Int", v, 0);
    ENTER();
    uint32_t u = (uint32_t)v;
    bs_byte(s, u >> 24);
    bs_byte(s, u >> 16);
    bs_byte(s, u >> 8);
    bs_byte(s, u);
    LEAVE();
}

/* The client's reader (sign in bit 6 of the first byte, arithmetic shifts):
 *   first  = ((v >> 25) & 0x40) | (v & 0x3f);   rest = (v ^ (v >> 31)) >> 6;   v >>= 6
 * then 7 bits per byte of what is left. */
void bs_vint(bv_bs *s, int32_t v) {
    TR("VInt", v, 0);
    ENTER();
    uint32_t first = (((uint32_t)v >> 25) & 0x40u) | ((uint32_t)v & 0x3Fu);
    int32_t rest = (v ^ (v >> 31)) >> 6;
    v >>= 6;
    if (rest == 0) {
        bs_byte(s, first);
        LEAVE();
        return;
    }
    bs_byte(s, first | 0x80u);
    rest >>= 7;
    for (;;) {
        unsigned more = rest ? 0x80u : 0u;
        bs_byte(s, ((unsigned)v & 0x7Fu) | more);
        v >>= 7;
        if (!rest) break;
        rest >>= 7;
    }
    LEAVE();
}

void bs_string(bv_bs *s, const char *str) {
    TR("String", str != NULL, 0);
    ENTER();
    if (!str) {
        bs_int(s, -1);
        LEAVE();
        return;
    }
    size_t n = strlen(str);
    bs_int(s, (int32_t)n);
    bs_raw(s, str, n);
    LEAVE();
}

void bs_dataref(bv_bs *s, int table, int row) {
    TR("DataRef", table, row);
    ENTER();
    bs_vint(s, table);
    if (table != 0) bs_vint(s, row);
    LEAVE();
}

void bs_array_vint(bv_bs *s, const int32_t *v, int n) {
    TR("ArrayVInt", n, 0);
    ENTER();
    bs_vint(s, n);
    for (int i = 0; i < n; i++) bs_vint(s, v[i]);
    LEAVE();
}

void bs_raw(bv_bs *s, const void *p, size_t n) {
    need(s, n);
    if (!s->failed && n) {
        memcpy(s->data + s->len, p, n);
        s->len += n;
    }
}

void rd_init(bv_rd *r, const void *p, size_t len) {
    r->p = (const uint8_t *)p;
    r->len = len;
    r->pos = 0;
    r->err = 0;
}

int rd_byte(bv_rd *r) {
    if (r->pos >= r->len) {
        r->err = 1;
        return 0;
    }
    return r->p[r->pos++];
}

int32_t rd_int(bv_rd *r) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = (v << 8) | (uint32_t)rd_byte(r);
    return (int32_t)v;
}

int32_t rd_vint(bv_rd *r) {
    uint32_t b = (uint32_t)rd_byte(r);
    uint32_t result = b & 0x3Fu;
    int sign = (b & 0x40u) != 0;
    int shift = 6;
    int more = 0;
    while (b & 0x80u) {
        b = (uint32_t)rd_byte(r);
        if (r->err || ++more > 4) { /* a VInt has at most 5 bytes */
            r->err = 1;
            break;
        }
        result |= (b & 0x7Fu) << shift;
        shift += 7;
    }
    /* sign bit set: the value is the two's complement over the payload bits that were read */
    if (sign) {
        if (shift >= 32) return (int32_t)(result | 0x80000000u);
        result |= ~0u << shift;
    }
    return (int32_t)result;
}
