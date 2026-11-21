#include "gamefix.h"

#include <stdio.h>
#include <string.h>

static uint32_t word_at(const uint8_t *t, uint32_t o) { return (uint32_t)t[o] | ((uint32_t)t[o + 1] << 8) | ((uint32_t)t[o + 2] << 16) | ((uint32_t)t[o + 3] << 24); }

int bv_gamefix_plan_localbattle(const uint8_t *text, uint32_t size, int bots, at_patch *out, int max, uint32_t *func, char *why, unsigned whyn) {
    /* push {r4-r11,lr}; add fp,sp,#0x1c; sub sp,sp,#0x44; mov r4,r3; mov sl,r2; mov r7,r1; mov sb,r0 */
    static const uint32_t shape[7] = {0xe92d4ff0u, 0xe28db01cu, 0xe24dd044u, 0xe1a04003u, 0xe1a0a002u, 0xe1a07001u, 0xe1a09000u};
    uint32_t at = 0;
    int hits = 0;
    if (func) *func = 0;
    if (why && whyn) why[0] = 0;
    for (uint32_t a = 0; a + sizeof shape <= size; a += 4) {
        int k = 0;
        while (k < 7 && word_at(text, a + 4u * (uint32_t)k) == shape[k]) k++;
        if (k == 7) {
            hits++;
            at = a;
        }
    }
    if (hits != 1) {
        if (why && whyn) snprintf(why, whyn, "start-game prologue found %d times (exactly 1 expected)", hits);
        return 0;
    }
    if (func) *func = at;
    int n = 0;
    if (n < max) {
        out[n].off = at + 12;
        out[n].old_word = 0xe1a04003u; /* mov r4, r3 */
        out[n].new_word = 0xe3a04003u; /* mov r4, #3 */
        strncpy(out[n].what, "local battle: start-game match kind r3 -> 3", sizeof out[n].what - 1);
        out[n].what[sizeof out[n].what - 1] = 0;
        n++;
    }
    if (bots) {
        if (at + 0x48 <= size && word_at(text, at + 0x44) == 0xe59b0010u) { /* ldr r0, [fp, #0x10] = args[6] */
            if (n < max) {
                out[n].off = at + 0x44;
                out[n].old_word = 0xe59b0010u;
                out[n].new_word = 0xe3a00001u; /* mov r0, #1 */
                strncpy(out[n].what, "local battle: args[6] (with bots) -> 1", sizeof out[n].what - 1);
                out[n].what[sizeof out[n].what - 1] = 0;
                n++;
            }
        } else if (why && whyn) {
            snprintf(why, whyn, "bot word at +0x44 is not ldr r0,[fp,#0x10]: bots not forced");
        }
    }
    return n;
}

int bv_gamefix_plan(const uint8_t *text, uint32_t size, at_patch *out, int max, int *sites) {
    int n = 0, found = 0;
    uint32_t getter = 0;
    if (sites) *sites = 0;
    if (size < 0x10000 + 32) return 0;
    for (uint32_t a = 0x10000; a + 20 <= size; a += 4) /* ldr r0,[r0,#4] ; ldr r1,[pc,#4] ; ldr r1,[pc,r1] ; b <tail> ; .word 0x0062e6d8 */
        if (word_at(text, a) == 0xe5900004u && word_at(text, a + 4) == 0xe59f1004u && word_at(text, a + 8) == 0xe79f1001u && (word_at(text, a + 12) >> 24) == 0xea &&
            word_at(text, a + 16) == 0x0062e6d8u) {
            getter = a;
            break;
        }
    if (!getter) return 0;
    for (uint32_t a = 0x10004; a + 4 <= size; a += 4) {
        uint32_t w = word_at(text, a);
        if ((w >> 24) != 0xeb) continue; /* bl (always) */
        uint32_t off = w & 0xffffff;
        int32_t soff = (off & 0x800000) ? (int32_t)off - (1 << 24) : (int32_t)off;
        if ((uint32_t)((int32_t)a + 8 + soff * 4) != getter) continue;
        uint32_t prev = word_at(text, a - 4);
        if ((prev & 0xfffffff0u) != 0xe1a00000u || (prev & 15) == 0 || (prev & 15) > 12) continue; /* mov r0, rX (X = r1..r12) */
        found++;
        if (n + 2 > max) continue;
        out[n].off = a - 4;
        out[n].old_word = prev;
        out[n].new_word = prev | 0x00100000u; /* movs r0, rX */
        strncpy(out[n].what, "NULL-safe accessor call: mov r0,rX -> movs r0,rX", sizeof out[n].what - 1);
        out[n].what[sizeof out[n].what - 1] = 0;
        n++;
        out[n].off = a;
        out[n].old_word = w;
        out[n].new_word = (w & 0x00ffffffu) | 0x1b000000u; /* blne */
        strncpy(out[n].what, "NULL-safe accessor call: bl -> blne", sizeof out[n].what - 1);
        out[n].what[sizeof out[n].what - 1] = 0;
        n++;
    }
    if (sites) *sites = found;
    return n;
}

