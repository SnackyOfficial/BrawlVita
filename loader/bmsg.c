#include "bmsg.h"

#include <string.h>
#include <time.h>

void bmsg_default_cfg(bmsg_cfg *c) {
    memset(c, 0, sizeof *c);
    c->account_hi = 0;
    c->account_lo = 1;
    c->pass_token = "VitaOfflineToken0000000000000000000000";
    c->country = "XX";
    c->player_name = "Player";
    c->brawler_row = 0;
    c->map_row = 7;
    c->home_map = -1;
    c->home_slot = 1;
    c->events = 1;
    /* one map per mode, rows of csv_logic/locations.csv (Build B): Gemgrab1 7 (event 0 = map_row), Survival1 13, SurvivalTeam1 34, Heist2 18, Wanted6 4,
     * Ball1 24 (all not "Disabled") */
    static const int maps[8] = {7, 13, 34, 18, 4, 24, 7, 7};
    memcpy(c->event_map, maps, sizeof maps);
    /* slots of the texts table (localization/texts.csv): user report 2026-11-11 "TID_EVENT_SLOT_NAME_5" = slot 5 does not exist */
    static const int slots[8] = {1, 2, 8, 4, 6, 3, 7, 9};
    memcpy(c->event_slot, slots, sizeof slots);
    c->unlock_all = 0;
    c->trophies = 0;
    c->account_trophies = 0;
    c->exp_level = 0;
    c->exp_points = 0;
    c->power_level = 0;
    c->home_flags = 4; /* the field-order reference (the mod) */
    c->brawl_pass = 0;
    c->pass_season = 2;
    c->pass_tokens = 0;
    c->pass_premium = 0;
    c->shop_disabled = 1; /* the field-order reference (the mod) */
    c->offer_test = 0;
    c->tutorial_steps = 20;
    c->box_coin_type = 7;
    c->box_unit_mode = 0;
    for (int i = 0; i < 16; i++) c->offer_type[i] = i;
    c->n_extra_keys = 0;
    c->thumbnail_row = 0;
    c->gems = -1;
    c->coins = -1;
    c->shop_offers = 0;
    c->offer_cur = 1;
    c->owned_skins = -1;
    c->res_markers = 0;
    c->box_pp_random = 1;
    c->skin_offer_row = 2;
    c->ack24125 = 1;
    c->team_f0 = 1;
    c->team_f8 = 1;
    c->team_m18 = 1;
    c->team_m1c = 3;
    c->sel_skin_row = -1;
    c->battle_win = 8;
    c->battle_draw = 0;
    c->battle_lose = -4;
    c->member_row = -1;
    c->team_m10 = -1;   /* -1 = auto (resolved at send time, cycle 6) */
    c->team_m14 = -1;
    c->team_m18 = -1;
    c->skin_offer_price = 15;
    c->offer_unit = 100;
    c->pass_claim_reply = 0;
    c->mode_push_map = -1;
    c->mode_push_after = 3;
    c->skin_offer_kind = 1;
    { static const int t[12] = {0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 2}; for (int i = 0; i < 12; i++) c->avatar_tail[i] = t[i]; }
}

/* 20104. Standard login-ok order; every Int is big endian, strings are Int length + bytes (-1 = null). */
void bmsg_login_ok(bv_bs *s, const bmsg_cfg *c) {
    bs_int(s, c->account_hi);
    bs_int(s, c->account_lo);
    bs_int(s, c->account_hi); /* home id = account id */
    bs_int(s, c->account_lo);
    bs_string(s, c->pass_token);
    bs_string(s, NULL);  /* facebook id */
    bs_string(s, NULL);  /* game center id */
    bs_int(s, 26);       /* server major version the client expects */
    bs_int(s, 184);      /* server build */
    bs_int(s, 1);        /* content version */
    bs_string(s, "dev"); /* environment */
    bs_int(s, 0);        /* session count */
    bs_int(s, 0);        /* play time (s) */
    bs_int(s, 0);        /* days since the account started playing */
    bs_string(s, NULL);  /* facebook app id */
    bs_string(s, NULL);  /* server time */
    bs_string(s, NULL);  /* account created date */
    bs_int(s, 0);        /* startup cooldown (s) */
    bs_string(s, NULL);  /* google service id */
    bs_string(s, c->country);
    /* 19 bytes whose individual meaning is not decoded: Int 0, Int 2, Int 0, 0xff 0xff, five zero bytes (as the client accepts them) */
    bs_int(s, 0);
    bs_int(s, 2);
    bs_int(s, 0);
    bs_byte(s, 0xFF);
    bs_byte(s, 0xFF);
    for (int i = 0; i < 5; i++) bs_byte(s, 0);
}

static void zeros(bv_bs *s, int n) {
    while (n-- > 0) bs_vint(s, 0);
}

/* (characters row, cards row) of each brawler unlock card, from Build B's csv_logic/characters.csv + cards.csv (notes/2026-11-10-page-guard/unlock_map.py);
 * the two rows marked Disabled in characters.csv (Homer 33, FireDude 40) are left out. Row numbers only, no game data. */
static const short k_unlock[][2] = {
    {0, 0},     {1, 4},     {2, 8},     {3, 12},    {4, 16},    {5, 20},    {6, 24},    {7, 28},    {8, 32},    {9, 36},    {10, 40},   {11, 44},  {12, 48},
    {13, 52},   {14, 56},   {15, 60},   {16, 64},   {17, 68},   {18, 72},   {19, 95},   {20, 100},  {21, 105},  {22, 110},  {23, 115},  {24, 120}, {25, 125},
    {26, 130},  {27, 177},  {28, 182},  {29, 188},  {30, 194},  {31, 200},  {32, 206},  {34, 218},  {35, 224},  {36, 230},  {37, 236},  {38, 279}, {39, 296},
};
#define N_UNLOCK ((int)(sizeof k_unlock / sizeof k_unlock[0]))

/* value of a config switch: an override from config_keys=K:V,... if given, else `dflt` */
static int key_value(const bmsg_cfg *c, int key, int dflt) {
    for (int i = 0; i < c->n_extra_keys && i < 8; i++)
        if (c->extra_key[i][0] == key) return c->extra_key[i][1];
    return dflt;
}

/* The owned brawlers: (characters row, unlock card row). The selected brawler is always first and always owned. */
static int owned_brawlers(const bmsg_cfg *c, short out[][2], int max) {
    int n = 0, sel = -1;
    for (int i = 0; i < N_UNLOCK; i++)
        if (k_unlock[i][0] == c->brawler_row) sel = i;
    if (sel >= 0 && n < max) { out[n][0] = k_unlock[sel][0]; out[n][1] = k_unlock[sel][1]; n++; }
    for (int i = 0; i < N_UNLOCK && n < max; i++) {
            if (i == sel) continue;
            /* update 144: without unlock_all, the brawlers the profile owns (a real account: Shelly + whatever it unlocked) */
            if (!c->unlock_all && !(k_unlock[i][0] >= 0 && k_unlock[i][0] < 96 && c->br_has[k_unlock[i][0]])) continue;
            out[n][0] = k_unlock[i][0];
            out[n][1] = k_unlock[i][1];
            n++;
        }
    return n;
}

/* 24101. The sequence follows the client's reader; comments name the groups. Counts that depend on the game's data tables are
 * taken from `g` (rows of table N). Brawler/skin/card lists contain only the selected brawler: a fresh account. */
/* update 56: one shop offer (reader libg+0x51ada8, 17 reads, notes/2026-11-10-page-guard/shop_offers2.md); item = {OfferType, amount, table, row} */
static int g_title_slot; /* update 136 (B11 experiment offer_title_slot): 1 = the title goes into the +0x30 Int+String object, not the +0x34 String */
static void put_offer(bv_bs *s, int kind, int price, int old_price, int n_items, const int item[][4], int claimed, int promo, const char *title) {
    bs_vint(s, n_items);
    for (int i = 0; i < n_items; i++) {
        bs_vint(s, item[i][0]);                  /* OfferType (shop_items.csv): 4 skin, 8 power points, 10 Big Box, ... */
        bs_vint(s, item[i][1]);                  /* amount */
        if (item[i][0] == 4) { bs_dataref(s, 0, 0); bs_vint(s, item[i][3]); }          /* skin: extra = skin row (table 29) */
        else { if (item[i][2]) bs_dataref(s, item[i][2], item[i][3]); else bs_dataref(s, 0, 0); bs_vint(s, 0); }
    }
    bs_vint(s, kind);                            /* +0x10 pricing kind */
    bs_vint(s, price);                           /* +0x0c price */
    bs_vint(s, 172800);                          /* +0x14 (time left? 2 days; INFERRED) */
    bs_vint(s, 0);                               /* +0x18 */
    bs_vint(s, 0);                               /* +0x1c */
    bs_bool(s, claimed);                         /* +0x20 bought */
    bs_vint(s, 0);                               /* +0x24 */
    bs_bool(s, promo);                           /* +0x2c (cycle 6: 1 only for box / power-point special offers): NOT a harmless promo flag - it selects the list-item layout (style 2); for a skin
                                                  * that is clip shop_listitem_4, missing in this ui.sc -> crash on opening the shop (run 47). Always 0. */
    bs_vint(s, old_price);                       /* +0x38 original price (INFERRED) */
    bs_int(s, 0);                                /* +0x30 object: Int + String */
    bs_string(s, g_title_slot == 1 ? title : NULL);   /* update 136 experiment: a localized-text object (Int + String) may be the real title */
    bs_bool(s, 0);                               /* +0x40 */
    bs_string(s, g_title_slot == 1 ? NULL : title);   /* +0x34 title (update 101: generated offers carry one; run 152: not shown) */
    bs_vint(s, -1);                              /* +0x3c */
    bs_bool(s, 0);                               /* +0x4c hidden */
    bs_vint(s, 0);                               /* +0x44 */
    bs_vint(s, 0);                               /* +0x48 */
}

/* update 102 (B10): the client shows (total - cost of the levels below) / cost of the next level: run 121 Sprout level 3, progress 32 sent as the
 * total -> "-18/50". So list 2 = total power points = sum of the costs below the level + the progress. */
int bmsg_offer_id(const bmsg_offer *o) {
    uint32_t h = (uint32_t)o->type * 7919u + (uint32_t)o->amount * 31u + (uint32_t)(o->row + 1) * 131u + (uint32_t)o->price * 17u + (uint32_t)o->kind * 3u;
    return 1000000 + (int)(h % 900000u);
}

