/* Host test of loader/passdata.c with a small made-up CSV in the game's format (header line, types line, rows; quoted fields). */
#include <stdio.h>
#include <string.h>

#include "../loader/passdata.h"

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char k_ms[] =
    "\"Name\",\"Type\",\"Index\",\"ProgressStart\",\"Progress\",\"League\",\"Tier\",\"Season\",\"SeasonEndRewardKeys\",\"PrimaryLvlUpRewardType\","
    "\"PrimaryLvlUpRewardCount\",\"PrimaryLvlUpRewardExtraData\",\"PrimaryLvlUpRewardData\",\"SecondaryLvlUpRewardType\",\"SecondaryLvlUpRewardCount\","
    "\"SecondaryLvlUpRewardExtraData\",\"SecondaryLvlUpRewardData\",\"DependsOnIndex\"\r\n"
    "\"string\",\"int\",\"int\",\"int\",\"int\",\"int\",\"int\",\"int\",\"int\",\"int\",\"int\",\"int\",\"string\",\"int\",\"int\",\"int\",\"string\",\"int\"\r\n"
    "\"Trophy_1\",\"1\",\"0\",\"0\",\"10\",\"0\",\"0\",\"0\",\"0\",\"1\",\"20\",\"0\",\"\",\"-1\",\"\",\"\",\"\",\"-1\"\r\n"
    "\"Goal_9_2_0\",\"9\",\"0\",\"-1\",\"1\",\"0\",\"0\",\"2\",\"0\",\"24\",\"1\",\"0\",\"TestSkinB\",\"14\",\"1\",\"\",\"\",\"-1\"\r\n"
    "\"Goal_9_2_1\",\"9\",\"1\",\"0\",\"150\",\"0\",\"0\",\"2\",\"0\",\"1\",\"100\",\"0\",\"\",\"-1\",\"\",\"\",\"\",\"-1\"\r\n"
    "\"Goal_10_2_3\",\"10\",\"3\",\"400\",\"500\",\"0\",\"0\",\"2\",\"0\",\"12\",\"50\",\"0\",\"\",\"-1\",\"\",\"\",\"\",\"-1\"\r\n";

static const char k_skins[] =
    "\"Name\",\"Conf\",\"Campaign\"\r\n\"string\",\"string\",\"int\"\r\n"
    "\"TestSkinA\",\"TestSkinA\",\"\"\r\n\"TestSkinB\",\"TestSkinB\",\"1\"\r\n\"Test,Comma\",\"x\",\"\"\r\n";

