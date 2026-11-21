/* Host test of loader/maprot.c (cycle 8 E3 / E6). */
#include <stdio.h>
#include <string.h>

#include "../loader/maprot.h"

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    static const char csv[] =
        "Name,Disabled,TID,GameMode\n"
        "string,Boolean,string,string\n"
        "HardRock,,TID_A,CoinRush\n"
        "OldMine,true,TID_B,CoinRush\n"
        "Skull,,TID_C,BattleRoyale\n"
        "\"Crystal, Arcade\",,TID_D,CoinRush\n"
        "Boss1,,TID_E,BossFight\n";
    int rows[8];
    int n = bv_maps_for_mode(csv, "CoinRush", rows, 8);
    CHECK(n == 2 && rows[0] == 0 && rows[1] == 3, "gem grab maps: rows 0 and 3, disabled row 1 skipped (%d)", n);
    CHECK(bv_maps_for_mode(csv, "BossFight", rows, 8) == 1 && rows[0] == 4, "boss fight map row 4");
    CHECK(bv_maps_for_mode("Name,TID\nstring,string\n", "CoinRush", rows, 8) == -1, "no GameMode column");
    int idx, left, id, idx2, left2, id2;
    bv_rotation(17280L * 1000 + 10, 1, 5, &idx, &left, &id);
    bv_rotation(17280L * 1001 + 10, 1, 5, &idx2, &left2, &id2);
    CHECK(left == 17270 && left2 == 17270, "seconds left in the 4h48m window (%d)", left);
    CHECK(idx2 == (idx + 1) % 5 && id2 != id, "next window: next map and a new event id");
    bv_rotation(17280L * 1000 + 10, 2, 5, &idx2, NULL, &id2);
    CHECK(id2 != id, "other slot: other event id");
    {   /* update 127: nameless continuation rows are not table rows */
        static const char c2[] = "Name,Disabled,GameMode\nstring,boolean,string\nA,,CoinRush\n,,\n,,\nB,,CoinRush\nC,true,CoinRush\n";
        int r2[8];
        CHECK(bv_maps_for_mode(c2, "CoinRush", r2, 8) == 2 && r2[0] == 0 && r2[1] == 1, "nameless rows skipped: B is table row 1 (%d)", r2[1]);
    }
    {   /* update 144 (req. 17): one global queue - 7 events take turns, each lasts 17280 s, only one changes at a time */
        const long P = 17280, base = 17280L * 5000;
        int changes = 0;
        for (long t = base; t < base + 2 * P; t += 60) {
            int moved = 0;
            for (int pos = 0; pos < 7; pos++) {
                int l1, l2;
                long k1 = bv_queue_rotation(t, pos, 7, P, &l1), k2 = bv_queue_rotation(t + 60, pos, 7, P, &l2);
                if (k2 != k1) { moved++; CHECK(k2 == k1 + 1, "rotation count grows by one"); }
                CHECK(l1 > 0 && l1 <= P, "seconds left inside one period (%d)", l1);
            }
            CHECK(moved <= 1, "at most one event changes per minute (%d)", moved);
            changes += moved;
        }
        CHECK(changes >= 13 && changes <= 15, "14 changes in two periods (7 events x 2): %d", changes);
        int l0, l1;
        long a = bv_queue_rotation(base + 5, 0, 7, P, &l0), b = bv_queue_rotation(base + 5 + P, 0, 7, P, &l1);
        CHECK(b == a + 1 && l0 == l1, "each event still lasts 4 h 48 min");
        /* the three rotating slots (queue positions 4, 5, 6) never show the same mode at once */
        int dup = 0;
        for (long t = base; t < base + 6 * P; t += 97) {
            int m[3];
            for (int j = 0; j < 3; j++) m[j] = bv_queue_mode(bv_queue_rotation(t, 4 + j, 7, P, NULL), j, 3, 4);
            if (m[0] == m[1] || m[1] == m[2] || m[0] == m[2]) dup++;
        }
        CHECK(dup == 0, "rotating slots always show three different modes (%d clashes)", dup);
    }
    if (g_fail) { printf("%d FAILURES\n", g_fail); return 1; }
    printf("ALL CHECKS PASSED\n");
    return 0;
}
