/* Host test of loader/offertitles.c (update 129, B11). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../loader/offertitles.h"

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    CHECK(!strcmp(bv_offer_tid("MEGA PACK"), "TID_BV_OFFER_0"), "mega pack key");
    CHECK(!strcmp(bv_offer_tid("TOKEN DOUBLER"), "TID_BV_OFFER_13"), "token doubler key");
    CHECK(!strcmp(bv_offer_tid("SOMETHING"), "SOMETHING") && bv_offer_tid(NULL) == NULL, "unknown unchanged");
    static const char in[] = "\"TID\",\"EN\"\r\n\"String\",\"String\"\r\n\"TID_A\",\"a\"";
    size_t ol;
    char *o = bv_offer_tids_patch(in, strlen(in), &ol);
    CHECK(o && ol == strlen(o) && strstr(o, "\"TID_A\",\"a\"\r\n\"TID_BV_OFFER_0\",\"MEGA PACK\"\r\n") && strstr(o, "\"TID_BV_OFFER_13\",\"TOKEN DOUBLER\"\r\n"),
          "rows appended after a newline");
    free(o);
    if (g_fail) { printf("%d FAILURES\n", g_fail); return 1; }
    printf("ALL CHECKS PASSED\n");
    return 0;
}
