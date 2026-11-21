#include "passdata.h"

#include <stdlib.h>
#include <string.h>

/* One CSV field (quotes removed, "" -> "). Advances *s to the next field / line. Returns 1 if the line goes on. */
static int csv_field(const char **s, char *out, size_t cap) {
    const char *p = *s;
    size_t n = 0;
    int quoted = (*p == '"');
    if (quoted) p++;
    while (*p) {
        if (quoted) {
            if (*p == '"' && p[1] == '"') { if (n + 1 < cap) out[n++] = '"'; p += 2; continue; }
            if (*p == '"') { quoted = 0; p++; continue; }
        } else if (*p == ',' || *p == '\n' || *p == '\r') {
            break;
        }
        if (n + 1 < cap) out[n++] = *p;
        p++;
    }
    out[n] = 0;
    int more = (*p == ',');
    if (*p == ',') p++;
    else {
        while (*p == '\r') p++;
        if (*p == '\n') p++;
    }
    *s = p;
    return more;
}

static void skip_line(const char **s) {
    const char *p = *s;
    int quoted = 0;
    while (*p && (quoted || *p != '\n')) { if (*p == '"') quoted = !quoted; p++; }
    if (*p == '\n') p++;
    *s = p;
}

static int to_int(const char *v, int dflt) { return (v && *v) ? atoi(v) : dflt; }

int bv_pass_load_milestones(bv_passdata *p, const char *csv) {
    p->n = 0;
    const char *s = csv;
    char f[64];
    int c_name = -1, c_type = -1, c_index = -1, c_season = -1, c_r1t = -1, c_r1c = -1, c_r1d = -1, c_r2t = -1, c_r2c = -1, c_r2d = -1;
    int col = 0, more = 1;
    while (*s && more) {
        more = csv_field(&s, f, sizeof f);
        if (!strcmp(f, "Name")) c_name = col;
        else if (!strcmp(f, "Type")) c_type = col;
        else if (!strcmp(f, "Index")) c_index = col;
        else if (!strcmp(f, "Season")) c_season = col;
        else if (!strcmp(f, "PrimaryLvlUpRewardType")) c_r1t = col;
        else if (!strcmp(f, "PrimaryLvlUpRewardCount")) c_r1c = col;
        else if (!strcmp(f, "PrimaryLvlUpRewardData")) c_r1d = col;
        else if (!strcmp(f, "SecondaryLvlUpRewardType")) c_r2t = col;
        else if (!strcmp(f, "SecondaryLvlUpRewardCount")) c_r2c = col;
        else if (!strcmp(f, "SecondaryLvlUpRewardData")) c_r2d = col;
        col++;
    }
    if (c_name < 0 || c_type < 0 || c_index < 0 || c_season < 0 || c_r1t < 0) return -1;
    skip_line(&s); /* the types line */
    char cells[24][64];
    while (*s) {
        int nc = 0;
        more = 1;
        while (more && nc < 24) more = csv_field(&s, cells[nc++], sizeof cells[0]);
        if (more) skip_line(&s);                 /* more columns than we keep */
        if (nc <= c_r1t) continue;
        int type = to_int(cells[c_type], -1);
        if ((type != 9 && type != 10 && type != 6) || p->n >= BV_PASS_MAX) continue;   /* update 183: 6 = Trophy Road (goal_6_<index>, season 0) */
        bv_pass_goal *g = &p->g[p->n++];
        memset(g, 0, sizeof *g);
        g->type = type;
        g->index = to_int(cells[c_index], -1);
        g->season = to_int(cells[c_season], 0);
        g->r1_type = to_int(cells[c_r1t], -1);
        g->r1_count = c_r1c >= 0 && c_r1c < nc ? to_int(cells[c_r1c], 1) : 1;
        if (c_r1d >= 0 && c_r1d < nc) { strncpy(g->r1_data, cells[c_r1d], sizeof g->r1_data - 1); }
        g->r2_type = c_r2t >= 0 && c_r2t < nc ? to_int(cells[c_r2t], -1) : -1;
        g->r2_count = c_r2c >= 0 && c_r2c < nc ? to_int(cells[c_r2c], 1) : 1;
        if (c_r2d >= 0 && c_r2d < nc) { strncpy(g->r2_data, cells[c_r2d], sizeof g->r2_data - 1); }
    }
    return p->n;
}

