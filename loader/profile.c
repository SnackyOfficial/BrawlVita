#include "profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void bv_profile_default(bv_profile *p) {
    memset(p, 0, sizeof *p);
    p->thumbnail = -1; p->name_color = -1;   /* update 182; update 184: after the memset (it wiped them: every brawler saved skin row 0) */
    for (int k = 0; k < 96; k++) p->skinsel[k] = -1;
    p->version = 1;
    snprintf(p->name, sizeof p->name, "Vita");
    p->selected_brawler = 0;
    p->selected_map = 7;
    p->n = 1;
    p->b[0].row = 0;
    p->tokens = -1;
}

void bv_profile_add_battle(bv_profile *p, long t, int kind, int result, int delta, int map, int brawler) {
    int k = p->n_hist % 10;
    p->hist[k].t = t;
    p->hist[k].kind = kind;
    p->hist[k].result = result;
    p->hist[k].delta = delta;
    p->hist[k].map = map;
    p->hist[k].brawler = brawler;
    p->hist[k].len = 0;   /* update 180: battle length (s), set by the caller */
    p->hist[k].np = 0;
    p->n_hist++;
}

void bv_profile_battle_player(bv_profile *p, int row, int team, int local, const char *name) {
    if (p->n_hist <= 0) return;
    int k = (p->n_hist - 1) % 10;
    if (p->hist[k].np >= 10) return;
    int i = p->hist[k].np++;
    p->hist[k].p[i].row = (short)row;
    p->hist[k].p[i].team = (signed char)team;
    p->hist[k].p[i].local = (signed char)(local != 0);
    int j = 0;
    for (const char *c = name ? name : ""; *c && j < 15; c++)
        if (*c != ',' && *c != '\n' && *c != '\r') p->hist[k].p[i].name[j++] = *c;   /* the file format: no commas / newlines */
    p->hist[k].p[i].name[j] = 0;
}

int bv_profile_quest_brawler(bv_profile *p, unsigned key, const int *owned, int n, int *made) {
    if (made) *made = 0;
    int m = p->n_qbr < 16 ? p->n_qbr : 16;
    for (int i = 0; i < m; i++)
        if (p->qbr[i].key == key) return p->qbr[i].row;
    if (n <= 0) return -1;
    uint32_t x = key * 2246822519u + 0x165667b1u;   /* the quest's own seed, not the selected brawler */
    x ^= x >> 13; x *= 0x85ebca6bu; x ^= x >> 16;
    int row = owned[x % (uint32_t)n];
    p->qbr[p->n_qbr % 16].key = key;
    p->qbr[p->n_qbr % 16].row = row;
    p->n_qbr++;
    if (made) *made = 1;
    return row;
}

int bv_profile_event_claimed(const bv_profile *p, int id) {
    int n = p->n_ev_claimed < 16 ? p->n_ev_claimed : 16;
    for (int i = 0; i < n; i++) if (p->ev_claimed[i] == id) return 1;
    return 0;
}

void bv_profile_claim_event(bv_profile *p, int id) {
    if (bv_profile_event_claimed(p, id)) return;
    p->ev_claimed[p->n_ev_claimed % 16] = id;
    p->n_ev_claimed++;
}

int bv_trophy_reset(int t, int *sp) {
    /* {lowest trophies of the band, trophies after the reset, star points} - 2020 table (recalled) */
    static const int tb[][3] = {{501, 500, 20}, {525, 524, 50}, {550, 549, 70}, {575, 574, 80}, {600, 599, 90}, {625, 624, 100}, {650, 649, 110},
                                {675, 674, 120}, {700, 699, 130}, {725, 724, 140}, {750, 749, 150}, {775, 774, 160}, {800, 799, 170}, {825, 824, 180},
                                {850, 849, 190}, {875, 874, 200}, {900, 885, 210}, {925, 900, 220}, {950, 920, 230}, {975, 940, 240}, {1000, 960, 250},
                                {1050, 980, 260}, {1100, 1000, 270}, {1150, 1020, 280}, {1200, 1040, 290}, {1250, 1060, 300}, {1300, 1080, 310},
                                {1350, 1100, 320}, {1400, 1120, 330}, {1450, 1140, 340}, {1500, 1150, 350}};
    if (sp) *sp = 0;
    int k = -1;
    for (int i = 0; i < (int)(sizeof tb / sizeof tb[0]); i++) if (t >= tb[i][0]) k = i;
    if (k < 0) return t;
    if (sp) *sp = tb[k][2];
    return tb[k][1];
}