int bmsg_pp_total(int power_index, int progress) {
    static const int cost[8] = {20, 30, 50, 80, 130, 210, 340, 550};
    int t = 0;
    for (int i = 0; i < power_index && i < 8; i++) t += cost[i];
    return t + (progress > 0 ? progress : 0);
}

int bmsg_home_map(const bmsg_cfg *c) { return c->home_map >= 0 ? c->home_map : c->map_row; }

int bmsg_quest_count(const bmsg_cfg *c) {
    int ns = c->season_tt * 2 > 6 ? 6 : c->season_tt * 2;
    return 2 + (ns > 0 ? ns : 0);
}

uint32_t bmsg_quest_seed(const bmsg_cfg *c, int k) {
    int daily = k < 2;
    uint32_t x = (uint32_t)(daily ? c->quest_day * 2 + k : 100000 + (c->season_tt - 1 - (k - 2) / 2) * 2 + ((k - 2) & 1)) * 2654435761u + 0x9e37u;
    x ^= x >> 15; x *= 0x2c1b3c6du; x ^= x >> 12;
    return x;
}

int bmsg_quest_wants_brawler(uint32_t x) { return (x >> 8) % 3u == 0; }

void bmsg_own_home_data(bv_bs *s, const bmsg_cfg *c, const bmsg_game *g) {
    g_title_slot = c->offer_title_slot;   /* update 136 */
    int n29 = g && g->table_rows ? g->table_rows(g->ctx, 29) : 0;
    int n52 = g && g->table_rows ? g->table_rows(g->ctx, 52) : 0;
    if (n29 < 0) n29 = 0;
    if (n52 < 0) n52 = 0;

    /* first seven values. The original mod writes 0, 0, 99999, 99999, 0, 200, 99999 (work/B_f37.txt): the third and fourth are the account's trophies
     * and highest trophies (user report 2026-11-11, update 34: Shelly's own 1000 trophies did not count towards the trophy road; all were 0 here). */
    bs_vint(s, 0);
    bs_vint(s, 0);
    bs_vint(s, c->account_trophies);
    bs_vint(s, c->account_trophies);
    bs_vint(s, 0);
    bs_vint(s, c->exp_level);
    bs_vint(s, c->exp_points);
    bs_dataref(s, 28, c->thumbnail_row); /* player thumbnail */
    bs_dataref(s, 43, c->name_color_row > 0 ? c->name_color_row : 0);   /* name colour (update 184: the saved one; run 222 kept row 7 but sent 0 here) */
    /* a list of 20 numbers 0..19 (the mod wrote the same). Update 46 experiment: probably the completed tutorial steps; config tutorial_steps=N sends
     * only 0..N-1, to see whether the client then starts its own (local) tutorial battle. Default 20 = unchanged. */
    {
        int nt = c->tutorial_steps < 0 ? 0 : (c->tutorial_steps > 20 ? 20 : c->tutorial_steps);
        bs_vint(s, nt);
        for (int i = 0; i < nt; i++) bs_vint(s, i);
    }
    bs_vint(s, 0);                       /* entries of the first reference list (none) */
    /* home +0x3c = OWNED SKINS (resources.md: a skin is owned if it is the brawler's default or in this list). The mod listed ALL of table 29, so every
     * skin showed as owned / "Obtained" (user 2026-11-12). Update 61: c->owned_skins = 0 sends none (only default skins); -1 = all (the mod). */
    {
        if (c->own_skins) {              /* cycle 4: the profile's owned skins */
            bs_vint(s, c->n_own_skins);
            for (int i = 0; i < c->n_own_skins; i++) bs_dataref(s, 29, c->own_skins[i]);
        } else {
            int ns = c->owned_skins < 0 ? n29 : (c->owned_skins < n29 ? c->owned_skins : n29);
            bs_vint(s, ns);
            for (int i = 0; i < ns; i++) bs_dataref(s, 29, i);
        }
    }

    zeros(s, 5);
    bs_bool(s, 0);
    bs_vint(s, 0);
    bs_vint(s, c->season_left > 0 ? c->season_left : 86400);   /* update 103 (D4): one of these is the season timer ("ends in 23h53m" with 86400) */
    bs_vint(s, 0);
    bs_vint(s, c->season_left > 0 ? c->season_left : 86400);
    bs_vint(s, 0);
    bs_bool(s, 0);
    bs_bool(s, 0);
    /* three bit-packed Booleans (notes/.../shop_pass.md): bit 2 sets home +0x84, the only "true" flag here and the candidate for the shop's
     * TID_SHOP_TEMPORARILY_DISABLED (update 38: config home_flags, console default 0; the mod and the old builder wrote 4) */
    bs_byte(s, (uint8_t)c->home_flags);
    bs_vint(s, 2);
    bs_vint(s, 2);
    bs_vint(s, 2);
    zeros(s, 2);                         /* home +0x94, +0x98 */
    /* update 56: home +0x9c = the REAL shop offers (the list at +0x100 is the quests). Buy = client command 519 (VInt index, DataRef). */
    {
        int no = c->shop_offers < 0 ? 0 : (c->shop_offers > 2 ? 2 : c->shop_offers);
        if (c->n_offers > 0) {                 /* cycle 5: generated table (shop_layout=1) */
            int n = c->n_offers > BMSG_MAX_OFFERS ? BMSG_MAX_OFFERS : c->n_offers;
            bs_vint(s, n);
            for (int i = 0; i < n; i++) {
                const bmsg_offer *o = &c->offers[i];
                const int tbl = (o->type == 8 || o->type == 2) ? 16 : 0;   /* power points / brawler deal carry DataRef(16,row); boxes and doublers none */
                if (o->type == 4 && o->bonus > 0) {   /* update 184: box + skin = a special offer (offer_2_items: box big, skin small); 184b: a Mega Box (10) */
                    const int item2[2][4] = {{10, 1, 0, 0}, {4, 1, 0, o->row}};
                    put_offer(s, o->kind, o->price, o->old_price, 2, item2, (c->offer_bought_mask >> i) & 1, 0, o->title);
                    continue;
                }
                const int item[1][4] = {{o->type, o->amount, tbl, o->row}};
                put_offer(s, o->kind, o->price, o->old_price, 1, item, (c->offer_bought_mask >> i) & 1, o->type == 4 ? 0 : o->promo, o->title);
            }
            no = -1;
        }
        if (no >= 0) bs_vint(s, no);
        if (no >= 1) { const int skin[1][4] = {{4, 1, 0, c->skin_offer_row}};  put_offer(s, c->skin_offer_kind, c->skin_offer_price, c->skin_offer_price, 1, skin, (c->offer_bought_mask >> 0) & 1, 0, NULL); }   /* skins row 2 = Colt Rockstar, 15 gems (no sale badge: +0x2c must stay 0) */
        if (no >= 2) { const int pp[1][4] = {{8, 100, 16, 1}}; put_offer(s, c->offer_cur, 100, 100, 1, pp, (c->offer_bought_mask >> 1) & 1, 0, NULL); } /* 100 power points for Colt */
    }
    bs_vint(s, 0);                       /* count of list 64ca4c */
    bs_vint(s, c->home_a4);              /* +0xa4 (update 114 E7: key bar 'available tokens' candidate, libg+0x52e218) */
    bs_vint(s, c->home_a8);              /* +0xa8 */
    bs_vint(s, 0);                       /* count of a VInt list */
    bs_vint(s, 1);
    bs_vint(s, 1);
    bs_dataref(s, 16, c->brawler_row);   /* selected brawler */
    bs_string(s, c->player_name);
    bs_string(s, NULL);
    /* four list counts (shop_pass.md): Int pairs, (V, DataRef, V), BRAWL PASS seasons (reader libg+0x4ef2ec), (V, V). Update 38: one season entry =
     * VInt season (milestones.csv Type 9/10 seasons 0..2), VInt pass tokens, Bool premium, VInt 0, Bool "free-track claimed bits follow" 0,
     * Bool "premium-track claimed bits follow" 0. Every Bool follows a VInt, so one byte per Bool matches the bit-packed reader. */
    bs_vint(s, 0);
    bs_vint(s, 0);
    if (c->brawl_pass) {
        bs_vint(s, 1);
        bs_vint(s, c->pass_season);
        bs_vint(s, c->pass_tokens);
        bs_bool(s, c->pass_premium != 0);
        bs_vint(s, 0);
        /* the two "claimed bits follow" Booleans are ADJACENT in the reader, so they share ONE byte (bit 0, bit 1). Update 38 wrote two bytes:
         * the client then read everything after it one byte off ("Too long String encountered", "Instance id out of bounds") and crashed at login. */
        /* cycle 4 EXPERIMENT pass_claimed_bits=1: a set flag is followed by 4 Int words (bit tier%32 of word tier/32, INFERRED order); then the
         * next flag is NOT adjacent to the first any more -> its own byte */
        int fb = 0, pb = 0;
        for (int w = 0; w < 4; w++) { fb |= c->pass_bits[0][w] != 0; pb |= c->pass_bits[1][w] != 0; }
        if (!c->pass_claimed_bits || (!fb && !pb)) {
            bs_byte(s, 0);
        } else if (fb) {
            bs_byte(s, 1);
            for (int w = 0; w < 4; w++) bs_int(s, (int32_t)c->pass_bits[0][w]);
            bs_byte(s, pb ? 1 : 0);
            if (pb) for (int w = 0; w < 4; w++) bs_int(s, (int32_t)c->pass_bits[1][w]);
        } else {
            bs_byte(s, 2);               /* free false, premium true: adjacent Bools in one byte, bit 1 */
            for (int w = 0; w < 4; w++) bs_int(s, (int32_t)c->pass_bits[1][w]);
        }
    } else {
        bs_vint(s, 0);
    }
    bs_vint(s, 0);
    bs_bool(s, 1);                       /* "has shop offers": the list at home fields +0x100 (reader libg+0x58ec6c) */
    /* shop offers, 0x3c-byte records (reader libg+0x64b30): +0x00..+0x24 10 VInts, +0x28/+0x29 two Booleans PACKED in one byte, +0x2c DataRef,
     * +0x30/+0x34/+0x38 VInts. +0x30 = item type: the shop's sorter libg+0x2c68c0 puts an offer in a category by libg+0x22b98c(+0x30) (true for 7, 8, 10,
     * 18) and needs +0x24 >= 1 for the main category (static, 2026-11-11). Update 44: config offer_test=N sends N labelling offers: +0x30 = a type candidate
     * (offer_types=, default 0..15), the number fields are markers (field i = (i + 1) * 11: 11 ... 110; then +0x34 = 132, +0x38 = 143). Default 0 = none. */
    {
        int nt = c->offer_test > 0 ? (c->offer_test > 16 ? 16 : c->offer_test) : 0;
        if (c->quests_on == 3) {
            /* update 104 (D3): 2020 quest system (see loader/bmsg.h quest_day) */
            static const int modes[5] = {0, 2, 3, 5, 6};                     /* gem grab, heist, bounty, brawl ball, solo showdown */
            static const int goal_s[4] = {2, 8, 15000, 4000}, goal_m[4] = {4, 15, 30000, 8000}, goal_l[4] = {8, 30, 60000, 16000};
            int nq = bmsg_quest_count(c);
            bs_vint(s, nq);
            for (int k = 0; k < nq; k++) {
                int daily = k < 2;
                uint32_t x = bmsg_quest_seed(c, k);
                int type = 1 + (int)(x % 4u), size = daily ? 0 : ((k - 2) & 1) ? 2 : 1;
                int f[10] = {0};
                f[0] = daily ? 1 + k : 100 + k;
                f[2] = type;
                f[3] = 0;
                f[4] = size == 0 ? goal_s[type - 1] : size == 1 ? goal_m[type - 1] : goal_l[type - 1];
                f[5] = size == 0 ? 100 : size == 1 ? 250 : 500;
                /* update 105 (run 124): quests with all of f1/f7/f9/+0x34 = 0 went to SEASON quests; the labelling run (all four non-zero) put every quest
                 * under DAILY -> one of them marks a daily quest (its time left, INFERRED): all four carry the refresh seconds for daily quests */
                if (daily) f[1] = f[6] = f[7] = f[9] = c->quest_refresh_s;
                f[8] = 1;
                for (int i = 0; i < 10; i++) bs_vint(s, f[i]);
                /* run 124: packed bool bit 0 = the pass-exclusive lock + label -> only the large season quests */
                bs_byte(s, (!daily && ((k - 2) & 1)) ? 1 : 0);
                /* update 144 (req. 24): was the SELECTED brawler (a restart with another brawler moved every brawler quest to it). Now the brawler
                 * chosen for this quest (random among the owned ones, saved in the profile by loader/hook_b.c) */
                if (k < 8 && c->quest_br[k] >= 0) bs_dataref(s, 16, c->quest_br[k]); else bs_dataref(s, 0, 0);
                bs_vint(s, modes[(x >> 12) % 5u]);
                bs_vint(s, daily ? c->quest_refresh_s : 0);   /* +0x34 (see above) */
                bs_vint(s, 1);
            }
            nt = 0;
        } else if (c->quests_on == 2) {
            /* update 102 (D3, run 121 quest screenshot with markers): +0x08 = mission type (TID_QUEST_DESC_<n>: 1 win games, 2 defeat enemies, 3 damage,
             * 4 heal), +0x0c = progress, +0x10 = goal, +0x14 = token reward, +0x30 = game mode (TID_GAME_MODE_<n>: 0 gem grab, 2 heist, 3 bounty,
             * 5 brawl ball, 6 solo showdown), +0x2c = brawler condition, "LVL x/x" = +0x20 or +0x38. Quests 4/5: first packed bool set (season quests?) */
            static const struct { int type, goal, reward, mode, hero, season; } q[6] = {
                {1, 2, 100, 0, 0, 0}, {2, 10, 150, 5, 0, 0}, {3, 20000, 200, 6, 1, 0}, {4, 5000, 150, 3, 0, 0}, {1, 5, 300, 2, 0, 1}, {2, 25, 300, 5, 1, 1}};
            bs_vint(s, 6);
            for (int k = 0; k < 6; k++) {
                int f[10] = {0};
                f[0] = k + 1;          /* (id?) */
                f[2] = q[k].type;
                f[3] = 0;              /* progress */
                f[4] = q[k].goal;
                f[5] = q[k].reward;
                f[8] = 1;              /* LVL (with +0x38) */
                for (int i = 0; i < 10; i++) bs_vint(s, f[i]);
                bs_byte(s, q[k].season ? 1 : 0);   /* +0x28 / +0x29 packed: bit 0 */
                if (q[k].hero) bs_dataref(s, 16, c->brawler_row); else bs_dataref(s, 0, 0);
                bs_vint(s, q[k].mode);
                bs_vint(s, 0);
                bs_vint(s, 1);
            }
            nt = 0;
        } else if (c->quests_on) {
            /* cycle 7 (D3) labelling experiment quests=1: four real quests. Static (quest text builder libg+0x205cdc): +0x30 = mission type
             * (TID_QUEST_DESC_<type>: 1 win games, 2 defeat enemies, 3 deal damage, 4 heal), +0x10 = <AMOUNT> (== 1 -> the _SINGLE text), +0x2c = brawler
             * condition (TID_QUEST_HERO_CONDITION). The other number fields carry markers (quest k: 10k + field index) so a screenshot names them. */
            static const int goal[4] = {3, 5, 8, 10};
            bs_vint(s, 4);
            for (int k = 0; k < 4; k++) {
                for (int i = 0; i < 10; i++) bs_vint(s, i == 4 ? goal[k] : 10 * (k + 1) + i);
                bs_byte(s, 0);
                if (k == 2) bs_dataref(s, 16, c->brawler_row); else bs_dataref(s, 0, 0);
                bs_vint(s, 1 + k);
                bs_vint(s, 10 * (k + 1) + 7);
                bs_vint(s, 10 * (k + 1) + 8);
            }
            nt = 0;
        } else
        bs_vint(s, nt);
        for (int k = 0; k < nt; k++) {
            /* update 45: markers (i + 1) * 10001 (10001 ... 100010): run 29 showed no offer with markers 11..110, most likely because one field is the
             * time left in seconds and they had all run out before the shop was opened */
            for (int i = 0; i < 10; i++) bs_vint(s, (i + 1) * 10001);
            bs_byte(s, 0);
            bs_dataref(s, 16, 0);
            bs_vint(s, c->offer_type[k]);
            bs_vint(s, 120012);
            bs_vint(s, 130013);
        }
    }
    bs_bool(s, 1);

    bs_vint(s, n52);                     /* reference list over table 52, three VInts each */
    for (int i = 0; i < n52; i++) {
        bs_dataref(s, 52, i);
        bs_vint(s, 1);
        bs_vint(s, 1);
        bs_vint(s, 1);
    }

    /* limits and costs */
    static const int32_t limits[] = {0, 100, 10, 30, 3, 80, 10, 50, 1000, 500, 50, 999900, 0}; /* 999900 as the original mod (was 0) */
    for (unsigned i = 0; i < sizeof limits / sizeof limits[0]; i++) bs_vint(s, (int)i < c->n_limits ? c->limits[i] : limits[i]);   /* update 115: config limits= */

    /* menu events: numbered 0..n, then n entries */
    int ev = c->events > 0 ? (c->events > 8 ? 8 : c->events) : 0;
    bs_vint(s, 1 + ev);
    bs_vint(s, 0);
    for (int i = 0; i < ev; i++) bs_vint(s, c->event_slot[i] > 0 ? c->event_slot[i] : 1 + i); /* the slot ids in use (was 0..n) */
    bs_vint(s, ev);
    for (int i = 0; i < ev; i++) {
        bs_vint(s, c->event_id[i] > 0 ? c->event_id[i] : 1);                 /* update 109: id per rotation */
        bs_vint(s, c->event_slot[i] > 0 ? c->event_slot[i] : 1 + i);
        bs_vint(s, 0);
        bs_vint(s, c->event_left[i] > 0 ? c->event_left[i] : 99999);         /* seconds left (update 109: real, 4h48m rotation) */
        bs_vint(s, c->new_event_tokens > 0 && c->ev_unclaimed[i] ? c->new_event_tokens : 0);   /* +0x10 (update 131 E4 experiment: new-event tokens?) */
        bs_dataref(s, 15, (i == 0 && !c->ev_maps_exact) || c->event_map[i] < 0 ? c->map_row : c->event_map[i]);   /* update 144: exact with rotation */
        /* update 148 (user T13b / req. 19): the card shows its NEW label only when +0x10 >= 1 AND this field (+0x18) <= 1 (event card code
         * libg+0x213604..0x213618); the mod's constant 2 hid it for ever. 1 while the new map's reward is unclaimed, else 2 as before. */
        bs_vint(s, c->new_event_tokens > 0 && c->ev_unclaimed[i] ? 1 : 2);
        bs_string(s, NULL);
        zeros(s, 3);
        bs_vint(s, 0);              /* modifiers (none) */
        bs_vint(s, 0);
        bs_vint(s, 0);
    }
    /* update 123 (E5): the UPCOMING list (config +0x54): the next map of each slot, starting when the current one ends */
    int nup = 0;
    for (int i = 0; c->upcoming_events && i < ev; i++) if (c->next_map[i] >= 0 && c->event_left[i] > 0) nup++;
    bs_vint(s, nup);
    for (int i = 0; nup && i < ev; i++) {
        if (c->next_map[i] < 0 || c->event_left[i] <= 0) continue;
        bs_vint(s, c->next_id[i] > 0 ? c->next_id[i] : 1);
        bs_vint(s, c->event_slot[i] > 0 ? c->event_slot[i] : 1 + i);
        bs_vint(s, c->event_left[i]);                 /* +8 starts in (seconds) */
        bs_vint(s, c->event_left[i] + 17280);         /* +0xc seconds left (one 4 h 48 min period after the start) */
        bs_vint(s, 0);
        bs_dataref(s, 15, c->next_map[i]);
        bs_vint(s, 2);
        bs_string(s, NULL);
        zeros(s, 3);
        bs_vint(s, 0);
        bs_vint(s, 0);
        bs_vint(s, 0);
    }

    /* upgrade price tables (game balance constants) */
    static const int32_t t1[] = {20, 35, 75, 140, 290, 480, 800, 1250};
    static const int32_t t2[] = {1, 2, 3, 4, 5, 10, 15, 20};
    static const int32_t t3[] = {10, 30, 80};
    static const int32_t t4[] = {6, 20, 60};
    static const int32_t t5[] = {20, 50, 140, 280};
    static const int32_t t6[] = {150, 400, 1200, 2600};
    bs_array_vint(s, t1, 8);
    bs_array_vint(s, t2, 8);
    bs_array_vint(s, t3, 3);
    bs_array_vint(s, t4, 3);
    bs_array_vint(s, t5, 4);
    bs_array_vint(s, t6, 4);

    for (int i = 0; i < 4; i++) bs_vint(s, c->tok_cfg[i]);   /* update 116 (E7): config +0xa8, +0xac = key bar maximum (libg+0x52e22c), +0xb0, +0xb4 */
    bs_vint(s, 10);
    bs_vint(s, 0);
    bs_byte(s, 0);
    bs_vint(s, 0);
    bs_vint(s, 0);
    bs_bool(s, 1);
    bs_vint(s, 0);
    /* config feature switches: Int pairs {key, value}, looked up by libg+0xd3004(config, key, default) (77 call sites, notes/.../config_keys.py).
     * Key 5 = "shop disabled": the shop's open function libg+0x138ee0 shows its "temporarily disabled" popup when lookup(5) == 1 (getter trace of
     * 2026-11-11, run 26). The original mod sent 5 = 1. Update 42: c->shop_disabled (console default 0) and extra pairs from config_keys=K:V,... */
    int nk = 3 + (c->quests_on == 3 ? 2 : 0), extra = 0;   /* update 108 (D8): keys 47 / 48 = quest timers */
    for (int i = 0; i < c->n_extra_keys && i < 8; i++)
        if (c->extra_key[i][0] != 1 && c->extra_key[i][0] != 37 && c->extra_key[i][0] != 5) extra++;
    bs_vint(s, nk + extra);
    bs_int(s, 1);
    bs_int(s, 41000000 + c->thumbnail_row);
    bs_int(s, 37);
    bs_int(s, key_value(c, 37, 0));   /* cycle 6: 0 = the BRAWL PASS home button is shown (libg+0x4114b8 lookup(37) == 0; run 86 VERIFIED). The mod sent 1. */
    bs_int(s, 5);
    bs_int(s, key_value(c, 5, c->shop_disabled));
    if (c->quests_on == 3) {
        bs_int(s, 47);
        bs_int(s, c->quest_refresh_s);
        bs_int(s, 48);
        bs_int(s, c->quest_batch_s);
    }
    for (int i = 0; i < c->n_extra_keys && i < 8; i++)
        if (c->extra_key[i][0] != 1 && c->extra_key[i][0] != 37 && c->extra_key[i][0] != 5) {
            bs_int(s, c->extra_key[i][0]);
            bs_int(s, c->extra_key[i][1]);
        }
    bs_vint(s, 0);
    bs_vint(s, 0);
    bs_int(s, 0);
    bs_int(s, 1);
    bs_vint(s, 0);
    bs_vint(s, 0);
    bs_bool(s, 1);
    bs_bool(s, 0);
    bs_vint(s, 0);
    bs_vint(s, 0);
    bs_vint(s, 1);
    bs_vint(s, 0);
    bs_vint(s, 1);
    bs_vint(s, 0);
    bs_vint(s, 1);
    bs_string(s, c->player_name);
    bs_vint(s, 1);
    bs_int(s, 0);
    bs_vint(s, 8);

    /* owned cards, then four resource entries (DataRef(5, n), count). Update 35: the unlock card of every owned brawler goes HERE, as in the original
     * mod (count = cards + 4, each card DataRef(23, row) with count 1); update 34 only had them in the last list, and the client still showed Shelly locked. */
    short own[64][2];
    int n = owned_brawlers(c, own, 64);
    /* update 56: resources.csv (table 5) row 0 = Diamonds (gems), row 1 = Gold (coins); the balance read libg+0x5a04c takes the first entry with that row.
     * The mod sent no gems entry and coins 0 (c->gems / c->coins = -1 keeps exactly that). */
    int with_gems = c->gems >= 0;
    int ncard = c->own_cards ? c->n_own_cards : 0;   /* update 174: owned gadget / star power cards in the same list */
    bs_vint(s, n + ncard + 4 + with_gems);
    for (int i = 0; i < n; i++) {
        bs_dataref(s, 23, own[i][1]);
        bs_vint(s, 1);
    }
    for (int i = 0; i < ncard; i++) { bs_dataref(s, 23, c->own_cards[i]); bs_vint(s, 1); }
    if (c->res_markers) {   /* update 62 experiment: the count above already said n + 4 (+1): keep that number of entries, rows chosen so each is unique */
        /* res_markers=1: rows 1,2,3,4,10 (run 59: coins NOT among them); =2 (update 67): rows 5..8 + 9 (the remaining candidates; run 65: deliveries
         * of item type 7 DO raise the coin counter, so coins are some list-0 row) */
        static const int rows1[5] = {1, 2, 3, 4, 10}, rows2[5] = {5, 6, 7, 8, 9};
        const int *rows = c->res_markers == 2 ? rows2 : rows1;
        if (with_gems) { bs_vint(s, 5); bs_vint(s, 0); bs_vint(s, 1000); }
        for (int i = 0; i < 4; i++) { bs_vint(s, 5); bs_vint(s, rows[i]); bs_vint(s, 1000 + rows[i]); }
    } else {
        if (with_gems) { bs_vint(s, 5); bs_vint(s, 0); bs_vint(s, c->gems); }
        /* update 68: COINS = list-0 row 8 (HARDWARE-VERIFIED run 66: res_markers=2 put 1008 in the coin slot); row 10 = star points (its slot
         * vanishes without it); row 1 is NOT coins (shop_offers2.md had the row names wrong) */
        const int32_t generic[4][3] = {{5, 1, 0}, {5, 8, c->coins >= 0 ? c->coins : 0}, {5, 9, 0}, {5, 10, c->star_points > 0 ? c->star_points : 1}};   /* update 130: star points (D6) */
        for (int i = 0; i < 4; i++) {
            bs_vint(s, generic[i][0]);
            bs_vint(s, generic[i][1]);
            bs_vint(s, generic[i][2]);
        }
    }

    /* per-brawler lists: trophies, highest trophies, (an empty list), power points, power level (the mod: 99999, 99999, 0, 8). Every owned brawler gets an
     * entry; trophies go to the selected brawler, power level c->power_level (8 = level 9, the maximum) to all (update 35). */
    for (int list = 0; list < 4; list++) {
        bs_vint(s, n);
        for (int i = 0; i < n; i++) {
            bs_dataref(s, 16, own[i][0]);
            int r = own[i][0], has = r >= 0 && r < 96 && c->br_has[r];   /* cycle 6 (G2): per-brawler values from the profile */
            bs_vint(s, list == 0 ? (has ? c->br_tro[r] : (r == c->brawler_row ? c->trophies : 0))
                     : list == 1 ? (has ? c->br_best[r] : (r == c->brawler_row ? c->trophies : 0))
                     : list == 2 ? (has ? bmsg_pp_total(c->br_pow[r], c->br_pp[r]) : 0)   /* update 102 (B10, run 121 '-18/50'): TOTAL power points */
                     : list == 3 ? (has ? c->br_pow[r] : c->power_level) : 0);
        }
        if (list == 1) bs_vint(s, 0);
    }

    /* last card list (the mod's l6, also built from the owned brawlers): the unlock cards again, count 1 each (kept from update 34) */
    bs_vint(s, n);
    for (int i = 0; i < n; i++) {
        bs_dataref(s, 23, own[i][1]);
        bs_vint(s, 1);
    }
    for (int i = 0; i < 12; i++) bs_vint(s, c->avatar_tail[i]);   /* update 57: configurable (avatar_tail=), default = the mod's 0,0,0,1,0,0,0,0,0,0,0,2 */
    bs_vint(s, 1585502369);
}