int bv_trophy_road_last(const char *csv, int trophies) {
    const char *s = csv;
    char f[64];
    int c_type = -1, c_index = -1, c_start = -1, c_prog = -1, col = 0, more = 1;
    while (*s && more) {
        more = csv_field(&s, f, sizeof f);
        if (!strcmp(f, "Type")) c_type = col;
        else if (!strcmp(f, "Index")) c_index = col;
        else if (!strcmp(f, "ProgressStart")) c_start = col;
        else if (!strcmp(f, "Progress")) c_prog = col;
        col++;
    }
    if (c_type < 0 || c_index < 0 || c_start < 0 || c_prog < 0) return -2;
    skip_line(&s); /* the types line */
    int best = -1, need = c_type > c_index ? c_type : c_index;
    if (c_start > need) need = c_start;
    if (c_prog > need) need = c_prog;
    char cells[24][64];
    while (*s) {
        int nc = 0;
        more = 1;
        while (more && nc < 24) more = csv_field(&s, cells[nc++], sizeof cells[0]);
        if (more) skip_line(&s);
        if (nc <= need) continue;
        if (to_int(cells[c_type], -1) != 6) continue;
        int idx = to_int(cells[c_index], -1), end = to_int(cells[c_start], 0) + to_int(cells[c_prog], 0);
        if (idx > best && end <= trophies) best = idx;
    }
    return best;
}

int bv_pass_load_skins(bv_passdata *p, const char *csv) {
    p->n_skins = 0;
    const char *s = csv;
    char f[64];
    int c_cost = -1, c_obtain = -1, col = 0, more = 1, c_conf = -1, c_camp = -1, c_coins = -1, c_lt = -1, c_tid = -1;
    while (*s && more) {                 /* header: find CostGems / ObtainType (update 184: + Conf, Campaign, CostCoins, CostLegendaryTrophies, ShopTID) */
        more = csv_field(&s, f, sizeof f);
        if (!strcmp(f, "CostGems")) c_cost = col;
        else if (!strcmp(f, "ObtainType")) c_obtain = col;
        else if (!strcmp(f, "Conf")) c_conf = col;
        else if (!strcmp(f, "Campaign")) c_camp = col;
        else if (!strcmp(f, "CostCoins")) c_coins = col;
        else if (!strcmp(f, "CostLegendaryTrophies")) c_lt = col;
        else if (!strcmp(f, "ShopTID")) c_tid = col;
        col++;
    }
    skip_line(&s); /* types */
    char cells[16][64];
    while (*s && p->n_skins < BV_PASS_SKINS) {
        int nc = 0;
        more = 1;
        while (more && nc < 16) more = csv_field(&s, cells[nc++], sizeof cells[0]);
        if (more) skip_line(&s);
        strncpy(p->skins[p->n_skins], cells[0], sizeof p->skins[0] - 1);
        p->skins[p->n_skins][sizeof p->skins[0] - 1] = 0;
        p->skin_cost[p->n_skins] = (short)(c_cost >= 0 && c_cost < nc ? to_int(cells[c_cost], 0) : 0);
        p->skin_obtain[p->n_skins] = (signed char)(c_obtain >= 0 && c_obtain < nc ? to_int(cells[c_obtain], 0) : 0);
        {   /* update 184 */
            int i = p->n_skins;
            size_t ln = strlen(cells[0]);
            p->skin_campaign[i] = (signed char)(c_camp >= 0 && c_camp < nc ? to_int(cells[c_camp], 0) : 0);
            p->skin_coins[i] = c_coins >= 0 && c_coins < nc ? to_int(cells[c_coins], 0) : 0;
            p->skin_lt[i] = c_lt >= 0 && c_lt < nc ? to_int(cells[c_lt], 0) : 0;
            p->skin_shop[i] = (unsigned char)(c_tid >= 0 && c_tid < nc && cells[c_tid][0] && !(ln >= 7 && !strcmp(cells[0] + ln - 7, "Default")));
            p->skin_brawler[i] = -1;
            strncpy(p->skin_conf[i], c_conf >= 0 && c_conf < nc ? cells[c_conf] : cells[0], sizeof p->skin_conf[0] - 1);
            p->skin_conf[i][sizeof p->skin_conf[0] - 1] = 0;
        }
        p->n_skins++;
    }
    return p->n_skins;
}