int bv_profile_league_end(bv_profile *p, int *lost) {
    int paid = 0, gone = 0;
    for (int i = 0; i < p->n; i++) {
        int sp = 0, t = bv_trophy_reset(p->b[i].trophies, &sp);
        gone += p->b[i].trophies - t;
        p->b[i].trophies = t;
        paid += sp;
    }
    p->star_points += paid;
    if (lost) *lost = gone;
    return paid;
}

int bv_tokens_refill(bv_profile *p, long now) {
    if (p->tokens < 0 || p->tokens > BV_TOKEN_CAP) p->tokens = BV_TOKEN_CAP;
    if (p->tokens >= BV_TOKEN_CAP) { p->token_time = 0; return 0; }
    if (p->token_time <= 0 || p->token_time > now) p->token_time = now;
    long n = (now - p->token_time) / BV_TOKEN_PERIOD;
    if (n > 0) {
        long t = p->tokens + n * BV_TOKEN_STEP;
        p->tokens = t >= BV_TOKEN_CAP ? BV_TOKEN_CAP : (int)t;
        p->token_time += n * BV_TOKEN_PERIOD;
    }
    if (p->tokens >= BV_TOKEN_CAP) { p->token_time = 0; return 0; }
    return (int)(p->token_time + BV_TOKEN_PERIOD - now);
}

int bv_tokens_spend(bv_profile *p, int want, long now) {
    bv_tokens_refill(p, now);
    if (want <= 0) return 0;
    int got = want < p->tokens ? want : p->tokens;
    if (got > 0 && p->tokens >= BV_TOKEN_CAP) p->token_time = now;   /* the pool was full: the refill clock starts now */
    p->tokens -= got;
    return got;
}

int bv_profile_total_trophies(const bv_profile *p) {
    int t = 0;
    for (int i = 0; i < p->n; i++) t += p->b[i].trophies;
    return t;
}

bv_brawler *bv_profile_find(bv_profile *p, int row) {
    for (int i = 0; i < p->n; i++)
        if (p->b[i].row == row) return &p->b[i];
    return NULL;
}

bv_brawler *bv_profile_unlock(bv_profile *p, int row) {
    if (row < 0) return NULL;
    bv_brawler *b = bv_profile_find(p, row);
    if (b) return b;
    if (p->n >= BV_PROFILE_MAX_BRAWLERS) return NULL;
    b = &p->b[p->n++];
    memset(b, 0, sizeof *b);
    b->row = row;
    return b;
}

void bv_profile_battle(bv_profile *p, int delta, int won) {
    bv_brawler *b = bv_profile_unlock(p, p->selected_brawler);
    p->battles++;
    if (won) p->wins++;
    if (!b) return;
    b->trophies += delta;
    if (b->trophies < 0) b->trophies = 0;
    if (b->trophies > b->best_trophies) b->best_trophies = b->trophies;
}

int bv_profile_own_card(bv_profile *p, int card_row) {   /* update 174 */
    if (card_row < 0) return 0;
    for (int i = 0; i < p->n_cards; i++) if (p->cards[i] == card_row) return 1;
    if (p->n_cards >= (int)(sizeof p->cards / sizeof p->cards[0])) return 0;
    p->cards[p->n_cards++] = card_row;
    return 1;
}
int bv_profile_has_skin(const bv_profile *p, int skin_row) {
    for (int i = 0; i < p->n_skins; i++)
        if (p->skins[i] == skin_row) return 1;
    return 0;
}

