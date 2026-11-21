/* Phase 3 (living world, L1): the bot world. 1000+ bot accounts made at install (all at 0 trophies), each with a generated gamer name, club,
 * region, hidden skill rating, personality and a daily play schedule. Bots play while the Vita is off: at boot the match OUTCOMES since the last
 * run are simulated from the clock (skill vs trophies + luck), so the leaderboards move by themselves. Bots unlock brawlers and power levels as
 * they play and follow the same trophy league resets as the player.
 * The same world feeds the leaderboards (24403), bot profiles (24113), matchmaking (opponents near your trophies) and the bot clubs.
 * Portable C11, no console headers: host-tested (tests/host_botworld_test.c). Names are our own word lists (no real people, no game text). */
#ifndef BV_BOTWORLD_H
#define BV_BOTWORLD_H
#include <stddef.h>
#include <stdint.h>

#include "bytestream.h"

#define BW_MAGIC 0x31574256u   /* "VBW1" */
#define BW_VERSION 2
#define BW_MAX_BOTS 1500
#define BW_MAX_CLUBS 80
#define BW_NBR 40              /* brawler slots (index into br_row[]) */
#define BW_NAME 16             /* 15 characters + 0, like the game's name limit */
#define BW_CLUB_NAME 24
#define BW_ID_HI 77            /* bot account ids: (BW_ID_HI, 1 + bot index); clubs: (BW_CLUB_ID_HI, 1 + club index) */
#define BW_CLUB_ID_HI 78
#define BW_REGIONS 20
#define BW_SLOTS 10            /* battle name slots TID_BOT_1..9 (index 0 unused) */
#define BW_CHAT 48             /* club chat lines kept per world (ring) */
#define BW_CHAT_LEN 64

enum { BW_P_AGGRESSIVE, BW_P_CAMPER, BW_P_BUSH, BW_P_SUPPORT, BW_P_SNIPER, BW_P_COUNT };
enum { BW_ROLE_NONE = 0, BW_ROLE_MEMBER = 1, BW_ROLE_SENIOR = 2, BW_ROLE_VICE = 3, BW_ROLE_PRESIDENT = 4 };

typedef struct bw_bot {
    char name[BW_NAME];
    uint16_t tro[BW_NBR], best[BW_NBR];
    uint8_t pow[BW_NBR];        /* 0 = not owned, 1..10 (10 = star power) */
    uint16_t skill;             /* hidden rating 0..1000 */
    uint8_t region, persona, hour0, role;
    uint8_t thumb, color, quit, fav;   /* quit: 1 = stopped playing (inactive); fav = favourite brawler slot */
    int16_t club;               /* club index, -1 = none */
    uint16_t daily_min;         /* minutes played on an average weekday */
    int8_t streak;              /* win (+) / loss (-) streak */
    uint8_t pad[3];
    uint32_t battles, wins, sd_wins, exp;
    uint32_t last_play;         /* unix time of the last simulated match */
} bw_bot;

typedef struct bw_club {
    char name[BW_CLUB_NAME];
    uint8_t region, type;       /* type 1 open, 2 invite only, 3 closed */
    uint16_t badge, req;        /* badge row (table 8), required trophies */
    uint16_t n_members;
    uint32_t created;
} bw_club;

typedef struct bw_chat {
    uint32_t t;
    int16_t bot;                /* -1 = the player, -2 = system line */
    int16_t club;
    uint8_t kind;               /* 0 text, 1 joined, 2 left, 3 kicked, 4 promoted, 5 demoted */
    uint8_t pad[3];
    char text[BW_CHAT_LEN];
} bw_chat;

typedef struct bw_params {
    int n_bots, n_clubs;
    int n_br;                   /* playable brawlers (characters rows), in unlock order */
    int br_row[BW_NBR];
    signed char br_rar[BW_NBR]; /* 0 trophy road, 1 rare .. 5 legendary, -1 unknown (= rare) */
    int n_badges, n_thumbs, n_colors;
    int catchup_days;           /* offline catch-up limit (0 = 30) */
} bw_params;

typedef struct bw_world {
    uint32_t magic, version, seed, size;
    int n_bots, n_clubs, n_br;
    int br_row[BW_NBR];
    signed char br_rar[BW_NBR];
    uint32_t created, sim_to;   /* the world is simulated up to sim_to (unix time) */
    uint32_t season_start, season_len;   /* trophy league resets (0 = none) */
    uint32_t matches;           /* simulated matches so far */
    int16_t player_club;        /* the player's club (-1 none) */
    uint8_t player_role, pad0;
    uint32_t player_seen;       /* last time the player was active (club kicks of inactive members) */
    int chat_n;                 /* lines written so far (ring index = chat_n % BW_CHAT) */
    bw_chat chat[BW_CHAT];
    bw_bot bots[BW_MAX_BOTS];
    bw_club clubs[BW_MAX_CLUBS];
} bw_world;

