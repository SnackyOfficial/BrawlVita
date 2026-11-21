/* Brawl Pass rewards (ISSUES D2): the game's own csv_logic/milestones.csv + skins.csv (read from the user's assets at boot) turned into the items of
 * a 24111 / command 203 delivery when the client claims a tier (client command 517: fields type, ?, season, tier; run 60: 9, 0, 2, 0 = Goal_9_2_0 =
 * the PocoStarr skin). Portable C11, host-tested (tests/host_passdata_test.c). */
#ifndef BV_PASSDATA_H
#define BV_PASSDATA_H

#include <stddef.h>

#include "bmsg.h"

#define BV_PASS_MAX 768   /* Type 9 (free) and 10 (premium) rows kept; update 183: + Type 6 Trophy Road (348 + 165 > 512) */
#define BV_PASS_SKINS 1024

typedef struct bv_pass_goal {
    int type, season, index;     /* Goal_<type>_<season>_<index> */
    int r1_type, r1_count;       /* PrimaryLvlUpRewardType / Count */
    char r1_data[40];            /* PrimaryLvlUpRewardData (e.g. a skin name) */
    int r2_type, r2_count;       /* Secondary... (-1 = none) */
    char r2_data[40];
} bv_pass_goal;

typedef struct bv_passdata {
    int n;
    bv_pass_goal g[BV_PASS_MAX];
    int n_skins;
    char skins[BV_PASS_SKINS][32]; /* skins.csv Name of row i (row = line index after the two header lines) */
    short skin_cost[BV_PASS_SKINS];    /* cycle 4: CostGems (0 = not sold for gems) */
    signed char skin_obtain[BV_PASS_SKINS]; /* ObtainType (0 = none = a normal shop skin) */
    int new_types;                     /* cycle 6 experiment pass_new_types: 16 gems -> item 8, 6/10/14 boxes -> item 1000 + reward type (box marker) */
    short skin_brawler[BV_PASS_SKINS];        /* update 184: characters row of the skin (skin_confs.csv Character), -1 = unknown / not a brawler */
    signed char skin_campaign[BV_PASS_SKINS]; /* Campaign column (1 Christmas, 2 Lunar New Year, 3 Halloween, ...) */
    unsigned char skin_shop[BV_PASS_SKINS];   /* 1 = has a ShopTID and is not a default skin */
    int skin_coins[BV_PASS_SKINS], skin_lt[BV_PASS_SKINS]; /* CostCoins / CostLegendaryTrophies */
    char skin_conf[BV_PASS_SKINS][32];        /* Conf column (skin_confs.csv Name) */
    int n_brawlers;                    /* update 183: characters.csv Name of each named row (reward type 3 = brawler unlock by name) */
    char brawlers[128][32];
} bv_passdata;

/* milestones.csv text -> Type 9 / 10 rows; returns the number kept (columns found by header name; -1 if the header is not understood) */
int bv_pass_load_milestones(bv_passdata *p, const char *csv);
/* update 91 (trophy road): the highest Type 6 Index with ProgressStart + Progress <= trophies; -1 if none, -2 if the header is not understood */
int bv_trophy_road_last(const char *csv, int trophies);
/* skins.csv text -> row names; returns the number of rows */
int bv_pass_load_skins(bv_passdata *p, const char *csv);
int bv_pass_skin_row(const bv_passdata *p, const char *name);  /* -1 if unknown */
/* cycle 4: the skin offer of `day` (days since 1970): a gem-priced shop skin not in owned[]; price after the discount. -1 if none. */
int bv_daily_skin(const bv_passdata *p, int day, const int *owned, int n_owned, int *price, int *discount_pct);
/* cycle 5: the special skin of the day (rows[0], discounted price) + up to max-1 further daily skins at full price, all different, none owned.
 * Returns how many were found. */
int bv_daily_skins(const bv_passdata *p, int day, const int *owned, int n_owned, int *rows, int *prices, int max);
const bv_pass_goal *bv_pass_find(const bv_passdata *p, int type, int season, int index);
/* update 184: skin_confs.csv text -> skin_brawler[] (needs skins + brawler names loaded); returns the number of skins mapped, -1 if no Character column */
int bv_pass_load_skin_confs(bv_passdata *p, const char *csv);
/* 0 = not sold, 1 = sold all year (also exclusive skins), 2 = seasonal (campaign 1 / 2 / 3) */
int bv_skin_kind(const bv_passdata *p, int row);
/* seasonal window of the skin's campaign for date mmdd (month * 100 + day): Halloween 1024-1107, Christmas 1218-0101, Lunar New Year 0121-0210 */
int bv_skin_in_season(const bv_passdata *p, int row, int mmdd);
/* full shop price: CostGems, else CostCoins (*in_coins = 1), else from CostLegendaryTrophies (30 / 80 / 150 / 300 gems), else 80 gems */
int bv_skin_shop_price(const bv_passdata *p, int row, int *in_coins);
/* skins for sale today (day-seeded order), only for brawlers with have_br[row] != 0 and not owned: seasonal == 0 -> kind 1, 1 -> kind 2 in season, 2 -> every kind 2 (test) */
int bv_shop_skin_list(const bv_passdata *p, int day, int mmdd, const int *owned, int n_owned, const unsigned char *have_br, int seasonal, int *rows, int max);

/* Items for a claim. Known reward types: 24 / 4 skin -> item 9 (DataRef(29,row)), 1 coins -> item coin_type, 12 power points -> item 6 for
 * `brawler_row` (coins instead when `maxed`). Anything else -> 1 coin (so the claim completes) and its type is reported in *unmapped (first one).
 * Returns the number of items (0 = no such tier). */
int bv_pass_items(const bv_passdata *p, int type, int season, int index, int coin_type, int brawler_row, int maxed, bmsg_box_item *out, int max,
                  int *unmapped);

#endif