int bv_daily_skin(const bv_passdata *p, int day, const int *owned, int n_owned, int *price, int *discount_pct) {
    /* candidates with weights: cheaper skins are more common (weight 300 / cost) */
    unsigned total = 0;
    for (int i = 0; i < p->n_skins; i++) {
        if (p->skin_cost[i] <= 0 || p->skin_obtain[i] != 0) continue;
        int own = 0;
        for (int k = 0; k < n_owned; k++) if (owned[k] == i) own = 1;
        if (!own) total += 300u / (unsigned)p->skin_cost[i] + 1u;
    }
    if (!total) return -1;
    unsigned x = (unsigned)day * 2654435761u + 0x9e3779b9u;
    x ^= x >> 15; x *= 2246822519u; x ^= x >> 13;
    unsigned pick = x % total, acc = 0;
    int row = -1;
    for (int i = 0; i < p->n_skins && row < 0; i++) {
        if (p->skin_cost[i] <= 0 || p->skin_obtain[i] != 0) continue;
        int own = 0;
        for (int k = 0; k < n_owned; k++) if (owned[k] == i) own = 1;
        if (own) continue;
        acc += 300u / (unsigned)p->skin_cost[i] + 1u;
        if (pick < acc) row = i;
    }
    /* discount 25..75 %, big discounts rarer (square of a uniform roll); +5 on Saturday / Sunday (day 0 = Thursday 1970-01-01) */
    unsigned r = (x >> 8) % 100u;
    int d = 25 + (int)(r * r * 50u / 9801u);
    int dow = (day + 4) % 7;                       /* 0 Sunday .. 6 Saturday */
    if (dow == 0 || dow == 6) d += 5;
    if (d > 75) d = 75;
    int cost = p->skin_cost[row];
    int pr = cost * (100 - d) / 100;
    if (pr < 1) pr = 1;
    if (price) *price = pr;
    if (discount_pct) *discount_pct = d;
    return row;
}

int bv_daily_skins(const bv_passdata *p, int day, const int *owned, int n_owned, int *rows, int *prices, int max) {
    int ex[1024 + 16];
    int nx = 0;
    for (int i = 0; i < n_owned && nx < 1024; i++) ex[nx++] = owned[i];
    int n = 0;
    for (int k = 0; n < max && k < 64; k++) {
        int pr = 0;
        int row = bv_daily_skin(p, k == 0 ? day : day * 31 + k, ex, nx, &pr, NULL);
        if (row < 0) break;
        rows[n] = row;
        prices[n] = k == 0 ? pr : p->skin_cost[row];   /* only the first one is on sale */
        n++;
        ex[nx++] = row;
    }
    return n;
}

int bv_pass_load_skin_confs(bv_passdata *p, const char *csv) {
    const char *s = csv;
    char f[64];
    int col = 0, more = 1, c_ch = -1;
    while (*s && more) {
        more = csv_field(&s, f, sizeof f);
        if (!strcmp(f, "Character")) c_ch = col;
        col++;
    }
    if (c_ch < 0 || c_ch >= 8) return -1;
    skip_line(&s); /* types */
    for (int i = 0; i < p->n_skins; i++) p->skin_brawler[i] = -1;
    int found = 0;
    char cells[8][64];
    while (*s) {
        int nc = 0;
        more = 1;
        while (more && nc < 8) more = csv_field(&s, cells[nc++], sizeof cells[0]);
        if (more) skip_line(&s);
        if (nc <= c_ch || !cells[0][0] || !cells[c_ch][0]) continue;
        int b = -1;
        for (int k = 0; k < p->n_brawlers; k++)
            if (!strcmp(p->brawlers[k], cells[c_ch])) { b = k; break; }
        if (b < 0) continue;
        for (int i = 0; i < p->n_skins; i++)
            if (!strcmp(p->skin_conf[i], cells[0])) { p->skin_brawler[i] = (short)b; found++; }
    }
    return found;
}

int bv_skin_kind(const bv_passdata *p, int row) {
    if (row < 0 || row >= p->n_skins || !p->skin_shop[row] || p->skin_brawler[row] < 0) return 0;
    int c = p->skin_campaign[row];
    return (c == 1 || c == 2 || c == 3) ? 2 : 1;
}

int bv_skin_in_season(const bv_passdata *p, int row, int mmdd) {
    if (row < 0 || row >= p->n_skins) return 0;
    switch (p->skin_campaign[row]) {
    case 3: return mmdd >= 1024 && mmdd <= 1107;   /* Halloween (user: October 24 - November 7) */
    case 1: return mmdd >= 1218 || mmdd <= 101;    /* Christmas (user: December 18 - January 1) */
    case 2: return mmdd >= 121 && mmdd <= 210;     /* Lunar New Year */
    default: return 0;
    }
}

