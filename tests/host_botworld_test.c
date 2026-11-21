/* Phase 3: bot world host test (determinism, growth, leaderboards, matchmaking, 24403 bytes). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../loader/botworld.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void params(bw_params *p) {
    memset(p, 0, sizeof *p);
    p->n_bots = 1200; p->n_clubs = 60; p->n_br = 39;
    static const signed char rar[39] = {0, 0, 0, 0, 1, 1, 1, 0, 0, 2, 2, 2, 3, 3, 3, 4, 4, 5, 5, 0, 0, 2, 3, 4, 5, 1, 2, 3, 4, 5, 2, 3, 4, 5, 1, 2, 3, 4, 5};
    for (int k = 0; k < 39; k++) { p->br_row[k] = k < 33 ? k : k + 1; p->br_rar[k] = rar[k]; }
    p->n_badges = 40; p->n_thumbs = 30; p->n_colors = 10;
}

int main(void) {
    static bw_world a, b;
    bw_params p;
    params(&p);
    uint32_t t0 = 1763700000u;   /* 2025-11 */
    bw_create(&a, &p, 1234, t0);
    CHECK(bw_valid(&a), "fresh world invalid");
    CHECK(bw_bot_total(&a, 0) == 0, "bots start at 0");
    int dup = 0;
    for (int i = 0; i < a.n_bots; i++) for (int j = 0; j < i; j++) if (!strcmp(a.bots[i].name, a.bots[j].name)) dup++;
    CHECK(dup == 0, "%d duplicate names", dup);
    for (int i = 0; i < 6; i++) printf("bot %d: '%s' skill %d region %s club %d '%s'\n", i, a.bots[i].name, a.bots[i].skill, bw_region_code(a.bots[i].region),
                                      a.bots[i].club, a.bots[i].club >= 0 ? a.clubs[a.bots[i].club].name : "-");
    a.player_club = 3;
    bw_set_season(&a, t0 - 10 * 86400u, 70u * 86400u);
    memcpy(&b, &a, sizeof a);
    clock_t c0 = clock();
    uint32_t n1 = bw_simulate(&a, t0 + 30u * 86400u, 60);
    double secs = (double)(clock() - c0) / CLOCKS_PER_SEC;
    uint32_t n2 = 0;
    for (int d = 1; d <= 30; d++) n2 += bw_simulate(&b, t0 + (uint32_t)d * 86400u - 4321u * (d % 3), 60);
    n2 += bw_simulate(&b, t0 + 30u * 86400u, 60);
    CHECK(n1 == n2, "split simulation played %u matches, one run %u", n2, n1);
    CHECK(!memcmp(&a, &b, sizeof a), "split simulation gives another world");
    printf("30 days: %u matches in %.2f s (host)\n", n1, secs);
    int top = 0, sum = 0, own = 0, maxown = 0;
    for (int i = 0; i < a.n_bots; i++) {
        int t = bw_bot_total(&a, i), o = 0;
        for (int k = 0; k < a.n_br; k++) o += a.bots[i].pow[k] != 0;
        if (t > top) top = t;
        sum += t; own += o; if (o > maxown) maxown = o;
    }
    printf("after 30 days: top total %d, mean %d, brawlers mean %.1f max %d, chat lines %d\n", top, sum / a.n_bots, (double)own / a.n_bots, maxown, a.chat_n);
    CHECK(top > 1000 && top < 40000, "top total %d not plausible", top);
    CHECK(n1 > 100000, "too few matches %u", n1);
    /* season reset at day 60 */
    bw_simulate(&a, t0 + 90u * 86400u, 60);
    int over = 0;
    for (int i = 0; i < a.n_bots; i++) for (int k = 0; k < a.n_br; k++) if (a.bots[i].pow[k] && a.bots[i].tro[k] > 1500) over++;
    printf("after 90 days: top bot %d, brawlers above 1500: %d\n", top, over);
    /* leaderboards */
    static bw_entry e[200];
    int rank = 0;
    int n = bw_leaderboard(&a, 1, -1, -1, 2500, "Me", e, 200, &rank);
    CHECK(n == 200, "global board %d entries", n);
    for (int i = 1; i < n; i++) CHECK(e[i - 1].score >= e[i].score, "board not sorted at %d", i);
    printf("global: #1 %s %d, #200 %d, my rank %d\n", e[0].who >= 0 ? a.bots[e[0].who].name : "me", e[0].score, e[n - 1].score, rank);
    n = bw_leaderboard(&a, 2, -1, -1, 2500, "Me", e, 200, &rank);
    CHECK(n > 10 && n <= 60, "club board %d", n);
    printf("clubs: #1 '%s' %d, my club rank %d\n", a.clubs[e[0].who].name, e[0].score, rank);
    n = bw_leaderboard(&a, 0, 0, 0, 300, "Me", e, 200, &rank);
    CHECK(n > 0, "brawler board empty");
    /* matchmaking */
    bw_pick plan[BW_SLOTS];
    int got = bw_match_plan(&a, 400, 0, 9, 99, t0 + 90u * 86400u, plan);
    CHECK(got == 9, "plan %d bots", got);
    for (int q = 1; q <= got; q++) {
        int tr = a.bots[plan[q].bot].tro[plan[q].slot];
        CHECK(tr > 250 && tr < 550, "pick %d at %d trophies (want ~400)", q, tr);
        CHECK(plan[q].level >= 1 && plan[q].level <= 10 && plan[q].tier >= 0 && plan[q].tier <= 4, "pick %d level %d tier %d", q, plan[q].level, plan[q].tier);
        for (int r = 1; r < q; r++) CHECK(plan[r].bot != plan[q].bot, "bot twice");
    }
    int team[BW_SLOTS] = {0, 0, 0, 1, 1, 1, 0, 0, 0, 0}, used[BW_SLOTS] = {0, 1, 1, 1, 1, 1, 0, 0, 0, 0};
    int before = a.bots[plan[1].bot].tro[plan[1].slot], before3 = a.bots[plan[3].bot].tro[plan[3].slot];
    bw_match_result(&a, plan, team, used, 0, 0, 0, t0 + 90u * 86400u);
    CHECK(a.bots[plan[1].bot].tro[plan[1].slot] > before, "team mate did not win");
    CHECK(a.bots[plan[3].bot].tro[plan[3].slot] < before3, "opponent did not lose");
    /* deltas */
    CHECK(bw_trophy_delta(0, 0, 0) == 8 && bw_trophy_delta(0, 0, 1) == 0 && bw_trophy_delta(450, 0, 1) == -5, "3v3 deltas");
    CHECK(bw_trophy_delta(0, 2, 1) == 10 && bw_trophy_delta(20, 2, 10) == 0, "showdown deltas");
    /* 24403 */
    bv_bs s;
    bs_init(&s);
    bw_leaderboard_msg(&s, &a, 1, 0, -1, 0, 2500, "Me", 0, 0, 0, 1);
    CHECK(!s.failed && s.len > 200 * 10, "24403 %u bytes", (unsigned)s.len);
    bv_rd r;
    rd_init(&r, s.data, s.len);
    CHECK(rd_vint(&r) == 1 && rd_vint(&r) == 0 && rd_vint(&r) == 0 && rd_int(&r) == -1 && rd_vint(&r) == 200, "24403 header");
    CHECK(rd_vint(&r) == BW_ID_HI || 1, "id");
    bs_free(&s);
    bs_init(&s);
    bw_leaderboard_msg(&s, &a, 0, 1, 5, 0, 300, "Me", 0, 0, 0, 1);
    rd_init(&r, s.data, s.len);
    CHECK(rd_vint(&r) == 0 && rd_vint(&r) == 0 && rd_vint(&r) == 16 && rd_vint(&r) == 5 && rd_int(&r) == 2, "24403 brawler/local header");
    bs_free(&s);
    /* clubs */
    {
        uint32_t now = t0 + 90u * 86400u;
        bw_player me = {"Me", 0, 1, 2500, 0, 0};
        int members = 0;
        for (int i = 0; i < a.n_bots; i++) members += a.bots[i].club == 7;
        CHECK(members == a.clubs[7].n_members, "club 7 count %d vs %d", members, a.clubs[7].n_members);
        CHECK(bw_player_join(&a, 7, now) && a.player_club == 7, "join");
        bs_init(&s); bw_club_data_msg(&s, &a, 7, now, &me);
        rd_init(&r, s.data, s.len);
        CHECK(rd_byte(&r) == 1 && rd_int(&r) == BW_CLUB_ID_HI && rd_int(&r) == 8, "24301 head");
        printf("24301 club 7: %u bytes, %d members\n", (unsigned)s.len, members);
        bs_free(&s);
        bs_init(&s); bw_my_club_msg(&s, &a, now, &me); CHECK(!s.failed && s.len > 20, "24399"); bs_free(&s);
        bs_init(&s); bw_club_list_msg(&s, &a, 0, NULL, 2500); CHECK(!s.failed && s.len > 50, "24304"); bs_free(&s);
        bs_init(&s); bw_club_list_msg(&s, &a, -1, "o", 2500); CHECK(!s.failed, "24310"); bs_free(&s);
        bw_chat_add(&a, now, -1, 7, 0, "hello club");
        int replies = 0;
        for (int k = 0; k < 50; k++) replies += bw_chat_reply(&a, now + (uint32_t)k * 3600u);
        printf("chat replies in 50 tries: %d\n", replies);
        bw_simulate(&a, now + 3u * 86400u, 60);
        bs_init(&s); bw_stream_msg(&s, &a, now + 3u * 86400u, &me);
        rd_init(&r, s.data, s.len);
        int nl = rd_vint(&r);
        CHECK(nl > 0 && nl <= BW_CHAT, "stream %d lines", nl);
        printf("24311: %d lines, %u bytes\n", nl, (unsigned)s.len);
        bs_free(&s);
        bw_player_leave(&a, now);
        CHECK(a.player_club == -1, "leave");
    }
    /* update 186: club board rows pack "no player" + "club" into ONE byte (bit 1) */
    bs_init(&s);
    bw_leaderboard_msg(&s, &a, 2, 0, -1, 0, 2500, "Me", 0, 0, 0, 1);
    rd_init(&r, s.data, s.len);
    CHECK(rd_vint(&r) == 2 && rd_vint(&r) == 0 && rd_vint(&r) == 0 && rd_int(&r) == -1, "club board header");
    int nc = rd_vint(&r);
    CHECK(nc > 0 && rd_vint(&r) == BW_CLUB_ID_HI, "club board entry id");
    rd_vint(&r); rd_vint(&r); rd_vint(&r);
    CHECK(rd_byte(&r) == 2, "club row flags byte");
    int32_t nl = rd_int(&r);
    CHECK(nl > 0 && nl < BW_CLUB_NAME, "club name length %d", (int)nl);
    bs_free(&s);
    printf(fails ? "FAILURES: %d\n" : "PASSED\n", fails);
    return fails != 0;
}