/* 24124: the selected brawler / map after the player changes them. No skin selected. */
void bmsg_avatar_update(bv_bs *s, const bmsg_cfg *c, int selected_map_row) {
    /* = the team (decode libg+0x398c10, mode_switch.md section 6): VInt +0, Bool +4, VInt +8, Long id, ... */
    bs_vint(s, c->team_f0);
    bs_vint(s, 0);
    bs_vint(s, c->team_f8);
    zeros(s, 4);
    bs_int(s, 1);
    /* update 161 (decode_home_card.md, CONFIRMED): after the id come VInt +0x10, the packed Bools +5/+6 (one byte), VInt +0x14 = EVENT ID, VInt +0x18
     * = EVENT SLOT, then the map +0x1c. The card builder libg+0x64937c looks the event up by slot; on a tap the client itself writes id / slot / map
     * into the team (libg+0x585750) and our reply overwrote id and slot with 0. Now the selected event's id and slot (0 / 0 when none). */
    {
        int ev = c->events < 0 ? 0 : (c->events > 8 ? 8 : c->events), sid = 0, sslot = 0;
        for (int i = 0; i < ev; i++) {
            int sl = c->event_slot[i] > 0 ? c->event_slot[i] : 1 + i;
            if (c->home_slot > 0 && sl == c->home_slot) { sid = c->event_id[i] > 0 ? c->event_id[i] : 1; sslot = sl; break; }
        }
        bs_vint(s, 0);       /* +0x10 */
        bs_byte(s, 0);       /* +5 / +6 (adjacent Bools, one byte) */
        /* update 164 (crash runs 186-191: every battle died at 100 % with update 161-163; update 158 without this change runs, run 192): the local battle
         * start uses these team fields -> back to 0 / 0 (as before update 161). The home card refresh stays open (decode_home_card.md). */
        (void)sid; (void)sslot;
        bs_vint(s, 0);       /* +0x14 event id */
        bs_vint(s, 0);       /* +0x18 event slot */
    }
    bs_dataref(s, 15, selected_map_row);
    /* update 147 (crash run 170): the VInt after the map is the MEMBER COUNT (team decoder libg+0x398c10: DataRef -> +0x1c, VInt -> count of the +0x20
     * member array). Update 146 sent the selected event slot here (3 for Brawl Ball) -> 3 members read from 1 -> crash. Always 1 member. */
    bs_vint(s, 1);
    bs_vint(s, 1);
    zeros(s, 4);
    bs_int(s, 1);
    bs_dataref(s, 16, c->member_row >= 0 ? c->member_row : c->brawler_row);  /* member +8 brawler (cycle 6: the viewed one with member_view) */
    if (c->sel_skin_row >= 0) bs_dataref(s, 29, c->sel_skin_row); /* member +0xc skin (cycle 6) */
    else bs_vint(s, 0);                 /* member +0xc skin: null */
    {   /* cycle 6 (G2): the member card follows the SELECTED brawler; type 1 (room) keeps the original bytes 0 / 0 / 1 */
        int r = c->brawler_row, has = r >= 0 && r < 96 && c->br_has[r];
        int tro = has ? c->br_tro[r] : c->trophies, best = has ? c->br_best[r] : tro, pw = (has ? c->br_pow[r] : c->power_level) + 1;
        bs_vint(s, c->team_m10 >= 0 ? c->team_m10 : (c->team_f0 == 1 ? 0 : tro));    /* member +0x10 trophies (run 77) */
        bs_vint(s, c->team_m14 >= 0 ? c->team_m14 : (c->team_f0 == 1 ? 0 : best));   /* member +0x14 highest -> rank (run 84) */
        bs_vint(s, c->team_m18 >= 0 ? c->team_m18 : (c->team_f0 == 1 ? 1 : pw));     /* member +0x18 power shown (run 78) */
    }
    bs_vint(s, c->team_m1c);            /* member +0x1c (original 3) */
    zeros(s, 4);
    bs_string(s, c->player_name);
    bs_vint(s, 100);
    bs_vint(s, 28000000 + c->thumbnail_row);
    bs_vint(s, 43000000 + c->name_color_row);   /* update 182 */
    bs_vint(s, 0);
    bs_vint(s, 0); /* skin slot A: null reference */
    bs_vint(s, 0); /* skin slot B: null reference */
    zeros(s, 4);
    bs_vint(s, 6);
}