int main(void) {
    static bv_passdata p;
    CHECK(bv_pass_load_milestones(&p, k_ms) == 3, "3 pass rows kept (Type 9/10), the trophy row skipped (%d)", p.n);
    CHECK(bv_pass_load_skins(&p, k_skins) == 3 && bv_pass_skin_row(&p, "TestSkinB") == 1 && bv_pass_skin_row(&p, "Test,Comma") == 2 &&
          bv_pass_skin_row(&p, "nope") == -1, "skin rows by name (quoted comma ok)");
    const bv_pass_goal *g = bv_pass_find(&p, 9, 2, 0);
    CHECK(g && g->r1_type == 24 && !strcmp(g->r1_data, "TestSkinB") && g->r2_type == 14 && g->r2_count == 1, "Goal_9_2_0 fields");
    bmsg_box_item it[4];
    int un = 0;
    int n = bv_pass_items(&p, 9, 2, 0, 7, 0, 1, it, 4, &un);
    CHECK(n == 2 && it[0].type == 9 && it[0].skin == 1 && it[0].amount == 1 && it[1].type == 7 && it[1].amount == 1 && un == 14,
          "claim 9/2/0 = skin row 1 + (type 14 not decoded -> 1 coin, reported) (n %d, un %d)", n, un);
    n = bv_pass_items(&p, 9, 2, 1, 7, 0, 1, it, 4, &un);
    CHECK(n == 1 && it[0].type == 7 && it[0].amount == 100 && un == -1, "claim 9/2/1 = 100 coins");
    n = bv_pass_items(&p, 10, 2, 3, 7, 5, 0, it, 4, &un);
    CHECK(n == 1 && it[0].type == 6 && it[0].brawler == 5 && it[0].amount == 50, "premium 10/2/3 = 50 power points for brawler 5 when not maxed");
    n = bv_pass_items(&p, 10, 2, 3, 7, 5, 1, it, 4, &un);
    CHECK(n == 1 && it[0].type == 7 && it[0].amount == 1 && un == 12, "maxed: power points become 1 coin (reported as type 12)");
    CHECK(bv_pass_items(&p, 9, 3, 0, 7, 0, 1, it, 4, &un) == 0, "unknown tier: no items");
    CHECK(bv_pass_load_milestones(&p, "\"Foo\",\"Bar\"\r\n") == -1, "a file without the expected header is refused");
    {   /* update 91: trophy road (Type 6) last reached index */
        static const char tr[] =
            "\"Name\",\"Type\",\"Index\",\"ProgressStart\",\"Progress\"\r\n\"string\",\"int\",\"int\",\"int\",\"int\"\r\n"
            "\"goal_6_0\",\"6\",\"0\",\"0\",\"10\"\r\n\"goal_6_1\",\"6\",\"1\",\"10\",\"20\"\r\n"
            "\"goal_6_2\",\"6\",\"2\",\"950\",\"50\"\r\n\"goal_6_3\",\"6\",\"3\",\"1000\",\"100\"\r\n"
            "\"Goal_9_2_0\",\"9\",\"40\",\"0\",\"1\"\r\n";
        CHECK(bv_trophy_road_last(tr, 1000) == 2, "1000 trophies -> index 2 (950+50) (%d)", bv_trophy_road_last(tr, 1000));
        CHECK(bv_trophy_road_last(tr, 5) == -1 && bv_trophy_road_last(tr, 99999) == 3, "none / all reached");
        CHECK(bv_trophy_road_last("\"Foo\"\r\n", 1000) == -2, "unknown header");
    }
    /* cycle 4: daily skin offer (CostGems / ObtainType found by header name) */
    static const char k_shop[] =
        "\"Name\",\"ObtainType\",\"CostGems\"\r\n\"string\",\"int\",\"int\"\r\n"
        "\"Default\",\"\",\"\"\r\n\"Cheap\",\"\",\"30\"\r\n\"PassOnly\",\"3\",\"80\"\r\n\"Dear\",\"\",\"150\"\r\n";
    CHECK(bv_pass_load_skins(&p, k_shop) == 4 && p.skin_cost[1] == 30 && p.skin_obtain[2] == 3 && p.skin_cost[0] == 0, "cost / obtain columns");
    int seen[4] = {0}, bad = 0, dmin = 100, dmax = 0;
    for (int day = 20000; day < 20400; day++) {
        int pr = 0, d = 0, row = bv_daily_skin(&p, day, NULL, 0, &pr, &d);
        if (row != 1 && row != 3) bad++;
        else seen[row]++;
        if (row >= 0 && (pr != p.skin_cost[row] * (100 - d) / 100 || d < 25 || d > 75)) bad++;
        if (d < dmin) dmin = d;
        if (d > dmax) dmax = d;
        if (bv_daily_skin(&p, day, NULL, 0, NULL, NULL) != row) bad++;   /* same day = same offer */
    }
    CHECK(bad == 0 && seen[1] > seen[3] && seen[3] > 0, "only priced shop skins, cheaper more often (%d / %d), stable per day (%d bad)", seen[1], seen[3], bad);
    CHECK(dmin >= 25 && dmax <= 75 && dmax > dmin, "discounts within 25..75 (%d..%d)", dmin, dmax);
    {
        const int own[1] = {1};
        int pr = 0;
        CHECK(bv_daily_skin(&p, 20001, own, 1, &pr, NULL) == 3, "an owned skin is never offered");
        const int own2[2] = {1, 3};
        CHECK(bv_daily_skin(&p, 20001, own2, 2, &pr, NULL) == -1, "all owned: no offer");
    }
    /* cycle 5: daily skin set (special + full-price others) */
    {
        static const char k_shop2[] =
            "\"Name\",\"ObtainType\",\"CostGems\"\r\n\"string\",\"int\",\"int\"\r\n"
            "\"A\",\"\",\"30\"\r\n\"B\",\"\",\"80\"\r\n\"C\",\"\",\"150\"\r\n\"D\",\"3\",\"80\"\r\n\"E\",\"\",\"30\"\r\n";
        bv_pass_load_skins(&p, k_shop2);
        int rows[6], prices[6];
        int n = bv_daily_skins(&p, 20771, NULL, 0, rows, prices, 6);
        int dup = 0, bad = 0;
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < i; j++) if (rows[i] == rows[j]) dup++;
            if (rows[i] == 3) bad++;
            if (i > 0 && prices[i] != p.skin_cost[rows[i]]) bad++;
        }
        CHECK(n == 4 && !dup && !bad && prices[0] < p.skin_cost[rows[0]], "4 sellable skins, all different, only the first discounted (%d, dup %d bad %d)", n, dup, bad);
        int pr0 = 0, d0 = 0;
        CHECK(bv_daily_skin(&p, 20771, NULL, 0, &pr0, &d0) == rows[0] && pr0 == prices[0], "the special = the cycle 4 daily skin");
    }
    printf(g_fail ? "%d CHECK(S) FAILED\n" : "ALL CHECKS PASSED\n", g_fail);
    return g_fail != 0;
}
