/* Writer for the game's message codec (Supercell "ByteStream"): the layout the client's decoders read.
 *  - Int: 4 bytes big endian.   - Byte / Bool: 1 byte.
 *  - VInt: variable length, 6 value bits + sign in the first byte, 7 bits in the following ones, high bit = "more".
 *  - String: Int length (-1 = null) followed by the bytes.   - DataRef: VInt table id, then VInt row id unless the table id is 0.
 *  - ArrayVInt: VInt count followed by the VInts.
 * Portable C11 (no console headers): unit-tested on the PC (tests/host_bmsg_test.c).
 */
#ifndef BV_BYTESTREAM_H
#define BV_BYTESTREAM_H

#include <stddef.h>
#include <stdint.h>

typedef struct bv_bs {
    uint8_t *data;
    size_t len, cap;
    int failed; /* an allocation failed: the stream is unusable */
} bv_bs;

void bs_init(bv_bs *s);
void bs_free(bv_bs *s);
void bs_byte(bv_bs *s, unsigned v);
void bs_bool(bv_bs *s, int v);
void bs_int(bv_bs *s, int32_t v);
void bs_vint(bv_bs *s, int32_t v);
void bs_string(bv_bs *s, const char *str); /* NULL = null string (-1) */
void bs_dataref(bv_bs *s, int table, int row);
void bs_array_vint(bv_bs *s, const int32_t *v, int n);
void bs_raw(bv_bs *s, const void *p, size_t n);

/* Reader used by the tests and by the 14363 request parser. */
typedef struct bv_rd {
    const uint8_t *p;
    size_t len, pos;
    int err;
} bv_rd;

void rd_init(bv_rd *r, const void *p, size_t len);
int32_t rd_int(bv_rd *r);
int rd_byte(bv_rd *r);
int32_t rd_vint(bv_rd *r);

#endif