void bmsg_ack(bv_bs *s) {
    bs_int(s, 0);
}

void bmsg_matchmake_status(bv_bs *s) {
    bs_int(s, 0); /* meanings not known yet: see bmsg.h */
    bs_int(s, 1);
    bs_int(s, 6);
    bs_int(s, 0);
    bs_int(s, 0);
    bs_bool(s, 1);
}

int bmsg_parse_14102(const void *body, size_t len, int *tick, int *first_id) {
    bv_rd r;
    rd_init(&r, body, len);
    (void)rd_vint(&r);            /* 0 in every body seen */
    int32_t t = rd_vint(&r);      /* tick */
    (void)rd_vint(&r);            /* checksum (0) */
    int32_t n = rd_vint(&r);      /* commands */
    int32_t id = n > 0 ? rd_vint(&r) : -1;
    if (r.err || n < 0) return -1;
    if (tick) *tick = (int)t;
    if (first_id) *first_id = (int)id;
    return (int)n;
}

int bmsg_parse_cmds(const void *body, size_t len, bmsg_cmd *out, int max) {
    bv_rd r;
    rd_init(&r, body, len);
    (void)rd_vint(&r); (void)rd_vint(&r); (void)rd_vint(&r);
    int32_t n = rd_vint(&r);
    if (r.err || n < 0) return 0;
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        bmsg_cmd *c = &out[k];
        memset(c, 0, sizeof *c);
        c->id = (int)rd_vint(&r);
        for (int b = 0; b < 4; b++) (void)rd_vint(&r);   /* base: tick, tick, -1, -1 */
        if (r.err) break;
        /* own fields: V = VInt, D = DataRef, I = Int, B = Bool byte */
        const char *lay = NULL;
        switch (c->id) {
        case 500: case 508: case 521: case 524: case 536: lay = "V"; break;
        case 503: case 514: case 523: case 528: case 531: case 535: lay = "VV"; break;
        case 515: lay = "VVVV"; break;
        case 517: lay = "VDVV"; break;
        case 519: lay = "VD"; break;
        case 505: case 506: case 507: case 520: case 525: case 527: case 529: case 532: case 537: case 538: lay = "D"; break;
        case 522: lay = "DI"; break;
        case 530: lay = "I"; break;
        case 534: lay = "VB"; break;
        case 509: case 512: case 526: case 533: lay = ""; break;
        default: break;
        }
        if (!lay) break;
        int f = 0;
        for (const char *q = lay; *q && !r.err; q++, f++) {
            int val = 0;
            if (*q == 'V') val = (int)rd_vint(&r);
            else if (*q == 'D') { int cl = (int)rd_vint(&r); val = cl ? (int)rd_vint(&r) : -1; c->cls = cl; }
            else if (*q == 'I') { uint32_t a = 0; for (int y = 0; y < 4; y++) a = (a << 8) | rd_byte(&r); val = (int)a; }
            else if (*q == 'B') val = rd_byte(&r);
            if (f < 4) c->v[f] = val;
        }
        if (r.err) break;
        k++;
    }
    return k;
}

