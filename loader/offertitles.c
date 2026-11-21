/* update 129 (B11, experiment offer_tids=1): shop offer titles as text ids. Run 124: the special pack showed no title although 24101 offer +0x34 carried
 * "MEGA PACK" (INFERRED: the tile looks the string up as a TID, like every other shop text). Our titles get keys TID_BV_OFFER_<n>; the same rows are
 * appended to localization/texts.csv in memory (shims/shim_asset.c). Pure logic, host-tested (tests/host_offertitles_test.c). */
#include "offertitles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_titles[] = {"MEGA PACK", "BIG BOX PACK", "DOUBLE MEGA", "BRAWL BOX BUNDLE", "WEEKEND BIG BOXES", "MEGA BOX BONANZA",
                                       "SKIN OF THE DAY", "SATURDAY GIFT", "GIFT", "DAILY SKIN", "FREE", "MEGA BOX", "POWER POINTS", "TOKEN DOUBLER",
                                       "SEASONAL SKIN", "EXCLUSIVE SKIN", "BIG BOX"};
static const char *const k_keys[] = {"TID_BV_OFFER_0", "TID_BV_OFFER_1", "TID_BV_OFFER_2", "TID_BV_OFFER_3", "TID_BV_OFFER_4", "TID_BV_OFFER_5", "TID_BV_OFFER_6",
                                     "TID_BV_OFFER_7", "TID_BV_OFFER_8", "TID_BV_OFFER_9", "TID_BV_OFFER_10", "TID_BV_OFFER_11", "TID_BV_OFFER_12", "TID_BV_OFFER_13",
                                     "TID_BV_OFFER_14", "TID_BV_OFFER_15", "TID_BV_OFFER_16"};

const char *bv_offer_tid(const char *title) {
    for (unsigned i = 0; title && i < sizeof k_titles / sizeof k_titles[0]; i++)
        if (!strcmp(title, k_titles[i])) return k_keys[i];
    return title;
}

char *bv_offer_tids_patch(const char *in, size_t len, size_t *out_len) {
    size_t extra = 0;
    for (unsigned i = 0; i < sizeof k_titles / sizeof k_titles[0]; i++) extra += strlen(k_keys[i]) + strlen(k_titles[i]) + 8;
    char *out = (char *)malloc(len + extra + 3);
    if (!out) return NULL;
    memcpy(out, in, len);
    size_t o = len;
    if (o > 0 && out[o - 1] != '\n') { out[o++] = '\r'; out[o++] = '\n'; }
    for (unsigned i = 0; i < sizeof k_titles / sizeof k_titles[0]; i++)
        o += (size_t)sprintf(out + o, "\"%s\",\"%s\"\r\n", k_keys[i], k_titles[i]);
    out[o] = 0;
    if (out_len) *out_len = o;
    return out;
}
