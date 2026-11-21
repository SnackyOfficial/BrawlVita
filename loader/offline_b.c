#include "offline_b.h"
#include "passdata.h"
#include "profile.h"
#include "botnames.h"

/* cycle 4: apply delivered items to the saved profile (skins owned, coins added) */
static void profile_add_items(bvb_state *st, const bmsg_box_item *it, int n) {
    if (!st->profile) return;
    for (int i = 0; i < n; i++) {
        if (it[i].type == 9 && it[i].skin >= 0) bv_profile_own_skin(st->profile, it[i].skin);
        else if (it[i].type == st->cfg.box_coin_type) st->profile->coins += it[i].amount;
        else if (it[i].type == 8) { st->profile->gems += it[i].amount; st->cfg.avatar_tail[1] = st->profile->gems; }   /* update 101: gems */
        else if (it[i].type == 1 && it[i].brawler >= 0) {                 /* update 144 (req. 10): a brawler from a box */
            bv_brawler *b = bv_profile_unlock(st->profile, it[i].brawler);
            if (b && it[i].brawler < 96) { st->cfg.br_has[it[i].brawler] = 1; st->cfg.br_tro[it[i].brawler] = 0; st->cfg.br_best[it[i].brawler] = 0; st->cfg.br_pow[it[i].brawler] = 0; }
        }
        else if (it[i].type == 4 && it[i].card >= 0) {                    /* update 174: a gadget / star power card */
            bv_profile_own_card(st->profile, it[i].card);
            st->cfg.own_cards = st->profile->cards; st->cfg.n_own_cards = st->profile->n_cards;
        }
        else if (it[i].type == 6 && it[i].brawler >= 0) {                 /* update 101 (B10): power points to that brawler */
            bv_brawler *b = bv_profile_unlock(st->profile, it[i].brawler);
            if (b) b->power_points += it[i].amount;
            if (b && it[i].brawler < 96) { st->cfg.br_has[it[i].brawler] = 1; st->cfg.br_pp[it[i].brawler] = (short)b->power_points; }
        }
    }
}
#include <string.h>
#include <time.h>
/* Phase 3: the profile of world bot `idx` (24113): the same builder as the player's, filled from the bot */
static void world_bot_profile(bvb_state *st, const bvb_game_if *gi, int idx) {
    static bmsg_cfg c;
    const bw_world *w = st->world;
    const bw_bot *b = &w->bots[idx];
    c = st->cfg;
    c.unlock_all = 0;
    memset(c.br_has, 0, sizeof c.br_has);
    c.brawler_row = w->br_row[b->fav < w->n_br ? b->fav : 0];
    for (int k = 0; k < w->n_br; k++) {
        int r = w->br_row[k];
        if (!b->pow[k] || r < 0 || r >= 96) continue;
        c.br_has[r] = 1; c.br_tro[r] = (short)b->tro[k]; c.br_best[r] = (short)b->best[k];
        c.br_pow[r] = (signed char)(b->pow[k] > 10 ? 9 : b->pow[k] - 1);
    }
    c.account_trophies = bw_bot_total(w, idx);
    c.thumbnail_row = b->thumb;
    c.name_color_row = b->color;
    c.exp_total = (int)b->exp;
    bv_bs ps;
    bs_init(&ps);
    bmsg_profile_msg(&ps, &c, BW_ID_HI, 1 + idx, b->name, (int)b->wins);
    /* the profile ends with Bool 0 (no club) + DataRef null: a bot in a club gets Bool 1 + the club header (S_138c88) */
    if (!ps.failed && b->club >= 0 && b->club < w->n_clubs && ps.len >= 2 && ps.data[ps.len - 1] == 0 && ps.data[ps.len - 2] == 0) {
        ps.len -= 2;
        bs_bool(&ps, 1);
        bw_club_header(&ps, w, b->club, st->cfg.account_trophies);
        bs_vint(&ps, 0);
    }
    if (!ps.failed) {
        if (gi->log) gi->log(gi->ctx, "world: 14113 profile of bot %d '%s' (%d trophies, %d battles) -> 24113 (%u bytes)", idx, b->name, c.account_trophies,
                             (int)b->battles, (unsigned)ps.len);
        gi->deliver(gi->ctx, BMSG_PLAYER_PROFILE, 0, ps.data, ps.len);
    }
    bs_free(&ps);
}

/* Phase 3 (L2): club messages from the bot world */
static bw_player world_me(const bvb_state *st) {
    bw_player p = {st->cfg.player_name, st->cfg.account_hi, st->cfg.account_lo, st->cfg.account_trophies, st->cfg.thumbnail_row, st->cfg.name_color_row};
    return p;
}
static void world_send(const bvb_game_if *gi, int type, bv_bs *s, const char *what) {
    if (!s->failed) {
        if (gi->log) gi->log(gi->ctx, "world: %s -> %d (%u bytes)", what, type, (unsigned)s->len);
        gi->deliver(gi->ctx, type, 0, s->data, s->len);
    }
    bs_free(s);
}
static void world_push_club(bvb_state *st, const bvb_game_if *gi, int with_data) {
    uint32_t now = (uint32_t)time(NULL);
    bw_player me = world_me(st);
    bv_bs s;
    bs_init(&s); bw_my_club_msg(&s, st->world, now, &me); world_send(gi, 24399, &s, "my club");
    if (st->world->player_club < 0) return;
    if (with_data) { bs_init(&s); bw_club_data_msg(&s, st->world, st->world->player_club, now, &me); world_send(gi, 24301, &s, "club data"); }
    bs_init(&s); bw_stream_msg(&s, st->world, now, &me); world_send(gi, 24311, &s, "club chat");
}
static int rd_long8(const uint8_t *p, size_t len, int *hi, int *lo) {
    if (!p || len < 8) return 0;
    *hi = (int)((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]);
    *lo = (int)((uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7]);
    return 1;
}
static void world_club_msg(bvb_state *st, const bvb_game_if *gi, int type, void *msg) {
    const uint8_t *p = NULL;
    size_t len = 0;
    if (gi->msg_payload) gi->msg_payload(gi->ctx, msg, &p, &len);
    uint32_t now = (uint32_t)time(NULL);
    bw_world *w = st->world;
    bw_player me = world_me(st);
    bv_bs s;
    int hi = 0, lo = 0;
    if (type == 14302 && rd_long8(p, len, &hi, &lo) && hi == BW_CLUB_ID_HI && lo >= 1 && lo <= w->n_clubs) {   /* club info */
        bs_init(&s); bw_club_data_msg(&s, w, lo - 1, now, &me); world_send(gi, 24301, &s, "14302 club info");
    } else if (type == 14303) {                                                      /* suggested clubs */
        bs_init(&s); bw_club_list_msg(&s, w, st->world_region, NULL, me.total); world_send(gi, 24304, &s, "14303 club list");
    } else if (type == 14324) {                                                      /* search: String + filters */
        char q[32] = "";
        if (p && len >= 4) {
            int32_t n = (int32_t)((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]);
            if (n > 0 && (size_t)n + 4 <= len) { if (n > 31) n = 31; memcpy(q, p + 4, (size_t)n); q[n] = 0; }
        }
        bs_init(&s); bw_club_list_msg(&s, w, -1, q, me.total); world_send(gi, 24310, &s, "14324 club search");
    } else if (type == 14305 && rd_long8(p, len, &hi, &lo) && hi == BW_CLUB_ID_HI && bw_player_join(w, lo - 1, now)) {   /* join */
        if (gi->log) gi->log(gi->ctx, "world: joined club %d '%s'", lo - 1, w->clubs[lo - 1].name);
        world_push_club(st, gi, 1);
        if (st->world_save) st->world_save();
    } else if (type == 14308) {                                                      /* leave */
        bw_player_leave(w, now);
        world_push_club(st, gi, 0);
        if (st->world_save) st->world_save();
    } else if (type == 14315 && w->player_club >= 0) {                               /* chat line */
        char t[BW_CHAT_LEN] = "";
        if (p && len >= 4) {
            int32_t n = (int32_t)((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]);
            if (n > 0 && (size_t)n + 4 <= len) { if (n > BW_CHAT_LEN - 1) n = BW_CHAT_LEN - 1; memcpy(t, p + 4, (size_t)n); t[n] = 0; }
        }
        bw_chat_add(w, now, -1, w->player_club, 0, t);
        bs_init(&s); bw_stream_entry_msg(&s, w, w->chat_n - 1, now, &me); world_send(gi, 24312, &s, "14315 your chat line");
        if (bw_chat_reply(w, now)) { bs_init(&s); bw_stream_entry_msg(&s, w, w->chat_n - 1, now, &me); world_send(gi, 24312, &s, "club mate reply"); }
    } else return;
}

