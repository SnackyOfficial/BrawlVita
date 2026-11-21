/* Server -> client messages of Build B's offline path, written in our own words from the codec layout
 * (docs/offline_server_spec.md sections 3, 4 and 8; docs/message_field_sequences.txt). Values are ours: a fresh account that owns
 * one brawler. The field *order and types* are what the client's decoders read, so they follow the layout exactly.
 *
 * Portable C11 (no console headers): unit-tested on the PC (tests/host_bmsg_test.c). UNVERIFIED against the real client.
 */
#ifndef BV_BMSG_H
#define BV_BMSG_H

#include "bytestream.h"

#define BMSG_LOGIN_OK 20104
#define BMSG_OWN_HOME_DATA 24101
#define BMSG_AVATAR_UPDATE 24124
#define BMSG_ACK 24125
/* Matchmaking (update 32). Layouts read from libg's decode functions (static, 2026-11-11): 20405 class ctor libg+0x8ff30, vtable libg+0x8e37b4,
 * decode libg+0x23cd3c = five readInt (libg+0x2fa918, 4 bytes big-endian) + one readBoolean (libg+0x672780); type slot returns 0x4fb5.
 * 20406 class ctor libg+0x3acfbc, vtable libg+0x8e51a4, decode = the empty base decode libg+0x4f3478; type slot returns 0x4fb6. */
#define BMSG_MATCHMAKE_STATUS 20405
#define BMSG_MATCHMAKE_CANCELLED 20406

#define BMSG_MAX_OFFERS 31   /* update 184: special skins + skin catalogue; 184b: more offers a day (offer_bought_mask bits 0..30) */
/* cycle 5: one generated shop offer (single item). type = OfferType (4 skin: row = skins row; 8 power points: row = brawler row) */
typedef struct bmsg_offer { int type, amount, row, kind, price, old_price, promo; const char *title; int bonus; } bmsg_offer;   /* update 184: bonus != 0 = a Mega Box item before the skin (special offer; 184b: was 100 coins) */ /* title: record +0x34 (update 101) */ /* promo = +0x2c (big special-offer tile; never for skins) */

