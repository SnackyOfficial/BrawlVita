/* Host test of loader/botnames.c (cycle 8 G6). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../loader/botnames.h"

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    static const char in[] = "\"TID\",\"EN\"\n\"string\",\"string\"\n\"TID_BATTLE\",\"Battle\"\n\"TID_BOT_1\",\"Bot 1\"\n\"TID_BOT_12\",\"Bot 12\"\r\n\"TID_BOTX\",\"x\"\n\"TID_ROBOT\",\"DEPUTY\"";
    size_t ol = 0;
    int rep = 0;
    char *o = bv_botnames_patch(in, strlen(in), &ol, 5, &rep);
    CHECK(o != NULL && rep == 2, "two bot lines replaced (%d)", rep);
    char want1[64], want12[64];
    snprintf(want1, sizeof want1, "\"TID_BOT_1\",\"%s\"\n", bv_botname(5, 1));
    snprintf(want12, sizeof want12, "\"TID_BOT_12\",\"%s\"\r\n", bv_botname(5, 12));
    CHECK(o && strstr(o, want1) != NULL, "TID_BOT_1 -> %s", bv_botname(5, 1));
    CHECK(o && strstr(o, want12) != NULL, "TID_BOT_12 keeps its CR");
    CHECK(o && strstr(o, "\"TID_BATTLE\",\"Battle\"\n") && strstr(o, "\"TID_BOTX\",\"x\"\n") && strstr(o, "\"TID_ROBOT\",\"DEPUTY\""), "other lines unchanged");
    CHECK(o && ol == strlen(o), "length matches");
    CHECK(strcmp(bv_botname(5, 1), bv_botname(5, 2)) != 0, "different bots get different names");
    free(o);
    if (g_fail) { printf("%d FAILURES\n", g_fail); return 1; }
    printf("ALL CHECKS PASSED\n");
    return 0;
}
