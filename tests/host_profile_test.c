/* Host test of loader/profile.c: default account, text round trip, battle results, bad input. */
#include <stdio.h>
#include <string.h>

#include "../loader/profile.h"

static int g_fail;
#define CHECK(c, ...)                         \
    do {                                      \
        if (!(c)) {                           \
            g_fail++;                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);              \
            printf("\n");                     \
        }                                     \
    } while (0)

int main(void) {
    bv_profile p, q;
    bv_profile_default(&p);
    CHECK(p.n == 1 && p.b[0].row == 0 && p.selected_brawler == 0 && p.selected_map == 7, "default account: one brawler, row 0, map 7");
    CHECK(bv_profile_total_trophies(&p) == 0, "no trophies at first");

    bv_profile_battle(&p, 8, 1);
    bv_profile_battle(&p, -3, 0);
    CHECK(p.b[0].trophies == 5 && p.b[0].best_trophies == 8 && p.battles == 2 && p.wins == 1, "battle results: %d/%d %d %d", p.b[0].trophies, p.b[0].best_trophies, p.battles, p.wins);
    bv_profile_battle(&p, -50, 0);
    CHECK(p.b[0].trophies == 0 && p.b[0].best_trophies == 8, "trophies never go below 0");

    CHECK(bv_profile_unlock(&p, 4) && bv_profile_unlock(&p, 4) == bv_profile_find(&p, 4) && p.n == 2, "unlocking twice keeps one entry");
    p.selected_brawler = 4;
    bv_profile_battle(&p, 10, 1);
    CHECK(bv_profile_find(&p, 4)->trophies == 10 && bv_profile_total_trophies(&p) == 10, "the selected brawler gets the trophies");
    p.coins = 1234; p.gems = 56; p.unlock_all = 1;
    snprintf(p.name, sizeof p.name, "Tester");

    char buf[4096];
    size_t need = bv_profile_format(&p, NULL, 0);
    size_t w = bv_profile_format(&p, buf, sizeof buf);
    CHECK(need == w && w > 0 && w < sizeof buf, "format: size query %u == written %u", (unsigned)need, (unsigned)w);
    int bad = bv_profile_parse(&q, buf);
    CHECK(bad == 0, "round trip parses cleanly (%d bad lines)\n%s", bad, buf);
    CHECK(q.n == 2 && q.coins == 1234 && q.gems == 56 && q.unlock_all == 1 && q.selected_brawler == 4 && !strcmp(q.name, "Tester") && q.battles == 4 && q.wins == 2,
          "round trip keeps every field");
    CHECK(bv_profile_find(&q, 0) && bv_profile_find(&q, 0)->best_trophies == 8 && bv_profile_find(&q, 4)->trophies == 10, "round trip keeps the brawlers");

    char small[16];
    size_t r = bv_profile_format(&p, small, sizeof small);
    CHECK(r == need, "a buffer that is too small reports the size needed (%u)", (unsigned)r);

    bad = bv_profile_parse(&q, "garbage\r\ncoins=7\r\nbrawler=3,1,0,0,0\r\nbrawler=x\nselected_brawler=99\n");
    CHECK(bad == 2 && q.coins == 7 && q.n == 1 && q.b[0].row == 3 && q.selected_brawler == 3 && q.b[0].best_trophies == 1,
          "bad lines are counted and skipped; CRLF works; a selected brawler that is not owned falls back to the first owned (%d bad, sel %d)", bad, q.selected_brawler);
    bad = bv_profile_parse(&q, "");
    CHECK(bad == 0 && q.n == 1 && q.b[0].row == 0, "an empty file gives the default account");

    for (int i = 0; i < BV_PROFILE_MAX_BRAWLERS + 5; i++) bv_profile_unlock(&q, 100 + i);
    CHECK(q.n == BV_PROFILE_MAX_BRAWLERS && bv_profile_unlock(&q, 5000) == NULL, "the list stops at its size");

    /* cycle 1 (stream G): owned skins and today's bought offers */
    bv_profile_default(&p);
    CHECK(p.n_skins == 0 && !bv_profile_has_skin(&p, 2), "a fresh account owns no extra skins");
    CHECK(bv_profile_own_skin(&p, 2) && bv_profile_own_skin(&p, 2) && p.n_skins == 1 && bv_profile_has_skin(&p, 2), "owning a skin twice keeps one entry");
    CHECK(!bv_profile_own_skin(&p, -1), "a negative skin row is refused");
    CHECK(bv_profile_mark_bought(&p, 20000, 0) && bv_profile_is_bought(&p, 20000, 0) && !bv_profile_is_bought(&p, 20000, 1), "offer 0 bought today");
    CHECK(!bv_profile_is_bought(&p, 20001, 0), "a bought offer is not bought on another day");
    CHECK(bv_profile_mark_bought(&p, 20000, 1) && p.n_bought == 2, "two offers bought today");
    w = bv_profile_format(&p, buf, sizeof buf);
    bad = bv_profile_parse(&q, buf);
    CHECK(bad == 0 && q.n_skins == 1 && q.skins[0] == 2 && q.offer_day == 20000 && q.n_bought == 2 && bv_profile_is_bought(&q, 20000, 1),
          "round trip keeps skins and today's offers (%d bad)\n%s", bad, buf);
    CHECK(bv_profile_mark_bought(&q, 20001, 5) && q.n_bought == 1 && !bv_profile_is_bought(&q, 20001, 0), "a new day clears the bought list");
    for (int i = 0; i < BV_PROFILE_MAX_SKINS + 3; i++) bv_profile_own_skin(&q, 1000 + i);
    CHECK(q.n_skins == BV_PROFILE_MAX_SKINS && !bv_profile_own_skin(&q, 9999), "the skin list stops at its size");

    /* cycle 4: claimed Brawl Pass tiers */
    bv_profile_default(&p);
    unsigned bits[4];
    CHECK(!bv_profile_is_claimed(&p, 9, 2, 0) && bv_profile_mark_claimed(&p, 9, 2, 0) && bv_profile_mark_claimed(&p, 9, 2, 0) && p.n_claimed == 1,
          "claim 9/2/0 once");
    bv_profile_mark_claimed(&p, 9, 2, 33);
    bv_profile_mark_claimed(&p, 10, 2, 1);
    bv_profile_mark_claimed(&p, 9, 1, 5);
    CHECK(bv_profile_claimed_bits(&p, 9, 2, bits) == 2 && bits[0] == 4u && bits[1] == 8u && bits[2] == 0 && bits[3] == 0, "type 9 bits of season 2: tier 0 -> bit 2, tier 33 -> bit 35 (client tests tier + 2)");
    CHECK(bv_profile_claimed_bits(&p, 10, 2, bits) == 1 && bits[0] == 8u, "type 10 bits of season 2: tier 1 -> bit 3");
    w = bv_profile_format(&p, buf, sizeof buf);
    bad = bv_profile_parse(&q, buf);
    CHECK(bad == 0 && q.n_claimed == 4 && bv_profile_is_claimed(&q, 9, 1, 5) && bv_profile_is_claimed(&q, 10, 2, 1), "claimed tiers survive the round trip");

    {   /* update 140: battle history */
        bv_profile h;
        bv_profile_default(&h);
        for (int i = 0; i < 12; i++) bv_profile_add_battle(&h, 1000 + i, i & 1 ? 2 : 0, i % 3, i - 4, 128, 16);
        CHECK(h.n_hist == 12 && h.hist[11 % 10].t == 1011, "ring of 10, newest last");
        char buf[4096];
        bv_profile_format(&h, buf, sizeof buf);
        bv_profile g;
        bv_profile_parse(&g, buf);
        CHECK(g.n_hist == 10 && g.hist[9].t == 1011 && g.hist[0].t == 1002 && g.hist[9].delta == 7, "battles saved oldest first and loaded");
    }
    {   /* update 131: claimed new events */
        bv_profile e;
        bv_profile_default(&e);
        CHECK(!bv_profile_event_claimed(&e, 61809), "new event not claimed");
        bv_profile_claim_event(&e, 61809);
        bv_profile_claim_event(&e, 61809);
        CHECK(bv_profile_event_claimed(&e, 61809) && e.n_ev_claimed == 1, "claimed once");
        for (int i = 0; i < 20; i++) bv_profile_claim_event(&e, 1000 + i);
        CHECK(!bv_profile_event_claimed(&e, 61809) && bv_profile_event_claimed(&e, 1019), "ring of 16 drops the oldest");
        char buf[4096];
        bv_profile_format(&e, buf, sizeof buf);
        bv_profile f;
        bv_profile_parse(&f, buf);
        CHECK(bv_profile_event_claimed(&f, 1019) && bv_profile_event_claimed(&f, 1004), "claimed ids saved and loaded");
    }
    {   /* update 130: trophy league */
        int sp;
        CHECK(bv_trophy_reset(480, &sp) == 480 && sp == 0, "<= 500 untouched");
        CHECK(bv_trophy_reset(510, &sp) == 500 && sp == 20, "501-524 -> 500, 20 SP");
        CHECK(bv_trophy_reset(1600, &sp) == 1150 && sp == 350, "1500+ -> 1150, 350 SP");
        bv_profile q2;
        bv_profile_default(&q2);
        q2.n = 2; q2.b[0].trophies = 700; q2.b[1].trophies = 100; q2.star_points = 5;
        int lost = 0;
        CHECK(bv_profile_league_end(&q2, &lost) == 130 && lost == 1 && q2.b[0].trophies == 699 && q2.star_points == 135, "league end over the profile");
    }
    {   /* update 117: token pool */
        bv_profile t;
        bv_profile_default(&t);
        CHECK(bv_tokens_refill(&t, 1000000) == 0 && t.tokens == 200, "new profile: full pool");
        CHECK(bv_tokens_spend(&t, 20, 1000000) == 20 && t.tokens == 180 && t.token_time == 1000000, "spend 20 from full starts the clock");
        CHECK(bv_tokens_refill(&t, 1000000 + 100) == 8540, "8540 s to the next refill");
        CHECK(bv_tokens_spend(&t, 500, 1000000 + 200) == 180 && t.tokens == 0, "cannot take more than the pool");
        CHECK(bv_tokens_refill(&t, 1000000 + 3 * 8640 + 5) == 8635 && t.tokens == 60, "3 refills after 3 periods");
        CHECK(bv_tokens_refill(&t, 1000000 + 30 * 8640) == 0 && t.tokens == 200 && t.token_time == 0, "capped at 200");
        char buf[4096];
        t.tokens = 55; t.token_time = 1234; t.pass_tokens = 77;
        bv_profile_format(&t, buf, sizeof buf);
        bv_profile u;
        bv_profile_parse(&u, buf);
        CHECK(u.tokens == 55 && u.token_time == 1234 && u.pass_tokens == 77, "token keys saved and loaded");
    }
    {   /* update 144 (req. 24): a quest keeps its brawler; the selected brawler plays no part; survives a save + load */
        bv_profile q;
        bv_profile_default(&q);
        int owned[5] = {0, 1, 2, 5, 9}, made = 0;
        int r1 = bv_profile_quest_brawler(&q, 123456u, owned, 5, &made);
        CHECK(made == 1 && r1 >= 0, "new quest: a brawler is picked and stored");
        q.selected_brawler = 9;
        int r2 = bv_profile_quest_brawler(&q, 123456u, owned, 5, &made);
        CHECK(made == 0 && r2 == r1, "same quest after another brawler was selected: same brawler (%d %d)", r1, r2);
        int owned2[6] = {0, 1, 2, 5, 9, 12};
        CHECK(bv_profile_quest_brawler(&q, 123456u, owned2, 6, &made) == r1 && !made, "a new brawler unlocked: assignment unchanged");
        q.selected_slot = 6;
        char buf[8192];
        bv_profile_format(&q, buf, sizeof buf);
        bv_profile w;
        bv_profile_parse(&w, buf);
        CHECK(bv_profile_quest_brawler(&w, 123456u, owned, 5, &made) == r1 && !made, "after a restart (save + load): same brawler");
        CHECK(w.selected_slot == 6, "selected event slot saved (req. 22)");
        int diff = 0;
        for (unsigned k = 1; k <= 40; k++) if (bv_profile_quest_brawler(&w, k * 7919u, owned, 5, &made) != r1) diff++;
        CHECK(diff > 0, "other quests get other brawlers (random, not one fixed choice)");
        bv_profile f;
        bv_profile_default(&f);
        CHECK(f.n == 1 && f.b[0].row == 0 && f.b[0].trophies == 0 && f.b[0].power == 0 && f.gems == 0 && f.coins == 0 && !f.unlock_all,
              "fresh account: Shelly only, 0 trophies, power 1, no gems / coins (req. 9)");
    }
    printf(g_fail ? "%d CHECK(S) FAILED\n" : "ALL CHECKS PASSED\n", g_fail);
    return g_fail != 0;
}