int bmsg_parse_box_open(const void *body, size_t len) {
    bv_rd r;
    rd_init(&r, body, len);
    (void)rd_vint(&r);          /* 0 */
    (void)rd_vint(&r);          /* tick */
    (void)rd_vint(&r);          /* checksum */
    int32_t n = rd_vint(&r);    /* commands */
    if (r.err || n < 1) return -1;
    int32_t id = rd_vint(&r);
    if (r.err || id != 500) return -1;
    for (int i = 0; i < 4; i++) (void)rd_vint(&r); /* base: tick, tick, -1, -1 */
    int32_t box = rd_vint(&r);
    return r.err || box < 0 ? -1 : (int)box;
}

static void box_delivery(bv_bs *s, int unit_type, const bmsg_box_item *items, int n, int ctype, int ctier2, int cseason);
void bmsg_box_delivery(bv_bs *s, int unit_type, const bmsg_box_item *items, int n) { box_delivery(s, unit_type, items, n, 0, 0, 0); }
void bmsg_box_delivery_claim(bv_bs *s, int unit_type, const bmsg_box_item *items, int n, int ctype, int ctier2, int cseason) {
    box_delivery(s, unit_type, items, n, ctype, ctier2, cseason);
}
static void box_delivery(bv_bs *s, int unit_type, const bmsg_box_item *items, int n, int ctype, int ctier2, int cseason) {
    bs_vint(s, 203);       /* server command id: give delivery items */
    bs_vint(s, 0);         /* f1 (-> home +0xbc, unknown) */
    bs_vint(s, 1);         /* one delivery unit */
    bs_vint(s, unit_type);
    bs_vint(s, n);
    for (int i = 0; i < n; i++) {
        bs_vint(s, items[i].amount);
        if ((items[i].type == 6 || items[i].type == 1) && items[i].brawler >= 0) bs_dataref(s, 16, items[i].brawler);   /* update 144: 1 = brawler unlock (ref8) */
        else bs_dataref(s, 0, 0);
        bs_vint(s, items[i].type);
        if (items[i].type == 9 && items[i].skin >= 0) bs_dataref(s, 29, items[i].skin); /* update 64: skin (ref0c) */
        else bs_dataref(s, 0, 0);
        bs_dataref(s, 0, 0); /* pin */
        if (items[i].type == 4 && items[i].card >= 0) bs_dataref(s, 23, items[i].card); else bs_dataref(s, 0, 0); /* type 4 reference (update 174: gadget / star power card) */
        bs_vint(s, 0);       /* extra */
    }
    bs_bool(s, 0);         /* hasExtra: the only Boolean here, one byte */
    bs_vint(s, ctype);     /* g30: pass claim type (cycle 6, handler 0x1fe5b4 marks the tier claimed when g34 >= 1) */
    bs_vint(s, ctier2);    /* g34: tier + 2 */
    bs_vint(s, cseason);   /* g38: season */
    bs_vint(s, 0);         /* server command tail: v18, v0c (-1 = unset), v08, long hi, long lo */
    bs_vint(s, -1);
    bs_vint(s, 0);
    bs_vint(s, 0);
    bs_vint(s, 0);
}

/* update 65: model 10 = Brawl Box, 11 = Mega Box, 12 = Big Box (user runs 57 + 60). Shop: Big Box sends boxType 1, Mega Box boxType 3 (both seen).
 * 4 / 5 (boxes.md guesses for Big / Mega) map the same way; anything else = Brawl Box. */
int bmsg_box_unit(int box_type) {
    switch (box_type) {
    case 1: case 4: return 12;   /* Big Box */
    case 3: case 5: return 11;   /* Mega Box */
    default: return 10;          /* Brawl Box */
    }
}

/* update 65: client command 517 (Brawl Pass reward claim; run 60: fields 9, 0, 2, 0 after the 4 base VInts) -> 1 and the 4 fields, else 0 */
int bmsg_parse_pass_claim(const void *body, size_t len, int f[4]) {
    bv_rd r;
    rd_init(&r, body, len);
    (void)rd_vint(&r);
    (void)rd_vint(&r);
    (void)rd_vint(&r);
    int32_t n = rd_vint(&r);
    if (r.err || n < 1) return 0;
    int32_t id = rd_vint(&r);
    if (r.err || id != 517) return 0;
    for (int i = 0; i < 4; i++) (void)rd_vint(&r);
    for (int i = 0; i < 4; i++) f[i] = (int)rd_vint(&r);
    return !r.err;
}

int bmsg_parse_selection(const void *body, size_t len, int *brawler_row, int *skin_row) {
    bv_rd r;
    rd_init(&r, body, len);
    *brawler_row = -1;
    *skin_row = -1;
    (void)rd_vint(&r);          /* 0 */
    (void)rd_vint(&r);          /* tick */
    (void)rd_vint(&r);          /* checksum */
    int32_t n = rd_vint(&r);
    if (r.err || n < 1) return 0;
    for (int i = 0; i < n && i < 32; i++) {
        int32_t id = rd_vint(&r);
        for (int k = 0; k < 4; k++) (void)rd_vint(&r);   /* base: tick, tick, -1, -1 */
        if (r.err) break;
        int cls, row;
        if (id == 525 || id == 522 || id == 506) {
            int32_t c = rd_vint(&r);
            cls = (int)c;
            row = c ? (int)rd_vint(&r) : -1;
            if (r.err) break;
            if (cls == 16 && id == 525) *brawler_row = row;   /* run 81: 522 = browsing the brawler screen, 525 (+506) = the Select button */
            if (cls == 29 && id == 506) *skin_row = row;
            if (id == 522) break;                        /* rest of 522 not decoded */
        } else {
            break;                                       /* unknown payload: stop */
        }
    }
    return *brawler_row >= 0 || *skin_row >= 0;
}

int bmsg_parse_view(const void *body, size_t len) {
    bv_rd r;
    rd_init(&r, body, len);
    (void)rd_vint(&r);
    (void)rd_vint(&r);
    (void)rd_vint(&r);
    int32_t n = rd_vint(&r);
    if (r.err || n < 1) return -1;
    int32_t id = rd_vint(&r);
    for (int k = 0; k < 4; k++) (void)rd_vint(&r);
    if (r.err || id != 522) return -1;
    int32_t c = rd_vint(&r);
    int32_t row = c ? rd_vint(&r) : -1;
    return (!r.err && c == 16) ? (int)row : -1;
}