typedef struct bmsg_cfg {
    int32_t account_hi, account_lo;   /* account / home id (64-bit, written as two Ints) */
    const char *pass_token;           /* any non-empty string */
    const char *country;              /* two letters */
    const char *player_name;
    int brawler_row;                  /* row of the characters table the player owns and has selected */
    int map_row;                      /* row of the locations table used for the one menu event and as the selected map */
    int events;                       /* number of menu events (0..8): event 0 uses map_row, event i > 0 uses event_map[i] */
    int event_map[8];                 /* rows of the locations table (data table 15) for events 1..7 (update 33) */
    int event_slot[8];                /* event slot ids (TID_EVENT_SLOT_NAME_<id>: 1 gem grab, 2 showdown, 3 daily, 4 team, 6 team 2, 7 special, 8 solo, 9 power
                                         play; there is no 5) (update 34) */
    int unlock_all;                   /* 1 = every enabled brawler is owned (its unlock card and its four per-brawler entries); 0 = only brawler_row (update 34) */
    int trophies;                     /* trophies (and highest trophies) of the selected brawler (update 34) */
    int account_trophies;             /* the account's trophies and highest trophies: trophy road, event and brawler unlocks (update 35) */
    int exp_level, exp_points;        /* 6th / 7th value of 24101 (the mod: 200, 99999); meaning inferred, not verified (update 35) */
    int power_level;                  /* power level index of every owned brawler (0 = level 1, 8 = level 9) (update 35) */
    int home_flags;                   /* byte of three packed Booleans after the timers (4 = as the mod; bit 2 = shop-disabled candidate) (update 38) */
    int brawl_pass;                   /* 1 = one Brawl Pass season entry (update 38) */
    int pass_season, pass_tokens, pass_premium; /* that entry: season index, pass tokens, premium bought */
    int shop_disabled;                /* config switch key 5 (1 = the shop shows "temporarily disabled"; the mod sent 1) (update 42) */
    int tutorial_steps;               /* length of the 0..N-1 list near the start of 24101 (20 = as the mod; update 46 experiment) */
    int box_coin_type;                /* item type used for coins in box deliveries (7; 3 and 2 are the other candidates) (update 50) */
    int box_unit_mode;                /* delivery unit type: 0 = 100 + box type (INFERRED best), 1 = box type, 2 = 101 (update 50) */
    int offer_test;                   /* number of labelling shop offers (0 = none, at most 16) (update 43/44) */
    int offer_type[16];               /* item type (+0x30) of each labelling offer */
    int n_extra_keys;                 /* extra config switches {key, value} (config_keys=K:V,...; keys 1/37/5 override the built-in ones) (update 42) */
    int extra_key[8][2];
    int thumbnail_row;                /* row of the player thumbnail table */
    int name_color_row;               /* update 182: row of the name colour table (43), 0 = default */
    int gems, coins;                  /* update 56: avatar resource list 0 rows 0 (Diamonds) / 1 (Gold); -1 = as the mod (no gems entry, coins 0) */
    int shop_offers;                  /* update 56: number of real shop offers at home +0x9c (0 = none, as the mod; max 2: skin + power points) */
    int offer_cur;                    /* update 56: pricing kind of those offers (+0x10): 1 or 3 = a resource (gems?), 0 = tokens, 2 = free */
    int sel_skin_row; /* cycle 6: skin of the team member (-1 = none) */
    int member_row;   /* cycle 6 (E2): brawler shown as the team member (-1 = the selected brawler) */
    int member_view_on; /* config member_view (0) */
    /* cycle 6 (G2, profile_brawlers): per-brawler stats by characters row (br_has[row] = 1 when set) */
    short br_tro[96], br_best[96];
    short br_pp[96];   /* update 101 (B10): power points toward the next level, 24101 list 2 */
    signed char br_pow[96];
    unsigned char br_has[96];
    int team_m10, team_m14, team_m18, team_m1c; /* cycle 5 team_member=A,B,C,D: member +0x10 (trophies, run 77) / +0x14 / +0x18 / +0x1c (0,0,1,3 = original) */
    int team_f0, team_f8; /* cycle 5 E1 experiment team_fields=A,B: team +0 / +8 in 24124 (1,1 = the original bytes) */
    int home_on_14109;
    int profile_reply;  /* update 114 (G5): answer 14113 with 24113 (config profile_reply, default 0) */
    int n_limits, limits[13]; /* update 115 (E7): config limits= overrides the 13 'limits' VInts of 24101 (n_limits 0 = defaults) */
    int play_tokens;  /* update 117 (E7): config play_tokens (0) */
    int star_points;  /* update 130 (D6): avatar resource row 10 (0 = the old 1) */
    int battle_end_labels; /* update 132 (A11): config battle_end_labels */
    int offer_title_slot;  /* update 136 (B11): config offer_title_slot */
    int battle_log;        /* update 140 (G7): config battle_log (1 guesses, 2 labelling) */
    long now;              /* update 167: unix time when the message is built (battle_log=3 age) */
    int battle_log_sd;     /* update 168b: 1 = showdown battles as their own row type 2 (crashed run 199) */
    int box_cards;         /* update 174: gadgets / star powers from boxes */
    short gadget_card[96][2], sp_card[96][2];   /* update 174: card rows per brawler (-1 none), from cards.csv */
    const int *own_cards; int n_own_cards;      /* update 174: owned gadget / star power card rows (profile) */
    int new_event_tokens, ev_unclaimed[8]; /* update 131 (E4): tokens per new event; per entry 1 = not paid yet (entry +0x10 experiment) */
    int tok_cfg[4]; /* update 116 (E7): config object +0xa8 / +0xac (key bar maximum) / +0xb0 / +0xb4 (config token_cfg=a,b,c,d; default 0) */
    int home_a4, home_a8; /* update 114 (E7): home fields +0xa4 / +0xa8 after the shop offers (key bar candidates; config home_tokens=A,B) */
    int quests_on; /* config quests (cycle 7 D3 labelling experiment) */
    int home_slot; /* update 146 (T14): event slot of the selected event, sent after the map in 24124 (was always 1 = Gem Grab) */
    int home_map, ev_maps_exact; /* update 144 (req. 22): map row of the selected event (-1 = map_row); 1 = every entry uses its own event_map */
    int exp_total; /* update 152: experience points (profile exp) for the 24113 stat 2 */
    int quest_br[8];
    int box_drops; /* update 144 (req. 10): 1 = 2020 box rules (brawler drops by rarity, see bmsg_box_roll) */
    signed char br_rar[96]; /* update 144: rarity of each brawler (characters row) from the user's cards.csv: 0 common (trophy road), 1 rare, 2 super rare,
                            * 3 epic, 4 mythic (mega_epic), 5 legendary; -1 unknown */ /* update 144 (req. 24): brawler row of quest k (-1 = any brawler), chosen once per quest and kept in the profile */
    int season_left; /* update 103 (D4): seconds left in the season (0 = the old fixed 86400) */
    int upcoming_events, next_map[8], next_id[8]; /* update 123 (E5): upcoming list (config upcoming_events) with the next rotation map per slot */
    int event_id[8], event_left[8]; /* update 109 (E3): per event entry: id (0 = the old 1) and seconds left (0 = the old 99999) */
    int quest_day, quest_refresh_s, season_tt, quest_batch_s; /* update 108: seconds to the next Tue/Thu 08:00 UTC */ /* update 104 (D3): quest day (08:00 UTC), seconds to the next one, Tue/Thu count since the season start */
    int battle_end_msg, battle_trophies; /* update 96 (A6) experiments */
    int battle_win, battle_draw, battle_lose; /* trophy changes for 3v3 (8 / 0 / -4) */ /* battle day (update 89) experiment: 14109 (leave the training cave) gets 24101 + 24124 */
    int ack24125; /* cycle 4 E1 experiment: 1 = answer periodic client messages with 24125 (old); 0 = not (24125 = 'team left', clears the team) */
    int mode_push_map, mode_push_after; /* cycle 1 experiment: after the N-th 10108 send a 24124 with map row M (-1 = off) */
    int pass_claim_reply;             /* update 65 experiment: 1 = answer command 517 with a minimal 24111/203 (1 coin) */
    int offer_unit;                   /* update 64: delivery unit type for an offer purchase (unknown; default 100) */
    int box_pp_random;                /* update 63: 1 = power points for a random owned brawler, 0 = the selected brawler (run 56 crashed on row 37) */
    int res_markers;                  /* update 62: 1 = resource list 0 gets rows 0..9 with 1000+row (find the coin row); 0 = normal */
    int skin_offer_kind;              /* update 62: pricing kind of the skin offer (0 = avatar+0x60 = GEMS, run 56) */
    const int *own_skins;             /* cycle 4: owned skin rows from the profile (used when non-NULL; else owned_skins below) */
    int n_own_skins;
    bmsg_offer offers[BMSG_MAX_OFFERS]; int n_offers; /* cycle 5 shop_layout: when n_offers > 0 it replaces the 2 fixed offers */
    int skin_offer_row, skin_offer_price; /* cycle 4: the daily skin offer (default: row 2 Rockstar Colt, 15 gems) */
    int offer_bought_mask;            /* cycle 4: bit i = shop offer i already bought today (+0x20) */
    int pass_claimed_bits;            /* cycle 4 EXPERIMENT: 1 = write the claimed-tier bits of the season entry */
    unsigned pass_bits[2][4];         /* [0] free (type 9), [1] premium (type 10) of pass_season */
    int owned_skins;                  /* update 61: entries of the owned-skins list home +0x3c (-1 = all of table 29 as the mod; 0 = only default skins) */
    int avatar_tail[12];              /* update 57: the 12 avatar VInts after the 8 commodity lists (the mod: 0,0,0,1,0,0,0,0,0,0,0,2) */
} bmsg_cfg;
/* update 144 (req. 24): quests sent with quests=3 (2 daily + season ones) and the seed of quest k (same value the builder rolls type / mode from;
 * it changes only when the quest itself refreshes: daily = 08:00 UTC day, season = Tue/Thu batch) */
