/* Raw Linux system calls inside Build B's libg.so.
 *
 * The game's own code contains hand-written syscall stubs and inline `mov r7,#nr ; svc #0` sequences (165 `svc #0` words in the ARM code;
 * roughly 40 are inline gettimeofday seeds of the obfuscator, the rest are file, socket and process calls of embedded helper code).
 * On the console `svc #0` is the *console* kernel's entry (r12 = its call number), so executed unpatched these would run with whatever r12 holds.
 * Found by the independent review of Port 2 (which has the same engine) and checked against Build B here.
 *
 * bv_rawsys_plan_b() only PLANS the patches (portable, unit-tested against the real library on the PC); loader/rawsys_vita.c writes them.
 * Every `svc #0` word that follows a load of r7 becomes `mov r0,#0` (calls whose success is harmless: gettimeofday, close, munmap ...) or
 * `mvn r0,#37` (-ENOSYS: the helper code takes its error path). The game server transport is not affected: its connect goes through the libc
 * PLT and the offline path replaces the send function (docs/REVIEW_m4.md).
 */
#ifndef BV_RAWSYS_H
#define BV_RAWSYS_H

#include <stddef.h>
#include <stdint.h>

#define RS_MAX 256

typedef struct rs_patch {
    uint32_t off;
    uint8_t len;      /* bytes written (4) */
    uint8_t bytes[8];
    char shim[16];    /* unused here, kept so that both ports share loader/rawsys_vita.c */
    char what[40];
    int nr;           /* decoded syscall number, -1 if the number is not an immediate */
} rs_patch;

typedef struct rs_stats {
    int svc_words;    /* `svc #0` words found in the text */
    int planned;
    int to_zero, to_enosys;
    int unknown_nr;   /* planned although the number could not be decoded (register-held) */
    int skipped;      /* words left alone (none: see rawsys.c) */
    int far_load;     /* planned although no r7 load is within 8 words in front (nr = -2): patched as unknown -> -ENOSYS */
} rs_stats;

int bv_rawsys_plan_b(const uint8_t *text, uint32_t size, rs_patch *out, int max, rs_stats *st);

struct so_module;
int bv_rawsys_apply(const struct so_module *m, const rs_patch *p, int n);

#endif