int bv_skin_shop_price(const bv_passdata *p, int row, int *in_coins) {
    if (in_coins) *in_coins = 0;
    if (row < 0 || row >= p->n_skins) return 0;
    if (p->skin_cost[row] > 0) return p->skin_cost[row];
    if (p->skin_coins[row] > 0) { if (in_coins) *in_coins = 1; return p->skin_coins[row]; }
    int lt = p->skin_lt[row];
    if (lt >= 50000) return 300;
    if (lt >= 10000) return 150;
    if (lt >= 2500) return 80;
    if (lt > 0) return 30;
    return 80;
}

int bv_shop_skin_list(const bv_passdata *p, int day, int mmdd, const int *owned, int n_owned, const unsigned char *have_br, int seasonal, int *rows, int max) {
    static int cand[BV_PASS_SKINS];
    int nc = 0;
    for (int i = 0; i < p->n_skins; i++) {
        int k = bv_skin_kind(p, i);
        if (!k) continue;
        if (seasonal ? (k != 2 || (seasonal == 1 && !bv_skin_in_season(p, i, mmdd))) : k != 1) continue;   /* 184b: seasonal 2 = any date (test) */
        int b = p->skin_brawler[i];
        if (b < 0 || b >= 96 || !have_br[b]) continue;
        int own = 0;
        for (int q = 0; q < n_owned; q++) if (owned[q] == i) { own = 1; break; }
        if (!own) cand[nc++] = i;
    }
    unsigned x = (unsigned)day * 2654435761u + (seasonal ? 0x51ed27u : 0x2545f491u);
    for (int i = nc - 1; i > 0; i--) {   /* day-seeded shuffle: the same order all day */
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        int j = (int)(x % (unsigned)(i + 1)), t = cand[i];
        cand[i] = cand[j];
        cand[j] = t;
    }
    int n = nc < max ? nc : max;
    for (int i = 0; i < n; i++) rows[i] = cand[i];
    return n;
}

int bv_pass_skin_row(const bv_passdata *p, const char *name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < p->n_skins; i++)
        if (!strcmp(p->skins[i], name)) return i;
    return -1;
}

const bv_pass_goal *bv_pass_find(const bv_passdata *p, int type, int season, int index) {
    for (int i = 0; i < p->n; i++)
        if (p->g[i].type == type && p->g[i].season == season && p->g[i].index == index) return &p->g[i];
    return NULL;
}

static int one_reward(const bv_passdata *p, int rtype, int count, const char *data, int coin_type, int brawler_row, int maxed, bmsg_box_item *it,
                      int *unmapped) {
    it->brawler = -1;
    it->skin = -1;
    it->amount = count > 0 ? count : 1;
    if (rtype == 24 || rtype == 4) {                   /* skin */
        int row = bv_pass_skin_row(p, data);
        if (row >= 0) { it->type = 9; it->amount = 1; it->skin = row; return 1; }
    } else if (rtype == 1) {                           /* coins */
        it->type = coin_type;
        return 1;
    } else if (p->new_types && rtype == 16) {         /* cycle 6: gems -> delivery item 8 (avatar+0x60) */
        it->type = 8;
        return 1;
    } else if (p->new_types && (rtype == 6 || rtype == 10 || rtype == 14)) { /* cycle 6: a box: marker, opened by offline_b.c */
        it->type = 1000 + rtype;
        return 1;
    } else if (rtype == 3 && data && data[0]) {        /* update 183: brawler unlock (Trophy Road: e.g. goal_6_1 = Shaman) -> delivery item 1 */
        for (int b = 0; b < p->n_brawlers; b++)
            if (!strcmp(p->brawlers[b], data)) { it->type = 1; it->amount = 1; it->brawler = b; return 1; }
    } else if (rtype == 12 && !maxed && brawler_row >= 0) { /* power points */
        it->type = 6;
        it->brawler = brawler_row;
        return 1;
    }
    if (unmapped && *unmapped < 0) *unmapped = rtype;
    it->type = coin_type;                              /* not decoded yet: 1 coin so the claim completes */
    it->amount = 1;
    return 1;
}

int bv_pass_items(const bv_passdata *p, int type, int season, int index, int coin_type, int brawler_row, int maxed, bmsg_box_item *out, int max,
                  int *unmapped) {
    if (unmapped) *unmapped = -1;
    const bv_pass_goal *g = bv_pass_find(p, type, season, index);
    if (!g || max < 1) return 0;
    int n = 0;
    if (g->r1_type >= 0) n += one_reward(p, g->r1_type, g->r1_count, g->r1_data, coin_type, brawler_row, maxed, &out[n], unmapped);
    if (g->r2_type >= 0 && n < max) n += one_reward(p, g->r2_type, g->r2_count, g->r2_data, coin_type, brawler_row, maxed, &out[n], unmapped);
    return n;
}