int bv_profile_own_skin(bv_profile *p, int skin_row) {
    if (skin_row < 0) return 0;
    if (bv_profile_has_skin(p, skin_row)) return 1;
    if (p->n_skins >= BV_PROFILE_MAX_SKINS) return 0;
    p->skins[p->n_skins++] = skin_row;
    return 1;
}

int bv_profile_is_bought(const bv_profile *p, int day, int offer_id) {
    if (day != p->offer_day) return 0;
    for (int i = 0; i < p->n_bought; i++)
        if (p->bought[i] == offer_id) return 1;
    return 0;
}

int bv_profile_mark_bought(bv_profile *p, int day, int offer_id) {
    if (day != p->offer_day) { p->offer_day = day; p->n_bought = 0; }
    if (bv_profile_is_bought(p, day, offer_id)) return 1;
    if (p->n_bought >= BV_PROFILE_MAX_BOUGHT) return 0;
    p->bought[p->n_bought++] = offer_id;
    return 1;
}

static int claim_id(int type, int season, int tier) { return type * 100000 + season * 1000 + tier; }

int bv_profile_is_claimed(const bv_profile *p, int type, int season, int tier) {
    int id = claim_id(type, season, tier);
    for (int i = 0; i < p->n_claimed; i++)
        if (p->claimed[i] == id) return 1;
    return 0;
}

int bv_profile_mark_claimed(bv_profile *p, int type, int season, int tier) {
    if (type < 0 || season < 0 || tier < 0 || tier >= 1000) return 0;
    if (bv_profile_is_claimed(p, type, season, tier)) return 1;
    if (p->n_claimed >= BV_PROFILE_MAX_CLAIMED) return 0;
    p->claimed[p->n_claimed++] = claim_id(type, season, tier);
    return 1;
}

int bv_profile_claimed_bits(const bv_profile *p, int type, int season, unsigned bits[4]) {
    int n = 0;
    bits[0] = bits[1] = bits[2] = bits[3] = 0;
    for (int i = 0; i < p->n_claimed; i++) {
        int id = p->claimed[i];
        if (id / 100000 != type || (id / 1000) % 100 != season) continue;
        int tier = id % 1000;
        int b = tier + 2;   /* cycle 6: the client tests bit tier + 2 (brawl_pass_cycle6.md Q3, CONFIRMED) */
        if (tier >= 0 && b < 128) { bits[b / 32] |= 1u << (b % 32); n++; }
    }
    return n;
}