int bmsg_quest_count(const bmsg_cfg *c);
int bmsg_home_map(const bmsg_cfg *c); /* update 144: home_map if set, else map_row */
uint32_t bmsg_quest_seed(const bmsg_cfg *c, int k);
int bmsg_quest_wants_brawler(uint32_t seed); /* 1 = this quest names one brawler */

/* Access to the game's own data tables (the number of rows). Returns < 0 if unknown. */
typedef struct bmsg_game {
    int (*table_rows)(void *ctx, int table);
    void *ctx;
} bmsg_game;

void bmsg_default_cfg(bmsg_cfg *c);

void bmsg_login_ok(bv_bs *s, const bmsg_cfg *c);                           /* 20104 */
void bmsg_own_home_data(bv_bs *s, const bmsg_cfg *c, const bmsg_game *g);  /* 24101 */
void bmsg_avatar_update(bv_bs *s, const bmsg_cfg *c, int selected_map_row);/* 24124 */
void bmsg_ack(bv_bs *s);                                                   /* 24125: four zero bytes */
/* 20405: five Ints and a Boolean. What each Int means is not known yet; we send (seconds 0, players found 1, players needed 6, 0, 0) and true. */
void bmsg_matchmake_status(bv_bs *s);
/* 20406: no body (the caller sends an empty message) */