int bmsg_parse_offer_buy(const void *body, size_t len) {
    bv_rd r;
    rd_init(&r, body, len);
    (void)rd_vint(&r);          /* 0 */
    (void)rd_vint(&r);          /* tick */
    (void)rd_vint(&r);          /* checksum */
    int32_t n = rd_vint(&r);    /* commands */
    if (r.err || n < 1) return -1;
    int32_t id = rd_vint(&r);
    if (r.err || id != 519) return -1;
    for (int i = 0; i < 4; i++) (void)rd_vint(&r); /* base: tick, tick, -1, -1 */
    int32_t idx = rd_vint(&r);
    return r.err || idx < 0 ? -1 : (int)idx;
}

int bmsg_offer_unit(int index, const bmsg_cfg *c, int dflt) {
    if (c->n_offers <= 0 || index < 0 || index >= c->n_offers || index >= BMSG_MAX_OFFERS) return dflt;
    int t = c->offers[index].type;
    if (t == 10) return bmsg_box_unit(3);
    if (t == 14) return bmsg_box_unit(1);
    if (t == 0 || t == 6) return bmsg_box_unit(2);
    return dflt;
}

int bmsg_owned_brawler_rows(const bmsg_cfg *c, int *rows, int max) {
    short own[64][2];
    int n = owned_brawlers(c, own, 64);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) rows[i] = own[i][0];
    return n;
}

int bmsg_offer_items(int index, const bmsg_cfg *c, bmsg_box_item *out) {
    if (c->n_offers > 0) {                     /* cycle 5: generated table */
        if (index < 0 || index >= c->n_offers || index >= BMSG_MAX_OFFERS) return 0;
        const bmsg_offer *o = &c->offers[index];
        out[0].brawler = -1;
        out[0].skin = -1;
        out[0].amount = o->amount;
        if (o->type == 4) {
            out[0].type = 9; out[0].amount = 1; out[0].skin = o->row;
            if (o->bonus <= 0) return 1;
            /* update 184b (user): the bonus of a special skin offer is a Mega Box: its rolled contents follow the skin */
            bmsg_box_item mb[BMSG_BOX_MAX];
            int m = bmsg_box_roll(3, (uint32_t)(index * 7919 + o->row * 131 + o->price), c, c->box_coin_type, mb);
            if (m > BMSG_BOX_MAX - 1) m = BMSG_BOX_MAX - 1;
            for (int k = 0; k < m; k++) out[1 + k] = mb[k];
            return 1 + (m > 0 ? m : 0);
        }
        if (o->type == 8) {                    /* cycle 6 (B7): power points if THAT brawler is below the maximum */
            int pw = (o->row >= 0 && o->row < 96 && c->br_has[o->row]) ? c->br_pow[o->row] : c->power_level;
            if (pw < 8) { out[0].type = 6; out[0].brawler = o->row; return 1; }
        }
        if (o->type == 0 || o->type == 6 || o->type == 10 || o->type == 14) {   /* cycle 6: boxes -> rolled contents, `amount` boxes */
            int bt = o->type == 10 ? 3 : o->type == 14 ? 1 : 2;
            int n = bmsg_box_roll(bt, (uint32_t)(index * 7919 + o->price), c, c->box_coin_type, out);
            for (int k = 1; k < o->amount && n > 0; k++) {
                bmsg_box_item more[BMSG_BOX_MAX];
                int m = bmsg_box_roll(bt, (uint32_t)(index * 7919 + k * 104729), c, c->box_coin_type, more);
                if (m > 0 && more[0].type == out[0].type) out[0].amount += more[0].amount;
            }
            return n;
        }
        if (o->type == 16) { out[0].type = 8; return 1; }          /* update 101: gems (203 item 8) */
        if (o->type == 2) { out[0].type = c->box_coin_type; out[0].amount = 500; return 1; }   /* brawler deal: every brawler is owned -> coins */
        out[0].type = c->box_coin_type;        /* maxed brawlers (or anything else): coins instead */
        return 1;
    }
    int no = c->shop_offers < 0 ? 0 : (c->shop_offers > 2 ? 2 : c->shop_offers);
    if (index < 0 || index >= no) return 0;
    if (index == 0) { out[0].type = 9; out[0].amount = 1; out[0].brawler = -1; out[0].skin = c->skin_offer_row; return 1; }   /* the daily skin (cycle 4) */
    if (c->power_level >= 8) { out[0].type = c->box_coin_type; out[0].amount = 100; out[0].brawler = -1; out[0].skin = -1; return 1; } /* maxed: coins back */
    out[0].type = 6; out[0].amount = 100; out[0].brawler = 1; out[0].skin = -1; return 1;                      /* 100 PP for Colt */
}

int bmsg_unlock_entry(int i, int *char_row, int *card_row) {
    if (i < 0 || i >= N_UNLOCK) return 0;
    *char_row = k_unlock[i][0];
    *card_row = k_unlock[i][1];
    return 1;
}

int bmsg_box_brawler(const bmsg_cfg *c, int r, uint32_t pick) {
    static const int odds[6] = {0, 277, 125, 56, 26, 11};   /* per 10000, rarity 1..5 (sources: docs/BOXES.md) */
    int acc = 0;
    for (int rar = 5; rar >= 1; rar--) {   /* rarest first: the bands do not overlap */
        int cand[64], nc = 0;
        for (int i = 0; i < N_UNLOCK && nc < 64; i++) {
            int row = k_unlock[i][0];
            if (row < 0 || row >= 96 || c->br_rar[row] != rar) continue;
            if (c->unlock_all || c->br_has[row]) continue;          /* owned already */
            cand[nc++] = row;
        }
        if (!nc) continue;                                           /* a finished rarity drops out (its chance goes back to coins / PP) */
        if (r >= acc && r < acc + odds[rar]) return cand[pick % (uint32_t)nc];
        acc += odds[rar];
    }
    return -1;
}

int bmsg_box_roll(int box_type, uint32_t seed, const bmsg_cfg *c, int coin_type, bmsg_box_item *out) {
    uint32_t x = seed * 2654435761u + 0x9e3779b9u;
#define RND(n) ((x = x * 1103515245u + 12345u), (int)((x >> 16) % (uint32_t)(n)))
    if (c->box_drops) {
        /* update 146 (user rules, run 168): Brawl Box = coins + 1 power-point stack (2 items), Big Box = coins + 1-2 stacks (2-3 items), Mega Box = coins
         * + 4 stacks (5 items); every brawler that drops adds one more item (so up to 3 / 4 / 6+). Brawler chances per roll as update 144 (docs/BOXES.md),
         * Big Box = 3 rolls, Mega Box = 10. Gadgets / star powers: their delivery item type is not decoded yet (docs/BOXES.md) - not given. */
        int unit = bmsg_box_unit(box_type);
        int rolls = unit == 11 ? 10 : unit == 12 ? 3 : 1;
        int stacks = unit == 11 ? 5 : unit == 12 ? 2 + RND(2) : 2;   /* update 174: the opening screen counts items without the coins (user: 1 / 1 / 4) */
        int n = 0, coins = 0, got[8], ng = 0;
        for (int k = 0; k < rolls; k++) {
            int r = RND(10000);
            int b = -1;
            if (ng < 3) {
                bmsg_cfg tmp;   /* brawlers dropped earlier in this box count as owned */
                const bmsg_cfg *cc = c;
                if (ng) { tmp = *c; for (int i = 0; i < ng; i++) if (got[i] >= 0 && got[i] < 96) tmp.br_has[got[i]] = 1; cc = &tmp; }
                b = bmsg_box_brawler(cc, r, (uint32_t)RND(1 << 15));
            }
            if (b >= 0) { got[ng++] = b; continue; }
            coins += 8 + RND(25);                                      /* coins per roll: UNVERIFIED amount (docs/BOXES.md) */
        }
        out[n].type = coin_type; out[n].amount = coins > 0 ? coins : 8; out[n].brawler = -1; out[n].skin = -1; n++;
        short own[64][2];
        int no = owned_brawlers(c, own, 64), elig[64], ne = 0;
        for (int i = 0; i < no; i++) {
            int rr = own[i][0], pw = (rr >= 0 && rr < 96 && c->br_has[rr]) ? c->br_pow[rr] : c->power_level;
            if (pw < 8) elig[ne++] = rr;
        }
        if (ne == 0) out[0].amount *= 2;                             /* every brawler maxed: coins instead of power points */
        int pp_total = rolls * (6 + RND(14));                         /* power points: UNVERIFIED amount */
        int ns = stacks < ne ? stacks : ne;
        if (unit != 11 && unit != 12 && ng > 0) ns = 0;                /* update 170 (user rule): a Brawl Box with a brawler = coins + that brawler (2 items) */
        for (int k = 0; k < ns && n < BMSG_BOX_MAX; k++) {
            int pick = RND(ne);                                        /* distinct brawlers: take it out of the list */
            out[n].type = 6; out[n].amount = pp_total / ns + (k == 0 ? pp_total % ns : 0); out[n].brawler = elig[pick]; out[n].skin = -1; n++;
            elig[pick] = elig[--ne];
        }
        for (int i = 0; i < ng && n < BMSG_BOX_MAX; i++) { out[n].type = 1; out[n].amount = 1; out[n].brawler = got[i]; out[n].skin = -1; out[n].card = -1; n++; }   /* item 1 = unlock (boxes.md) */
        if (c->box_cards) {   /* update 174: per roll 2 % gadget (brawler level 7+), 1 % star power (level 9) - at most one card per box */
            for (int k = 0; k < rolls && n < BMSG_BOX_MAX; k++) {
                int r = RND(10000), want_sp = r >= 200 && r < 300;
                if (r >= 300) continue;
                int cand[64], ncand = 0;
                for (int i = 0; i < no && ncand < 64; i++) {
                    int rr = own[i][0];
                    if (rr < 0 || rr >= 96) continue;
                    int pw = c->br_has[rr] ? c->br_pow[rr] : c->power_level;
                    for (int j = 0; j < 2; j++) {
                        int card = want_sp ? c->sp_card[rr][j] : c->gadget_card[rr][j];
                        if (card < 0 || pw < (want_sp ? 8 : 6)) continue;
                        int owned = 0;
                        for (int q = 0; q < c->n_own_cards; q++) if (c->own_cards && c->own_cards[q] == card) owned = 1;
                        if (!owned) cand[ncand++] = card;
                    }
                }
                if (!ncand) continue;
                out[n].type = 4; out[n].amount = 1; out[n].brawler = -1; out[n].skin = -1; out[n].card = cand[RND(ncand)]; n++;
                break;
            }
        }
        return n;
    }
    /* update 64: boxType 1 = Big Box (user run 57: the shop's Big Box sent 1); 2 = Mega Box (GUESS, check the log line); 3 = Brawl Box (guess); 4 / 5 as
     * boxes.md inferred (Big / Mega) */
    int unit = bmsg_box_unit(box_type);
    int scale = unit == 11 ? 10 : unit == 12 ? 3 : 1;   /* Mega x10, Big x3, Brawl x1 */
    short own[64][2];
    int no = owned_brawlers(c, own, 64);
    out[0].type = coin_type;
    out[0].amount = scale * (20 + RND(41));  /* 20..60 coins for a small box */
    out[0].brawler = -1;
    out[0].skin = -1;
    /* update 110 (C4): power points by each brawler's own level (was: none while the global power_level config said 8 = 'all maxed') */
    int elig[64], ne = 0;
    for (int i = 0; i < no; i++) {
        int r = own[i][0], pw = (r >= 0 && r < 96 && c->br_has[r]) ? c->br_pow[r] : c->power_level;
        if (pw < 8) elig[ne++] = r;
    }
    int n = 1;
    if (ne == 0) {
        out[0].amount *= 2;                  /* everyone maxed: double coins (the PP card crashed for a maxed brawler, user run 57) */
    } else {
        int stacks = unit == 10 ? 1 : 2, first = -1;
        for (int k = 0; k < stacks && n < 3; k++) {
            int b = elig[RND(ne)];
            if (b == first && ne > 1) b = elig[(RND(ne - 1) + 1 + (int)(b == elig[0] ? 0 : 0)) % ne];
            if (b == first) continue;
            first = b;
            out[n].type = 6;
            out[n].amount = scale * (10 + RND(31)) / stacks + 5;
            out[n].brawler = b;
            out[n].skin = -1;
            n++;
        }
    }
    int gem_chance = unit == 11 ? 25 : unit == 12 ? 15 : 8;   /* bonus gems */
    if (RND(100) < gem_chance && n < 4) {
        out[n].type = 8;
        out[n].amount = 3 + RND(8);
        out[n].brawler = -1;
        out[n].skin = -1;
        n++;
    }
#undef RND
    return n;
}

