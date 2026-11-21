/* Host test of loader/csvpatch.c (cycle 8 A13). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../loader/csvpatch.h"

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    static const char in[] = "\"Name\",\"Damage\",\"DamagePercent\",\"Tid\"\r\n\"string\",\"int\",\"int\",\"string\"\r\n"
                             "\"ShellyWeapon\",300,,\"TID_A\"\r\n\"ColetteWeapon\",200,35,\"TID_B, x\"\r\n\"Other\",1,2,\"T\"\r\n";
    size_t ol;
    int done;
    char *a = bv_csv_patch(in, strlen(in), "ColetteWeapon", "DamagePercent", "37", &ol, &done);
    CHECK(a && done && strstr(a, "\"ColetteWeapon\",200,37,\"TID_B, x\"\r\n") && ol == strlen(a), "percent 35 -> 37");
    char *b = a ? bv_csv_patch(a, ol, "ColetteWeapon", "Damage", "500", &ol, &done) : NULL;
    CHECK(b && strstr(b, "\"ColetteWeapon\",500,37,") && strstr(b, "\"ShellyWeapon\",300,,\"TID_A\""), "damage 200 -> 500, other rows unchanged");
    CHECK(bv_csv_patch(in, strlen(in), "Nobody", "Damage", "1", &ol, &done) == NULL && !done, "unknown row");
    CHECK(bv_csv_patch(in, strlen(in), "ColetteWeapon", "Nope", "1", &ol, &done) == NULL, "unknown column");
    char *c = bv_csv_patch(in, strlen(in), "ShellyWeapon", "DamagePercent", "10", &ol, &done);
    CHECK(c && strstr(c, "\"ShellyWeapon\",300,10,\"TID_A\""), "empty cell filled");
    {   /* update 127: enable rows of some modes */
        static const char loc[] = "\"Name\",\"Disabled\",\"GameMode\"\r\n\"string\",\"boolean\",\"string\"\r\n"
                                  "\"Gem1\",true,\"CoinRush\"\r\n\"Gem2\",,\"CoinRush\"\r\n\"Take1\",true,\"BossRace\"\r\n\"Hot1\",\"true\",\"KingOfHill\"\r\n";
        static const char *const m[] = {"CoinRush", "KingOfHill"};
        size_t el;
        int cnt;
        char *e = bv_csv_enable_modes(loc, strlen(loc), "GameMode", m, 2, "Disabled", &el, &cnt, NULL, NULL);
        CHECK(e && cnt == 2 && strstr(e, "\"Gem1\",,\"CoinRush\"\r\n") && strstr(e, "\"Hot1\",,\"KingOfHill\"") && strstr(e, "\"Take1\",true,\"BossRace\"")
              && strstr(e, "\"Gem2\",,\"CoinRush\"") && el == strlen(e), "enable modes: 2 rows changed (%d)", cnt);
        free(e);
    }
    {   /* update 167: row '*' */
        const char *cc = "Name,ShadowScaleX,Other\n\"String\",\"int\",\"int\"\nA,100,1\n,,2\nB,,3\nC,80,4\n";
        size_t ol = 0; int dn = 0;
        char *o = bv_csv_patch(cc, strlen(cc), "*", "ShadowScaleX", "0", &ol, &dn);
        const char *want = "Name,ShadowScaleX,Other\n\"String\",\"int\",\"int\"\nA,0,1\n,,2\nB,,3\nC,0,4\n";
        CHECK(o && dn && !strcmp(o, want), "star row: %s", o ? o : "(null)");
        free(o);
    }
    {   /* update 175: copy row */
        const char *t = "Name,A,B\r\n\"string\",\"string\",\"int\"\r\nSnowy,x_snow,1\r\nDefault,rock,2\r\n";
        size_t ol = 0;
        char *o = bv_csv_copy_row(t, strlen(t), "Snowy", "Default", &ol);
        CHECK(o && !strcmp(o, "Name,A,B\r\n\"string\",\"string\",\"int\"\r\nSnowy,rock,2\r\nDefault,rock,2\r\n") && ol == strlen(o), "copy row: %s", o ? o : "(null)");
        free(o);
    }
    free(a); free(b); free(c);
    if (g_fail) { printf("%d FAILURES\n", g_fail); return 1; }
    printf("ALL CHECKS PASSED\n");
    return 0;
}
