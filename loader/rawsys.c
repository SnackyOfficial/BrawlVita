#include "rawsys.h"

#include <string.h>

static uint32_t rd32(const uint8_t *t, uint32_t off) {
    return (uint32_t)t[off] | ((uint32_t)t[off + 1] << 8) | ((uint32_t)t[off + 2] << 16) | ((uint32_t)t[off + 3] << 24);
}

#define SVC0 0xEF000000u
#define ARM_MOV_R0_0 0xE3A00000u /* mov r0,#0 */
#define ARM_MVN_R0_37 0xE3E00025u /* mvn r0,#37 = -38 = -ENOSYS */

/* Looks up to 8 words before the svc for the instruction that loads r7. Returns 1 when one was found; *nr is the immediate or -1. */
static int find_r7_load(const uint8_t *t, uint32_t size, uint32_t svc, int *nr) {
    for (int back = 1; back <= 8 && svc >= (uint32_t)(4 * back); back++) {
        uint32_t at = svc - 4u * (uint32_t)back;
        uint32_t w = rd32(t, at);
        if ((w & 0xFFFFF000u) == 0xE3A07000u) { /* mov r7,#imm8 ror 2*rot */
            uint32_t imm = w & 0xFF, rot = ((w >> 8) & 0xF) * 2;
            *nr = (int)(rot ? ((imm >> rot) | (imm << (32 - rot))) : imm);
            return 1;
        }
        if ((w & 0xFFF0F000u) == 0xE3007000u) { /* movw r7,#imm16 */
            *nr = (int)(((w >> 4) & 0xF000u) | (w & 0xFFFu));
            return 1;
        }
        if ((w & 0xFFFFF000u) == 0xE59F7000u) { /* ldr r7,[pc,#off] */
            uint32_t lit = at + 8 + (w & 0xFFF);
            *nr = lit + 4 <= size ? (int)rd32(t, lit) : -1;
            return 1;
        }
        if ((w & 0xFFFFFFF0u) == 0xE1A07000u) { /* mov r7,rX */
            *nr = -1;
            return 1;
        }
    }
    return 0;
}

int bv_rawsys_plan_b(const uint8_t *text, uint32_t size, rs_patch *out, int max, rs_stats *st) {
    rs_stats local;
    if (!st) st = &local;
    memset(st, 0, sizeof *st);
    int n = 0;
    for (uint32_t o = 0; o + 4 <= size; o += 4) {
        if (rd32(text, o) != SVC0) continue;
        st->svc_words++;
        int nr;
        /* The obfuscator sometimes schedules the load of r7 far from the svc; those words are real syscalls too (they sit inside the same
         * helper functions as the decoded ones), so they are patched as unknown calls (-ENOSYS) instead of being left to the console kernel. */
        if (!find_r7_load(text, size, o, &nr)) nr = -2;
        if (n >= max) continue;
        rs_patch *p = &out[n++];
        memset(p, 0, sizeof *p);
        p->off = o;
        p->len = 4;
        p->nr = nr;
        int zero;
        switch (nr) {
        case 6:   /* close */
        case 78:  /* gettimeofday */
        case 91:  /* munmap */
        case 125: /* mprotect */
        case 172: /* prctl */
        case 263: /* clock_gettime */
            zero = 1;
            break;
        default:
            zero = 0;
        }
        uint32_t w = zero ? ARM_MOV_R0_0 : ARM_MVN_R0_37;
        p->bytes[0] = (uint8_t)w;
        p->bytes[1] = (uint8_t)(w >> 8);
        p->bytes[2] = (uint8_t)(w >> 16);
        p->bytes[3] = (uint8_t)(w >> 24);
        strncpy(p->what, zero ? "svc -> 0" : "svc -> -ENOSYS", sizeof p->what - 1);
        if (nr < 0) st->unknown_nr++;
        if (nr == -2) st->far_load++;
        if (zero) st->to_zero++;
        else st->to_enosys++;
        st->planned++;
    }
    return n;
}