static void profile_done(bvb_state *st, const bvb_game_if *gi, const char *what) {
    if (!st->profile) return;
    if (st->profile_save) st->profile_save();
    if (gi->log) gi->log(gi->ctx, "offline: profile saved after %s: gems %d, coins %d, %d skins, %d claimed tiers", what, st->profile->gems, st->profile->coins,
                         st->profile->n_skins, st->profile->n_claimed);
}

#include <stdlib.h>
#include <string.h>
#include <time.h>

static const int k_types[] = {10100, 10101, 10108, 14109, 14110, 14350, 14353, 14363, 14362, 14354, 14700, 14701, 14104};

static int bvb_slot(int type) {
    for (unsigned i = 0; i < sizeof k_types / sizeof k_types[0]; i++)
        if (k_types[i] == type) return (int)i;
    return -1;
}

int bvb_ring_push(const bvb_mem *m, uint32_t q, uint32_t msg) {
    uint32_t arr = m->rd32(m->ctx, q);
    uint32_t cap = m->rd32(m->ctx, q + 4);
    uint32_t count = m->rd32(m->ctx, q + 8);
    uint32_t tail = m->rd32(m->ctx, q + 16);
    if (!arr || cap == 0 || cap > 0x100000u || count >= cap || tail >= cap) return 0;
    m->wr32(m->ctx, arr + 4u * tail, msg);
    m->wr32(m->ctx, q + 16, (tail + 1u) % cap);
    m->wr32(m->ctx, q + 8, count + 1u);
    return 1;
}

static int rows_cb(void *ctx, int table) {
    const bvb_game_if *gi = (const bvb_game_if *)ctx;
    return gi->table_rows ? gi->table_rows(gi->ctx, table) : -1;
}

void bvb_init(bvb_state *st, const bvb_game_if *gi) {
    memset(st, 0, sizeof *st);
    bmsg_default_cfg(&st->cfg);
    st->selected_map = bmsg_home_map(&st->cfg);
    st->resend_home = 0; /* update 17: see handle_send; resend_home=1 in config.txt brings back the original mod's behaviour */
    st->game.table_rows = rows_cb;
    st->game.ctx = (void *)gi;
}

typedef enum { M_LOGIN_OK, M_HOME, M_AVATAR, M_ACK, M_MM_STATUS, M_MM_CANCELLED } which;

static void send_msg(bvb_state *st, const bvb_game_if *gi, which w) {
    bv_bs s;
    bs_init(&s);
    int type = 0;
    switch (w) {
    case M_LOGIN_OK:
        type = BMSG_LOGIN_OK;
        bmsg_login_ok(&s, &st->cfg);
        break;
    case M_HOME:
        type = BMSG_OWN_HOME_DATA;
        bmsg_own_home_data(&s, &st->cfg, &st->game);
        break;
    case M_AVATAR:
        type = BMSG_AVATAR_UPDATE;
        bmsg_avatar_update(&s, &st->cfg, st->selected_map);
        break;
    case M_ACK:
        type = BMSG_ACK;
        bmsg_ack(&s);
        break;
    case M_MM_STATUS:
        type = BMSG_MATCHMAKE_STATUS;
        bmsg_matchmake_status(&s);
        break;
    case M_MM_CANCELLED:
        type = BMSG_MATCHMAKE_CANCELLED; /* empty body */
        break;
    }
    if (s.failed) {
        if (gi->log) gi->log(gi->ctx, "offline: out of memory building message %d", type);
    } else {
        if (gi->log) gi->log(gi->ctx, "offline: -> client %d (%u bytes)", type, (unsigned)s.len);
        gi->deliver(gi->ctx, type, type == BMSG_LOGIN_OK ? 1 : 0, s.data, s.len);
    }
    bs_free(&s);
}

#define BVB_MAX_DEPTH 6   /* update 163: Brawl TV chains 14700 -> 24700 -> 14701 -> 24701 -> 14104 inside each other's deliveries */

static int handle_send(bvb_state *st, const bvb_game_if *gi, void *msg);

int bvb_handle_send(bvb_state *st, const bvb_game_if *gi, void *msg) {
    if (gi->lock) gi->lock(gi->ctx);
    int r;
    if (st->depth >= BVB_MAX_DEPTH) {
        /* every reply we deliver may make the client send a request that we answer with replies again: stop the chain */
        if (gi->log) gi->log(gi->ctx, "offline: message handled %d levels deep: swallowed without an answer", st->depth);
        r = 0;
    } else {
        st->depth++;
        r = handle_send(st, gi, msg);
        st->depth--;
    }
    if (gi->unlock) gi->unlock(gi->ctx);
    return r;
}

