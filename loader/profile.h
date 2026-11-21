/* The offline player's progress (plan D.3): what OwnHomeData is built from at login and what battle results / accepted commands change.
 * Stored as a small text file of `key=value` lines (ux0:data/BrawlVita/profile.txt), so the user can read or edit it in VitaShell.
 * Portable C11, no console headers: host-tested (tests/host_profile_test.c). NOT yet used by the message builders.
 */
#ifndef BV_PROFILE_H
#define BV_PROFILE_H
#include <stddef.h>
#include <stdint.h>

#define BV_PROFILE_MAX_BRAWLERS 96 /* rows of the characters table we keep (v29 has far fewer playable brawlers) */
#define BV_PROFILE_MAX_SKINS 256   /* owned (bought / won) skins: rows of the skins table (29); default skins need no entry */
#define BV_PROFILE_MAX_BOUGHT 32   /* offers bought on the current offer day */
#define BV_PROFILE_MAX_CLAIMED 512 /* claimed Brawl Pass tiers */

typedef struct bv_brawler {
    int row;          /* row of the characters table (data table 16) */
    int trophies;
    int best_trophies;
    int power;        /* power level - 1 (0 = level 1) */
    int power_points;
} bv_brawler;

typedef struct bv_profile {
    int version;
    char name[32];
    int coins, gems, star_points;
    int selected_brawler; /* row of the characters table */
    int selected_map;     /* row of the locations table */
    int selected_slot;    /* update 144 (req. 22): event slot the player selected last (0 = none: Gem Grab) */
    int unlock_all;       /* 1 = every playable brawler is listed as owned (rows from the game's tables at login) */
    int battles, wins;
    int trophy_delta;
    int tokens;           /* update 117 (E7): battle tokens available (key bar); -1 = never set (= full) */
    long token_time;      /* update 117: unix time the current refill period started (0 = full) */
    int pass_tokens;      /* update 117: Brawl Pass tokens earned in battles */
    int exp;              /* update 151: experience points earned in battles (results page EXPERIENCE row) */
    int n_hist;           /* update 140 (G7): battles kept for the battle log (ring of 10, newest at (n_hist - 1) % 10) */
    struct { long t; int kind, result, delta, map, brawler, len; int np; struct { short row; signed char team, local; char name[16]; } p[10]; } hist[10];   /* update 159: players */
    int n_ev_claimed, ev_claimed[16]; /* update 131 (E4): rotation event ids whose new-event tokens were paid (ring of 16) */
    int n_qbr; struct { unsigned key; int row; } qbr[16]; /* update 144 (req. 24): quest seed -> assigned brawler row (ring of 16) */
    long season_start;    /* update 103 (D4): unix time the current 70-day season started (0 = not yet) */     /* update 96: sum of battle trophy changes (added to the account trophies at boot) */
    int n;                /* owned brawlers in b[] */
    bv_brawler b[BV_PROFILE_MAX_BRAWLERS];
    int n_skins;          /* cycle 1 (stream G): owned skins -> 24101 home +0x3c list */
    int skins[BV_PROFILE_MAX_SKINS];
    int offer_day;        /* day number (days since 1970) the bought[] list belongs to; another day clears it */
    int n_bought;
    int bought[BV_PROFILE_MAX_BOUGHT]; /* offer ids bought on offer_day -> offer record +0x20 */
    int n_claimed;        /* cycle 4: claimed pass tiers, id = type * 100000 + season * 1000 + tier (type 9 free, 10 premium) */
    int claimed[BV_PROFILE_MAX_CLAIMED];
    int thumbnail, name_color;   /* update 182: player icon (table 28) / name colour (table 43) rows, -1 = default */
    short skinsel[96];    /* update 182: selected skin row per brawler row, -1 = default */
    int n_cards;          /* update 174: owned gadget / star power cards (cards table rows) */
    int cards[192];   /* update 188: 64 -> 192 (every gadget and star power, profile=max) */
} bv_profile;

void bv_profile_default(bv_profile *p);                    /* a fresh account: brawler row 0 owned and selected, map row 7 */
int bv_profile_total_trophies(const bv_profile *p);
bv_brawler *bv_profile_find(bv_profile *p, int row);       /* NULL if not owned */
bv_brawler *bv_profile_unlock(bv_profile *p, int row);     /* owned afterwards (NULL only if the list is full or row < 0) */
int bv_profile_own_card(bv_profile *p, int card_row);      /* update 174 */
/* Applies a battle result to the selected brawler: trophies change by `delta` (never below 0), best trophies follow, battles / wins count. */
void bv_profile_battle(bv_profile *p, int delta, int won);
/* update 117 (E7): v29 token pool, cap BV_TOKEN_CAP, +BV_TOKEN_STEP every BV_TOKEN_PERIOD s */
#define BV_TOKEN_CAP 200
#define BV_TOKEN_STEP 20
#define BV_TOKEN_PERIOD 8640
/* update 130 (D6): trophy league season end for one brawler: new trophy count (unchanged at <= 500), *star_points = reward */
int bv_trophy_reset(int trophies, int *star_points);
int bv_profile_event_claimed(const bv_profile *p, int event_id);
void bv_profile_claim_event(bv_profile *p, int event_id);   /* update 131 (E4) */
/* update 144 (req. 24): the brawler assigned to the quest with this seed: the saved one, else one picked at random from `owned` (n rows, in a
 * stable order) and saved. Returns the row, or -1 when n == 0. *made = 1 when a new assignment was stored. */
int bv_profile_quest_brawler(bv_profile *p, unsigned key, const int *owned, int n, int *made);
void bv_profile_add_battle(bv_profile *p, long t, int kind, int result, int delta, int map, int brawler);
/* update 159: one player of the newest battle (row = characters row, team, local 1 = you); max 10 */
void bv_profile_battle_player(bv_profile *p, int row, int team, int local, const char *name);   /* update 140 (G7) */
/* applies it to every owned brawler; returns the star points paid, *lost = trophies removed in total */
int bv_profile_league_end(bv_profile *p, int *lost);
int bv_tokens_refill(bv_profile *p, long now);           /* adds the refills due; returns the seconds to the next one (0 = full) */
int bv_tokens_spend(bv_profile *p, int want, long now);  /* refills, then takes up to `want`; returns the tokens earned */

/* Text form. parse returns the number of lines it did not understand (they are ignored); unknown keys keep the file usable by older builds. */
size_t bv_profile_format(const bv_profile *p, char *out, size_t cap); /* bytes written (without the 0), or the size needed if cap is too small */
int bv_profile_parse(bv_profile *p, const char *text);                /* starts from bv_profile_default */
/* cycle 1: skins and offers */
int bv_profile_has_skin(const bv_profile *p, int skin_row);
int bv_profile_own_skin(bv_profile *p, int skin_row);                 /* 1 = owned afterwards (0 only if full or row < 0) */
int bv_profile_is_bought(const bv_profile *p, int day, int offer_id); /* 0 when `day` is not the stored offer day */
int bv_profile_mark_bought(bv_profile *p, int day, int offer_id);     /* a new day clears the list first; 1 = stored */
/* cycle 4: Brawl Pass tiers */
int bv_profile_is_claimed(const bv_profile *p, int type, int season, int tier);
int bv_profile_mark_claimed(bv_profile *p, int type, int season, int tier); /* 1 = stored (or already there) */
/* the claimed bits of one track (type 9 / 10) of `season` as 4 words, bit (tier % 32) of word (tier / 32); returns the number of claimed tiers */
int bv_profile_claimed_bits(const bv_profile *p, int type, int season, unsigned bits[4]);
#endif
