/* Build B's self-check ("anti-tamper") in libg.so.
 *
 * libg+0xb528c is an obfuscated routine, called from libg+0x3430a8 with (0, 0), that inspects its surroundings: it re-protects the pages of six libc functions
 * (sysconf, mprotect, dlopen, dlsym, dladdr, getaddrinfo), looks at their first bytes, probes for a few library files and checks that the libc functions come
 * from libc. When anything looks wrong it sets a flag byte on its stack frame and the routine then calls a function chosen at random from the time of day
 * (gettimeofday, seeded through a small linear generator and a table of ten code addresses) with garbage arguments: a deliberately unpredictable crash.
 *
 * On the console this check cannot succeed (the "libc" functions are the loader's shims, no libc.so exists), and the first hardware run with the raw system calls
 * neutralised died exactly there (crash dump of 2026-10-25: pc = libg+0x1a6784, r0 = 0x991a6768 = target of the random call; reproduced step by step on the PC with
 * an ARM interpreter that runs the real routine: sysconf x6, mprotect x6, gettimeofday, blx r0). The two blocks that set the flag are `mov r0,#1 ; strb r0,[sp,#3]`;
 * with `mov r0,#0` the routine continues into its real work whatever the time is.
 *
 * bv_antitamper_plan_b() only PLANS the patches (portable, unit-tested against the real library on the PC); loader/antitamper_vita.c writes them.
 * Every patch states the word it expects: a different libg.so is left alone.
 */
#ifndef BV_ANTITAMPER_H
#define BV_ANTITAMPER_H

#include <stdint.h>

/* 33 random-call sites x 2 words + 2 flag setters = 68 patches in Build B's libg.so: 64 was too small (the surplus would have been dropped), 128 leaves room */
#define AT_MAX 128
/* how far (bytes) from the call the divide-by-ten generator of the same macro may lie, in front of it or behind it */
#define AT_GEN_WINDOW 0x400u

typedef struct at_patch {
    uint32_t off;       /* offset in libg.so (word aligned, ARM code) */
    uint32_t old_word;  /* expected instruction */
    uint32_t new_word;
    char what[56];
} at_patch;

/* returns the number of patches whose old word was found; *mismatch counts the table entries that did not match */
int bv_antitamper_plan_b(const uint8_t *text, uint32_t size, at_patch *out, int max, int *mismatch);

/* Second layer: the random call itself. The same macro (a pc-relative table base plus an offset in r1, stored to a stack slot, then `blx r0`; the offset comes from
 * a time-seeded generator whose division by ten uses the constant 0xcccccccd) is compiled into 33 places of libg (19 with the generator in front of the call, 14 with it behind). Each `str r0,[slot] ; blx r0` becomes
 * `mov r0,#0 ; str r0,[slot]`: no call, and the slot that the loop around the call tests for zero reads zero, so the block ends as if the call had returned.
 * Sites are found by shape, never by address, and a site is only taken when every part of the shape is there (the generator may be on either side of the call, within AT_GEN_WINDOW). Returns the number of patches written to out[]. */
int bv_antitamper_plan_reactions_b(const uint8_t *text, uint32_t size, at_patch *out, int max, int *sites);

/* Finds the random-call sites in the image as it is NOW (store offsets of each `blx r0`, at most max of them) and returns how many there are in all.
 * Run on the already patched library it must return 0; the loader logs that number so that a site this shape misses shows up in the next log. */
int bv_antitamper_find_reactions_b(const uint8_t *text, uint32_t size, uint32_t *site_off, int max);

struct so_module;
int bv_antitamper_apply(const struct so_module *m, const at_patch *p, int n);

#endif
