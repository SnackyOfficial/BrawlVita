#include "antitamper.h"

#include <string.h>

static const struct {
    uint32_t off, old_word, new_word;
    const char *what;
} k_table[] = {
    /* flag setter reached from the second stage of the check (libg+0xb809c: bne 0xb62a8) */
    {0xb62a8, 0xe3a00001u, 0xe3a00000u, "self-check flag, probe stage: mov r0,#1 -> mov r0,#0"},
    /* flag setter reached from the first stage (libg+0xb62a0: beq 0xb62c0, via the state set at 0xb62b4) */
    {0xb62c0, 0xe3a00001u, 0xe3a00000u, "self-check flag, hook stage: mov r0,#1 -> mov r0,#0"},
};

int bv_antitamper_plan_b(const uint8_t *text, uint32_t size, at_patch *out, int max, int *mismatch) {
    int n = 0, bad = 0;
    for (unsigned i = 0; i < sizeof k_table / sizeof k_table[0]; i++) {
        uint32_t off = k_table[i].off;
        if (off + 8 > size) {
            bad++;
            continue;
        }
        uint32_t w = (uint32_t)text[off] | ((uint32_t)text[off + 1] << 8) | ((uint32_t)text[off + 2] << 16) | ((uint32_t)text[off + 3] << 24);
        uint32_t next = (uint32_t)text[off + 4] | ((uint32_t)text[off + 5] << 8) | ((uint32_t)text[off + 6] << 16) | ((uint32_t)text[off + 7] << 24);
        /* the patched word must be followed by `strb r0,[sp,#3]` (the flag store), or the table entry is not what it was read as */
        if (w != k_table[i].old_word || next != 0xe5cd0003u || n >= max) {
            bad++;
            continue;
        }
        out[n].off = off;
        out[n].old_word = w;
        out[n].new_word = k_table[i].new_word;
        strncpy(out[n].what, k_table[i].what, sizeof out[n].what - 1);
        out[n].what[sizeof out[n].what - 1] = 0;
        n++;
    }
    if (mismatch) *mismatch = bad;
    return n;
}

static uint32_t word_at(const uint8_t *t, uint32_t off) {
    return (uint32_t)t[off] | ((uint32_t)t[off + 1] << 8) | ((uint32_t)t[off + 2] << 16) | ((uint32_t)t[off + 3] << 24);
}

static int is_div10_movw(uint32_t w) {
    return (w & 0xffff0fffu) == 0xe30c0ccdu; /* movw rX,#0xcccd: the low half of the divide-by-ten constant 0xcccccccd */
}

/* The generator (the time-seeded divide-by-ten) belongs to the same macro as the call, but the control-flow flattening lays the blocks out in any order: in 19 sites of
 * the real libg.so it is in front of the call, in the other 14 it is 0x200..0x2e0 bytes BEHIND it (e.g. libg+0x11a8ec, the call that ended the run of 2026-10-25).
 * Looking only backwards missed those 14 (and matched a few of the others only because the generator of a neighbouring site happened to lie in front). */
static int generator_near(const uint8_t *text, uint32_t size, uint32_t a) {
    for (uint32_t b = a - 16; b + AT_GEN_WINDOW > a && b >= 0x10000; b -= 4)
        if (is_div10_movw(word_at(text, b))) return 1;
    for (uint32_t b = a + 4; b < a + AT_GEN_WINDOW && b + 4 <= size; b += 4)
        if (is_div10_movw(word_at(text, b))) return 1;
    return 0;
}

int bv_antitamper_find_reactions_b(const uint8_t *text, uint32_t size, uint32_t *site_off, int max) {
    int found = 0;
    if (size < 0x10000 + 16) return 0;
    for (uint32_t a = 0x10000; a + 4 <= size; a += 4) {
        if (word_at(text, a) != 0xe12fff30u) continue;            /* blx r0 */
        uint32_t st = word_at(text, a - 4), add = word_at(text, a - 8);
        if (add != 0xe0800001u) continue;                          /* add r0, r0, r1 */
        uint32_t top = st >> 20;
        if ((top != 0xe58 && top != 0xe50) || ((st >> 12) & 15) != 0) continue; /* str r0, [rn, #+-imm] */
        if (word_at(text, a - 12) != 0xe08f0000u && (word_at(text, a - 12) & 0xfffff000u) != 0xe28de000u) continue; /* add r0,pc,r0 (or the base kept in lr, as in 2 sites) */
        if (!generator_near(text, size, a)) continue;
        if (site_off && found < max) site_off[found] = a;
        found++;
    }
    return found;
}

int bv_antitamper_plan_reactions_b(const uint8_t *text, uint32_t size, at_patch *out, int max, int *sites) {
    uint32_t offs[AT_MAX];
    int found = bv_antitamper_find_reactions_b(text, size, offs, AT_MAX);
    int n = 0;
    for (int i = 0; i < found && i < AT_MAX; i++) {
        uint32_t a = offs[i];
        uint32_t st = word_at(text, a - 4);
        if (n + 2 > max) break; /* the caller compares *sites with n / 2 and reports the shortfall */
        out[n].off = a - 4;
        out[n].old_word = st;
        out[n].new_word = 0xe3a00000u; /* mov r0,#0 */
        strncpy(out[n].what, "self-check random call: str r0,[slot] -> mov r0,#0", sizeof out[n].what - 1);
        out[n].what[sizeof out[n].what - 1] = 0;
        n++;
        out[n].off = a;
        out[n].old_word = 0xe12fff30u;
        out[n].new_word = st; /* the store of r0 (now zero) into the same slot */
        strncpy(out[n].what, "self-check random call: blx r0 -> str r0,[slot]", sizeof out[n].what - 1);
        out[n].what[sizeof out[n].what - 1] = 0;
        n++;
    }
    if (sites) *sites = found;
    return n;
}
