/* cycle 8 (G6): bot names served instead of texts.csv "Bot 1".."Bot 9" (see botnames.c). Pure logic, host-tested. */
#ifndef BV_BOTNAMES_H
#define BV_BOTNAMES_H

#include <stddef.h>
#include <stdint.h>

const char *bv_botname(uint32_t seed, int n);
/* Phase 3: per-battle names of the planned world bots (slot 1..9; NULL = the seeded name) */
extern const char *bv_botname_override[10];
/* copy of texts.csv with the TID_BOT_<n> values replaced; malloc'd (caller frees), NULL if out of memory */
char *bv_botnames_patch(const char *in, size_t len, size_t *out_len, uint32_t seed, int *replaced);

#endif
