/* Host test of the offline logic (loader/offline_b.c) with a fake game, and of the ARM encoders (loader/armenc.h).
 * With the real libg.so (argument 1) it also checks that the words at the patched addresses are what hook_b.c expects and that
 * the branch we would write decodes back to the intended target.
 *
 * Build: cc -Iloader tests/host_offline_test.c loader/offline_b.c loader/bmsg.c loader/bytestream.c -o host_offline_test
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "armenc.h"
#include "offline_b.h"

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); g_fail++; } } while (0)

typedef struct {
    int type;     /* type of the next outgoing message */
    uint8_t body[16];
    size_t body_len;
    int encoded;
    char events[64][24];
    int nev;
    int delivered_type[32];
    int delivered_flag[32];
    size_t delivered_len[32];
    uint8_t last24124[256];
    size_t last24124_len;
    int nd;
    int reenter;      /* the fake client answers every delivery with a new request */
    int locks, unlocks;
    bvb_state *st;
    const bvb_game_if *gi;
} fake;

static void ev(fake *f, const char *s) {
    if (f->nev < 64) snprintf(f->events[f->nev++], 24, "%s", s);
}
static int f_type(void *c, void *m) { (void)m; return ((fake *)c)->type; }
static void f_encode(void *c, void *m) { (void)m; ((fake *)c)->encoded++; ev((fake *)c, "encode"); }
static int f_payload(void *c, void *m, const uint8_t **p, size_t *n) {
    (void)m;
    fake *f = (fake *)c;
    if (!f->body_len || !f->encoded) return 0; /* the body exists only after encode() */
    *p = f->body;
    *n = f->body_len;
    return 1;
}
static void f_deliver(void *c, int type, int flag, const uint8_t *d, size_t n) {
    fake *f = (fake *)c;
    if (f->reenter && f->nd < 31) {
        f->nd++;
        bvb_handle_send(f->st, f->gi, (void *)1);
        f->nd--;
    }
    char b[24];
    snprintf(b, sizeof b, "deliver %d", type);
    ev(f, b);
    if (f->nd < 32) {
        f->delivered_type[f->nd] = type;
        f->delivered_flag[f->nd] = flag;
        f->delivered_len[f->nd] = n;
        f->nd++;
    }
    if (type == 24124 && n <= sizeof f->last24124) {
        memcpy(f->last24124, d, n);
        f->last24124_len = n;
    }
}
static void f_prepare(void *c) { ev((fake *)c, "login_prepare"); }
static void f_done(void *c) { ev((fake *)c, "login_done"); }
static void f_after(void *c) { ev((fake *)c, "after_login"); }
static int f_rows(void *c, int t) { (void)c; return t == 29 ? 4 : t == 52 ? 3 : -1; }
static void f_log(void *c, const char *fmt, ...) { (void)c; (void)fmt; }
static uint32_t mem_rd(void *c, uint32_t a) { return ((uint32_t *)c)[a / 4]; }
static void mem_wr(void *c, uint32_t a, uint32_t v) { ((uint32_t *)c)[a / 4] = v; }
static void f_lock(void *c) { ((fake *)c)->locks++; }
static void f_unlock(void *c) { ((fake *)c)->unlocks++; }

static void setup(fake *f, bvb_game_if *gi, bvb_state *st) {
    memset(f, 0, sizeof *f);
    memset(gi, 0, sizeof *gi);
    gi->ctx = f;
    gi->msg_type = f_type;
    gi->msg_encode = f_encode;
    gi->msg_payload = f_payload;
    gi->deliver = f_deliver;
    gi->login_prepare = f_prepare;
    gi->login_done = f_done;
    gi->after_login = f_after;
    gi->table_rows = f_rows;
    gi->log = f_log;
    gi->lock = f_lock;
    gi->unlock = f_unlock;
    f->st = st;
    f->gi = gi;
    bvb_init(st, gi);
}