/* Request 14363: two VInts (the first is ignored); returns the second, or `fallback` if it is negative or the body is short. */
/* cycle 1: 14362 = event card tap (libg class 0x92eec, mode_switch.md): VInt event id (entry +0x00), VInt slot (entry +0x04). 1 = parsed */
/* cycle 5 (ISSUES A6): battle end 14110 */
#define BMSG_BE_ROWS 12
typedef struct bmsg_battle_end {
    int v50, v54, v58;        /* first three VInts (meaning not known yet: result / rank / mode INFERRED) */
    int s0, s1;               /* sub-object libg+0x4f6228: two VInts */
    int n_rows;               /* rows announced */
    struct { int c1, r1, c2, r2, v, flag, num; } row[BMSG_BE_ROWS]; /* DataRef, DataRef, VInt, Bool, Int */
} bmsg_battle_end;
/* 1 = the whole body decoded, 0 = it ran out early (fields read so far are kept) */
/* cycle 5: rows of the owned brawlers (selected first), for generated offers */
int bmsg_owned_brawler_rows(const bmsg_cfg *c, int *rows, int max);
/* cycle 6: delivery unit of a bought table offer: the box model for box offers (10 Brawl, 11 Mega, 12 Big), else `dflt` */
int bmsg_offer_unit(int index, const bmsg_cfg *c, int dflt);
/* cycle 6: brawler / skin selection commands in a 14102 body (525 / 522 brawler DataRef(16,row), 506 skin DataRef(29,row)); walks the
 * commands while their payload is known. Returns 1 if anything was found (rows stay -1 if not). */
int bmsg_parse_selection(const void *body, size_t len, int *brawler_row, int *skin_row);
/* cycle 6: the brawler row of a 522 (browsing) command in a 14102 body, -1 if none */
int bmsg_parse_view(const void *body, size_t len);
int bmsg_parse_14110(const void *body, size_t len, bmsg_battle_end *out);
/* update 102: total power points of a brawler at power index p (0 = level 1) with `progress` toward the next level */
int bmsg_pp_total(int power_index, int progress);
/* update 103 (B13/B14): stable id of a generated offer (bought marks survive a changed offer order); >= 1000000, never a legacy index */
int bmsg_offer_id(const bmsg_offer *o);
/* update 96: the real 14110 layout (run 110 hex): VInt result, VInt my team, VInt v58, DataRef location, VInt n, n x (DataRef brawler, DataRef skin,
 * VInt team, Bool local, String name). 1 = fully decoded. */