int bv_gamefix_plan_pages_cave(const uint8_t *text, uint32_t size, uint32_t cave_off, const uint8_t *cave_mem, at_patch *out, int max, int *ncave, char *why, unsigned whyn) {
    if (ncave) *ncave = 0;
    if (why && whyn) why[0] = 0;
    if (max < 7 || size < 0x10000 + 64) {
        if (why) snprintf(why, whyn, "bad arguments (max %d, size 0x%x)", max, (unsigned)size);
        return 0;
    }
    uint32_t site = 0, found = 0;
    for (uint32_t a = 0x10004; a + 32 <= size; a += 4)
        if ((word_at(text, a) >> 24) == 0xeb && word_at(text, a + 4) == 0xe1550006u && (word_at(text, a + 8) >> 24) == 0x0a && word_at(text, a + 12) == 0xe598001cu &&
            word_at(text, a + 16) == 0xe3500000u && (word_at(text, a + 20) >> 24) == 0x0a && (word_at(text, a + 24) == 0xe1a00006u || word_at(text, a + 24) == 0xe1b00006u) &&
            ((word_at(text, a + 28) >> 24) == 0xeb || (word_at(text, a + 28) >> 24) == 0x1b)) {
            /* the last two words are `mov r0,r6 ; bl accessor` or, once the accessor patches are in, `movs r0,r6 ; blne accessor`: both are the site */
            site = a;
            found++;
        }
    if (found != 1) {
        if (why) snprintf(why, whyn, "the site matched %u times (expected once)", (unsigned)found);
        return 0;
    }
    uint32_t cave = 0;
    if (cave_mem) {
        /* the loader's own code block outside the image (vita_mem.c bv_mem_alloc_near): word aligned, not inside the scanned code, 5 zero words. The block tail is not used. */
        if ((cave_off & 3u) || cave_off < size) {
            if (why) snprintf(why, whyn, "site found at libg+0x%x but the given cave libg+0x%x is unaligned or inside the image (0x%x)", (unsigned)site, (unsigned)cave_off, (unsigned)size);
            return 0;
        }
        for (uint32_t o = 0; o < 20; o += 4)
            if (word_at(cave_mem, o) != 0) {
                if (why) snprintf(why, whyn, "site found at libg+0x%x but the given cave libg+0x%x is not zero (0x%08x at +%u)", (unsigned)site, (unsigned)cave_off, (unsigned)word_at(cave_mem, o), (unsigned)o);
                return 0;
            }
        cave = cave_off;
    } else {
        /* the cave: 5 zero words in the zeroed end of the code block. First choice: 16 bytes after the last non-zero word. If something non-zero sits in that tail (the log of 2026-11-05
         * found the site but no room), the highest run of 16 zero words (64 bytes) within the last 4 KB of the block is used, 16 bytes into the run. */
        uint32_t end = size;
        while (end >= 4 && word_at(text, end - 4) == 0) end -= 4;
        cave = (end + 3u + 16u) & ~3u;
        int ok = cave + 20 <= size;
        for (uint32_t o = cave; ok && o < cave + 20; o += 4)
            if (word_at(text, o) != 0) ok = 0;
        if (!ok) {
            cave = 0;
            uint32_t lo = size > 0x1000 ? size - 0x1000 : 0x10000;
            for (uint32_t s0 = (size - 64) & ~3u; s0 >= lo && !cave; s0 -= 4) {
                int zero = 1;
                for (uint32_t o = s0; o < s0 + 64; o += 4)
                    if (word_at(text, o) != 0) { zero = 0; break; }
                if (zero) cave = s0 + 16;
            }
            if (!cave) {
                if (why) snprintf(why, whyn, "site found at libg+0x%x but no zero run in the last 4 KB of the block (last non-zero word ends at 0x%x of 0x%x)", (unsigned)site, (unsigned)end, (unsigned)size);
                return 0;
            }
        }
    }
    uint32_t w = word_at(text, site);
    int32_t off = (w & 0x800000) ? (int32_t)(w & 0xffffff) - (1 << 24) : (int32_t)(w & 0xffffff);
    uint32_t callee = (uint32_t)((int32_t)site + 8 + off * 4);
    int32_t back = ((int32_t)callee - (int32_t)(cave + 4 + 8)) >> 2;
    int32_t fwd = ((int32_t)cave - (int32_t)(site + 8)) >> 2;
    if (back < -(1 << 23) || back >= (1 << 23) || fwd < -(1 << 23) || fwd >= (1 << 23)) {
        if (why) snprintf(why, whyn, "the cave at 0x%x is out of branch range of the site", (unsigned)cave);
        return 0;
    }
    const uint32_t helper[5] = {0xe92d4010u, 0xeb000000u | ((uint32_t)back & 0xffffff), 0xe3560000u, 0x11550006u, 0xe8bd8010u};
    static const char *const names[5] = {"page-change helper: push {r4,lr}", "page-change helper: bl original callee", "page-change helper: cmp r6,#0",
                                         "page-change helper: cmpne r5,r6", "page-change helper: pop {r4,pc}"};
    int n = 0;
    for (int i = 0; i < 5; i++, n++) {
        out[n].off = cave + 4u * (uint32_t)i;
        out[n].old_word = 0;
        out[n].new_word = helper[i];
        strncpy(out[n].what, names[i], sizeof out[n].what - 1);
        out[n].what[sizeof out[n].what - 1] = 0;
    }
    out[n].off = site;
    out[n].old_word = w;
    out[n].new_word = 0xeb000000u | ((uint32_t)fwd & 0xffffff);
    strncpy(out[n].what, "page change: bl callee -> bl helper", sizeof out[n].what - 1);
    out[n].what[sizeof out[n].what - 1] = 0;
    n++;
    out[n].off = site + 4;
    out[n].old_word = 0xe1550006u;
    out[n].new_word = 0xe1a00000u;
    strncpy(out[n].what, "page change: cmp r5,r6 -> nop (helper sets flags)", sizeof out[n].what - 1);
    out[n].what[sizeof out[n].what - 1] = 0;
    n++;
    if (ncave) *ncave = 5;
    return n;
}