static int seq(const fake *f, const char *const *want, int n) {
    if (f->nev != n) return 0;
    for (int i = 0; i < n; i++)
        if (strcmp(f->events[i], want[i])) return 0;
    return 1;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

int main(int argc, char **argv) {
    fake f;
    bvb_game_if gi;
    bvb_state st;

    /* 1. the first message (login) */
    setup(&f, &gi, &st);
    f.type = 10100;
    CHECK(bvb_handle_send(&st, &gi, (void *)1) == 0, "returns 0");
    static const char *const login[] = {"login_prepare", "deliver 20104", "login_done", "deliver 24101", "deliver 24124", "after_login"};
    CHECK(seq(&f, login, 6), "login sequence wrong (%d events, first '%s')", f.nev, f.nev ? f.events[0] : "");
    CHECK(f.delivered_flag[0] == 1 && f.delivered_flag[1] == 0 && f.delivered_flag[2] == 0, "only LoginOk carries flag 1");
    CHECK(f.encoded == 0, "the login message itself is not encoded (as the original)");
    CHECK(f.delivered_len[0] > 100 && f.delivered_len[1] > 100 && f.delivered_len[2] > 20, "message sizes");

    /* 2. update 17 default: after the login NO OwnHomeData. KeepAlive (10108) and every other periodic message get the 4-byte ack only
     * (a 24101 on every KeepAlive ended in a use-after-free on the console, run of 2026-11-11) */
    setup(&f, &gi, &st);
    CHECK(st.resend_home == 0, "the default does not resend the home (%d)", st.resend_home);
    {
        static const int periodic[] = {10108, 10110, 10111, 14361, 14366, 14102, 14350};
        static const char *const ack_only[] = {"encode", "deliver 24125"};
        for (unsigned i = 0; i < sizeof periodic / sizeof periodic[0]; i++) {
            f.type = periodic[i];
            f.nev = 0;
            f.nd = 0;
            bvb_handle_send(&st, &gi, (void *)1);
            CHECK(seq(&f, ack_only, 2), "default: message %d gets the ack only (%d events, first '%s')", periodic[i], f.nev, f.nev ? f.events[0] : "");
            for (int e = 0; e < f.nev; e++) CHECK(strcmp(f.events[e], "deliver 24101") != 0, "default: message %d must not deliver 24101", periodic[i]);
        }
        CHECK(f.delivered_len[0] == 4, "ack has 4 bytes");
    }

    /* update 102: total power points */
    CHECK(bmsg_pp_total(0, 5) == 5 && bmsg_pp_total(2, 32) == 82 && bmsg_pp_total(8, 0) == 1410, "pp totals (%d %d %d)", bmsg_pp_total(0, 5), bmsg_pp_total(2, 32), bmsg_pp_total(8, 0));

    /* 2z. update 96: the real 14110 of run 110 (3v3 bot match) and the 23456 built from it */
    {
        static const uint8_t b14110[] = {0x00,0x01,0x00,0x0f,0x07,0x06,0x10,0x1a,0x00,0x01,0x01,0x00,0x00,0x00,0x06,'P','l','a','y','e','r',
            0x10,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x01,'1',0x10,0x23,0x00,0x00,0x00,0x00,0x00,0x00,0x01,'2',0x10,0x1e,0x00,0x00,0x00,0x00,0x00,0x00,0x01,'3',
            0x10,0x1c,0x00,0x01,0x00,0x00,0x00,0x00,0x01,'4',0x10,0x1b,0x00,0x01,0x00,0x00,0x00,0x00,0x01,'5'};
        bmsg_be2 b2;
        CHECK(bmsg_parse_14110b(b14110, sizeof b14110, &b2) == 1, "run 110 14110 decodes fully");
        CHECK(b2.result == 0 && b2.my_team == 1 && b2.loc_c == 15 && b2.loc_r == 7 && b2.n == 6, "14110 header (%d %d (%d,%d) %d)", b2.result, b2.my_team, b2.loc_c, b2.loc_r, b2.n);
        CHECK(b2.row[0].bc == 16 && b2.row[0].br == 26 && b2.row[0].local == 1 && b2.row[0].team == 1 && !strcmp(b2.row[0].name, "Player"), "local player row");
        CHECK(b2.row[5].br == 27 && b2.row[5].team == 1 && !strcmp(b2.row[5].name, "5") && b2.row[1].team == 0, "bot rows");
        bv_bs s;
        bs_init(&s);
        {   /* update 114: 24113 profile */
            bmsg_cfg pc;
            bmsg_default_cfg(&pc);
            bv_bs ps;
            bs_init(&ps);
            bmsg_profile_msg(&ps, &pc, 0, 1, NULL, 3);
            int rows[64];
            int nb = bmsg_owned_brawler_rows(&pc, rows, 64);
            CHECK(!ps.failed && ps.data[0] == 0 && ps.data[1] == 1 && ps.data[2] == 0 && (int)ps.data[3] == nb, "24113 head: id 0/1, null dataref, %d heroes", nb);
            CHECK(ps.len > 20 && ps.data[ps.len - 1] == 0 && ps.data[ps.len - 2] == 0, "24113 tail: no club, null dataref");
            bs_free(&ps);
        }
        bmsg_battle_end_msg(&s, 0, 0, 8, &b2, 0, 7);
        /* 17 VInts (all one byte here: 0,0,0,8,0...), then the packed bool byte with bit 4 = 0x10, VInt -1 = 0x7f (sign bit 6 + 0x3f), bool byte 0, VInt 6 */
        CHECK(!s.failed && s.len > 30 && s.data[3] == 8 && s.data[17] == 0x10 && s.data[18] == 0x7f && s.data[19] == 0 && s.data[20] == 6, "23456 head (%02x %02x %02x %02x)",
              s.data[17], s.data[18], s.data[19], s.data[20]);
        CHECK(s.data[21] == 1 && s.data[22] == 16 && s.data[23] == 26, "23456 first row: local player bit (featured slot), brawler (16,26)");
        /* row 0: skin DataRef 0 (24), 3 VInts (25..27), has-long bool byte 1 (28), Long 0 / 7 (29..36), name length 6 (37..40) */
        CHECK(s.data[24] == 0 && s.data[28] == 1 && s.data[36] == 7 && s.data[40] == 6 && s.data[41] == 'P', "23456 row 0 long + name (%02x %02x %02x)", s.data[28], s.data[36], s.data[40]);
        bs_free(&s);
        setup(&f, &gi, &st);
        st.cfg.battle_end_msg = 1;
        st.cfg.home_on_14109 = 1;
        f.type = 14101;
        bvb_handle_send(&st, &gi, (void *)1);
        int home = 0;
        for (int e = 0; e < f.nev; e++) home |= !strcmp(f.events[e], "deliver 24101");
        CHECK(home, "14101 after a battle gets the home (battle_end_msg)");
    }

    /* 2a. update 89: 14109 (leave the training cave) gets nothing by default, 24101 + 24124 with home_on_14109 */
    setup(&f, &gi, &st);
    f.type = 14109;
    bvb_handle_send(&st, &gi, (void *)1);
    {
        int home = 0;
        for (int e = 0; e < f.nev; e++) home |= !strcmp(f.events[e], "deliver 24101");
        CHECK(!home, "default: 14109 does not deliver 24101");
    }
    setup(&f, &gi, &st);
    st.cfg.home_on_14109 = 1;
    f.type = 14109;
    bvb_handle_send(&st, &gi, (void *)1);
    static const char *const back_home[] = {"encode", "deliver 24125", "deliver 24101", "deliver 24124"};
    CHECK(seq(&f, back_home, 4), "home_on_14109: 14109 -> ack, 24101, 24124 (%d events)", f.nev);

    /* 2b. resend_home=1 (config): the original mod's answer to any later message: release, 24101, 24124, 24125, 24124 */
    setup(&f, &gi, &st);
    st.resend_home = 1;
    f.type = 10108;
    bvb_handle_send(&st, &gi, (void *)1);
    static const char *const later[] = {"encode", "deliver 24101", "deliver 24124", "deliver 24125", "deliver 24124"};
    CHECK(seq(&f, later, 5), "later-message sequence wrong");
    CHECK(f.delivered_len[2] == 4, "ack has 4 bytes");

    /* 2c. default: a map selection gets the ack and a 24124 with the picked row, still no 24101 */
    setup(&f, &gi, &st);
    f.type = 14363;
    {
        bv_bs q;
        bs_init(&q);
        bs_vint(&q, 3);
        bs_vint(&q, 12);
        memcpy(f.body, q.data, q.len);
        f.body_len = q.len;
        bs_free(&q);
    }
    bvb_handle_send(&st, &gi, (void *)1);
    static const char *const pick[] = {"encode", "deliver 24125", "deliver 24124"};
    CHECK(seq(&f, pick, 3), "default 14363 sequence wrong (%d events)", f.nev);
    CHECK(st.selected_map == 12, "default 14363: selected map %d", st.selected_map);
    {
        bv_bs want;
        bs_init(&want);
        bmsg_avatar_update(&want, &st.cfg, 12);
        CHECK(f.last24124_len == want.len && !memcmp(f.last24124, want.data, want.len), "default 14363: the 24124 carries the new map row");
        bs_free(&want);
    }
    f.type = 14353;
    f.nev = 0;
    f.nd = 0;
    f.body_len = 0;
    bvb_handle_send(&st, &gi, (void *)1);
    CHECK(seq(&f, pick, 3) && st.selected_map == st.cfg.map_row, "default 14353: ack + 24124, selection reset (%d)", st.selected_map);

    /* 2d. update 32: Play (14103) -> ack + matchmaking status 20405 (5 Ints + 1 Boolean = 21 bytes); cancel (14106) -> ack + empty 20406 */
    setup(&f, &gi, &st);
    f.type = 14103;
    bvb_handle_send(&st, &gi, (void *)1);
    static const char *const play[] = {"encode", "deliver 24125", "deliver 20405"};
    CHECK(seq(&f, play, 3), "14103 sequence wrong (%d events, last '%s')", f.nev, f.nev ? f.events[f.nev - 1] : "");
    CHECK(f.nd >= 2 && f.delivered_len[1] == 21 && f.delivered_flag[1] == 0, "20405 has 21 bytes (%u)", f.nd >= 2 ? (unsigned)f.delivered_len[1] : 0u);
    f.type = 14106;
    f.nev = 0;
    f.nd = 0;
    bvb_handle_send(&st, &gi, (void *)1);
    static const char *const cancel[] = {"encode", "deliver 24125", "deliver 20406"};
    CHECK(seq(&f, cancel, 3), "14106 sequence wrong (%d events)", f.nev);
    CHECK(f.nd >= 2 && f.delivered_len[1] == 0, "20406 has an empty body");

    /* 2e. update 33: six menu events, one map per mode (locations rows 7, 13, 34, 18, 4, 24) */
    {
        bmsg_cfg c;
        bmsg_default_cfg(&c);
        bv_bs one, six;
        bs_init(&one);
        bs_init(&six);
        bmsg_own_home_data(&one, &c, NULL);
        c.events = 6;
        bmsg_own_home_data(&six, &c, NULL);
        static const uint8_t ids[] = {7, 0, 1, 2, 8, 4, 6, 3}; /* update 34: real slot ids, no slot 5 */
        int found_ids = 0, maps_in_order = 1;
        for (size_t i = 0; i + sizeof ids <= six.len; i++)
            if (!memcmp(six.data + i, ids, sizeof ids)) found_ids = 1;
        static const int rows[6] = {7, 13, 34, 18, 4, 24};
        size_t at = 0;
        for (int k = 0; k < 6 && maps_in_order; k++) {
            const uint8_t pat[2] = {15, (uint8_t)rows[k]};
            size_t j = at;
            while (j + 2 <= six.len && memcmp(six.data + j, pat, 2)) j++;
            if (j + 2 > six.len) maps_in_order = 0;
            else at = j + 2;
        }
        CHECK(six.len > one.len && found_ids, "6 events: slot list 0..6 present, message grows (%u -> %u)", (unsigned)one.len, (unsigned)six.len);
        CHECK(maps_in_order, "6 events: map references 7, 13, 34, 18, 4, 24 appear in order");
        bs_free(&one);
        bs_free(&six);
    }
    /* 2f. update 34: owned brawlers. Default = one brawler (row 0) with its unlock card (table 23 row 0); unlock_all = 39 brawlers, each card once;
     * the selected brawler carries the trophies */
    {
        bmsg_cfg c;
        bmsg_default_cfg(&c);
        bv_bs one, all;
        bs_init(&one);
        bs_init(&all);
        bmsg_own_home_data(&one, &c, NULL);
        c.unlock_all = 1;
        c.trophies = 1000;
        c.brawler_row = 2;
        bmsg_own_home_data(&all, &c, NULL);
        static const uint8_t card0[] = {1, 23, 0, 1}; /* count 1, DataRef(23, 0), 1 */
        int has_card0 = 0;
        for (size_t i = 0; i + sizeof card0 <= one.len; i++)
            if (!memcmp(one.data + i, card0, sizeof card0)) has_card0 = 1;
        CHECK(has_card0, "default: Shelly's unlock card (table 23 row 0) is listed with count 1");
        /* unlock_all: the card list = count 39, then (23, row, 1) x 39 starting with Bull's card (row 8, the selected brawler 2) */
        static const uint8_t card_head[] = {39, 23, 8, 1, 23, 0, 1};
        int has_all = 0;
        for (size_t i = 0; i + sizeof card_head <= all.len; i++)
            if (!memcmp(all.data + i, card_head, sizeof card_head)) has_all = 1;
        CHECK(has_all, "unlock_all: 39 unlock cards, the selected brawler's first");
        /* trophies 1000 = VInt 0xa8 0x0f after DataRef(16, 2) in the first per-brawler list */
        static const uint8_t tro[] = {39, 16, 2, 0xa8, 0x0f};
        int has_tro = 0;
        for (size_t i = 0; i + sizeof tro <= all.len; i++)
            if (!memcmp(all.data + i, tro, sizeof tro)) has_tro = 1;
        CHECK(has_tro, "unlock_all: the trophy list has 39 entries and the selected brawler has 1000 trophies");
        /* update 35: the first card list = 39 cards + 4 resources = 43, Bull's card first; the account trophies are the 3rd and 4th value */
        static const uint8_t first[] = {43, 23, 8, 1, 23, 0, 1};
        int has_first = 0;
        for (size_t i = 0; i + sizeof first <= all.len; i++)
            if (!memcmp(all.data + i, first, sizeof first)) has_first = 1;
        CHECK(has_first, "unlock_all: the first card list carries the 39 unlock cards (count 43)");
        bmsg_cfg c2 = c;
        c2.account_trophies = 1000;
        c2.power_level = 8;
        bv_bs t;
        bs_init(&t);
        bmsg_own_home_data(&t, &c2, NULL);
        static const uint8_t head[] = {0, 0, 0xa8, 0x0f, 0xa8, 0x0f, 0};
        CHECK(t.len > sizeof head && !memcmp(t.data, head, sizeof head), "account trophies 1000 are the 3rd and 4th value of 24101");
        static const uint8_t lvl[] = {39, 16, 2, 8};
        int has_lvl = 0;
        for (size_t i = 0; i + sizeof lvl <= t.len; i++)
            if (!memcmp(t.data + i, lvl, sizeof lvl)) has_lvl = 1;
        CHECK(has_lvl, "power level 8 in the last per-brawler list");
        bs_free(&t);
        bs_free(&one);
        bs_free(&all);
    }

    /* 2h. update 38: Brawl Pass season entry and the packed flag byte. The four list counts follow the two strings (name, then null = Int -1). */
    {
        bmsg_cfg c;
        bmsg_default_cfg(&c);
        c.player_name = "P";
        bv_bs off, on;
        bs_init(&off);
        bs_init(&on);
        bmsg_own_home_data(&off, &c, NULL);
        c.brawl_pass = 1;
        c.pass_season = 2;
        c.pass_tokens = 100;
        c.pass_premium = 1;
        c.home_flags = 0;
        bmsg_own_home_data(&on, &c, NULL);
        /* "P" = Int 1 + 'P', then null string Int -1, then 0, 0, count 1, season 2, tokens 100 (VInt 0xa4 0x01), premium 1, 0, 0, 0, then list 4 = 0 */
        /* update 39: the last two Booleans share one byte (bit-packed reader) */
        static const uint8_t bp[] = {0, 0, 0, 1, 'P', 0xff, 0xff, 0xff, 0xff, 0, 0, 1, 2, 0xa4, 0x01, 1, 0, 0, 0, 1};
        int has_bp = 0;
        for (size_t i = 0; i + sizeof bp <= on.len; i++)
            if (!memcmp(on.data + i, bp, sizeof bp)) has_bp = 1;
        CHECK(has_bp, "Brawl Pass: one season entry (2, 100 tokens, premium) after the name strings");
        CHECK(on.len == off.len + 6, "Brawl Pass adds 6 bytes: season 1, tokens 2 (VInt 100), premium 1, VInt 1, packed Bools 1 (%u -> %u)", (unsigned)off.len,
              (unsigned)on.len);
        bs_free(&off);
        bs_free(&on);
    }

    /* 2i. update 42: config feature switches (Int pairs). Default reference = 3 pairs with 5 -> 1; shop_disabled 0 -> 5 -> 0; config_keys adds / overrides */
    {
        bmsg_cfg c;
        bmsg_default_cfg(&c);
        bv_bs a, b;
        bs_init(&a);
        bs_init(&b);
        bmsg_own_home_data(&a, &c, NULL);
        /* cycle 6: key 37 defaults to 0 (shows the BRAWL PASS home button, run 86 VERIFIED); the mod sent 1 */
        static const uint8_t ref[] = {0, 0, 0, 37, 0, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0, 1};
        int has_ref = 0;
        for (size_t i = 0; i + sizeof ref <= a.len; i++)
            if (!memcmp(a.data + i, ref, sizeof ref)) has_ref = 1;
        CHECK(has_ref, "default: switch 37 = 0 (pass button), 5 = 1 (as the mod)");
        c.shop_disabled = 0;
        c.n_extra_keys = 2;
        c.extra_key[0][0] = 18;
        c.extra_key[0][1] = 1;
        c.extra_key[1][0] = 37;
        c.extra_key[1][1] = 0;
        bmsg_own_home_data(&b, &c, NULL);
        static const uint8_t got[] = {4, 0, 0, 0, 1};                                          /* count 4, then key 1 */
        static const uint8_t tail[] = {0, 0, 0, 37, 0, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0, 0, 0, 0, 0, 18, 0, 0, 0, 1}; /* 37 -> 0 (override), 5 -> 0, 18 -> 1 (extra) */
        int has_got = 0, has_tail = 0;
        for (size_t i = 0; i + sizeof got <= b.len; i++)
            if (!memcmp(b.data + i, got, sizeof got)) has_got = 1;
        for (size_t i = 0; i + sizeof tail <= b.len; i++)
            if (!memcmp(b.data + i, tail, sizeof tail)) has_tail = 1;
        CHECK(has_got && has_tail && b.len == a.len + 8, "shop_disabled 0 + config_keys 18:1,37:0: 4 pairs, 37 -> 0, 5 -> 0, 18 -> 1 (%u -> %u)",
              (unsigned)a.len, (unsigned)b.len);
        bs_free(&a);
        bs_free(&b);
    }

    /* 2j. update 43: labelling shop offers. One offer = type, 9 marker VInts (22..55 one byte, 66..110 two bytes), packed Bools byte, DataRef(16,0),
     * 121, 132, 143 (two bytes each) = 1 + 14 + 1 + 2 + 6 = 24 bytes */
    {
        bmsg_cfg c;
        bmsg_default_cfg(&c);
        bv_bs a, b;
        bs_init(&a);
        bs_init(&b);
        bmsg_own_home_data(&a, &c, NULL);
        c.offer_test = 2;
        c.offer_type[0] = 3;
        c.offer_type[1] = 9;
        bmsg_own_home_data(&b, &c, NULL);
        /* update 45: 10 markers (i+1)*10001 (3 bytes each as VInt), Bools byte, DataRef(16,0), type (+0x30), 120012, 130013 (3 bytes each)
         * = 30 + 1 + 2 + 1 + 6 = 40 bytes. VInt 10001 = 0x91 0x9c 0x01, 120012 = 0x8c 0xd3 0x0e, 130013 = 0x9d 0xef 0x0f */
        static const uint8_t o1[] = {2, 0x91, 0x9c, 0x01};                                                    /* count 2, first marker */
        static const uint8_t o2[] = {0, 16, 0, 3, 0x8c, 0xd3, 0x0e, 0x9d, 0xef, 0x0f, 0x91, 0x9c, 0x01};       /* end of offer 1, offer 2 starts */
        static const uint8_t o3[] = {0, 16, 0, 9, 0x8c, 0xd3, 0x0e, 0x9d, 0xef, 0x0f};                         /* end of offer 2: type 9 */
        int h1 = 0, h2 = 0, h3 = 0;
        for (size_t i = 0; i + sizeof o1 <= b.len; i++)
            if (!memcmp(b.data + i, o1, sizeof o1)) h1 = 1;
        for (size_t i = 0; i + sizeof o2 <= b.len; i++)
            if (!memcmp(b.data + i, o2, sizeof o2)) h2 = 1;
        for (size_t i = 0; i + sizeof o3 <= b.len; i++)
            if (!memcmp(b.data + i, o3, sizeof o3)) h3 = 1;
        CHECK(h1 && h2 && h3 && b.len == a.len + 80, "2 labelling offers, 40 bytes each, item type in +0x30 (%u -> %u)", (unsigned)a.len, (unsigned)b.len);
        bs_free(&a);
        bs_free(&b);
    }

    /* 2l. update 56: gems / coins (avatar resource list 0: DataRef(5,0) gems first, coins row 1) and real shop offers at home +0x9c */
    {
        bmsg_cfg c;
        bmsg_default_cfg(&c);
        CHECK(c.gems == -1 && c.coins == -1 && c.shop_offers == 0, "reference defaults keep the mod's message (no gems entry, coins 0, no offers)");
        bv_bs a, b, o;
        bs_init(&a);
        bs_init(&b);
        bs_init(&o);
        bmsg_own_home_data(&a, &c, NULL);
        c.gems = 1000;
        c.coins = 5000;
        bmsg_own_home_data(&b, &c, NULL);
        /* + gems entry 05 00 [1000 = 2 bytes] = 4 bytes, coins 0 -> 5000 = +1 byte */
        CHECK(b.len == a.len + 5, "gems 1000 + coins 5000 add 5 bytes (%u -> %u)", (unsigned)a.len, (unsigned)b.len);
        uint8_t g[4];
        bv_bs t;
        bs_init(&t);
        bs_vint(&t, 5); bs_vint(&t, 0); bs_vint(&t, 1000);
        memcpy(g, t.data, 4);
        bs_free(&t);
        int hit = 0;
        for (size_t i = 0; i + 4 <= b.len; i++)
            if (!memcmp(b.data + i, g, 4)) hit = 1;
        CHECK(hit, "the gems entry DataRef(5,0) = 1000 is in the message");
        c.shop_offers = 2;
        bmsg_own_home_data(&o, &c, NULL);
        /* offer 1 (skin): items 5 + kind 1 + price 1 + 172800 (3) + 2 + bool + 1 + bool + old 1 + Int 4 + null string 4 + bool + null string 4 + -1 (1)
         * + bool + 2 = 33; offer 2 (power points): items 7 (amount 100 and DataRef(16,1) 2 bytes each) + price 2 + old 2 ... = 37 */
        CHECK(o.len == b.len + 70, "two shop offers add 33 + 37 bytes (%u -> %u)", (unsigned)b.len, (unsigned)o.len);
        bs_free(&a);
        bs_free(&b);
        bs_free(&o);
    }

    /* 2m. update 64 / cycle 1: offer purchase (519), box model ids, no power points when maxed, skin items, 14362 event tap */
    {
        /* the real 14102 of run 57: tick 261, 1 command 519 (87 08), base 84 04 84 04 7f 7f, index 0, DataRef null */
        static const uint8_t buy[] = {0x00, 0x85, 0x04, 0x00, 0x01, 0x87, 0x08, 0x84, 0x04, 0x84, 0x04, 0x7f, 0x7f, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff};
        CHECK(bmsg_parse_offer_buy(buy, sizeof buy) == 0, "519 from run 57 = offer index 0 (%d)", bmsg_parse_offer_buy(buy, sizeof buy));
        static const uint8_t box1[] = {0x00, 0x99, 0x12, 0x00, 0x01, 0xb4, 0x07, 0x98, 0x12, 0x98, 0x12, 0x7f, 0x7f, 0x01, 0xff, 0xff, 0xff, 0xff};
        CHECK(bmsg_parse_offer_buy(box1, sizeof box1) == -1 && bmsg_parse_box_open(box1, sizeof box1) == 1, "a box command is not a purchase; run 57 Big Box = type 1");
        CHECK(bmsg_box_unit(1) == 12 && bmsg_box_unit(4) == 12 && bmsg_box_unit(3) == 11 && bmsg_box_unit(5) == 11 && bmsg_box_unit(2) == 10,
              "box model ids (update 65, runs 57 + 60): Big (type 1) -> 12, Mega (type 3) -> 11, Brawl -> 10");
        /* run 60: the Poco Starr claim = command 517 with 9, 0, 2, 0 */
        static const uint8_t claim[] = {0x00, 0x90, 0x30, 0x00, 0x01, 0x85, 0x08, 0x8f, 0x30, 0x8f, 0x30, 0x7f, 0x7f, 0x09, 0x00, 0x02, 0x00, 0xff, 0xff, 0xff, 0xff};
        int pf[4] = {-1, -1, -1, -1};
        CHECK(bmsg_parse_pass_claim(claim, sizeof claim, pf) == 1 && pf[0] == 9 && pf[1] == 0 && pf[2] == 2 && pf[3] == 0, "517 claim from run 60 parses to 9 0 2 0");
        CHECK(bmsg_parse_pass_claim(buy, sizeof buy, pf) == 0, "a 519 is not a pass claim");
        bmsg_cfg c;
        bmsg_default_cfg(&c);
        bmsg_box_item it[4];
        c.power_level = 8;
        int ni = bmsg_box_roll(1, 1234, &c, 7, it);
        CHECK(ni == 1 && it[0].type == 7 && it[0].amount >= 2 * 3 * 20 && it[0].amount <= 2 * 3 * 60, "maxed brawlers: Big Box (type 1) = coins only, x3 doubled (%d items, %d coins)", ni, it[0].amount);
        c.power_level = 5;
        ni = bmsg_box_roll(3, 99, &c, 7, it);
        CHECK(ni == 2 && it[1].type == 6 && it[1].amount >= 100 && it[1].amount <= 400 && it[0].amount >= 200, "not maxed: Mega Box (type 3) coins + power points x10 (%d, %d)", it[0].amount, it[1].amount);
        c.shop_offers = 2;
        ni = bmsg_offer_items(0, &c, it);
        CHECK(ni == 1 && it[0].type == 9 && it[0].skin == 2 && it[0].amount == 1, "offer 0 delivers the Rockstar Colt skin (type 9, skins row 2)");
        c.power_level = 8;
        ni = bmsg_offer_items(1, &c, it);
        CHECK(ni == 1 && it[0].type == c.box_coin_type, "offer 1 with maxed brawlers delivers coins instead of power points");
        CHECK(bmsg_offer_items(2, &c, it) == 0 && bmsg_offer_items(-1, &c, it) == 0, "unknown offer index: nothing");
        /* the skin goes into ref0c as DataRef(29,2): ... type 09, then 1d 02 */
        bv_bs d;
        bs_init(&d);
        bmsg_offer_items(0, &c, it);
        bmsg_box_delivery(&d, 100, it, 1);
        static const uint8_t want[] = {0x8b, 0x03, 0x00, 0x01, 0xa4, 0x01, 0x01, 0x01, 0x00, 0x09, 0x1d, 0x02, 0x00, 0x00, 0x00};
        CHECK(d.len >= sizeof want && !memcmp(d.data, want, sizeof want), "skin delivery bytes: 203, f1 0, 1 unit, unit 100, 1 item {1, null, 9, DataRef(29,2), ...}");
        bs_free(&d);
        /* 14362: VInt event id, VInt slot */
        static const uint8_t tap[] = {0x01, 0x02};
        int id = -9, sl = -9;
        CHECK(bmsg_parse_14362(tap, sizeof tap, &id, &sl) == 1 && id == 1 && sl == 2, "14362 body (1, 2) parses");
        bmsg_default_cfg(&c);
        c.events = 6;
        static const int slots[6] = {1, 2, 8, 4, 6, 3}, maps[6] = {-1, 13, 34, 18, 4, 24};
        for (int i = 0; i < 6; i++) { c.event_slot[i] = slots[i]; c.event_map[i] = maps[i]; }
        CHECK(bmsg_map_for_slot(&c, 1) == c.map_row && bmsg_map_for_slot(&c, 2) == 13 && bmsg_map_for_slot(&c, 3) == 24 && bmsg_map_for_slot(&c, 9) == -1,
              "slot -> map: slot 1 = event 0 (map_row), slot 2 = Showdown map 13, slot 3 = map 24, unknown slot -1");
    }

    /* 2p. cycle 6: selection commands, bodies from run 79 */
    {
        static const uint8_t b4[] = {0x00, 0x9a, 0x02, 0x00, 0x02, 0xba, 0x07, 0x99, 0x02, 0x99, 0x02, 0x7f, 0x7f, 0x1d, 0x89, 0x02,
                                     0x8d, 0x08, 0x99, 0x02, 0x99, 0x02, 0x7f, 0x7f, 0x10, 0x20, 0xff, 0xff, 0xff, 0xff};
        static const uint8_t b2[] = {0x00, 0xb0, 0x01, 0x00, 0x01, 0x8a, 0x08, 0xaf, 0x01, 0xaf, 0x01, 0x7f, 0x7f, 0x10, 0x20, 0x00, 0x00, 0x00, 0x02,
                                     0xff, 0xff, 0xff, 0xff};
        static const uint8_t box[] = {0x00, 0x38, 0x00, 0x01, 0x8e, 0x08, 0x37, 0x37, 0x7f, 0x7f, 0xff, 0xff, 0xff, 0xff};
        int br = -2, sk = -2;
        CHECK(bmsg_parse_selection(b4, sizeof b4, &br, &sk) && br == 32 && sk == 137, "506 skin 137 + 525 brawler 32 (%d, %d)", br, sk);
        CHECK(!bmsg_parse_selection(b2, sizeof b2, &br, &sk) && br == -1 && sk == -1, "522 (browsing) does not select (%d, %d)", br, sk);
        CHECK(bmsg_parse_view(b2, sizeof b2) == 32 && bmsg_parse_view(b4, sizeof b4) == -1, "522 = viewing brawler 32; 506/525 are not views");
        CHECK(!bmsg_parse_selection(box, sizeof box, &br, &sk) && br == -1 && sk == -1, "other commands: nothing");
        bmsg_cfg c;
        bmsg_default_cfg(&c);
        bv_bs a0, a1;
        bs_init(&a0); bs_init(&a1);
        bmsg_avatar_update(&a0, &c, 7);
        c.sel_skin_row = 137;
        bmsg_avatar_update(&a1, &c, 7);
        CHECK(a1.len == a0.len + 2, "member skin DataRef(29,137) = 3 bytes instead of 1 (%u -> %u)", (unsigned)a0.len, (unsigned)a1.len);
        bs_free(&a0); bs_free(&a1);
    }

    /* 2o. cycle 5: battle end 14110 parser + generated offer table */
    {
        bv_bs b;
        bs_init(&b);
        bs_vint(&b, 1); bs_vint(&b, 2); bs_vint(&b, 300);       /* v50 v54 v58 */
        bs_vint(&b, 0); bs_vint(&b, 1);                         /* sub: 2 VInts */
        bs_vint(&b, 2);                                         /* 2 rows */
        bs_dataref(&b, 16, 5); bs_dataref(&b, 0, 0); bs_vint(&b, 1); bs_byte(&b, 1); bs_int(&b, 77);
        bs_dataref(&b, 16, 200); bs_dataref(&b, 29, 2); bs_vint(&b, 0); bs_byte(&b, 0); bs_int(&b, -1);
        bmsg_battle_end be;
        int ok = bmsg_parse_14110(b.data, b.len, &be);
        CHECK(ok && be.v50 == 1 && be.v54 == 2 && be.v58 == 300 && be.s1 == 1 && be.n_rows == 2, "14110 header");
        CHECK(be.row[0].c1 == 16 && be.row[0].r1 == 5 && be.row[0].c2 == 0 && be.row[0].r2 == -1 && be.row[0].flag == 1 && be.row[0].num == 77, "14110 row 0");
        CHECK(be.row[1].r1 == 200 && be.row[1].c2 == 29 && be.row[1].r2 == 2 && be.row[1].num == -1, "14110 row 1 (2-byte VInt row, skin ref)");
        CHECK(!bmsg_parse_14110(b.data, b.len - 3, &be), "a cut 14110 is reported as partly decoded");
        bs_free(&b);

        bmsg_cfg c;
        bmsg_default_cfg(&c);
        c.shop_offers = 2;
        bv_bs h0, h1;
        bs_init(&h0); bs_init(&h1);
        bmsg_own_home_data(&h0, &c, NULL);
        c.n_offers = 2;                                         /* the same two offers as a generated table -> identical bytes */
        bmsg_offer o0 = {4, 1, c.skin_offer_row, c.skin_offer_kind, c.skin_offer_price, c.skin_offer_price};
        bmsg_offer o1 = {8, 100, 1, c.offer_cur, 100, 100};
        c.offers[0] = o0; c.offers[1] = o1;
        bmsg_own_home_data(&h1, &c, NULL);
        CHECK(h0.len == h1.len && !memcmp(h0.data, h1.data, h0.len), "generated table with the 2 fixed offers = the verified bytes (%u / %u)", (unsigned)h0.len,
              (unsigned)h1.len);
        bmsg_box_item it[2];
        c.power_level = 8;
        CHECK(bmsg_offer_items(0, &c, it) == 1 && it[0].type == 9 && it[0].skin == c.skin_offer_row, "table offer 0 delivers the skin");
        CHECK(bmsg_offer_items(1, &c, it) == 1 && it[0].type == c.box_coin_type && it[0].amount == 100, "table offer 1 (maxed) delivers coins");
        CHECK(bmsg_offer_items(2, &c, it) == 0, "no offer 2");
        {   /* cycle 6: box offers */
            bmsg_cfg d = c;
            bmsg_offer m3 = {10, 3, 0, d.skin_offer_kind, 119, 240, 1};
            bmsg_offer fr = {0, 1, 0, d.offer_cur, 0, 0, 0};
            d.offers[0] = m3; d.offers[1] = fr; d.n_offers = 2;
            bmsg_box_item one[4];
            int n1 = bmsg_offer_items(0, &d, it), n2;
            d.offers[0].amount = 1;
            n2 = bmsg_offer_items(0, &d, one);
            CHECK(n1 >= 1 && it[0].type == d.box_coin_type && it[0].amount > one[0].amount && n2 >= 1, "Mega Box x3 delivers more coins than x1 (%d / %d)", it[0].amount, one[0].amount);
            CHECK(bmsg_offer_unit(0, &d, 100) == 11 && bmsg_offer_unit(1, &d, 100) == 10 && bmsg_offer_unit(5, &d, 100) == 100, "box offers open as Mega / Brawl box models");
            bv_bs h2; bs_init(&h2);
            bmsg_own_home_data(&h2, &d, NULL);
            CHECK(!h2.failed && h2.len > 0, "24101 with box offers builds");
            bs_free(&h2);
        }
        int rows[64];
        CHECK(bmsg_owned_brawler_rows(&c, rows, 64) >= 1, "owned brawler rows");
        bs_free(&h0); bs_free(&h1);
    }

    /* 2n. cycle 4: owned skins from the profile, bought offers, claimed-bit experiment */
    {
        bmsg_cfg c;
        bmsg_default_cfg(&c);
        c.owned_skins = 0;
        c.brawl_pass = 1;
        bv_bs a0, a1, b0, b1, p0, p1;
        bs_init(&a0); bs_init(&a1); bs_init(&b0); bs_init(&b1); bs_init(&p0); bs_init(&p1);
        bmsg_own_home_data(&a0, &c, NULL);
        static const int rows[2] = {2, 214};
        c.own_skins = rows; c.n_own_skins = 2;
        bmsg_own_home_data(&a1, &c, NULL);
        /* DataRef(29,2) = 1d 02 (2 bytes), DataRef(29,214) = 1d + 2-byte VInt (3 bytes) */
        CHECK(a1.len == a0.len + 5, "two owned skins add 2 + 3 bytes (%u -> %u)", (unsigned)a0.len, (unsigned)a1.len);
        c.shop_offers = 2;
        bmsg_own_home_data(&b0, &c, NULL);
        c.offer_bought_mask = 1;
        bmsg_own_home_data(&b1, &c, NULL);
        int diff = 0, at = -1;
        for (size_t i = 0; i < b0.len && i < b1.len; i++) if (b0.data[i] != b1.data[i]) { diff++; at = (int)i; }
        CHECK(b0.len == b1.len && diff == 1 && at >= 0 && b1.data[at] == 1 && b0.data[at] == 0, "offer 0 bought = exactly one byte 0 -> 1 (%d diffs)", diff);
        c.pass_bits[0][0] = 1;
        bmsg_own_home_data(&p0, &c, NULL);
        CHECK(p0.len == b1.len, "claimed bits are not written while the experiment is off");
        c.pass_claimed_bits = 1;
        bmsg_own_home_data(&p1, &c, NULL);
        CHECK(p1.len == b1.len + 16 + 1, "free bits on: flag byte 1 + 4 Ints + premium flag byte 0 (%u -> %u)", (unsigned)b1.len, (unsigned)p1.len);
        bs_free(&a0); bs_free(&a1); bs_free(&b0); bs_free(&b1); bs_free(&p0); bs_free(&p1);
    }

    /* 2k. update 50: box opening. 14102 with client command 500 (VInt 500 = b4 07, 4 base VInts, box type 4) -> 24125 + 24111 (command 203) */
    {
        static const uint8_t box[] = {0x00, 0xbf, 0x04, 0x00, 0x01, 0xb4, 0x07, 0xbe, 0x04, 0xbe, 0x04, 0x7f, 0x7f, 0x04, 0xff, 0xff, 0xff, 0xff};
        CHECK(bmsg_parse_box_open(box, sizeof box) == 4, "box open command 500 with box type 4 recognised");
        static const uint8_t b2[] = {0x00, 0xbf, 0x04, 0x00, 0x01, 0x8a, 0x08, 0xbe, 0x04, 0xbe, 0x04, 0x7f, 0x7f, 0x10, 0x08};
        CHECK(bmsg_parse_box_open(b2, sizeof b2) == -1, "another command (522) is not a box");
        setup(&f, &gi, &st);
        st.cfg.unlock_all = 1;
        f.type = 14102;
        memcpy(f.body, box, sizeof box);
        f.body_len = sizeof box;
        bvb_handle_send(&st, &gi, (void *)1);
        static const char *const bx[] = {"encode", "deliver 24125", "deliver 24111"};
        CHECK(seq(&f, bx, 3), "box: ack + 24111 (%d events, last '%s')", f.nev, f.nev ? f.events[f.nev - 1] : "");
        bmsg_box_item it[4];
        int ni = bmsg_box_roll(4, 1234u, &st.cfg, 7, it);
        /* update 110 (C4): coins + 1..2 power point stacks for brawlers below the maximum (per-brawler level) + maybe bonus gems; at most 4 items */
        CHECK(ni >= 2 && ni <= 4 && it[0].type == 7 && it[0].amount >= 60 && it[0].amount <= 180 && it[1].type == 6 && it[1].amount >= 20 && it[1].amount <= 65 &&
                  it[1].brawler >= 0,
              "Big Box roll: 60..180 coins (%d), 20..65 power points (%d) for an owned brawler (%d), %d items", it[0].amount, it[1].amount, it[1].brawler, ni);
        bv_bs d;
        bs_init(&d);
        bmsg_box_delivery(&d, 104, it, ni);
        const uint8_t head[] = {0x8b, 0x03, 0x00, 0x01, 0xa8, 0x01, (uint8_t)ni}; /* VInt 203, f1 0, 1 unit, unit 104, ni items */
        CHECK(d.len > sizeof head && !memcmp(d.data, head, sizeof head), "24111 body starts with command 203, unit 104, %d items", ni);
        static const uint8_t tail[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x7f, 0x00, 0x00, 0x00}; /* hasExtra, g30..g38, v18, v0c -1, v08, long */
        CHECK(d.len > sizeof tail && !memcmp(d.data + d.len - sizeof tail, tail, sizeof tail), "24111 ends with the Boolean, 3 VInts and the 5-VInt tail");
        bs_free(&d);
    }

    /* 2g. update 36: 14102 command batches, two bodies logged on the console (run 21) */
    {
        static const uint8_t b2[] = {0x00, 0xbf, 0x04, 0x00, 0x01, 0x8a, 0x08, 0xbe, 0x04, 0xbe, 0x04, 0x7f, 0x7f, 0x10, 0x08, 0x00, 0x00, 0x00, 0x02, 0xff, 0xff, 0xff, 0xff};
        static const uint8_t b3[] = {0x00, 0x9f, 0x05, 0x00, 0x02, 0xba, 0x07, 0x9e, 0x05, 0x9e, 0x05, 0x7f, 0x7f, 0x1d, 0x0e,
                                     0x8d, 0x08, 0x9e, 0x05, 0x9e, 0x05, 0x7f, 0x7f, 0x10, 0x08, 0xff, 0xff, 0xff, 0xff};
        static const uint8_t b1[] = {0x00, 0x88, 0x03, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff};
        int tick = 0, id = 0;
        CHECK(bmsg_parse_14102(b2, sizeof b2, &tick, &id) == 1 && tick == 319 && id == 522, "14102 #2: 1 command, tick 319, id 522 (%d %d)", tick, id);
        CHECK(bmsg_parse_14102(b3, sizeof b3, &tick, &id) == 2 && tick == 351 && id == 506, "14102 #3: 2 commands, tick 351, first id 506 (%d %d)", tick, id);
        CHECK(bmsg_parse_14102(b1, sizeof b1, &tick, &id) == 0 && tick == 200 && id == -1, "14102 #1: no commands, tick 200");
        CHECK(bmsg_parse_14102(b1, 2, &tick, &id) == -1, "a cut body is refused");
    }

    /* 3. map selection with resend_home=1 */
    setup(&f, &gi, &st);
    st.resend_home = 1;
    f.type = 14363;
    bv_bs q;
    bs_init(&q);
    bs_vint(&q, 3);
    bs_vint(&q, 12);
    memcpy(f.body, q.data, q.len);
    f.body_len = q.len;
    bs_free(&q);
    bvb_handle_send(&st, &gi, (void *)1);
    CHECK(seq(&f, later, 5), "14363 sequence wrong");
    CHECK(st.selected_map == 12, "selected map %d", st.selected_map);
    {
        bv_bs want;
        bs_init(&want);
        bmsg_avatar_update(&want, &st.cfg, 12);
        CHECK(f.last24124_len == want.len && !memcmp(f.last24124, want.data, want.len), "the last 24124 carries the new map row");
        bs_free(&want);
    }
    /* 14353 resets it */
    f.type = 14353;
    f.nev = 0;
    f.nd = 0;
    f.body_len = 0;
    bvb_handle_send(&st, &gi, (void *)1);
    CHECK(st.selected_map == st.cfg.map_row, "14353 resets the selection");

    /* 4. resend_home off: only the acknowledgement */
    setup(&f, &gi, &st);
    st.resend_home = 0;
    f.type = 14350;
    bvb_handle_send(&st, &gi, (void *)1);
    static const char *const quiet[] = {"encode", "deliver 24125"};
    CHECK(seq(&f, quiet, 2), "quiet mode sequence wrong");

    /* 4b. a client that answers every reply with a new request must not recurse without bound; the handler is locked around each call */
    setup(&f, &gi, &st);
    f.type = 10108;
    f.reenter = 1;
    bvb_handle_send(&st, &gi, (void *)1);
    CHECK(st.depth == 0, "depth back to 0 (%d)", st.depth);
    CHECK(f.locks == f.unlocks && f.locks > 1 && f.locks <= 100, "lock calls balanced and bounded (%d/%d)", f.locks, f.unlocks);

    /* 4c. the receive-queue push mirrors the game's own producer (checked against libg+0x239864..0x2398d8) */
    {
        uint32_t mem[64];
        memset(mem, 0, sizeof mem);
        bvb_mem bm = {mem, mem_rd, mem_wr};
        /* queue struct at word 8 (address 32): array at address 128 (word 32), capacity 4 */
        mem[8] = 128; mem[9] = 4; mem[10] = 0; mem[12] = 2; /* array, cap, count, tail @ q+16 = word 12 */
        CHECK(bvb_ring_push(&bm, 32, 0xAAAA0001u) == 1 && mem[32 + 2] == 0xAAAA0001u && mem[12] == 3 && mem[10] == 1, "push at tail 2");
        CHECK(bvb_ring_push(&bm, 32, 0xAAAA0002u) == 1 && mem[32 + 3] == 0xAAAA0002u && mem[12] == 0 && mem[10] == 2, "tail wraps to 0");
        CHECK(bvb_ring_push(&bm, 32, 0xAAAA0003u) == 1 && mem[32] == 0xAAAA0003u && mem[12] == 1 && mem[10] == 3, "push at 0");
        CHECK(bvb_ring_push(&bm, 32, 0xAAAA0004u) == 1 && mem[10] == 4, "queue becomes full");
        CHECK(bvb_ring_push(&bm, 32, 0xAAAA0005u) == 0 && mem[10] == 4 && mem[12] == 2, "a full queue refuses and stays intact");
        mem[8] = 0;
        CHECK(bvb_ring_push(&bm, 32, 1) == 0, "no array: refused");
        mem[8] = 128; mem[9] = 0;
        CHECK(bvb_ring_push(&bm, 32, 1) == 0, "zero capacity: refused");
        mem[9] = 4; mem[10] = 0; mem[12] = 9;
        CHECK(bvb_ring_push(&bm, 32, 1) == 0, "tail out of range: refused");
    }

    /* 5. ARM encoders: known words of the real libg.so */
    uint32_t w;
    CHECK(arm_b(0xea5b8, 0xea5bc, &w) == 0 && w == 0xeaffffffu, "arm_b of the b at 0xea5b8 gives %08x", w);
    CHECK(arm_b(0xea5b4, 0xeb914, &w) == 0 && w == 0xea0004d6u, "arm_b 0xea5b4 -> 0xeb914 gives %08x", w);
    CHECK(arm_b(1, 8, &w) == -1, "unaligned branch is refused");
    CHECK(arm_b(0, 0x10000000u, &w) == -1, "out-of-range branch is refused");
    uint8_t t[8];
    arm_trampoline(t, 0x98123457u);
    CHECK(rd32(t) == 0xE51FF004u && rd32(t + 4) == 0x98123457u, "trampoline layout");

    if (argc > 1) {
        FILE *fp = fopen(argv[1], "rb");
        if (!fp) {
            printf("cannot open %s\n", argv[1]);
            return 2;
        }
        static uint8_t img[0x500000];
        size_t got = fread(img, 1, sizeof img, fp);
        fclose(fp);
        (void)got;
        /* libg's text segment starts at file offset 0 / vaddr 0 (first PT_LOAD), so vaddr == file offset for the code */
        struct { uint32_t off, word; const char *what; } exp[] = {
            {0x2392f4, 0xE92D4FF0u, "send-message prologue push {r4-r11,lr}"},
            {0x4a7ee4, 0xE92D4830u, "createMessage prologue"},
            {0x3deaf8, 0xE59F1008u, "getTable first word"},
            {0x2829b0, ARM_MOV_R0_1, "flag function A (mov r0,#1)"},
            {0x3ad0b4, ARM_MOV_R0_0, "flag function B (mov r0,#0)"},
            {0xea5b4, 0x1A000003u, "handshake bne +0xc"},
        };
        for (unsigned i = 0; i < sizeof exp / sizeof exp[0]; i++) {
            uint32_t got_w = rd32(img + exp[i].off);
            CHECK(got_w == exp[i].word, "libg+0x%x (%s) is %08x, expected %08x", exp[i].off, exp[i].what, got_w, exp[i].word);
        }
        /* the bne at 0xea5b4 jumps to 0xea5c8: decode and compare, then our replacement jumps to 0xeb914 */
        uint32_t bne = rd32(img + 0xea5b4);
        int32_t off = (int32_t)(bne << 8) >> 6;
        CHECK((uint32_t)(0xea5b4 + 8 + off) == 0xea5c8u, "bne target decodes to %x", (unsigned)(0xea5b4 + 8 + off));
        uint32_t nb;
        arm_b(0xea5b4, 0xeb914, &nb);
        int32_t off2 = (int32_t)(nb << 8) >> 6;
        CHECK((uint32_t)(0xea5b4 + 8 + off2) == 0xeb914u, "our branch decodes to %x", (unsigned)(0xea5b4 + 8 + off2));
        printf("real libg.so: %u words checked\n", (unsigned)(sizeof exp / sizeof exp[0]));
    }

    if (g_fail) {
        printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("ALL CHECKS PASSED\n");
    return 0;
}