typedef struct bmsg_be2 {
    int result, my_team, v58, loc_c, loc_r, n;
    int tokens;   /* update 117 (E7): tokens earned, written to 23456 +0x58 (filled by the server) */
    int xp;       /* update 151 (req. 3 / 16): experience earned -> +0xa8 pair (0, xp) = the EXPERIENCE row (run 175 labelling) */
    int rank_on, tro_before, best_before, xp_before; /* update 162 (decode_trophyroad_rank.md B): +0xb0 triples (1, trophies before, best before),
                                                      * (5, xp before, xp before) -> rank badge and XP bar on the Proceed page */
    int labels;   /* update 132 (A11): 1 = fields +0x60..+0x90 carry markers 100 * index (labelling run) */
    struct { int bc, br, sc, sr, team, local; char name[20]; int tro, best, pow; } row[BMSG_BE_ROWS]; /* tro/best/pow: filled by the server (update 100) */
} bmsg_be2;
int bmsg_parse_14110b(const void *body, size_t len, bmsg_be2 *out);
/* update 96: 23456 BattleEnd (decoder libg+0x5b48a4). kind 0 = 3v3 layout, 2 = solo showdown (result = placement); result 0 win / 1 lose / 2 draw */
#define BMSG_BATTLE_END 23456
void bmsg_battle_end_msg(bv_bs *s, int kind, int result, int trophy_delta, const bmsg_be2 *be, int acc_hi, int acc_lo);

/* update 114 (G5): 24113 player profile, the answer to 14113 (Long player id). Decoder libg+0x58aa14: PlayerProfile (libg+0x38f3b4: VInt id hi, VInt id lo,
 * DataRef, VInt n x hero {DataRef brawler, DataRef skin, VInt trophies, VInt highest, VInt power}, VInt m x stat {VInt index, VInt value},
 * PlayerDisplayData libg+0x3016a4 {String name, VInt 100, VInt 28000000 + thumbnail, VInt 43000000 + colour, VInt}), Bool club (0), DataRef (null).
 * Stat indices (INFERRED, common server knowledge): 1 3v3 wins, 3 trophies, 4 highest trophies, 5 brawlers, 8 solo wins, 11 duo wins. */
#define BMSG_PLAYER_PROFILE 24113
#define BMSG_BATTLE_LOG 23458   /* update 140 (G7): answer to 14114 */
typedef struct bmsg_battle { long t; int kind, result, delta, map, brawler, len; int np; struct { int row, team, local, tro, pow; const char *name; } p[10]; } bmsg_battle;   /* update 159: players */
/* update 140: 23458 with n battles (newest first); labels = markers instead of guesses */
void bmsg_battle_log_msg(bv_bs *s, const bmsg_cfg *c, const bmsg_battle *b, int n, int labels);
void bmsg_profile_msg(bv_bs *s, const bmsg_cfg *c, int id_hi, int id_lo, const char *name, int wins);
int bmsg_parse_14362(const void *body, size_t len, int *event_id, int *slot);
/* the map row of the event that uses slot `slot` in our 24101 (event 0 -> map_row, event i -> event_map[i]); -1 if no event has that slot */
int bmsg_map_for_slot(const bmsg_cfg *c, int slot);
int bmsg_parse_14363(const void *body, size_t len, int fallback);
/* Request 14102 (the client's command batch; layout read from bodies logged on the console 2026-11-11): VInt 0, VInt tick, VInt checksum, VInt count, then
 * the commands (VInt id, then the command's own fields). Returns the command count (-1 if the body is short) and the first command's id (-1 if none).
 * Seen: 522 / 525 with DataRef(16, brawler) (brawler pick), 506 with DataRef(29, row) (skin table, probably). */