int bv_gamefix_plan_pages_ex(const uint8_t *text, uint32_t size, at_patch *out, int max, int *ncave, char *why, unsigned whyn) {
    return bv_gamefix_plan_pages_cave(text, size, 0, NULL, out, max, ncave, why, whyn);
}
int bv_gamefix_plan_pages(const uint8_t *text, uint32_t size, at_patch *out, int max, int *ncave) { return bv_gamefix_plan_pages_ex(text, size, out, max, ncave, NULL, 0); }

int bv_gamefix_plan_ifaddrs(const uint8_t *text, uint32_t size, at_patch *out, int max, int *callers) {
    if (callers) *callers = 0;
    if (max < 4 || size < 0x10000 + 64) return 0;
    uint32_t callee = 0;
    int n_callers = 0;
    for (uint32_t a = 0x10000; a + 32 <= size; a += 4) {
        if (word_at(text, a) != 0xe1a0000du || (word_at(text, a + 4) >> 24) != 0xeb || word_at(text, a + 8) != 0xe59d6000u || word_at(text, a + 12) != 0xe3560000u ||
            (word_at(text, a + 16) >> 24) != 0x0a || word_at(text, a + 20) != 0xe1dd00b8u || word_at(text, a + 24) != 0xe3500002u || (word_at(text, a + 28) >> 24) != 0x0a)
            continue;
        uint32_t w = word_at(text, a + 4);
        int32_t off = (w & 0x800000) ? (int32_t)(w & 0xffffff) - (1 << 24) : (int32_t)(w & 0xffffff);
        uint32_t target = (uint32_t)((int32_t)(a + 4) + 8 + off * 4);
        if (callee && callee != target) return 0; /* two different callees: not what this was written for */
        callee = target;
        n_callers++;
    }
    if (!callee || callee + 64 > size) return 0;
    if ((word_at(text, callee) & 0xffff0000u) != 0xe92d0000u) return 0; /* push {...} */
    int netlink = 0;
    for (uint32_t o = callee; o < callee + 64; o += 4) /* mov r0,#0x10 ; mov r1,#3 : socket(AF_NETLINK, SOCK_RAW, ...) */
        if (word_at(text, o) == 0xe3a00010u && word_at(text, o + 4) != 0 && word_at(text, o + 4) == 0xe3a01003u) netlink = 1;
    if (!netlink) return 0;
    static const uint32_t stub[4] = {0xe3a01000u, 0xe5801000u, 0xe3a00000u, 0xe12fff1eu}; /* mov r1,#0 ; str r1,[r0] ; mov r0,#0 ; bx lr */
    static const char *const names[4] = {"getifaddrs: mov r1,#0", "getifaddrs: str r1,[r0] (empty list)", "getifaddrs: mov r0,#0 (success)", "getifaddrs: bx lr"};
    for (int i = 0; i < 4; i++) {
        out[i].off = callee + 4u * (uint32_t)i;
        out[i].old_word = word_at(text, callee + 4u * (uint32_t)i);
        out[i].new_word = stub[i];
        strncpy(out[i].what, names[i], sizeof out[i].what - 1);
        out[i].what[sizeof out[i].what - 1] = 0;
    }
    if (callers) *callers = n_callers;
    return 4;
}