size_t bv_profile_format(const bv_profile *p, char *out, size_t cap) {
    size_t at = 0;
#define PUT(...)                                                                    \
    do {                                                                            \
        int k_ = snprintf(out && at < cap ? out + at : NULL, out && at < cap ? cap - at : 0, __VA_ARGS__); \
        if (k_ > 0) at += (size_t)k_;                                               \
    } while (0)
    PUT("# BrawlVita offline profile. One key=value per line; brawler=row,trophies,best,power,power_points\n");
    PUT("version=%d\n", p->version);
    PUT("name=%s\n", p->name);
    PUT("coins=%d\ngems=%d\nstar_points=%d\n", p->coins, p->gems, p->star_points);
    PUT("selected_brawler=%d\nselected_map=%d\nselected_slot=%d\nunlock_all=%d\n", p->selected_brawler, p->selected_map, p->selected_slot, p->unlock_all);
    PUT("battles=%d\nwins=%d\ntrophy_delta=%d\nseason_start=%ld\n", p->battles, p->wins, p->trophy_delta, p->season_start);
    PUT("tokens=%d\ntoken_time=%ld\npass_tokens=%d\nexp=%d\n", p->tokens, p->token_time, p->pass_tokens, p->exp);
    for (int i = 0; i < (p->n_ev_claimed < 16 ? p->n_ev_claimed : 16); i++) PUT("event_claimed=%d\n", p->ev_claimed[i]);
    {   /* update 144: oldest first, so the ring order survives a reload */
        int n = p->n_qbr < 16 ? p->n_qbr : 16;
        for (int i = 0; i < n; i++) { int k = (p->n_qbr - n + i) % 16; PUT("quest_brawler=%u,%d\n", p->qbr[k].key, p->qbr[k].row); }
    }
    {   /* update 140: battles oldest first, so loading them again keeps the order */
        int n = p->n_hist < 10 ? p->n_hist : 10;
        for (int i = 0; i < n; i++) {
            int k = (p->n_hist - n + i) % 10;
            PUT("battle=%ld,%d,%d,%d,%d,%d,%d\n", p->hist[k].t, p->hist[k].kind, p->hist[k].result, p->hist[k].delta, p->hist[k].map, p->hist[k].brawler, p->hist[k].len);
            for (int j = 0; j < p->hist[k].np && j < 10; j++)   /* update 159: the battle's players, right after their battle line */
                PUT("battle_player=%d,%d,%d,%s\n", p->hist[k].p[j].row, p->hist[k].p[j].team, p->hist[k].p[j].local, p->hist[k].p[j].name);
        }
    }
    for (int i = 0; i < p->n; i++) PUT("brawler=%d,%d,%d,%d,%d\n", p->b[i].row, p->b[i].trophies, p->b[i].best_trophies, p->b[i].power, p->b[i].power_points);
    for (int i = 0; i < p->n_skins; i++) PUT("skin=%d\n", p->skins[i]);
    for (int i = 0; i < p->n_cards; i++) PUT("card=%d\n", p->cards[i]);   /* update 174 */
    if (p->thumbnail >= 0) PUT("thumbnail=%d\n", p->thumbnail);   /* update 182 */
    if (p->name_color >= 0) PUT("name_color=%d\n", p->name_color);
    for (int i = 0; i < 96; i++) if (p->skinsel[i] >= 0) PUT("skinsel=%d,%d\n", i, p->skinsel[i]);
    PUT("offer_day=%d\n", p->offer_day);
    for (int i = 0; i < p->n_bought; i++) PUT("bought=%d\n", p->bought[i]);
    for (int i = 0; i < p->n_claimed; i++) PUT("claimed=%d\n", p->claimed[i]);
#undef PUT
    return at;
}