int bmsg_parse_14102(const void *body, size_t len, int *tick, int *first_id);
/* update 181: every command of a 14102 with its own fields (layouts from notes/libg_map/messages_client.md). DataRef fields give v = row, cls in c.
 * Returns the number decoded (stops at the first command with an unknown layout). */
typedef struct bmsg_cmd { int id; int v[4]; int cls; } bmsg_cmd;
int bmsg_parse_cmds(const void *body, size_t len, bmsg_cmd *out, int max);

/* Boxes (update 50; static analysis notes/2026-11-10-page-guard/boxes.md). The client opens a box with client command 500 in 14102: VInt 500, the 4 base
 * VInts, VInt boxType (INFERRED: 1..3 token boxes, 4 Big Box, 5 Mega Box) and then waits for the server command 203 ("give delivery items") carried in
 * message 24111. Returns the box type of the FIRST command if it is 500, else -1. */
int bmsg_parse_box_open(const void *body, size_t len);
#define BMSG_SERVER_COMMAND 24111
typedef struct bmsg_box_item {
    int type;     /* 6 = power points for `brawler` (INFERRED), 7 = resource / coins (INFERRED; 2 and 3 are other candidates) */
    int amount;
    int brawler;  /* row of the characters table for type 6 (else -1) */
    int skin;     /* update 64: row of the skins table (29) for type 9 (skin), else -1 */
    int card;     /* update 174: row of the cards table (23) for type 4 (gadget / star power), else -1 */
} bmsg_box_item;
#define BMSG_BOX_MAX 8 /* update 146: items one box can deliver (Mega Box 5 + brawlers); callers' arrays must hold this many */
/* update 64: client command 519 (offer purchase: VInt index, DataRef) inside a 14102 -> the offer index, -1 if the first command is not 519 */
int bmsg_parse_offer_buy(const void *body, size_t len);
/* update 65: client command 517 = Brawl Pass reward claim (4 fields, run 60: 9, 0, 2, 0); 1 = parsed */
int bmsg_parse_pass_claim(const void *body, size_t len, int f[4]);
/* update 64: what the offer at `index` (home +0x9c, as built by bmsg_own_home_data) delivers; returns the number of items (0 = unknown index) */
int bmsg_offer_items(int index, const bmsg_cfg *c, bmsg_box_item *out);
/* update 64: delivery unit type of a box (the 3D model id: sc3d/bbox_10 Brawl Box, 11 Big Box, 12 Mega Box) for the client's boxType */
int bmsg_box_unit(int box_type);
/* 24111 body: command 203 with one delivery unit of type `unit_type` and n items (layout of decode libg+0x5b7dbc). */
void bmsg_box_delivery(bv_bs *s, int unit_type, const bmsg_box_item *items, int n);
/* cycle 6: the same, for a Brawl Pass claim: g30/g34/g38 = claim type, tier + 2, season (the client marks the tier claimed) */
void bmsg_box_delivery_claim(bv_bs *s, int unit_type, const bmsg_box_item *items, int n, int ctype, int ctier2, int cseason);
/* The contents of a box: coins + power points for one owned brawler, scaled by box type (1-3 small, 4 Big Box x3, 5 Mega Box x10). seed = anything that
 * changes per box (the 14102 tick). coin_type = the item type used for coins. Returns the number of items (2). */
int bmsg_box_roll(int box_type, uint32_t seed, const bmsg_cfg *c, int coin_type, bmsg_box_item *out);
/* update 144: i-th brawler unlock card (characters row, cards row); 0 when i is past the end */
int bmsg_unlock_entry(int i, int *char_row, int *card_row);
/* update 144 (req. 10): one Brawl Box roll for the brawler drop: rarity 1..5 hit by the 2020 odds (per 10000: rare 277, super rare 125, epic 56,
 * mythic 26, legendary 11), only rarities that still have a brawler to unlock count; returns that brawler's row or -1 (r = 0..9999) */
int bmsg_box_brawler(const bmsg_cfg *c, int r, uint32_t pick);

#endif