static void rd_dataref(bv_rd *r, int *cls, int *row) {
    int32_t c = rd_vint(r);
    *cls = (int)c;
    *row = c ? (int)rd_vint(r) : -1;
}

int bmsg_parse_14110(const void *body, size_t len, bmsg_battle_end *o) {
    bv_rd r;
    memset(o, 0, sizeof *o);
    rd_init(&r, body, len);
    o->v50 = (int)rd_vint(&r);
    o->v54 = (int)rd_vint(&r);
    o->v58 = (int)rd_vint(&r);
    o->s0 = (int)rd_vint(&r);
    o->s1 = (int)rd_vint(&r);
    int n = (int)rd_vint(&r);
    if (r.err) return 0;
    o->n_rows = n;
    for (int i = 0; i < n && i < BMSG_BE_ROWS; i++) {
        rd_dataref(&r, &o->row[i].c1, &o->row[i].r1);
        rd_dataref(&r, &o->row[i].c2, &o->row[i].r2);
        o->row[i].v = (int)rd_vint(&r);
        o->row[i].flag = rd_byte(&r);
        o->row[i].num = (int)rd_int(&r);
        if (r.err) return 0;
    }
    return n >= 0 && n <= BMSG_BE_ROWS;
}

int bmsg_parse_14110b(const void *body, size_t len, bmsg_be2 *o) {
    bv_rd r;
    memset(o, 0, sizeof *o);
    rd_init(&r, body, len);
    o->result = (int)rd_vint(&r);
    o->my_team = (int)rd_vint(&r);
    o->v58 = (int)rd_vint(&r);
    rd_dataref(&r, &o->loc_c, &o->loc_r);
    int n = (int)rd_vint(&r);
    if (r.err || n < 0) return 0;
    o->n = n > BMSG_BE_ROWS ? BMSG_BE_ROWS : n;
    for (int i = 0; i < n; i++) {
        int bc, br, sc, sr;
        rd_dataref(&r, &bc, &br);
        rd_dataref(&r, &sc, &sr);
        int team = (int)rd_vint(&r);
        int local = rd_byte(&r) & 1;
        int32_t sl = rd_int(&r);
        char name[20] = "";
        for (int32_t k = 0; k < sl && !r.err; k++) {
            int ch = rd_byte(&r);
            if (k < (int32_t)sizeof name - 1) { name[k] = (char)ch; name[k + 1] = 0; }
        }
        if (r.err) return 0;
        if (i < BMSG_BE_ROWS) {
            o->row[i].bc = bc; o->row[i].br = br; o->row[i].sc = sc; o->row[i].sr = sr;
            o->row[i].team = team; o->row[i].local = local;
            memcpy(o->row[i].name, name, sizeof name);
        }
    }
    return 1;
}

/* bit-packed booleans as the client reads them (readBoolean libg+0x672780): consecutive bools share a byte, LSB first; any other field starts anew */
typedef struct { size_t at; int bit; } be_bits;
static void be_bool(bv_bs *s, be_bits *b, int v) {
    if (b->bit == 0) { b->at = s->len; bs_byte(s, 0); }
    if (v && !s->failed) s->data[b->at] |= (uint8_t)(1u << b->bit);
    b->bit = (b->bit + 1) & 7;
}

void bmsg_battle_log_msg(bv_bs *s, const bmsg_cfg *c, const bmsg_battle *b, int n, int labels) {
    /* update 150 (user T27 'loading forever'): +0x50 = 'full list': the handler libg+0x2ac4cc creates the singleton's list only when it is set; with
     * 0 and no list yet it drops every entry (0x2ac524 -> 0x2ac64c) and the Battle Log screen never gets its data */
    bs_bool(s, 1);                                    /* message +0x50 */
    bs_vint(s, n);
    for (int i = 0; i < n; i++) {
        int L = labels ? 100 * (i + 1) : 0;           /* labelling: entry i fields = L + field number */
        /* guesses: +0 = kind of entry (1 3v3 'tvst', 2 showdown 'deathmatch'), +0x1c / +4 / +8 / +0xc = result / rank / trophy change / - */
        /* update 154 (labelling run 178): +0 / +0x1c = 1 gave the championship header; other values the ranked header -> 0 */
        bs_vint(s, labels ? L + 1 : 0);
        /* update 167 (libg map: row age text 0x544fcc..0x54509c = entry +0x1c + seconds since the 23458 arrived): battle_log=3 sends the battle's age */
        { long age = c->battle_log == 3 && b[i].t > 0 && c->now > b[i].t ? c->now - b[i].t : 0; bs_vint(s, labels ? L + 2 : (int)age); }
        /* update 152 (crash run 175): this VInt is entry +4 = the ROW TYPE (row builder libg+0x665144 switch: 1 3v3 'battlelog_tvst_entry', 2 solo
         * showdown 'battlelog_deathmatch_entry', 3 co-op, 4 boss fight, 5 duo showdown, 6 raid; anything else = no row clip -> crash at libg+0x23f468) */
        /* update 168b: the solo showdown row (type 2, branch libg+0x665370) looks the local player up with 0x36e120 and crashed on NULL (run 199 dump
         * libg+0x6653a0); until that lookup is understood showdown battles are listed as type 1 rows unless battle_log_sd=1 */
        bs_vint(s, labels ? 1 : (b[i].kind == 2 && c->battle_log_sd ? 2 : 1));
        bs_vint(s, labels ? L + 4 : b[i].delta);
        bs_vint(s, labels ? L + 5 : b[i].len);        /* +0xc = battle length in seconds ('VS 1m 45s', run 178); update 180: recorded */
        bs_bool(s, 0);                                /* +0x10 */
        if (b[i].map >= 0) bs_dataref(s, 15, b[i].map); else bs_vint(s, 0);   /* +0x14 location */
        /* +0x18 = result text index (run 178: TID_BATTLE_LOG_TVST_RESULT_<+0x18>): 0 victory, 1 defeat, 2 draw; showdown: the rank (INFERRED) */
        bs_vint(s, labels ? L + 6 : b[i].result);
        /* update 158: +0x20 / +0x30 / +0x38 do not move the '<TIME> ago' text (runs 180-182): the client measures the age from when the list
         * arrives (wrapper time set by the 23458 handler libg+0x2ac580); the labelled run's '1m 50s' matched +0xc (battle length) + seconds on screen */
        bs_vint(s, labels ? L + 7 : 0);               /* +0x20 */
        long long ms = (long long)b[i].t * 1000LL;    /* +0x28 Long: battle time (ms, INFERRED) */
        bs_int(s, (int32_t)(ms >> 32));
        bs_int(s, (int32_t)ms);
        bs_int(s, 0);                                 /* +0x30 Long */
        bs_int(s, labels ? L + 8 : 0);
        bs_vint(s, labels ? L + 9 : 0);               /* +0x38 */
        bs_bool(s, 0);                                /* +0x3c */
        /* update 159: every player of the battle (profile battle_player lines); older battles without them: only you */
        int np = b[i].np > 0 ? b[i].np : 1;
        bs_vint(s, np);
        for (int j = 0; j < np; j++) {
            int own = b[i].np > 0 ? b[i].p[j].local : 1;
            int row = b[i].np > 0 ? b[i].p[j].row : b[i].brawler;
            int has = row >= 0 && row < 96 && c->br_has[row];
            bs_vint(s, labels ? L + 10 : 0);                                     /* player +0 */
            bs_int(s, own ? c->account_hi : 0);                                  /* player +4 id (LogicLong) */
            bs_int(s, own ? c->account_lo : 1000 + j);
            /* update 170 (libg map): player +8 = TEAM - the 3v3 row picks teammates with 0x3d7964(entry, team == local's +8) and enemies with team ==
             * (local +8 == 0) (0x4a3a0c / 0x3cbe60); update 159 wrote the team at +0, so the right side stayed empty */
            bs_vint(s, labels ? L + 11 : (b[i].np > 0 ? (b[i].p[j].team ? 1 : 0) : 0));
            bs_bool(s, own);                                                     /* player +0xc */
            bs_dataref(s, 16, row >= 0 ? row : 0);
            int tro = b[i].np > 0 ? b[i].p[j].tro : -1, pw = b[i].np > 0 ? b[i].p[j].pow : -1;
            if (tro < 0) tro = has ? c->br_tro[row] : c->trophies;
            if (pw < 0) pw = (has ? c->br_pow[row] : c->power_level) + 1;
            bs_vint(s, labels ? L + 12 : tro);                                   /* player +0x14 trophies */
            bs_vint(s, labels ? L + 13 : pw);                                    /* player +0x18 power */
            bs_vint(s, labels ? L + 14 : 0);                                     /* player +0x1c */
            bs_string(s, own ? c->player_name : (b[i].np > 0 && b[i].p[j].name && b[i].p[j].name[0] ? b[i].p[j].name : "Bot"));
            bs_vint(s, 100);
            bs_vint(s, 28000000 + c->thumbnail_row);
            bs_vint(s, 43000000);
            bs_vint(s, 0);
        }
        bs_vint(s, labels ? L + 15 : 0);              /* +0x40 */
        bs_bool(s, 0);                                /* +0x44 */
        bs_vint(s, labels ? L + 16 : -1);             /* +0x48 = challenge id: 0 = the Championship Challenge header (run 179); -1 = none -> RANKED (row code libg+0x665b24) */
        bs_bool(s, 0);                                /* +0x4c */
        bs_vint(s, labels ? L + 17 : 0);              /* +0x50 */
    }
}

