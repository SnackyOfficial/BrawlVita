/* ARM (A32) instruction encoders for the few patches Build B's code needs. Portable, header only, unit-tested (tests/host_offline_test.c). */
#ifndef BV_ARMENC_H
#define BV_ARMENC_H

#include <stdint.h>

/* b <to> placed at <from> (unconditional). Returns 0 and the word in *out, or -1 if the branch is out of range / unaligned. */
static inline int arm_b(uint32_t from, uint32_t to, uint32_t *out) {
    if ((from | to) & 3u) return -1;
    int32_t off = ((int32_t)(to - (from + 8u))) >> 2;
    if (off < -(1 << 23) || off >= (1 << 23)) return -1;
    *out = 0xEA000000u | ((uint32_t)off & 0x00FFFFFFu);
    return 0;
}

#define ARM_LDR_PC_NEXT 0xE51FF004u /* ldr pc, [pc, #-4]: jumps to the word that follows (a bx-style interworking load) */
#define ARM_MOV_R0_0 0xE3A00000u
#define ARM_MOV_R0_1 0xE3A00001u
#define ARM_BX_LR 0xE12FFF1Eu

static inline void arm_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static inline uint32_t arm_get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* The 8-byte trampoline: ldr pc,[pc,#-4] ; .word target. `target` carries the Thumb bit if the destination is Thumb code. */
static inline void arm_trampoline(uint8_t out[8], uint32_t target) {
    arm_put32(out, ARM_LDR_PC_NEXT);
    arm_put32(out + 4, target);
}

#endif