int bv_profile_parse(bv_profile *p, const char *text) {
    bv_profile_default(p);
    int bad = 0, brawlers_seen = 0;
    const char *s = text;
    while (s && *s) {
        const char *e = strchr(s, '\n');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        char line[160];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, s, len);
        line[len] = 0;
        if (len && line[len - 1] == '\r') line[--len] = 0;
        s = e ? e + 1 : NULL;
        if (!len || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) { bad++; continue; }
        *eq = 0;
        const char *k = line, *v = eq + 1;
        int a, b, c, d, f;
        if (!strcmp(k, "version")) p->version = atoi(v);
        else if (!strcmp(k, "name")) snprintf(p->name, sizeof p->name, "%s", v);
        else if (!strcmp(k, "coins")) p->coins = atoi(v);
        else if (!strcmp(k, "gems")) p->gems = atoi(v);
        else if (!strcmp(k, "star_points")) p->star_points = atoi(v);
        else if (!strcmp(k, "selected_brawler")) p->selected_brawler = atoi(v);
        else if (!strcmp(k, "selected_map")) p->selected_map = atoi(v);
        else if (!strcmp(k, "selected_slot")) p->selected_slot = atoi(v);
        else if (!strcmp(k, "unlock_all")) p->unlock_all = atoi(v);
        else if (!strcmp(k, "battles")) p->battles = atoi(v);
        else if (!strcmp(k, "wins")) p->wins = atoi(v);
        else if (!strcmp(k, "trophy_delta")) p->trophy_delta = atoi(v);
        else if (!strcmp(k, "season_start")) p->season_start = atol(v);
        else if (!strcmp(k, "tokens")) p->tokens = atoi(v);
        else if (!strcmp(k, "token_time")) p->token_time = atol(v);
        else if (!strcmp(k, "pass_tokens")) p->pass_tokens = atoi(v);
        else if (!strcmp(k, "exp")) p->exp = atoi(v);
        else if (!strcmp(k, "event_claimed")) bv_profile_claim_event(p, atoi(v));
        else if (!strcmp(k, "quest_brawler")) {
            unsigned key = 0;
            int row = -1;
            if (sscanf(v, "%u,%d", &key, &row) == 2 && row >= 0) { p->qbr[p->n_qbr % 16].key = key; p->qbr[p->n_qbr % 16].row = row; p->n_qbr++; }
        }
        else if (!strcmp(k, "battle")) {
            long t = 0;
            int a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0, a6 = 0;
            if (sscanf(v, "%ld,%d,%d,%d,%d,%d,%d", &t, &a1, &a2, &a3, &a4, &a5, &a6) >= 6) {   /* update 180: optional 7th = length */
                bv_profile_add_battle(p, t, a1, a2, a3, a4, a5);
                p->hist[(p->n_hist - 1) % 10].len = a6 > 0 && a6 < 3600 ? a6 : 0;
            }
        }
        else if (!strcmp(k, "battle_player")) {
            int r = 0, tm = 0, lo = 0, used = 0;
            if (sscanf(v, "%d,%d,%d,%n", &r, &tm, &lo, &used) == 3) bv_profile_battle_player(p, r, tm, lo, v + used);
        }
        else if (!strcmp(k, "brawler") && sscanf(v, "%d,%d,%d,%d,%d", &a, &b, &c, &d, &f) == 5 && a >= 0) {
            if (!brawlers_seen++) p->n = 0; /* the file's list replaces the default one */
            bv_brawler *x = bv_profile_unlock(p, a);
            if (x) { x->trophies = b < 0 ? 0 : b; x->best_trophies = c < x->trophies ? x->trophies : c; x->power = d < 0 ? 0 : d; x->power_points = f < 0 ? 0 : f; }
        } else if (!strcmp(k, "thumbnail") && sscanf(v, "%d", &a) == 1 && a >= 0) { p->thumbnail = a;   /* update 182 */
        } else if (!strcmp(k, "name_color") && sscanf(v, "%d", &a) == 1 && a >= 0) { p->name_color = a;
        } else if (!strcmp(k, "skinsel")) { int b2 = -1, s2 = -1; if (sscanf(v, "%d,%d", &b2, &s2) == 2 && b2 >= 0 && b2 < 96 && s2 > 0) p->skinsel[b2] = (short)s2;   /* update 184: row 0 = the bug's value (Shelly's default), not a choice */
        } else if (!strcmp(k, "card") && sscanf(v, "%d", &a) == 1 && a >= 0) {   /* update 174 */
            bv_profile_own_card(p, a);
        } else if (!strcmp(k, "skin") && sscanf(v, "%d", &a) == 1 && a >= 0) {
            bv_profile_own_skin(p, a);
        } else if (!strcmp(k, "offer_day")) {
            p->offer_day = atoi(v);
            p->n_bought = 0;                 /* bought= lines must follow their offer_day line (format writes them in that order) */
        } else if (!strcmp(k, "claimed") && sscanf(v, "%d", &a) == 1 && a >= 0) {
            bv_profile_mark_claimed(p, a / 100000, (a / 1000) % 100, a % 1000);
        } else if (!strcmp(k, "bought") && sscanf(v, "%d", &a) == 1) {
            if (p->n_bought < BV_PROFILE_MAX_BOUGHT && !bv_profile_is_bought(p, p->offer_day, a)) p->bought[p->n_bought++] = a;
        } else bad++;
    }
    if (!bv_profile_find(p, p->selected_brawler)) p->selected_brawler = p->n ? p->b[0].row : 0;
    return bad;
}