static int handle_send(bvb_state *st, const bvb_game_if *gi, void *msg) {
    int type = gi->msg_type(gi->ctx, msg);
    int slot = bvb_slot(type);
    uint32_t n;
    if (slot >= 0) {
        n = ++st->count[slot];
    } else {
        ++st->unknown;
        int k;
        for (k = 0; k < 32 && st->other_count[k] && st->other_type[k] != type; k++) {}
        if (k < 32) {
            st->other_type[k] = type;
            n = ++st->other_count[k];
        } else {
            n = st->unknown;
        }
    }
    if (gi->log && (n <= 3 || !(n & (n - 1))))
        gi->log(gi->ctx, "offline: client -> server message %d (#%u of this type)%s", type, n, slot < 0 ? " [not in the known list]" : "");

    if (type == 10100) {
        if (st->logged_in) {
            if (gi->log) gi->log(gi->ctx, "offline: a second 10100 arrived: answering it like the first");
        }
        st->logged_in = 1;
        gi->login_prepare(gi->ctx);
        send_msg(st, gi, M_LOGIN_OK);
        gi->login_done(gi->ctx);
        send_msg(st, gi, M_HOME);
        send_msg(st, gi, M_AVATAR);
        if (gi->after_login) gi->after_login(gi->ctx);
        if (st->world && st->world->player_club >= 0) world_push_club(st, gi, 0);   /* Phase 3: the player's club + chat (offline catch-up lines) */
        return 0;
    }

    /* The original encodes every swallowed message (vtable slot 2) and leaves it alone afterwards. The body of a map selection only
     * exists in the message's stream once it was encoded, so encode first, then read it. */
    gi->msg_encode(gi->ctx, msg);
    /* Phase 3: the bot world keeps playing while the game runs (every 60 s of real time), saved every 10 minutes */
    if (st->world) {
        uint32_t now = (uint32_t)time(NULL);
        if (now - st->world_tick >= 60u) {
            uint32_t m = bw_simulate(st->world, now, 0);
            st->world_tick = now;
            if (m && gi->log && (st->world->matches & 0xfff) < m) gi->log(gi->ctx, "world: live: %u bot matches (total %u)", (unsigned)m, (unsigned)st->world->matches);
        }
        if (now - st->world_saved >= 600u && st->world_save) { st->world_save(); st->world_saved = now; }
    }
    /* What the client asks for (plan D.1): the body of every message except the keep-alive, as hex, for the first 8 of each type. This is how the
     * messages behind Play, the brawler menu, boxes and the shop are found (14102 carries the client's commands). */
    if (gi->log && type != 10108 && n <= 8 && gi->msg_payload) {
        const uint8_t *p = NULL;
        size_t len = 0;
        if (gi->msg_payload(gi->ctx, msg, &p, &len) && p) {
            char hex[64 * 3 + 1];
            size_t k = len < 64 ? len : 64;
            for (size_t i = 0; i < k; i++) {
                static const char d[] = "0123456789abcdef";
                hex[i * 3] = d[p[i] >> 4];
                hex[i * 3 + 1] = d[p[i] & 15];
                hex[i * 3 + 2] = ' ';
            }
            hex[k ? k * 3 - 1 : 0] = 0;
            gi->log(gi->ctx, "offline: body of %d #%u: %u bytes: %s%s", type, n, (unsigned)len, hex, len > 64 ? " ..." : "");
        } else {
            gi->log(gi->ctx, "offline: body of %d #%u: not readable", type, n);
        }
    }
    /* update 36: every 14102 that carries commands is logged (the first-8 rule above missed the user's event switches), with the first command id */
    if (type == 14102 && gi->log && gi->msg_payload) {
        const uint8_t *p = NULL;
        size_t len = 0;
        int tick = 0, id = -1;
        if (gi->msg_payload(gi->ctx, msg, &p, &len) && p) {
            int nc = bmsg_parse_14102(p, len, &tick, &id);
            if (nc > 0 && st->cmd_logged < 400) {
                st->cmd_logged++;
                char hex[96 * 3 + 1];
                size_t k = len < 96 ? len : 96;
                for (size_t i = 0; i < k; i++) {
                    static const char d[] = "0123456789abcdef";
                    hex[i * 3] = d[p[i] >> 4];
                    hex[i * 3 + 1] = d[p[i] & 15];
                    hex[i * 3 + 2] = ' ';
                }
                hex[k ? k * 3 - 1 : 0] = 0;
                gi->log(gi->ctx, "offline: client commands (14102): tick %d, %d command(s), first id %d: %s", tick, nc, id, hex);
            }
        }
    }
    int picked = st->cfg.map_row;
    if (type == 14363) {
        const uint8_t *p = NULL;
        size_t len = 0;
        if (gi->msg_payload && gi->msg_payload(gi->ctx, msg, &p, &len) && p && len <= 64) picked = bmsg_parse_14363(p, len, st->cfg.map_row);
        else if (gi->log) gi->log(gi->ctx, "offline: 14363 has no readable body after encoding: keeping map row %d", picked);
    }
    if (!st->resend_home) {
        /* default since update 17: no OwnHomeData after the login. Every 24101 makes the client rebuild its home ("Was already loading other game, overriding!");
         * answering KeepAlive (10108) and the other periodic messages with it freed the home while a game thread still read it: use-after-free at libg+0x42f21c
         * (run of 2026-11-11, 264 s). With the acknowledgement only, the same build stayed 600 s at the home screen. A map selection still gets its 24124.
         * cycle 4 (ISSUES E1): 24125 is the client's 'team left' message (handler libg+0x1a8218 deletes the team, run 69 probe): experiment ack24125=0. */
        if (st->cfg.ack24125) send_msg(st, gi, M_ACK);
        /* update 89 (run 97): leaving the training cave the client sends 14109 and waits on the loading screen (92 %) for the home */
        if (type == 14109 || type == 14110 || type == 14101) { extern volatile int bv_battle_left __attribute__((weak)); if (&bv_battle_left) bv_battle_left = 1; }   /* update 112 (J3): home controls (weak: host tests) */
        if ((type == 14109 || ((type == 14101 || type == 14108) && st->cfg.battle_end_msg)) && st->cfg.home_on_14109) {
            if (gi->log) gi->log(gi->ctx, "offline: 14109 (back home from the training cave) -> 24101 + 24124 (home_on_14109=1)");
            send_msg(st, gi, M_HOME);
            send_msg(st, gi, M_AVATAR);
        }
        if (type == 14353) st->selected_map = bmsg_home_map(&st->cfg);   /* update 144 (req. 22): the selected event, not always Gem Grab */
        /* cycle 6: 14354 (team: member skin, body VInt 0 + DataRef(29,row), run 79) */
        if (type == 14354 && gi->msg_payload) {
            const uint8_t *p = NULL;
            size_t len = 0;
            if (gi->msg_payload(gi->ctx, msg, &p, &len) && p && len >= 2) {
                bv_rd r;
                rd_init(&r, p, len);
                (void)rd_vint(&r);
                int32_t c = rd_vint(&r);
                int32_t row = c ? rd_vint(&r) : -1;
                if (!r.err && c == 29 && row >= 0) {
                    st->cfg.sel_skin_row = (int)row;
                    if (gi->log) gi->log(gi->ctx, "offline: 14354 team skin: skin row %d -> 24124", (int)row);
                    send_msg(st, gi, M_AVATAR);
                }
            }
        }
        /* update 140 (G7): the Battle Log button sends 14114 (empty body, run 154) -> 23458 with the profile's battles, newest first */
        /* update 163 (decode_brawl_tv.md, layouts CONFIRMED): the Brawl TV popup's OK sends 14700 (VInt -1, Bool 1) -> 24700 channel list
         * (VInt count, {VInt id, String, String}); then 14701 -> 24701 next-up (VInt channel, VInt, Bool hasAccount, Int hi, Int lo; a negative
         * high int makes the client retry); then 14104 (account Long + Bool) = start spectating -> the loader starts a local watch-only bot match on
         * the next frame (bv_tv_pending); without the loader hook: 24105 reason 1 (spectate disabled, a clean abort). */
        if (type == 14700 || type == 14701 || type == 14104) {
            extern volatile int bv_tv_pending __attribute__((weak));
            extern int bv_brawl_tv_on __attribute__((weak));
            bv_bs ts;
            bs_init(&ts);
            int reply = 0;
            if (type == 14700) { bs_vint(&ts, 1); bs_vint(&ts, 0); bs_string(&ts, "Brawl TV"); bs_string(&ts, ""); reply = 24700; }
            else if (type == 14701) { bs_vint(&ts, 0); bs_vint(&ts, 0); bs_bool(&ts, 1); bs_int(&ts, 0); bs_int(&ts, 1); reply = 24701; }
            else if (&bv_tv_pending && (!&bv_brawl_tv_on || bv_brawl_tv_on)) { bv_tv_pending = 1; }   /* update 168: brawl_tv=0 -> 24105 abort */
            else { bs_int(&ts, 1); reply = 24105; }
            if (gi->log) gi->log(gi->ctx, "offline: Brawl TV %d -> %s", type, reply ? (reply == 24700 ? "24700 channels" : reply == 24701 ? "24701 next-up" : "24105 abort")
                                                                          : "watch-only bot match on the next frame");
            if (reply && !ts.failed) gi->deliver(gi->ctx, reply, 0, ts.data, ts.len);
            bs_free(&ts);
        }
        /* Phase 3 (ui.md section 7): the leaderboards popup asks 14403 (Bool local, VInt type 1 total / 0 brawler / 2 clubs, DataRef brawler,
         * VInt) and waits for 24403 echoing type + brawler; built from the bot world (the player is ranked in) */
        if (type == 14403 && st->world && gi->msg_payload) {
            const uint8_t *p = NULL;
            size_t len = 0;
            int local = 0, lt = 1, row = -1, cls = 0;
            if (gi->msg_payload(gi->ctx, msg, &p, &len) && p && len >= 2) {
                bv_rd r;
                rd_init(&r, p, len);
                local = rd_byte(&r) & 1;
                lt = rd_vint(&r);
                cls = rd_vint(&r);
                if (cls) { int rr = rd_vint(&r); if (cls == 16) row = rr; }
                if (r.err) { lt = 1; row = -1; }
            }
            int score = lt == 0 ? (row >= 0 && row < 96 && st->cfg.br_has[row] ? st->cfg.br_tro[row] : 0) : st->cfg.account_trophies;
            bv_bs ls;
            bs_init(&ls);
            bw_simulate(st->world, (uint32_t)time(NULL), 0);
            bw_leaderboard_msg(&ls, st->world, lt, local, row, st->world_region, score, st->cfg.player_name, st->cfg.thumbnail_row, st->cfg.name_color_row,
                               st->cfg.account_hi, st->cfg.account_lo);
            if (!ls.failed) {
                if (gi->log) gi->log(gi->ctx, "world: 14403 leaderboard (type %d, %s, brawler row %d) -> 24403 (%u bytes, your score %d)", lt,
                                     local ? "local" : "global", row, (unsigned)ls.len, score);
                gi->deliver(gi->ctx, 24403, 0, ls.data, ls.len);
            }
            bs_free(&ls);
        }
        int w14113 = 0;
        if (st->world && (type == 14302 || type == 14303 || type == 14305 || type == 14308 || type == 14315 || type == 14324)) world_club_msg(st, gi, type, msg);
        if (type == 14113 && st->world && gi->msg_payload) {
            const uint8_t *p = NULL;
            size_t len = 0;
            if (gi->msg_payload(gi->ctx, msg, &p, &len) && p && len >= 8) {
                int hi = (int)((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]);
                int lo = (int)((uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7]);
                if (hi == BW_ID_HI && lo >= 1 && lo <= st->world->n_bots) { world_bot_profile(st, gi, lo - 1); w14113 = 1; }
            }
        }
        if (type == 14114 && st->cfg.battle_log && st->profile) {
            bmsg_battle hb[10];
            int n = st->profile->n_hist < 10 ? st->profile->n_hist : 10;
            for (int i = 0; i < n; i++) {
                int k = (st->profile->n_hist - 1 - i) % 10;
                hb[i].t = st->profile->hist[k].t; hb[i].kind = st->profile->hist[k].kind; hb[i].result = st->profile->hist[k].result;
                hb[i].delta = st->profile->hist[k].delta; hb[i].map = st->profile->hist[k].map; hb[i].brawler = st->profile->hist[k].brawler;
                hb[i].len = st->profile->hist[k].len;   /* update 180 */
                hb[i].np = st->profile->hist[k].np;   /* update 159 */
                for (int j = 0; j < hb[i].np && j < 10; j++) {
                    hb[i].p[j].row = st->profile->hist[k].p[j].row; hb[i].p[j].team = st->profile->hist[k].p[j].team;
                    hb[i].p[j].local = st->profile->hist[k].p[j].local; hb[i].p[j].name = st->profile->hist[k].p[j].name;
                    hb[i].p[j].tro = -1; hb[i].p[j].pow = -1;
                }
            }
            bv_bs ls;
            bs_init(&ls);
            st->cfg.now = (long)time(NULL);   /* update 167 */
            bmsg_battle_log_msg(&ls, &st->cfg, hb, n, st->cfg.battle_log == 2);
            if (!ls.failed) {
                if (gi->log) gi->log(gi->ctx, "offline: 14114 battle log -> client 23458 (%d battles, %u bytes%s)", n, (unsigned)ls.len, st->cfg.battle_log == 2 ? ", labelling" : "");
                if (gi->log && n > 0) gi->log(gi->ctx, "offline: battle log: newest battle at %ld, now %ld -> %ld s ago", hb[0].t, (long)time(NULL), (long)time(NULL) - hb[0].t);   /* update 155 */
                gi->deliver(gi->ctx, BMSG_BATTLE_LOG, 0, ls.data, ls.len);
            }
            bs_free(&ls);
        }
        /* update 114 (G5): the profile screen asks 14113 (Long player id, run 19: 00 00 00 00 00 00 00 01 00 00) and waits for 24113 */
        if (type == 14113 && st->cfg.profile_reply && gi->msg_payload && !w14113) {   /* (a world bot was answered above) */
            const uint8_t *p = NULL;
            size_t len = 0;
            int hi = st->cfg.account_hi, lo = st->cfg.account_lo;
            if (gi->msg_payload(gi->ctx, msg, &p, &len) && p && len >= 8) {
                hi = (int)((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]);
                lo = (int)((uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7]);
            }
            int own = hi == st->cfg.account_hi && lo == st->cfg.account_lo;
            bv_bs ps;
            bs_init(&ps);
            bmsg_profile_msg(&ps, &st->cfg, hi, lo, own ? NULL : "Brawler", own && st->profile ? st->profile->wins : 0);
            if (own && st->world && st->world->player_club >= 0 && !ps.failed && ps.len >= 2 && ps.data[ps.len - 1] == 0 && ps.data[ps.len - 2] == 0) {
                ps.len -= 2;   /* Phase 3: your club on your profile */
                bs_bool(&ps, 1);
                bw_club_header(&ps, st->world, st->world->player_club, st->cfg.account_trophies);
                bs_vint(&ps, 0);
            }
            if (!ps.failed) {
                if (gi->log) gi->log(gi->ctx, "offline: 14113 profile of %d/%d (%s) -> client 24113 (%u bytes)", hi, lo, own ? "own" : "other", (unsigned)ps.len);
                gi->deliver(gi->ctx, BMSG_PLAYER_PROFILE, 0, ps.data, ps.len);
            }
            bs_free(&ps);
        }
        /* cycle 5 (ISSUES A6): the battle end, decoded + full hex (evidence for trophies) */
        if (type == 14110 && gi->log) {
            const uint8_t *p = NULL;
            size_t len = 0;
            if (gi->msg_payload && gi->msg_payload(gi->ctx, msg, &p, &len) && p) {
                bmsg_battle_end be;
                int ok = bmsg_parse_14110(p, len, &be);
                gi->log(gi->ctx, "offline: 14110 battle end (%s, %u bytes): %d %d %d | sub %d %d | %d rows", ok ? "decoded" : "PARTLY decoded", (unsigned)len, be.v50,
                        be.v54, be.v58, be.s0, be.s1, be.n_rows);
                for (int i = 0; i < be.n_rows && i < BMSG_BE_ROWS; i++)
                    gi->log(gi->ctx, "offline:   14110 row %d: (%d,%d) (%d,%d) v %d flag %d num %d", i, be.row[i].c1, be.row[i].r1, be.row[i].c2, be.row[i].r2,
                            be.row[i].v, be.row[i].flag, be.row[i].num);
                /* update 96 (A6): the real layout + the battle result */
                bmsg_be2 b2;
                int ok2 = bmsg_parse_14110b(p, len, &b2);
                gi->log(gi->ctx, "offline: 14110 (%s): result %d, my team %d, v58 %d, location (%d,%d), %d players", ok2 ? "decoded" : "PARTLY decoded", b2.result,
                        b2.my_team, b2.v58, b2.loc_c, b2.loc_r, b2.n);
                int played = -1;
                for (int i = 0; i < b2.n; i++) {
                    gi->log(gi->ctx, "offline:   player %d: brawler (%d,%d) skin (%d,%d) team %d%s '%s'", i, b2.row[i].bc, b2.row[i].br, b2.row[i].sc, b2.row[i].sr,
                            b2.row[i].team, b2.row[i].local ? " LOCAL" : "", b2.row[i].name);
                    if (b2.row[i].local && b2.row[i].bc == 16 && played < 0) played = b2.row[i].br;
                }
                if (ok2 && st->cfg.battle_end_msg) {
                    int kind = b2.n >= 10 ? 2 : 0, delta;
                    if (kind == 2) {
                        static const int sd[10] = {10, 8, 6, 4, 0, -2, -4, -6, -8, -10};
                        int pl = b2.result < 1 ? 1 : (b2.result > 10 ? 10 : b2.result);
                        delta = sd[pl - 1];
                    } else {
                        delta = b2.result == 0 ? st->cfg.battle_win : b2.result == 1 ? st->cfg.battle_lose : st->cfg.battle_draw;
                    }
                    if (played < 0) played = st->cfg.brawler_row;
                    int bt = (played >= 0 && played < 96 && st->cfg.br_has[played]) ? st->cfg.br_tro[played] : st->cfg.trophies;
                    b2.rank_on = 1;   /* update 162: rank badge = this brawler's trophies / highest BEFORE the battle */
                    b2.tro_before = bt;
                    b2.best_before = (played >= 0 && played < 96 && st->cfg.br_has[played]) ? st->cfg.br_best[played] : bt;
                    if (bt + delta < 0) delta = -bt;   /* never below 0 trophies */
                    extern volatile int bv_tv_match __attribute__((weak));
                    if (st->cfg.battle_trophies && !(&bv_tv_match && bv_tv_match)) {   /* update 184: a watched Brawl TV match gives nothing */
                        if (played >= 0 && played < 96) {
                            st->cfg.br_has[played] = 1;
                            st->cfg.br_tro[played] = (short)(bt + delta);
                            if (st->cfg.br_tro[played] > st->cfg.br_best[played]) st->cfg.br_best[played] = st->cfg.br_tro[played];
                        }
                        st->cfg.account_trophies += delta;
                        if (st->cfg.account_trophies < 0) st->cfg.account_trophies = 0;
                        if (st->profile) {
                            int keep = st->profile->selected_brawler;
                            st->profile->selected_brawler = played;
                            bv_profile_battle(st->profile, delta, kind == 2 ? b2.result == 1 : b2.result == 0);
                            bv_profile_add_battle(st->profile, (long)time(NULL), kind, b2.result, delta, b2.loc_c == 15 ? b2.loc_r : -1, played);   /* update 140 (G7) */
                            {   /* update 180: length = from the first running frame of the match (loader) to this 14110 */
                                extern volatile long bv_match_t0 __attribute__((weak));
                                long t0 = &bv_match_t0 ? bv_match_t0 : 0, len = t0 ? (long)time(NULL) - t0 : 0;
                                st->profile->hist[(st->profile->n_hist - 1) % 10].len = len > 0 && len < 3600 ? (int)len : 0;
                            }
                            for (int q = 0; q < b2.n && q < BMSG_BE_ROWS; q++)   /* update 159: the players for the battle log */
                                {   /* update 170: bots arrive with their TID_BOT number as the name ("1".."9"): save the shown name instead */
                                    const char *nm = b2.row[q].name;
                                    extern uint32_t bv_botname_seed __attribute__((weak));
                                    if (nm && nm[0] >= '1' && nm[0] <= '9' && !nm[1] && &bv_botname_seed) nm = bv_botname(bv_botname_seed, nm[0] - '0');
                                    bv_profile_battle_player(st->profile, b2.row[q].bc == 16 ? b2.row[q].br : 0, b2.row[q].team, b2.row[q].local, nm);
                                }
                            st->profile->selected_brawler = keep;
                            st->profile->trophy_delta += delta;
                            profile_done(st, gi, "a battle");
                        }
                    }
                    /* update 100: trophies / highest / power per row (the local player from the profile values, bots like the local brawler) */
                    {
                        int lr = played >= 0 && played < 96 ? played : 0;
                        int ltro = st->cfg.br_has[lr] ? st->cfg.br_tro[lr] : st->cfg.trophies, lbest = st->cfg.br_has[lr] ? st->cfg.br_best[lr] : ltro;
                        int lpow = (st->cfg.br_has[lr] ? st->cfg.br_pow[lr] : st->cfg.power_level) + 1;
                        for (int i = 0; i < b2.n; i++) {
                            int r = b2.row[i].bc == 16 ? b2.row[i].br : -1, has = r >= 0 && r < 96 && st->cfg.br_has[r];
                            b2.row[i].tro = b2.row[i].local ? ltro : (has ? st->cfg.br_tro[r] : ltro);
                            b2.row[i].best = b2.row[i].local ? lbest : (has ? st->cfg.br_best[r] : lbest);
                            (void)has;
                            b2.row[i].pow = lpow;   /* update 162 (decode_bot_power.md): bots now play at the played brawler's level (loader/main.c bot level patch) */
                        }
                    }
                    /* Phase 3 (world_match=1): the planned world bots by name slot: their trophies / levels on the result rows, then the
                     * result moves their trophies like the player's */
                    if (st->world && st->plan_on) {
                        int team[BW_SLOTS] = {0}, used[BW_SLOTS] = {0}, nb = 0;
                        for (int i = 0; i < b2.n && i < BMSG_BE_ROWS; i++) {
                            const char *nm = b2.row[i].name;
                            if (b2.row[i].local || !nm || nm[0] < '1' || nm[0] > '9' || nm[1]) continue;
                            int sl = nm[0] - '0';
                            const bw_pick *pk = &st->plan[sl];
                            if (pk->bot < 0 || pk->bot >= st->world->n_bots) continue;
                            const bw_bot *bb = &st->world->bots[pk->bot];
                            team[sl] = b2.row[i].team; used[sl] = 1; nb++;
                            if (pk->slot >= 0 && pk->slot < st->world->n_br && bb->pow[pk->slot]) {
                                b2.row[i].tro = bb->tro[pk->slot]; b2.row[i].best = bb->best[pk->slot];
                                b2.row[i].pow = pk->level > 9 ? 9 : pk->level < 1 ? 1 : pk->level;
                            }
                        }
                        extern volatile int bv_tv_match __attribute__((weak));
                        bw_match_result(st->world, st->plan, team, used, kind, b2.result, b2.my_team, (uint32_t)time(NULL));
                        st->plan_on = 0;
                        if (st->world_save) { st->world_save(); st->world_saved = (uint32_t)time(NULL); }
                        gi->log(gi->ctx, "world: battle result -> %d world bots updated (kind %d, result %d%s)", nb, kind, b2.result,
                                &bv_tv_match && bv_tv_match ? ", Brawl TV" : "");
                    }
                    /* update 117 (E7): battle tokens from the pool -> Brawl Pass */
                    if (st->cfg.play_tokens && st->profile) {
                        int want = kind == 2 ? 22 - 2 * (b2.result < 1 ? 1 : b2.result > 10 ? 10 : b2.result) : b2.result == 0 ? 20 : b2.result == 2 ? 15 : 10;
                        long now = (long)time(NULL);
                        b2.tokens = bv_tokens_spend(st->profile, want, now);
                        st->profile->pass_tokens += b2.tokens;
                        st->cfg.pass_tokens = st->profile->pass_tokens > 0 ? st->profile->pass_tokens : 1;
                        st->cfg.home_a4 = st->profile->tokens;
                        st->cfg.home_a8 = bv_tokens_refill(st->profile, now);
                        profile_done(st, gi, "battle tokens");
                        gi->log(gi->ctx, "offline: tokens: +%d of %d (pool %d left, next +20 in %d s, Brawl Pass tokens %d)", b2.tokens, want, st->profile->tokens,
                                st->cfg.home_a8, st->profile->pass_tokens);
                    }
                    /* update 151 (req. 3 / 16): experience - 3v3 win 8 / draw 6 / loss 4, showdown 9 for 1st down to 1 (ESTIMATED 2020 values, not verified) */
                    {
                        int pl = b2.result < 1 ? 1 : (b2.result > 10 ? 10 : b2.result);
                        b2.xp = kind == 2 ? 10 - pl : (b2.result == 0 ? 8 : b2.result == 1 ? 4 : 6);
                        if (b2.xp < 1) b2.xp = 1;
                        b2.xp_before = st->profile ? st->profile->exp : 0;   /* update 162: XP bar start */
                        if (st->profile) { st->profile->exp += b2.xp; st->cfg.exp_total = st->profile->exp; profile_done(st, gi, "battle experience"); }
                        gi->log(gi->ctx, "offline: experience +%d (profile total %d)", b2.xp, st->profile ? st->profile->exp : -1);
                    }
                    b2.labels = st->cfg.battle_end_labels;   /* update 132 (A11) */
                    bv_bs bs;
                    bs_init(&bs);
                    bmsg_battle_end_msg(&bs, kind, b2.result, delta, &b2, st->cfg.account_hi, st->cfg.account_lo);
                    if (!bs.failed) {
                        gi->log(gi->ctx, "offline: -> client 23456 battle end (%u bytes): kind %d, result %d, trophies %+d for brawler row %d (now %d), account %d%s",
                                (unsigned)bs.len, kind, b2.result, delta, played, bt + delta, st->cfg.account_trophies, st->cfg.battle_trophies ? "" : " (battle_trophies=0: not kept)");
                        gi->deliver(gi->ctx, BMSG_BATTLE_END, 0, bs.data, bs.len);
                    }
                    bs_free(&bs);
                }
                char hex[3 * 128 + 1];
                for (size_t off = 0; off < len && off < 512; off += 128) {
                    size_t k = len - off < 128 ? len - off : 128, j;
                    for (j = 0; j < k; j++) {
                        static const char d[] = "0123456789abcdef";
                        hex[j * 3] = d[p[off + j] >> 4];
                        hex[j * 3 + 1] = d[p[off + j] & 15];
                        hex[j * 3 + 2] = ' ';
                    }
                    hex[k * 3 - 1] = 0;
                    gi->log(gi->ctx, "offline:   14110 bytes %u..: %s", (unsigned)off, hex);
                }
            }
        }
        if (type == 14363) {
            st->selected_map = picked;
            if (gi->log) gi->log(gi->ctx, "offline: 14363 selects map row %d", st->selected_map);
        }
        if (type == 14353 || type == 14363) send_msg(st, gi, M_AVATAR);
        /* cycle 1 (ISSUES E1): an event card tap sends 14362 (event id, slot), only when the client has a team object (mode_switch.md). The real server
         * answers with a 24124 carrying the event's map; Play follows that map (INFERRED). */
        if (type == 14362) {
            const uint8_t *p = NULL;
            size_t len = 0;
            int ev_id = -1, ev_slot = -1;
            if (gi->msg_payload && gi->msg_payload(gi->ctx, msg, &p, &len) && p && bmsg_parse_14362(p, len, &ev_id, &ev_slot)) {
                { extern volatile int bv_home_refresh_pending __attribute__((weak)); if (&bv_home_refresh_pending) bv_home_refresh_pending = 1; }   /* update 167 */
                int m = bmsg_map_for_slot(&st->cfg, ev_slot);
                if (m >= 0) {
                    st->selected_map = m;
                    st->cfg.home_map = m;   /* update 144 (req. 22): kept for 14353 and the next boot */
                    st->cfg.home_slot = ev_slot;   /* update 146 (T14) */
                    if (st->profile && st->profile->selected_slot != ev_slot) { st->profile->selected_slot = ev_slot; profile_done(st, gi, "selected event"); }
                }
                if (gi->log) gi->log(gi->ctx, "offline: 14362 event tap: event id %d, slot %d -> map row %d%s", ev_id, ev_slot, st->selected_map,
                                     m < 0 ? " (no event with that slot: map unchanged)" : "");
                /* update 131 (E4): the first tap of a new rotation event pays its tokens to the Brawl Pass */
                if (st->cfg.new_event_tokens > 0 && st->profile && ev_id > 0 && !bv_profile_event_claimed(st->profile, ev_id)) {
                    bv_profile_claim_event(st->profile, ev_id);
                    st->profile->pass_tokens += st->cfg.new_event_tokens;
                    st->cfg.pass_tokens = st->profile->pass_tokens;
                    for (int i = 0; i < 8; i++) if (st->cfg.event_id[i] == ev_id) st->cfg.ev_unclaimed[i] = 0;
                    profile_done(st, gi, "new event tokens");
                    if (gi->log) gi->log(gi->ctx, "offline: new event %d: +%d Brawl Pass tokens (now %d)", ev_id, st->cfg.new_event_tokens, st->profile->pass_tokens);
                }
            } else if (gi->log) {
                gi->log(gi->ctx, "offline: 14362 event tap: body not readable");
            }
            send_msg(st, gi, M_AVATAR);
        }
        /* cycle 1 experiment mode_push=ROW@N (default off): after the N-th keep-alive, a 24124 with map ROW. Pass = the Play button shows that mode
         * (decides whether 24124 drives the selected mode, mode_switch.md section 3) */
        if (type == 10108) {
            st->keepalives++;
            if (st->cfg.mode_push_map >= 0 && !st->mode_pushed && st->keepalives >= (uint32_t)(st->cfg.mode_push_after > 0 ? st->cfg.mode_push_after : 1)) {
                st->mode_pushed = 1;
                st->selected_map = st->cfg.mode_push_map;
                if (gi->log) gi->log(gi->ctx, "offline: mode_push experiment: 24124 with map row %d after keep-alive #%u", st->selected_map, (unsigned)st->keepalives);
                send_msg(st, gi, M_AVATAR);
            }
        }
        /* Play (update 32). Tapping session 2026-11-11: Play sends 14103 and the client then waits in matchmaking with the exit button grey; Circle sends
         * 14106 and nothing happened. There are no battles yet: 14103 gets a matchmaking status (20405) and 14106 the "cancelled" reply (20406), which
         * should bring the player back to the home screen. */
        if (type == 14103) send_msg(st, gi, M_MM_STATUS);
        if (type == 14106) send_msg(st, gi, M_MM_CANCELLED);
        /* update 50: a box being opened (client command 500 in 14102) waits for the server command 203 in 24111 with the box contents (boxes.md) */
        if (type == 14102 && gi->msg_payload) {
            const uint8_t *p = NULL;
            size_t len = 0;
            int tick = 0, first = -1;
            if (gi->msg_payload(gi->ctx, msg, &p, &len) && p && bmsg_parse_14102(p, len, &tick, &first) > 0) {
                /* cycle 6: brawler / skin selection -> the team member follows (home showed Shelly whatever was picked, run 79) */
                int sb = -1, ss = -1;
                if (st->cfg.member_view_on) {         /* cycle 6 experiment member_view (E2): the team member follows the viewed brawler */
                    int vr = bmsg_parse_view(p, len);
                    if (vr >= 0 && vr != (st->cfg.member_row >= 0 ? st->cfg.member_row : st->cfg.brawler_row)) {
                        st->cfg.member_row = vr;
                        if (gi->log) gi->log(gi->ctx, "offline: member_view: viewing brawler row %d -> 24124", vr);
                        send_msg(st, gi, M_AVATAR);
                    }
                }
                if (bmsg_parse_selection(p, len, &sb, &ss)) {
                    st->cfg.member_row = -1;
                    if (sb >= 0 && sb != st->cfg.brawler_row) {
                        st->cfg.brawler_row = sb;
                        st->cfg.sel_skin_row = st->profile && sb < 96 ? st->profile->skinsel[sb] : -1;   /* update 182: that brawler's saved skin */
                    }
                    if (ss >= 0) {
                        st->cfg.sel_skin_row = ss;
                        if (st->profile && st->cfg.brawler_row >= 0 && st->cfg.brawler_row < 96) st->profile->skinsel[st->cfg.brawler_row] = (short)ss;   /* update 182 */
                    }
                    if (gi->log) gi->log(gi->ctx, "offline: selection: brawler row %d, skin row %d -> 24124", st->cfg.brawler_row, st->cfg.sel_skin_row);
                    if (st->profile) { st->profile->selected_brawler = st->cfg.brawler_row; profile_done(st, gi, "a selection"); }
                    send_msg(st, gi, M_AVATAR);
                }
                {   /* update 181: hero level-ups (520) and new-event reveals (503 slot, 2) are kept in the profile */
                    static const int k_pp[8] = {20, 30, 50, 80, 130, 210, 340, 550}, k_coins[8] = {20, 35, 75, 140, 290, 480, 800, 1250};
                    bmsg_cmd cmds[32];
                    int nc = bmsg_parse_cmds(p, len, cmds, 32), changed = 0;
                    for (int q = 0; q < nc && st->profile; q++) {
                        if (cmds[q].id == 520 && cmds[q].cls == 16 && cmds[q].v[0] >= 0) {
                            int row = cmds[q].v[0];
                            bv_brawler *b = bv_profile_find(st->profile, row);
                            if (b && b->power >= 0 && b->power < 8) {
                                int lv = b->power, pp = k_pp[lv], co = k_coins[lv];
                                b->power++;
                                b->power_points = b->power_points > pp ? b->power_points - pp : 0;
                                st->profile->coins = st->profile->coins > co ? st->profile->coins - co : 0;
                                st->cfg.coins = st->profile->coins;
                                if (row < 96) { st->cfg.br_pow[row] = (signed char)b->power; st->cfg.br_pp[row] = (short)b->power_points; }
                                changed = 1;
                                if (gi->log) gi->log(gi->ctx, "offline: brawler row %d upgraded to power %d (-%d power points, -%d coins)", row, b->power + 1, pp, co);
                            }
                        } else if (cmds[q].id == 505 && cmds[q].cls == 28 && cmds[q].v[0] >= 0) {   /* update 182: player icon */
                            st->profile->thumbnail = cmds[q].v[0]; st->cfg.thumbnail_row = cmds[q].v[0]; changed = 1;
                            if (gi->log) gi->log(gi->ctx, "offline: player icon row %d kept", cmds[q].v[0]);
                        } else if (cmds[q].id == 527 && cmds[q].cls == 43 && cmds[q].v[0] >= 0) {   /* update 182: name colour */
                            st->profile->name_color = cmds[q].v[0]; st->cfg.name_color_row = cmds[q].v[0]; changed = 1;
                            if (gi->log) gi->log(gi->ctx, "offline: name colour row %d kept", cmds[q].v[0]);
                        } else if (cmds[q].id == 503 && cmds[q].v[1] == 2) {
                            int slot = cmds[q].v[0];
                            for (int i = 0; i < 8; i++) {
                                int sl = st->cfg.event_slot[i] > 0 ? st->cfg.event_slot[i] : 1 + i;
                                if (sl != slot) continue;
                                int ev_id = st->cfg.event_id[i];
                                if (ev_id > 0 && !bv_profile_event_claimed(st->profile, ev_id)) {
                                    bv_profile_claim_event(st->profile, ev_id);
                                    if (st->cfg.new_event_tokens > 0) { st->profile->pass_tokens += st->cfg.new_event_tokens; st->cfg.pass_tokens = st->profile->pass_tokens; }
                                    changed = 1;
                                    if (gi->log) gi->log(gi->ctx, "offline: new event %d revealed (slot %d, 503) - kept as seen", ev_id, slot);
                                }
                                st->cfg.ev_unclaimed[i] = 0;
                            }
                        }
                    }
                    if (changed) profile_done(st, gi, "upgrade / event reveal");
                }
                int box = bmsg_parse_box_open(p, len);
                if (box >= 0) {
                    bmsg_box_item it[BMSG_BOX_MAX];
                    int ni = bmsg_box_roll(box, (uint32_t)tick ^ (st->boxes_opened * 7919u), &st->cfg, st->cfg.box_coin_type, it);
                    /* update 63: mode 3 (default) = the box's 3D model id: sc3d/bbox_10 / 11 / 12 exist (run 56: unit 101 -> "sc3d/bbox_101.scw not found",
                     * the box did not render); Brawl Box (types 1-3) 10, Big Box (4) 11, Mega Box (5) 12 */
                    int unit = st->cfg.box_unit_mode == 1 ? box : st->cfg.box_unit_mode == 2 ? 101 :
                               st->cfg.box_unit_mode == 3 ? bmsg_box_unit(box) : 100 + box;
                    bv_bs s;
                    bs_init(&s);
                    bmsg_box_delivery(&s, unit, it, ni);
                    st->boxes_opened++;
                    if (gi->log)
                        gi->log(gi->ctx, "offline: box opened (type %d, #%u): -> client 24111 / command 203, unit %d: %d coins (item type %d), %d power points for brawler row %d",
                                box, (unsigned)st->boxes_opened, unit, it[0].amount, it[0].type, ni > 1 ? it[1].amount : 0, ni > 1 ? it[1].brawler : -1);
                    if (!s.failed) gi->deliver(gi->ctx, BMSG_SERVER_COMMAND, 0, s.data, s.len);
                    bs_free(&s);
                    if (st->profile) {           /* cycle 4: the client paid locally (Big Box 30 gems, Mega Box 80 gems); mirror it */
                        int unit_b = bmsg_box_unit(box);
                        st->profile->gems -= unit_b == 12 ? 30 : unit_b == 11 ? 80 : 0;
                        if (st->profile->gems < 0) st->profile->gems = 0;
                        profile_add_items(st, it, ni);
                        profile_done(st, gi, "a box");
                    }
                }
                /* update 65: Brawl Pass reward claim (command 517, run 60: Poco Starr claim loaded forever). Logged always; answered only with the
                 * experiment pass_claim_reply=1 (a minimal delivery: does the client wait for ANY 203?) */
                int pf[4];
                if (bmsg_parse_pass_claim(p, len, pf)) {
                    /* fields: type (9 PREMIUM / 10 FREE, brawl_pass_cycle6.md), ?, season, tier = milestones.csv Goal_<type>_<season>_<tier> (run 60: 9 0 2 0 = PocoStarr) */
                    bmsg_box_item it[BMSG_BOX_MAX] = {{st->cfg.box_coin_type, 1, -1, -1}};
                    int ni = 0, unmapped = -1;
                    if (st->cfg.pass_claim_reply >= 2 && st->pass)
                        ni = bv_pass_items(st->pass, pf[0], pf[2], pf[3], st->cfg.box_coin_type, st->cfg.brawler_row, st->cfg.power_level >= 8, it, BMSG_BOX_MAX, &unmapped);
                    if (ni == 0 && st->cfg.pass_claim_reply >= 1) { it[0].type = st->cfg.box_coin_type; it[0].amount = 1; it[0].brawler = -1; it[0].skin = -1; ni = 1; }
                    if (gi->log) {
                        gi->log(gi->ctx, "offline: pass reward claim (517): fields %d %d %d %d -> %d item(s)%s", pf[0], pf[1], pf[2], pf[3], ni,
                                !st->cfg.pass_claim_reply ? " (not answered: pass_claim_reply=0)" : (st->pass && st->cfg.pass_claim_reply >= 2) ? "" : " (1 coin: no reward tables)");
                        for (int k = 0; k < ni; k++)
                            gi->log(gi->ctx, "offline:   item %d: type %d amount %d skin row %d brawler row %d", k, it[k].type, it[k].amount, it[k].skin, it[k].brawler);
                        if (unmapped >= 0) gi->log(gi->ctx, "offline:   reward type %d is not decoded yet: sent 1 coin for it", unmapped);
                    }
                    if (ni > 0) {
                        bv_bs s;
                        bs_init(&s);
                        int unit = 100;
                        if (it[0].type >= 1000) {          /* cycle 6: a box reward -> open it like a bought box (unit = model) */
                            int rt = it[0].type - 1000, bt = rt == 10 ? 3 : rt == 14 ? 1 : 2;
                            unit = bmsg_box_unit(bt);
                            ni = bmsg_box_roll(bt, (uint32_t)(pf[3] * 7919 + pf[0]), &st->cfg, st->cfg.box_coin_type, it);
                            if (gi->log) gi->log(gi->ctx, "offline:   reward type %d = a box: opened as unit %d, %d coins", rt, unit, ni ? it[0].amount : 0);
                        }
                        bmsg_box_delivery_claim(&s, unit, it, ni, pf[0], pf[3] + 2, pf[2]);   /* cycle 6: the client marks the tier claimed */
                        if (!s.failed) gi->deliver(gi->ctx, BMSG_SERVER_COMMAND, 0, s.data, s.len);
                        bs_free(&s);
                        if (st->profile) {
                            bv_profile_mark_claimed(st->profile, pf[0], pf[2], pf[3]);
                            profile_add_items(st, it, ni);
                            profile_done(st, gi, "a pass claim");
                        }
                    }
                }
                /* update 64: offer purchase (command 519, user run 57: buying the skin loaded forever) -> 24111 / 203 with the offer's item */
                int oi = bmsg_parse_offer_buy(p, len);
                if (oi >= 0) {
                    bmsg_box_item it[BMSG_BOX_MAX];   /* update 146: up to 8 items */
                    int ni = bmsg_offer_items(oi, &st->cfg, it);
                    if (gi->log) gi->log(gi->ctx, "offline: offer %d bought: -> client 24111 / command 203, unit %d, %d item(s), first type %d amount %d skin row %d", oi,
                                         st->cfg.offer_unit, ni, ni ? it[0].type : -1, ni ? it[0].amount : 0, ni ? it[0].skin : -1);
                    if (ni > 0) {
                        bv_bs s;
                        bs_init(&s);
                        bmsg_box_delivery(&s, bmsg_offer_unit(oi, &st->cfg, st->cfg.offer_unit), it, ni);   /* cycle 6: box offers open as boxes */
                        if (!s.failed) gi->deliver(gi->ctx, BMSG_SERVER_COMMAND, 0, s.data, s.len);
                        bs_free(&s);
                        if (st->profile) {       /* cycle 4: offer 0 = 15 gems (skin), offer 1 = 100 coins (power points) */
                            if (st->cfg.n_offers > 0 && oi < st->cfg.n_offers && oi < BMSG_MAX_OFFERS) {   /* cycle 5: generated table */
                                const bmsg_offer *o = &st->cfg.offers[oi];
                                if (o->kind == st->cfg.offer_cur) st->profile->coins -= o->price;
                                else st->profile->gems -= o->price;
                            } else if (oi == 0) st->profile->gems -= st->cfg.skin_offer_price;
                            else if (oi == 1) st->profile->coins -= 100;
                            if (st->profile->gems < 0) st->profile->gems = 0;
                            if (st->profile->coins < 0) st->profile->coins = 0;
                            /* update 103 (B13/B14): stable id for generated offers; the live mask too (a 24101 after a match showed it unbought) */
                            bv_profile_mark_bought(st->profile, st->day, (st->cfg.n_offers > 0 && oi < st->cfg.n_offers) ? bmsg_offer_id(&st->cfg.offers[oi]) : oi);
                            if (oi < BMSG_MAX_OFFERS) st->cfg.offer_bought_mask |= 1 << oi;
                            profile_add_items(st, it, ni);
                            profile_done(st, gi, "an offer");
                        }
                    }
                }
            }
        }
        return 0;
    }
    if (type == 14109 && gi->log && n <= 3) gi->log(gi->ctx, "offline: 14109 reaches a helper of the original mod that is not reproduced");
    send_msg(st, gi, M_HOME);
    send_msg(st, gi, M_AVATAR);
    if (type == 14353) st->selected_map = bmsg_home_map(&st->cfg); /* the original resets to its default; update 144: the selected event */
    if (st->cfg.ack24125) send_msg(st, gi, M_ACK);
    if (type == 14363) {
        st->selected_map = picked;
        if (gi->log) gi->log(gi->ctx, "offline: 14363 selects map row %d", st->selected_map);
    }
    send_msg(st, gi, M_AVATAR);
    return 0;
}
