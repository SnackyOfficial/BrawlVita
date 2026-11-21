/* update 129 (B11): shop offer titles as text ids (see offertitles.c). */
#ifndef BV_OFFERTITLES_H
#define BV_OFFERTITLES_H

#include <stddef.h>

/* "MEGA PACK" -> "TID_BV_OFFER_0"; unknown titles (and NULL) are returned unchanged */
const char *bv_offer_tid(const char *title);
/* copy of texts.csv with the TID_BV_OFFER_<n> rows appended (malloc'd, NULL if out of memory) */
char *bv_offer_tids_patch(const char *in, size_t len, size_t *out_len);

#endif