void bmsg_profile_msg(bv_bs *s, const bmsg_cfg *c, int id_hi, int id_lo, const char *name, int wins) {
    int rows[64];
    int n = bmsg_owned_brawler_rows(c, rows, 64);
    int best_acc = c->account_trophies;
    bs_vint(s, id_hi);
    bs_vint(s, id_lo);
    bs_vint(s, 0);                                  /* profile +8 DataRef: null */
    bs_vint(s, n);
    for (int i = 0; i < n; i++) {
        int r = rows[i], has = r >= 0 && r < 96 && c->br_has[r];
        int tro = has ? c->br_tro[r] : c->trophies, best = has ? c->br_best[r] : tro, pw = (has ? c->br_pow[r] : c->power_level) + 1;
        if (best < tro) best = tro;
        bs_dataref(s, 16, r);
        bs_vint(s, 0);                              /* skin: null (default look) */
        bs_vint(s, tro);
        bs_vint(s, best);
        bs_vint(s, pw);
    }
    if (c->profile_reply == 2) {                    /* update 115: labelling, stat i = 1000 + i for i = 1..24 */
        bs_vint(s, 24);
        for (int i = 1; i <= 24; i++) {
            bs_vint(s, i);
            bs_vint(s, 1000 + i);
        }
    } else {
        /* run 133 labelling: 1 3v3 wins, 2 experience (level), 4 highest trophies, 8 solo wins, 11 duo wins, 14 power play rank, 15 challenge wins,
         * 9 / 12 / 13 robo rumble / boss fight / rampage level (shown as TID_RAID_BOSS_DIFFICULTY_<n>: left out) */
        /* update 152 (req. 3): stat 2 = experience points (the profile's level bar), from the profile */
        static const int idx[] = {1, 3, 4, 5, 8, 11, 15, 2};
        int val[8] = {wins, c->account_trophies, best_acc, n, 0, 0, 0, c->exp_total};
        bs_vint(s, 8);
        for (int i = 0; i < 8; i++) {
            bs_vint(s, idx[i]);
            bs_vint(s, val[i]);
        }
    }
    bs_string(s, name ? name : c->player_name);
    bs_vint(s, 100);
    bs_vint(s, 28000000 + c->thumbnail_row);
    bs_vint(s, 43000000 + c->name_color_row);   /* update 182 */
    bs_vint(s, 0);
    bs_bool(s, 0);                                  /* no club */
    bs_vint(s, 0);                                  /* message +0x58 DataRef: null */
}

void bmsg_battle_end_msg(bv_bs *s, int kind, int result, int trophy_delta, const bmsg_be2 *be, int acc_hi, int acc_lo) {
    be_bits b = {0, 0};
    /* 17 VInts +0x50..+0x90: kind, result, tokens, trophy change, the rest 0.
     * run 154 labelling (100 * index; Heist defeat screen): +0x58 battle tokens (defeat row), +0x64 double-token event tokens, +0x68 token doubler
     * tokens (TOTAL = +0x58 + +0x64 + +0x68), +0x6c token doubler remaining (remaining-doubler row), +0x8c coin-shower event coins, +0x90 UNDERDOG
     * trophies (also subtracted in the shown trophy change). +0x60, +0x70..+0x88 not visible on page 1 (experience bar?). Huge markers made the screen
     * run at 3-5 fps (count-up animations). */
    bs_vint(s, kind);
    bs_vint(s, result);
    bs_vint(s, be ? be->tokens : 0);   /* +0x58 tokens (update 117) */
    bs_vint(s, trophy_delta);
    for (int i = 4; i < 17; i++) bs_vint(s, be && be->labels ? (be->labels == 2 ? i : 100 * i) : 0);   /* update 132 (A11) markers; update 150: labels=2 -> 4..16 */
    /* 7 bools +0x94..+0x9a: +0x98 (index 4) = show the trophy row */
    for (int i = 0; i < 7; i++) be_bool(s, &b, i == 4);
    b.bit = 0;
    bs_vint(s, -1);          /* +0x9c */
    be_bool(s, &b, 0);       /* +0xa0 */
    b.bit = 0;
    int n = be ? be->n : 0;
    bs_vint(s, n);
    for (int i = 0; i < n; i++) {
        /* update 100 (run 118 screenshots): rows with +0 = 0 filled the first page's three slots -> +0 = OPPONENT; our team gets 0 */
        /* update 101 (A10, static: result screen ctor loop libg+0x3df888): +0 != 0 -> the row goes into the FEATURED slot (last slot, the local player);
         * +0 == 0 and +1 == 0 -> the next team slot from the end; +1 != 0 -> not shown. So +0 = local player, +1 = opponent. */
        int opp = be->row[i].team != be->my_team;
        be_bool(s, &b, be->row[i].local);   /* +0: the local player */
        be_bool(s, &b, opp);                /* +1: opponent (not shown) */
        be_bool(s, &b, 0);      /* +2 */
        b.bit = 0;
        bs_dataref(s, be->row[i].bc, be->row[i].br);
        bs_dataref(s, be->row[i].sc, be->row[i].sr);
        /* update 100: +0xc trophies, +0x10 highest trophies (rank), +0x14 power level, like our 24124 member (run 118: zeros = rank badge cycling) */
        bs_vint(s, be->row[i].tro);
        bs_vint(s, be->row[i].best);
        bs_vint(s, be->row[i].pow);
        be_bool(s, &b, 1);      /* update 97: the row Long (player id) must exist: the result screen reads row+0x18 (run 112 crash at libg+0x587b18) */
        b.bit = 0;
        bs_int(s, be->row[i].local ? acc_hi : 0);
        bs_int(s, be->row[i].local ? acc_lo : 1000 + i);
        bs_string(s, be->row[i].name[0] ? be->row[i].name : "Bot");
        /* update 99: PlayerDisplayData like our 24124 member (100, thumbnail global id 28000000 + row, name colour 43000000, 0); zeros = data id 0 =
         * nothing found (run 116 crashed ~3 s after the 23456 with zeros here; no usable dump) */
        bs_vint(s, 100);
        bs_vint(s, 28000000);
        bs_vint(s, 43000000);
        bs_vint(s, 0);
    }
    if (be && be->labels == 2) {   /* update 150 (req. 3 / 16 labelling): +0xa8 = list of VInt pairs (decoder libg+0x147f50) -> (k, 30 + k) */
        bs_vint(s, 6);
        for (int k = 0; k < 6; k++) { bs_vint(s, k); bs_vint(s, 30 + k); }
    } else if (be && be->xp > 0) {   /* update 151: pair type 0 = the result row ('Victory' / 'Defeat' / 'Draw' + amount) under EXPERIENCE */
        bs_vint(s, 1);
        bs_vint(s, 0);
        bs_vint(s, be->xp);
    } else bs_vint(s, 0);    /* +0xa8 array */
    bs_vint(s, 0);           /* +0xac array */
    if (be && !be->labels && (be->rank_on || be->xp > 0)) {   /* update 162: +0xb0 = VInt triples (decoder libg+0x1b0190); key 1 = brawler rank track
                                                                  * (badge clip rank_trophies, libg+0x8c814 runs only when found: 0x460cd8), key 5 = player level */
        bs_vint(s, (be->rank_on ? 1 : 0) + (be->xp > 0 ? 1 : 0));
        if (be->rank_on) { bs_vint(s, 1); bs_vint(s, be->tro_before); bs_vint(s, be->best_before > be->tro_before ? be->best_before : be->tro_before); }
        if (be->xp > 0) { bs_vint(s, 5); bs_vint(s, be->xp_before); bs_vint(s, be->xp_before); }
    } else bs_vint(s, 0);    /* +0xb0 array */
    bs_vint(s, 0);           /* +0xb4 DataRef null */
    be_bool(s, &b, 0);       /* +0xb8 object absent */
    be_bool(s, &b, 0);       /* +0xbc object absent */
}

int bmsg_parse_14362(const void *body, size_t len, int *event_id, int *slot) {
    bv_rd r;
    rd_init(&r, body, len);
    int32_t id = rd_vint(&r);
    int32_t sl = rd_vint(&r);
    if (r.err) return 0;
    if (event_id) *event_id = (int)id;
    if (slot) *slot = (int)sl;
    return 1;
}

int bmsg_map_for_slot(const bmsg_cfg *c, int slot) {
    int ev = c->events < 0 ? 0 : (c->events > 8 ? 8 : c->events);
    for (int i = 0; i < ev; i++) {
        int s = c->event_slot[i] > 0 ? c->event_slot[i] : 1 + i;   /* the same rule as the event writer */
        if (s == slot) return ((i == 0 && !c->ev_maps_exact) || c->event_map[i] < 0) ? c->map_row : c->event_map[i];
    }
    return -1;
}

int bmsg_parse_14363(const void *body, size_t len, int fallback) {
    bv_rd r;
    rd_init(&r, body, len);
    (void)rd_vint(&r);
    int32_t v = rd_vint(&r);
    if (r.err || v < 0) return fallback;
    return (int)v;
}