/* A fresh world: every bot at 0 trophies with the first brawler at level 1, clubs filled. now = install time. */
void bw_create(bw_world *w, const bw_params *p, uint32_t seed, uint32_t now);
/* 1 = `w` holds a usable world of this build (magic, version, size). */
int bw_valid(const bw_world *w);
/* Plays every bot's scheduled matches with a start time in [sim_to, to) (catch-up limited to catchup_days), with the season resets that fall
 * inside. Deterministic: two calls over [a,b) + [b,c) give the same world as one over [a,c). Returns the matches played. */
uint32_t bw_simulate(bw_world *w, uint32_t to, int catchup_days);
void bw_set_season(bw_world *w, uint32_t start, uint32_t len);

int bw_bot_total(const bw_world *w, int i);           /* total trophies of bot i */
int bw_bot_online(const bw_world *w, int i, uint32_t now);
int bw_club_trophies(const bw_world *w, int c);
int bw_club_online(const bw_world *w, int c, uint32_t now);
int bw_slot_of_row(const bw_world *w, int row);       /* brawler slot of a characters row, -1 if not playable */
const char *bw_region_code(int r);
const char *bw_region_name(int r);
int bw_region_index(const char *code);                /* -1 if unknown */

/* Leaderboards. type 1 = total trophies, 0 = one brawler (row), 2 = clubs. region < 0 = global. The player is ranked in (player_total /
 * player_brawler trophies, his club). Returns the entries written (<= max), sorted best first; *own_rank = the player's place (1-based, 0 = not
 * in the list). Entry: who >= 0 bot index, -1 the player; for clubs who = club index (-1 = the player's club is not used). */
typedef struct bw_entry { int who, score; } bw_entry;
int bw_leaderboard(const bw_world *w, int type, int brawler_row, int region, int player_score, const char *player_name, bw_entry *out, int max,
                   int *own_rank);

/* Matchmaking. The player plays brawler row `row` with `trophies`; streak > 0 win streak (tougher bots), < 0 losses (easier). Picks up to `n` bots
 * near those trophies (online bots first) into plan[1..n]: bot index, brawler slot, level 1..10, AI tier 0..4. Returns the bots picked. */
typedef struct bw_pick { int bot, slot, level, tier; } bw_pick;
int bw_match_plan(const bw_world *w, int trophies, int streak, int n, uint32_t seed, uint32_t now, bw_pick *plan);
/* A battle the player played: kind 0 = 3v3 (result 0 win, 1 loss, 2 draw for team `my_team`), 2 = showdown (result = the player's place 1..10).
 * team[n] / used[n] for name slots 1..9 (used 0 = slot empty). The bots' brawler trophies change like the player's. */
void bw_match_result(bw_world *w, const bw_pick *plan, const int *team, const int *used, int kind, int result, int my_team, uint32_t now);

/* Trophy change of one match at `trophies` (3v3: result 0 win, 1 loss, 2 draw; showdown: place 1..10). */
int bw_trophy_delta(int trophies, int kind, int result);

/* 24403 leaderboard reply (layout: notes/libg_map/ui.md section 7 + the 24403 / S_c4c90 / S_3c3774 / 0x452adc decoders). */
void bw_leaderboard_msg(bv_bs *s, const bw_world *w, int type, int local, int brawler_row, int region, int player_score, const char *player_name,
                        int player_thumb, int player_color, int acc_hi, int acc_lo);

/* Club chat (ring). bot -1 = the player. */
void bw_chat_add(bw_world *w, uint32_t t, int bot, int club, int kind, const char *text);


/* ---- Phase 3 (L2): clubs + chat (layouts: notes/libg_map/messages_server.md S_138c88 / S_68122c / S_67d56c, stream entry factory 0x58d58:
 * type 2 chat = base + String, type 4 event = base + VInt event (1 kicked, 2 accepted, 3 joined, 4 left, 5 promoted, 6 demoted) + target id +
 * String target name; base 0x510850 = id (2 VInt), sender id (2 VInt), String sender name, VInt sender role, VInt age seconds, Bool) ---- */
typedef struct bw_player { const char *name; int hi, lo, total, thumb, color; } bw_player;
int bw_sc_role(int role);                                   /* our role -> the game's (1 member, 2 president, 3 senior, 4 vice) */
void bw_club_header(bv_bs *s, const bw_world *w, int c, int player_total);
void bw_club_data_msg(bv_bs *s, const bw_world *w, int c, uint32_t now, const bw_player *pl);   /* 24301 */
void bw_my_club_msg(bv_bs *s, const bw_world *w, uint32_t now, const bw_player *pl);            /* 24399 */
void bw_club_list_msg(bv_bs *s, const bw_world *w, int region, const char *query, int player_total);   /* 24304 (query NULL) / 24310 */
void bw_stream_msg(bv_bs *s, const bw_world *w, uint32_t now, const bw_player *pl);             /* 24311: the player's club chat */
void bw_stream_entry_msg(bv_bs *s, const bw_world *w, int line, uint32_t now, const bw_player *pl);   /* 24312: one line (chat_n index) */
int bw_player_join(bw_world *w, int c, uint32_t now);      /* 1 = joined */
void bw_player_leave(bw_world *w, uint32_t now);
int bw_chat_reply(bw_world *w, uint32_t now);              /* an online club mate answers the player: 1 = a line was added */

#endif
