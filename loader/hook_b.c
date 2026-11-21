#include "hook_b.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "../shims/shim.h"
#include "armenc.h"
#include "gtrace.h"
#include "kubridge_api.h"
#include "log.h"
#include "offline_b.h"
#include "passdata.h"
#include "profile.h"
#include "botworld.h"
#include "botnames.h"

#include <time.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>

/* libg.so (Build B) virtual addresses; all of them are ARM (A32) code or data, see docs/offline_server_spec.md section 8 */
#define OFF_SEND_MESSAGE 0x2392f4u   /* replaced: the game's "send a message to the server" function, prologue push {r4-r11,lr} */
#define OFF_CREATE_MESSAGE 0x4a7ee4u /* createMessage(pointer, int type) -> message object */
#define OFF_GET_TABLE 0x3deaf8u      /* getTable(int id) -> table object; the row count is the int at +12 */
#define OFF_MANAGER_GLOBAL 0xc86d10u /* pointer to the object whose +4 member holds the connection state (+208, +212) */
#define OFF_HANDSHAKE 0xea5b4u       /* bne -> b 0xeb914, applied after the login */
#define OFF_HANDSHAKE_TO 0xeb914u
#define OFF_FLAG_FALSE 0x2829b0u     /* function returning 1 ("mov r0,#1"): its first byte becomes 0, the function returns 0 */
#define OFF_FLAG_TRUE 0x3ad0b4u      /* function returning 0 ("mov r0,#0"): its first byte becomes 1, the function returns 1 */
#define GAME_SERVER_PORT 9339

static const so_module *g_m;
static bvb_options g_opt;
static bvb_state g_st;
static bvb_game_if g_gi;
static uint8_t g_orig[8];
static int g_hook_installed, g_hook_armed, g_boot_ok;
static uint32_t g_manager; /* M = *(*(libg+0xc86d10)+4), remembered between login_prepare and login_done */
static uint8_t g_zero5[8];
static uint32_t g_messages;
static SceUID g_lock = -1;

static uint32_t A(uint32_t off) { return g_m->load + off; }
/* user-space pointers on the console start well above 0x40000000 (heap 0x81..., modules 0x98...); small values are never objects */
static int plausible(uint32_t p) { return p >= 0x40000000u && !(p & 3u); }  /* object / vtable pointers: aligned */
static int plausible_data(uint32_t p) { return p >= 0x40000000u; }         /* byte buffers */
static void glog(void *ctx, const char *fmt, ...) {
    (void)ctx;
    va_list ap;
    va_start(ap, fmt);
    bv_logv(fmt, ap);
    va_end(ap);
}

void bvb_hook_default_options(bvb_options *o) {
    o->enabled = 1;
    o->patches = 1;
    o->resend_home = 0; /* update 17: OwnHomeData only at login (a resend on every KeepAlive ended in a use-after-free, run of 2026-11-11) */
    o->brawler_row = 0;
    o->map_row = 7;
    o->events = 6; /* update 33: Gem Grab, Showdown, Duo Showdown, Heist, Bounty, Brawl Ball (user report 2026-11-11: only one Gem Grab event) */
    for (int i = 0; i < 8; i++) {
        o->event_map[i] = -1;
        o->event_slot[i] = 0;
    }
    /* update 34 (user report 2026-11-11: Shelly not unlocked, the other modes locked behind trophies): all brawlers owned, 1000 trophies */
    o->unlock_all = 1;
    o->trophies = 1000;
    o->account_trophies = 1000; /* update 35 (user: the trophy road did not count Shelly's 1000) */
    o->power_level = 8;
    /* update 38 (user: shop "temporarily closed", no Brawl Pass button; static analysis notes/.../shop_pass.md) */
    o->home_flags = 4; /* update 42: back to the mod's value; the shop switch turned out to be config key 5 (getter trace, run 26) */
    o->shop_disabled = 0;
    o->n_extra_keys = 0;
    /* update 52: the list at home +0x100 is the QUEST list, not shop offers: with these 16 entries the quest icon shows next to the home brawler and on
     * the event (HARDWARE-VERIFIED run 37 screenshot; user saw quests + reached the Brawl Pass through them in run 32); without them quests vanish (run 35).
     * Field meanings still unknown (values are the labelling markers); config offer_test=0 turns them off. */
    o->offer_test = 16;
    o->tutorial_steps = 20;
    o->box_coin_type = 7;
    o->gems = 1000;   /* update 56: so boxes can be bought (Big Box 30, Mega Box 80) */
    o->coins = 5000;
    o->shop_offers = 2;
    o->offer_cur = 1;
    o->owned_skins = 0;
    o->res_markers = 0;
    o->box_pp_random = 0;
    o->offer_unit = 100;
    o->profile_on = 1;
    o->pass_claimed_bits = 0;
    o->shop_layout = 0;
    o->pass_new_types = 0;
    o->profile_brawlers = 1; /* default since update 92 (user verified, update 86) */
    o->home_on_14109 = 1;    /* default since update 92 (run 99) */
    o->offer_title_slot = 1; /* update 138 (B11 VERIFIED run 157: the special offer shows the +0x30 text object's string as its title) */
    o->profile_gems = -1;
    o->battle_win = 8;
    o->battle_draw = 0;
    o->battle_lose = -4;
    o->trophy_road_claimed = 1; /* default since update 92 (user, run 102) */
    o->member_view = 1;      /* default since update 92 (user verified, update 86) */
    o->pass_claim_reply = 2; /* update 66: real rewards (run 63: the client accepted a 203 for its claim) */
    o->ack24125 = 1;
    o->team_f0 = 1;
    o->team_f8 = 1;
    o->team_m10 = -1;
    o->team_m14 = -1;   /* -1 = auto: 0 for type 1, else the brawler's (highest) trophies */   /* -1 = auto: 0 for team type 1 (original bytes), else the brawler's trophies (run 77) */
    o->team_m18 = -1;   /* -1 = auto: 1 for team type 1, else power level + 1 (run 78: shown as POWER) */
    o->team_m1c = 3;
    o->mode_push_map = -1;
    o->mode_push_after = 3;
    o->skin_offer_kind = 0; /* update 62: kind 0 = avatar+0x60 = gems (the skin was priced in coins with kind 1) */ /* update 61: the user does not want every skin owned */
    o->box_unit_mode = 3; /* update 63: box model ids 10/11/12 */
    for (int i = 0; i < 16; i++) o->offer_type[i] = i;
    o->brawl_pass = 1;
    o->pass_season = 2;
    o->pass_tokens = 1;   /* cycle 6: > 0 = the home pass button shows "in progress" (run 86: 1/75) */
    o->pass_premium = 0;   /* update 103 (D5, user): the Brawl Pass is not owned for free */
    o->season_days = 70;
    o->release_old = 0;      /* default since update 92 (runs 96-102: slot 2 = 2.6 s of CPU, nothing breaks without it) */
}

/* Writes into libg's text through the privileged copy and reads the bytes back. Returns 1 when the memory holds what was written. */
static int write_code(uint32_t off, const void *src, size_t n) {
    uint8_t *dst = g_m->host + off;
    int r = kuKernelCpuUnrestrictedMemcpy(dst, src, (SceSize)n);
    kuKernelFlushCaches(dst, (SceSize)n);
    int same = memcmp(dst, src, n) == 0;
    if (r < 0 || !same) bv_log("offline: patch at libg+0x%x FAILED (kuKernelCpuUnrestrictedMemcpy returned 0x%08x, memory %s)", off, (unsigned)r, same ? "matches" : "differs");
    return same;
}

/* ---- calls into the game ---- */
static uint32_t vslot(uint32_t obj, unsigned slot) {
    if (!plausible(obj)) return 0;
    uint32_t vt = *(uint32_t *)(uintptr_t)obj;
    if (!plausible(vt)) return 0;
    return *(uint32_t *)(uintptr_t)(vt + 4 * slot);
}

static int gi_msg_type(void *ctx, void *msg) {
    (void)ctx;
    uint32_t fn = vslot((uint32_t)(uintptr_t)msg, 5);
    if (!fn) {
        bv_log("offline: message %p has no usable vtable", msg);
        return -1;
    }
    return ((int (*)(void *))(uintptr_t)fn)(msg);
}

/* vtable slot 2 of a message class is encode(): it writes the fields into the message's stream (length at msg+24, bytes at msg+36) */
static void gi_msg_encode(void *ctx, void *msg) {
    (void)ctx;
    uint32_t fn = vslot((uint32_t)(uintptr_t)msg, 2);
    if (fn) ((void (*)(void *))(uintptr_t)fn)(msg);
}

static int gi_msg_payload(void *ctx, void *msg, const uint8_t **p, size_t *len) {
    (void)ctx;
    int32_t n = *(int32_t *)((uint8_t *)msg + 24);
    uint32_t ptr = *(uint32_t *)((uint8_t *)msg + 36);
    if (n <= 0 || !plausible_data(ptr)) return 0;
    *p = (const uint8_t *)(uintptr_t)ptr;
    *len = (size_t)n;
    return 1;
}

/* the manager object: *(*(libg+0xc86d10)+4); 0 when the game has not created it yet */
static uint32_t manager(void) {
    uint32_t G = *(uint32_t *)(uintptr_t)A(OFF_MANAGER_GLOBAL);
    if (!plausible(G)) return 0;
    uint32_t M = *(uint32_t *)(uintptr_t)(G + 4);
    return plausible(M) ? M : 0;
}

static uint32_t mem_rd32(void *ctx, uint32_t a) { (void)ctx; return *(volatile uint32_t *)(uintptr_t)a; }
static void mem_wr32(void *ctx, uint32_t a, uint32_t v) { (void)ctx; *(volatile uint32_t *)(uintptr_t)a = v; }

typedef int (*mutex_fn)(void *slot);

/* Puts a decoded message into the client's receive queue (manager + 60, mutex slot at manager + 56), the step that makes the game's logic
 * thread see it. The mutex is a bionic one (a 4-byte slot), so it has to be locked through the pthread shim, not through newlib. */
static int queue_push(uint32_t M, uint32_t msg) {
    static mutex_fn lock, unlock;
    if (!lock) lock = (mutex_fn)(uintptr_t)bv_shim_lookup("pthread_mutex_lock");
    if (!unlock) unlock = (mutex_fn)(uintptr_t)bv_shim_lookup("pthread_mutex_unlock");
    if (!lock || !unlock) return 0;
    bvb_mem bm = {NULL, mem_rd32, mem_wr32};
    lock((void *)(uintptr_t)(M + 56));
    int ok = bvb_ring_push(&bm, M + 60, msg);
    unlock((void *)(uintptr_t)(M + 56));
    return ok;
}

static void gi_deliver(void *ctx, int type, int flag, const uint8_t *data, size_t len) {
    (void)ctx;
    void *(*create)(void *, int) = (void *(*)(void *, int))(uintptr_t)A(OFF_CREATE_MESSAGE);
    uint8_t *msg = (uint8_t *)create(g_zero5, type);
    if (!plausible((uint32_t)(uintptr_t)msg)) {
        bv_log("offline: createMessage(%d) returned %p: message dropped", type, (void *)msg);
        return;
    }
    uint8_t *buf = NULL;
    if (len > 0) {
        buf = (uint8_t *)malloc(len);
        if (!buf) return;
        memcpy(buf, data, len);
    }
    *(int32_t *)(msg + 4) = flag;
    *(int32_t *)(msg + 28) = (int32_t)len;
    *(uint8_t **)(msg + 36) = buf;
    uint32_t fn = vslot((uint32_t)(uintptr_t)msg, 3);
    if (!fn) {
        bv_log("offline: message %d has no vtable slot 3: dropped", type);
        return;
    }
    ((void (*)(void *))(uintptr_t)fn)(msg);
    if (type == 24101) {
        /* update 40: the decoded objects (decode libg+0xb1908: msg+0x50 = holder with +0x0c home fields / +0x10 config, msg+0x54 = avatar) for the getter
         * tracer (gtrace.h); logged so a trace line can be matched to them */
        uint32_t holder = *(uint32_t *)(msg + 0x50), avatar = *(uint32_t *)(msg + 0x54);
        uint32_t home = plausible(holder) ? *(uint32_t *)(uintptr_t)(holder + 0x0c) : 0, config = plausible(holder) ? *(uint32_t *)(uintptr_t)(holder + 0x10) : 0;
        bv_gtrace_set_objects(home, config, avatar, holder);
        bv_log("offline: 24101 decoded: holder 0x%08x, home fields 0x%08x, config 0x%08x, avatar 0x%08x", (unsigned)holder, (unsigned)home, (unsigned)config,
               (unsigned)avatar);
    }
    uint32_t M = manager();
    if (!M) {
        bv_log("offline: no manager object: message %d decoded but not queued", type);
        return;
    }
    if (!queue_push(M, (uint32_t)(uintptr_t)msg)) bv_log("offline: the receive queue refused message %d (full or not valid)", type);
}

/* ---- the replacement of the send function ---- */
static int tramp_install(void);

static void tramp_revert(void) {
    if (g_hook_armed) {
        write_code(OFF_SEND_MESSAGE, g_orig, 8);
        g_hook_armed = 0;
    }
}

static void gi_login_prepare(void *ctx) {
    (void)ctx;
    tramp_revert(); /* the original code of the old connection object may send messages itself */
    uint32_t G = *(uint32_t *)(uintptr_t)A(OFF_MANAGER_GLOBAL);
    g_manager = 0;
    if (plausible(G)) {
        uint32_t M = *(uint32_t *)(uintptr_t)(G + 4);
        if (plausible(M)) {
            g_manager = M;
            uint32_t old = *(uint32_t *)(uintptr_t)(M + 212);
            *(uint32_t *)(uintptr_t)(M + 212) = 0;
            uint32_t fn = vslot(old, 2);
            bv_log("offline: login: manager %p, old connection object %p; its vtable slots 1/2/7 point to libg+0x%x / +0x%x / +0x%x",
                   (void *)(uintptr_t)M, (void *)(uintptr_t)old, vslot(old, 1) - g_m->load, fn - g_m->load, vslot(old, 7) - g_m->load);
            if (fn && g_opt.release_old) {
                bv_log("offline: calling slot 2 of the old connection object (as the original mod does; release_old=0 skips it)");
                uint64_t t0 = sceKernelGetProcessTimeWide();
                ((void (*)(void *))(uintptr_t)fn)((void *)(uintptr_t)old);
                bv_log("offline: slot 2 of the old connection object returned after %u ms", (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000));   /* update 88 (H) */
            }
        } else {
            bv_log("offline: login: manager member is %p, state left alone", (void *)(uintptr_t)M);
        }
    } else {
        bv_log("offline: login: manager global is %p, state left alone", (void *)(uintptr_t)G);
    }
    uint64_t tt0 = sceKernelGetProcessTimeWide();
    tramp_install();
    bv_log("offline: login prepare: trampolines installed in %u ms", (unsigned)((sceKernelGetProcessTimeWide() - tt0) / 1000));   /* update 88 (H) */
}

static void gi_login_done(void *ctx) {
    (void)ctx;
    if (g_manager) *(int32_t *)(uintptr_t)(g_manager + 208) = 5;
}

static void gi_after_login(void *ctx) {
    (void)ctx;
    uint32_t w;
    if (arm_b(A(OFF_HANDSHAKE), A(OFF_HANDSHAKE_TO), &w)) return;
    uint8_t code[4];
    arm_put32(code, w);
    uint32_t before = arm_get32(g_m->host + OFF_HANDSHAKE);
    write_code(OFF_HANDSHAKE, code, 4);
    bv_log("offline: post-login patch at libg+0x%x: %08x -> %08x", OFF_HANDSHAKE, before, w);
}

static int gi_table_rows(void *ctx, int table) {
    (void)ctx;
    void *(*get)(int) = (void *(*)(int))(uintptr_t)A(OFF_GET_TABLE);
    uint8_t *t = (uint8_t *)get(table);
    if (!plausible((uint32_t)(uintptr_t)t)) return -1;
    int32_t n = *(int32_t *)(t + 12);
    bv_log("offline: data table %d has %d rows", table, n);
    return n >= 0 && n < 100000 ? n : -1;
}

static void gi_lock(void *ctx) {
    (void)ctx;
    if (g_lock >= 0) sceKernelLockMutex(g_lock, 1, NULL);
}
static void gi_unlock(void *ctx) {
    (void)ctx;
    if (g_lock >= 0) sceKernelUnlockMutex(g_lock, 1);
}

static int send_hook(void *self, void *msg) {
    (void)self;
    g_messages++;
    return bvb_handle_send(&g_st, &g_gi, msg);
}

static int tramp_install(void) {
    uint8_t code[8];
    arm_trampoline(code, (uint32_t)(uintptr_t)&send_hook);
    int ok = write_code(OFF_SEND_MESSAGE, code, 8);
    g_hook_armed = ok;
    return ok;
}

/* ---- public ---- */
/* cycle 4 (stream G): saved progress. profile.txt is created from the config defaults on the first boot and then owns gems, coins, owned skins,
 * today's bought offers and claimed pass tiers. */
static bv_profile g_prof;
static int g_fresh;   /* update 144: a real (non-test) account, see bvb_hook_boot */
/* update 143 (user: a personal and a test account): config profile_slot=NAME -> ux0:data/BrawlVita/profile_NAME.txt (each slot keeps its own file;
 * switching slots never touches the other). The slot 'personal' starts as a copy of the old profile.txt. No line = profile.txt as before. */
char bv_profile_slot[24];
static char g_profile_path[96] = BV_DATA_DIR "/profile.txt";
static char g_profile_tmp[96] = BV_DATA_DIR "/profile.tmp";
#define PROFILE_PATH g_profile_path
/* update 188 (v0.1): config profile=normal|modded|max. Each is its own save (profile_<name>.txt) and bot world (world_<name>.bin).
 * modded / max: gems, coins, star points and power points never run out (topped up at boot and before every save). */
int bv_profile_mode;   /* 0 normal, 1 modded, 2 max */
static void profile_topup(void) {
    if (bv_profile_mode < 1) return;
    g_prof.gems = 999999;
    g_prof.coins = 999999;
    g_prof.star_points = 999999;
    for (int i = 0; i < g_prof.n; i++) if (g_prof.b[i].power < 9 && g_prof.b[i].power_points < 30000) g_prof.b[i].power_points = 30000;
    if (!g_opt.avatar_tail_set) g_st.cfg.avatar_tail[1] = g_prof.gems;
    g_st.cfg.coins = g_prof.coins;
    g_st.cfg.star_points = g_prof.star_points;
    for (int i = 0; i < g_prof.n; i++) { int r = g_prof.b[i].row; if (r >= 0 && r < 96) g_st.cfg.br_pp[r] = (short)g_prof.b[i].power_points; }
}
static void profile_save(void) {
    profile_topup();
    static char buf[32768];
    size_t n = bv_profile_format(&g_prof, buf, sizeof buf);
    if (n >= sizeof buf) { bv_log("profile: too big to save (%u bytes)", (unsigned)n); return; }
    SceUID fd = sceIoOpen(g_profile_tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) { bv_log("profile: cannot write profile.tmp (0x%08x)", (unsigned)fd); return; }
    sceIoWrite(fd, buf, (SceSize)n);
    sceIoClose(fd);
    sceIoRemove(PROFILE_PATH);
    sceIoRename(g_profile_tmp, PROFILE_PATH);
}

/* update 66: the Brawl Pass reward tables from the user's own assets (read once) */
static bv_passdata g_pass;
static char *read_text(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return NULL;
    long sz = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    char *b = (sz > 0 && sz < (8 << 20)) ? (char *)malloc((size_t)sz + 1) : NULL;
    if (b) {
        long got = sceIoRead(fd, b, (SceSize)sz);
        b[got > 0 ? got : 0] = 0;
    }
    sceIoClose(fd);
    return b;
}
#include "maprot.h"
#include "offertitles.h"
int bv_box_drops; /* update 144 config box_drops */
/* update 144: rarity of each brawler unlock card (cards.csv columns Name ... Rarity; named rows only, as the game counts them) */
static int bv_csv_rarities(const char *csv, bmsg_cfg *c) {
    static const char *const names[6] = {"common", "rare", "super_rare", "epic", "mega_epic", "legendary"};
    signed char rar_by_card[400];
    memset(rar_by_card, -1, sizeof rar_by_card);
    const char *s = csv;
    int col = 0, c_rar = -1;
    while (*s && *s != '\n') {   /* header */
        const char *f = s;
        while (*s && *s != ',' && *s != '\n') s++;
        if ((s - f == 8 && !strncmp(f, "\"Rarity\"", 8)) || (s - f == 6 && !strncmp(f, "Rarity", 6))) c_rar = col;
        col++;
        if (*s == ',') s++;
    }
    if (c_rar < 0) return 0;
    if (*s) s++;
    while (*s && *s != '\n') s++;   /* types line */
    if (*s) s++;
    int row = 0;
    while (*s) {
        int named = !(*s == ',' || (*s == '"' && s[1] == '"' && s[2] == ','));
        const char *f = s;
        int k = 0;
        signed char rv = -1;
        while (*s && *s != '\n') {
            const char *a = s;
            int q = *s == '"';
            if (q) { s++; while (*s && *s != '"') s++; if (*s) s++; }
            while (*s && *s != ',' && *s != '\n') s++;
            if (k == c_rar) {
                for (int i = 0; i < 6; i++) {
                    size_t L = strlen(names[i]);
                    const char *v = a + q;
                    if ((size_t)(s - v) >= L && !strncmp(v, names[i], L) && (v[L] == '"' || v[L] == ',' || v[L] == '\n' || v[L] == '\r' || !v[L])) rv = (signed char)i;
                }
            }
            k++;
            if (*s == ',') s++;
        }
        (void)f;
        if (*s) s++;
        if (!named) continue;
        if (row < 400) rar_by_card[row] = rv;
        row++;
    }
    int n = 0, cr, kr;
    for (int i = 0; bmsg_unlock_entry(i, &cr, &kr); i++)
        if (cr >= 0 && cr < 96 && kr >= 0 && kr < 400 && rar_by_card[kr] >= 0) { c->br_rar[cr] = rar_by_card[kr]; n++; }
    return n;
}
/* ---- Phase 3 (living world): the bot world (loader/botworld.c) ----
 * config world=1: ux0:data/BrawlVita/world[_SLOT].bin holds 1200 bots + 60 clubs made at the first boot (all at 0 trophies); every boot plays the
 * bots' matches since the last run from the clock (up to world_days, default 30), the game keeps it moving (offline_b.c, every 60 s). */
int bv_world_on, bv_world_match, bv_world_bots = 1200, bv_world_days = 30;
char bv_world_region[4] = "US";
static bw_world *g_world;
static char g_world_path[96] = BV_DATA_DIR "/world.bin";
static void world_save(void) {
    if (!g_world) return;
    char tmp[104];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_world_path);
    SceUID fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) { bv_log("world: cannot write %s (0x%08x)", tmp, (unsigned)fd); return; }
    int w = sceIoWrite(fd, g_world, (SceSize)sizeof *g_world);
    sceIoClose(fd);
    if (w != (int)sizeof *g_world) { bv_log("world: short write %d of %u bytes: kept the old file", w, (unsigned)sizeof *g_world); sceIoRemove(tmp); return; }
    sceIoRemove(g_world_path);
    sceIoRename(tmp, g_world_path);
}
static int csv_rows(const char *path) {   /* named data rows of a csv (header + types lines left out) */
    char *t = read_text(path);
    if (!t) return -1;
    int n = 0, line = 0;
    for (const char *s = t; *s;) {
        const char *e = strchr(s, '\n');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (line >= 2 && l > 1 && s[0] != ',' && !(s[0] == '"' && s[1] == '"')) n++;
        line++;
        s += l + (e ? 1 : 0);
    }
    free(t);
    return n;
}
static void world_boot(void) {
    if (!bv_world_on) return;
    extern char bv_profile_slot[24];
    if (bv_profile_slot[0]) snprintf(g_world_path, sizeof g_world_path, BV_DATA_DIR "/world_%s.bin", bv_profile_slot);
    g_world = (bw_world *)malloc(sizeof *g_world);
    if (!g_world) { bv_log("world: no memory for the world (%u bytes): world off", (unsigned)sizeof *g_world); return; }
    uint32_t now = (uint32_t)time(NULL);
    int loaded = 0;
    SceUID fd = sceIoOpen(g_world_path, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        int r = sceIoRead(fd, g_world, (SceSize)sizeof *g_world);
        sceIoClose(fd);
        loaded = r == (int)sizeof *g_world && bw_valid(g_world);
        if (!loaded) bv_log("world: %s is not a world of this build (%d bytes read): a new world is made", g_world_path, r);
    }
    if (!loaded) {
        bw_params p;
        memset(&p, 0, sizeof p);
        p.n_bots = bv_world_bots;
        p.n_clubs = 60;
        if (g_st.cfg.br_rar[1] < 0) {   /* rarities (also without box_drops) */
            char *cards = read_text(BV_DATA_DIR "/assets/csv_logic/cards.csv");
            if (cards) { bv_csv_rarities(cards, &g_st.cfg); free(cards); }
        }
        int cr, kr;
        for (int i = 0; bmsg_unlock_entry(i, &cr, &kr) && p.n_br < BW_NBR; i++) {
            p.br_row[p.n_br] = cr;
            p.br_rar[p.n_br] = cr >= 0 && cr < 96 ? g_st.cfg.br_rar[cr] : -1;
            if (i == 0) p.br_rar[p.n_br] = 0;
            p.n_br++;
        }
        p.n_badges = csv_rows(BV_DATA_DIR "/assets/csv_logic/alliance_badges.csv");
        p.n_thumbs = csv_rows(BV_DATA_DIR "/assets/csv_logic/player_thumbnails.csv");
        p.n_colors = csv_rows(BV_DATA_DIR "/assets/csv_logic/name_colors.csv");
        bw_create(g_world, &p, now ^ 0x5eed1234u, now);
        bv_log("world: NEW world %s: %d bots, %d clubs, %d brawlers, %d badges / %d thumbnails / %d name colours (first bot '%s', club '%s')", g_world_path,
               g_world->n_bots, g_world->n_clubs, g_world->n_br, p.n_badges, p.n_thumbs, p.n_colors, g_world->bots[0].name, g_world->clubs[0].name);
    }
    if (g_opt.season_timer && g_prof.season_start > 0)
        bw_set_season(g_world, (uint32_t)g_prof.season_start, (uint32_t)(g_opt.season_days > 0 ? g_opt.season_days : 70) * 86400u);
    uint32_t from = g_world->sim_to;
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    uint32_t m = bw_simulate(g_world, now, bv_world_days);
    unsigned ms = (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000);
    int top = 0, best = -1;
    for (int i = 0; i < g_world->n_bots; i++) { int t = bw_bot_total(g_world, i); if (t > top) { top = t; best = i; } }
    bv_log("world: %s, offline for %u s: %u bot matches simulated in %u ms (total %u); top bot '%s' %d trophies", loaded ? "loaded" : "made",
           (unsigned)(now > from ? now - from : 0), (unsigned)m, ms, (unsigned)g_world->matches, best >= 0 ? g_world->bots[best].name : "-", top);
    world_save();
    g_st.world = g_world;
    g_st.world_save = world_save;
    g_st.world_region = bw_region_index(bv_world_region);
    if (g_st.world_region < 0) g_st.world_region = 0;
    g_st.world_tick = g_st.world_saved = now;
    bv_log("world: on (region %s = %s, world_match=%d)", bw_region_code(g_st.world_region), bw_region_name(g_st.world_region), bv_world_match);
}
const bw_world *bvb_world(void) { return g_world; }
/* the player's streak from the battle history (3v3 only): > 0 wins in a row, < 0 losses */
static int player_streak(void) {
    int s = 0;
    for (int i = 0; i < g_prof.n_hist && i < 10; i++) {
        int k = (g_prof.n_hist - 1 - i) % 10;
        if (g_prof.hist[k].kind != 0) break;
        int r = g_prof.hist[k].result;
        if (r == 0) { if (s < 0) break; s++; }
        else if (r == 1) { if (s > 0) break; s--; }
        else break;
    }
    return s;
}
/* called on the game thread when a battle starts (main.c bv_start_game_wrap): picks the opponents / team mates from the world; names[n] = the
 * world bot for name slot n (TID_BOT_n). Returns the bots planned (0 = world_match off). */
int bvb_world_plan(const char *names[BW_SLOTS]) {
    for (int n = 0; n < BW_SLOTS; n++) { names[n] = NULL; bv_botname_override[n] = NULL; }
    g_st.plan_on = 0;
    if (!g_world || !bv_world_match) return 0;
    uint32_t now = (uint32_t)time(NULL);
    bw_simulate(g_world, now, 0);
    int r = g_st.cfg.brawler_row;
    int t = r >= 0 && r < 96 && g_st.cfg.br_has[r] ? g_st.cfg.br_tro[r] : g_st.cfg.trophies;
    int streak = player_streak();
    for (int n = 0; n < BW_SLOTS; n++) g_st.plan[n].bot = -1;
    int got = bw_match_plan(g_world, t, streak, BW_SLOTS - 1, now ^ (uint32_t)sceKernelGetProcessTimeWide(), now, g_st.plan);
    for (int n = 1; n <= got; n++) {
        names[n] = g_world->bots[g_st.plan[n].bot].name;
        bv_botname_override[n] = names[n];
    }
    g_st.plan_on = got > 0;
    bv_log("world: match plan for brawler row %d at %d trophies, streak %d: %d bots (slot 1 '%s' %d trophies level %d tier %d)", r, t, streak, got,
           got ? names[1] : "-", got ? g_world->bots[g_st.plan[1].bot].tro[g_st.plan[1].slot] : 0, got ? g_st.plan[1].level : 0, got ? g_st.plan[1].tier : 0);
    return got;
}
const bw_pick *bvb_world_pick(int slot) {
    if (!g_st.plan_on || slot < 1 || slot >= BW_SLOTS || g_st.plan[slot].bot < 0) return NULL;
    return &g_st.plan[slot];
}
/* update 174: one csv line -> up to 32 fields (quotes removed); returns the field count, *next = the following line */
static int csv_line(const char *s, char f[][48], int maxf, const char **next) {
    int k = 0;
    while (*s && *s != '\n' && k < maxf) {
        int q = *s == '"', o = 0;
        if (q) s++;
        while (*s && (q ? *s != '"' : (*s != ',' && *s != '\n' && *s != '\r'))) { if (o < 47) f[k][o++] = *s; s++; }
        if (q && *s == '"') s++;
        while (*s && *s != ',' && *s != '\n') s++;
        f[k][o] = 0;
        k++;
        if (*s == ',') s++;
    }
    while (*s && *s != '\n') s++;
    *next = *s ? s + 1 : s;
    return k;
}
/* gadget (MetaType 5) / star power (MetaType 4) card rows of each brawler: Target = the brawler's Name in characters.csv (named rows only) */
static int bv_csv_cards(const char *cards, const char *chars, bmsg_cfg *c) {
    static char names[128][48];
    char f[32][48];
    const char *s = chars, *nx;
    int nn = 0;
    csv_line(s, f, 32, &nx); s = nx; csv_line(s, f, 32, &nx); s = nx;   /* header + types */
    while (*s && nn < 128) { int k = csv_line(s, f, 1, &nx); s = nx; if (k && f[0][0]) strcpy(names[nn++], f[0]); }
    memset(c->gadget_card, -1, sizeof c->gadget_card);
    memset(c->sp_card, -1, sizeof c->sp_card);
    s = cards;
    int k = csv_line(s, f, 32, &nx), c_tgt = -1, c_meta = -1;
    for (int i = 0; i < k; i++) { if (!strcmp(f[i], "Target")) c_tgt = i; if (!strcmp(f[i], "MetaType")) c_meta = i; }
    if (c_tgt < 0 || c_meta < 0) return 0;
    s = nx; csv_line(s, f, 32, &nx); s = nx;
    int row = 0, found = 0;
    while (*s) {
        k = csv_line(s, f, 32, &nx); s = nx;
        if (!k || !f[0][0]) continue;
        int meta = atoi(f[c_meta]);
        if ((meta == 4 || meta == 5) && c_tgt < k)
            for (int b = 0; b < nn && b < 96; b++)
                if (!strcmp(names[b], f[c_tgt])) {
                    short *slot = meta == 5 ? c->gadget_card[b] : c->sp_card[b];
                    if (slot[0] < 0) slot[0] = (short)row; else if (slot[1] < 0) slot[1] = (short)row;
                    found++;
                    break;
                }
        row++;
    }
    return found;
}
static int g_trophy_last = -2; /* update 91: last reached trophy road (Type 6) index, -1 none, -2 unknown */
int bv_trophy_last_pub = -2;   /* update 133 (H5): copy for loader/main.c's trophy road row filter */
/* update 101 (B10): power points needed from power level index p (0 = level 1) to the next level, v29 */
static const int k_pp_cost[8] = {20, 30, 50, 80, 130, 210, 340, 550};
static const bv_passdata *load_pass_tables(void) {
    char *ms = read_text(BV_DATA_DIR "/assets/csv_logic/milestones.csv");
    char *sk = read_text(BV_DATA_DIR "/assets/csv_logic/skins.csv");
    int n = ms ? bv_pass_load_milestones(&g_pass, ms) : -2;
    g_trophy_last = ms ? bv_trophy_road_last(ms, g_opt.account_trophies) : -2;   /* update 91 (trophy road) */
    int ns = sk ? bv_pass_load_skins(&g_pass, sk) : -2;
    {   /* update 183: brawler names for Trophy Road brawler rewards */
        char *ch = read_text(BV_DATA_DIR "/assets/csv_logic/characters.csv");
        g_pass.n_brawlers = 0;
        if (ch) {
            char f[32][48];
            const char *s = ch, *nx;
            csv_line(s, f, 32, &nx); s = nx; csv_line(s, f, 32, &nx); s = nx;
            while (*s && g_pass.n_brawlers < 128) { int k = csv_line(s, f, 1, &nx); s = nx; if (k && f[0][0]) { strncpy(g_pass.brawlers[g_pass.n_brawlers], f[0], 31); g_pass.n_brawlers++; } }
            free(ch);
        }
    }
    {   /* update 184: which brawler each skin belongs to (skin_confs.csv Character) */
        char *sc = read_text(BV_DATA_DIR "/assets/csv_logic/skin_confs.csv");
        if (!sc) sc = read_text(BV_DATA_DIR "/assets/csv_client/skin_confs.csv");
        int nm = sc ? bv_pass_load_skin_confs(&g_pass, sc) : -2;
        bv_log("shop: %d skins mapped to their brawler (skin_confs.csv)", nm);
        free(sc);
    }
    free(ms);
    free(sk);
    bv_log("Brawl Pass tables: %d pass tiers (milestones.csv), %d skins (skins.csv)%s", n, ns, (n > 0 && ns > 0) ? "" : " - claims get 1 coin");
    return (n > 0 && ns > 0) ? &g_pass : NULL;
}

int bv_skin_special = 1, bv_shop_date, bv_shop_seasonal_all;   /* 184b: shop_seasonal_all */   /* update 184: config skin_special (0 = skins back under SKINS), shop_date=MMDD (0 = today) */
/* update 184: a random owned brawler row other than not_row (Brawl TV hero), -1 if none */
int bvb_random_owned_brawler(unsigned seed, int not_row) {
    int br[64], c[64], nc = 0, nb = bmsg_owned_brawler_rows(&g_st.cfg, br, 64);
    for (int i = 0; i < nb; i++) if (br[i] >= 0 && br[i] != not_row) c[nc++] = br[i];
    if (!nc) return -1;
    seed ^= seed >> 13; seed *= 0x5bd1e995u; seed ^= seed >> 15;
    return c[seed % (unsigned)nc];
}

void bvb_hook_configure(const so_module *libg, const bvb_options *o) {
    g_m = libg;
    g_opt = *o;
}

static int check_prologue(uint32_t off, uint32_t expect, const char *what) {
    uint32_t w = arm_get32(g_m->host + off);
    if (w != expect) {
        bv_log("offline: %s at libg+0x%x starts with %08x, expected %08x: this is not the library the offsets belong to", what, off, w, expect);
        return 0;
    }
    return 1;
}

void bvb_hook_boot(void) {
    if (!g_m || !g_opt.enabled) {
        bv_log("offline: disabled by configuration");
        return;
    }
    int ok = check_prologue(OFF_SEND_MESSAGE, 0xE92D4FF0u, "send-message function") &&
             check_prologue(OFF_CREATE_MESSAGE, 0xE92D4830u, "createMessage") &&
             check_prologue(OFF_GET_TABLE, 0xE59F1008u, "getTable") &&
             check_prologue(OFF_FLAG_FALSE, ARM_MOV_R0_1, "flag function A") && check_prologue(OFF_FLAG_TRUE, ARM_MOV_R0_0, "flag function B");
    if (!ok) {
        bv_log("offline: hook disabled");
        g_opt.enabled = 0;
        return;
    }
    memcpy(g_orig, g_m->host + OFF_SEND_MESSAGE, 8);
    g_boot_ok = 1;
    if (g_opt.patches) {
        /* the original mod flips these two boolean getters at start-up */
        uint8_t zero = 0, one = 1;
        write_code(OFF_FLAG_FALSE, &zero, 1);
        write_code(OFF_FLAG_TRUE, &one, 1);
        bv_log("offline: boot patches applied (libg+0x%x returns 0, libg+0x%x returns 1)", OFF_FLAG_FALSE, OFF_FLAG_TRUE);
    }
    memset(&g_gi, 0, sizeof g_gi);
    g_gi.msg_type = gi_msg_type;
    g_gi.msg_encode = gi_msg_encode;
    g_gi.msg_payload = gi_msg_payload;
    g_gi.deliver = gi_deliver;
    g_gi.login_prepare = gi_login_prepare;
    g_gi.login_done = gi_login_done;
    g_gi.after_login = gi_after_login;
    g_gi.table_rows = gi_table_rows;
    g_gi.log = glog;
    g_lock = sceKernelCreateMutex("bv_offline", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
    g_gi.lock = gi_lock;
    g_gi.unlock = gi_unlock;
    bvb_init(&g_st, &g_gi);
    g_st.resend_home = g_opt.resend_home;
    g_st.cfg.brawler_row = g_opt.brawler_row;
    g_st.cfg.map_row = g_opt.map_row;
    g_st.cfg.events = g_opt.events;
    g_st.cfg.unlock_all = g_opt.unlock_all;
    g_st.cfg.trophies = g_opt.trophies;
    g_st.cfg.account_trophies = g_opt.account_trophies;
    g_st.cfg.power_level = g_opt.power_level;
    g_st.cfg.home_flags = g_opt.home_flags;
    g_st.cfg.shop_disabled = g_opt.shop_disabled;
    g_st.cfg.offer_test = g_opt.offer_test;
    g_st.cfg.tutorial_steps = g_opt.tutorial_steps;
    g_st.cfg.box_coin_type = g_opt.box_coin_type;
    g_st.cfg.gems = g_opt.gems;
    g_st.cfg.coins = g_opt.coins;
    g_st.cfg.shop_offers = g_opt.shop_offers;
    g_st.cfg.offer_cur = g_opt.offer_cur;
    g_st.cfg.owned_skins = g_opt.owned_skins;
    g_st.cfg.res_markers = g_opt.res_markers;
    g_st.cfg.box_pp_random = g_opt.box_pp_random;
    g_st.cfg.offer_unit = g_opt.offer_unit;
    g_st.cfg.pass_claim_reply = g_opt.pass_claim_reply;
    if (g_opt.pass_claim_reply >= 2) g_st.pass = load_pass_tables();
    if (g_st.pass) ((bv_passdata *)g_st.pass)->new_types = g_opt.pass_new_types;   /* tables are allocated writable at boot */
    g_st.cfg.ack24125 = g_opt.ack24125;
    g_st.cfg.home_on_14109 = g_opt.home_on_14109;
    g_st.cfg.profile_reply = g_opt.profile_reply;
    g_st.cfg.home_a4 = g_opt.home_a4;
    g_st.cfg.n_limits = g_opt.n_limits;
    memcpy(g_st.cfg.tok_cfg, g_opt.tok_cfg, sizeof g_st.cfg.tok_cfg);
    memcpy(g_st.cfg.limits, g_opt.limits, sizeof g_st.cfg.limits);
    g_st.cfg.home_a8 = g_opt.home_a8;
    g_st.cfg.quests_on = g_opt.quests_on;
    g_st.cfg.battle_end_msg = g_opt.battle_end_msg;
    g_st.cfg.battle_trophies = g_opt.battle_trophies;
    g_st.cfg.battle_win = g_opt.battle_win;
    g_st.cfg.battle_draw = g_opt.battle_draw;
    g_st.cfg.battle_lose = g_opt.battle_lose;
    g_st.cfg.member_view_on = g_opt.member_view;
    g_st.cfg.team_m10 = g_opt.team_m10;   /* -1 = auto: resolved in bmsg_avatar_update from the selected brawler (cycle 6, G2) */
    g_st.cfg.team_m14 = g_opt.team_m14;
    g_st.cfg.team_m18 = g_opt.team_m18;
    g_st.cfg.team_m1c = g_opt.team_m1c;
    g_st.cfg.team_f0 = g_opt.team_f0;
    g_st.cfg.team_f8 = g_opt.team_f8;
    g_st.cfg.mode_push_map = g_opt.mode_push_map;
    g_st.cfg.mode_push_after = g_opt.mode_push_after;
    g_st.cfg.skin_offer_kind = g_opt.skin_offer_kind;
    /* update 62: GEMS = avatar+0x60 = avatar_tail[1] (HARDWARE-VERIFIED run 55: avatar_tail[1] = 2222 showed 2222 gems in the bar); not a list-0 entry */
    if (g_opt.gems >= 0 && !g_opt.avatar_tail_set) g_st.cfg.avatar_tail[1] = g_opt.gems;
    g_st.cfg.gems = -1;
    if (g_opt.avatar_tail_set) for (int i = 0; i < 12; i++) g_st.cfg.avatar_tail[i] = g_opt.avatar_tail[i];
    g_st.cfg.box_unit_mode = g_opt.box_unit_mode;
    memcpy(g_st.cfg.offer_type, g_opt.offer_type, sizeof g_st.cfg.offer_type);
    g_st.cfg.n_extra_keys = g_opt.n_extra_keys;
    memcpy(g_st.cfg.extra_key, g_opt.extra_key, sizeof g_st.cfg.extra_key);
    g_st.cfg.brawl_pass = g_opt.brawl_pass;
    g_st.cfg.pass_season = g_opt.pass_season;
    g_st.cfg.pass_tokens = g_opt.pass_tokens;
    g_st.cfg.pass_premium = g_opt.pass_premium;
    /* cycle 4 (stream G): saved progress */
    if (g_opt.profile_on) {
        if (bv_profile_slot[0]) {   /* update 143 */
            snprintf(g_profile_path, sizeof g_profile_path, BV_DATA_DIR "/profile_%s.txt", bv_profile_slot);
            snprintf(g_profile_tmp, sizeof g_profile_tmp, BV_DATA_DIR "/profile_%s.tmp", bv_profile_slot);
            bv_log("profile: slot '%s' -> %s", bv_profile_slot, g_profile_path);
        }
        /* update 144 (user requirements 8 / 9): every slot except 'test' (and the old unnamed profile.txt) is a REAL account: it starts fresh (Shelly
         * only, 0 trophies, power 1, no gems / coins) and the test keys of config.txt (gems, coins, trophies, account_trophies, power_level, unlock_all,
         * profile_gems, give_skin) are ignored for it, so test values can never leak into it. A new personal file is never copied from another one. */
        g_fresh = bv_profile_slot[0] && strcmp(bv_profile_slot, "test") != 0;
        if (g_fresh) {
            g_opt.unlock_all = 0;
            g_opt.account_trophies = 0;
            g_opt.trophies = 0;
            g_opt.power_level = 0;
            g_opt.gems = 0;
            g_opt.coins = 0;
            g_opt.profile_gems = -1;
            g_opt.n_give_skin = 0;
            g_st.cfg.unlock_all = 0;
            g_st.cfg.account_trophies = 0;
            g_st.cfg.trophies = 0;
            g_st.cfg.power_level = 0;
            bv_log("profile: '%s' is a real account: test values from config.txt ignored (fresh start values)", bv_profile_slot);
        }
        char *txt = read_text(PROFILE_PATH);
        if (txt) {
            int bad = bv_profile_parse(&g_prof, txt);
            free(txt);
            bv_log("profile: loaded %s (%d unknown lines): gems %d, coins %d, %d skins, %d claimed tiers", PROFILE_PATH, bad, g_prof.gems, g_prof.coins,
                   g_prof.n_skins, g_prof.n_claimed);
        } else {
            bv_profile_default(&g_prof);
            g_prof.gems = g_opt.gems >= 0 ? g_opt.gems : 0;
            g_prof.coins = g_opt.coins >= 0 ? g_opt.coins : 0;
            profile_save();
            bv_log("profile: new %s from the config defaults: gems %d, coins %d", PROFILE_PATH, g_prof.gems, g_prof.coins);
        }
        if (bv_profile_mode == 2) {   /* update 188: profile=max - everything unlocked and maxed (applied at every start) */
            int cr, kr, nb = 0, nc = 0, nsk = 0, ncl = 0;
            for (int i = 0; bmsg_unlock_entry(i, &cr, &kr); i++) {
                bv_brawler *b = bv_profile_unlock(&g_prof, cr);
                if (!b) continue;
                if (b->trophies < 1250) b->trophies = 1250;   /* rank 35 */
                if (b->best_trophies < b->trophies) b->best_trophies = b->trophies;
                b->power = 9;                                 /* power 10 */
                nb++;
            }
            char *cd = read_text(BV_DATA_DIR "/assets/csv_logic/cards.csv"), *ch = read_text(BV_DATA_DIR "/assets/csv_logic/characters.csv");
            if (cd && ch) {
                bv_csv_cards(cd, ch, &g_st.cfg);
                for (int r = 0; r < 96; r++)
                    for (int k = 0; k < 2; k++) {
                        if (g_st.cfg.gadget_card[r][k] >= 0) nc += bv_profile_own_card(&g_prof, g_st.cfg.gadget_card[r][k]);
                        if (g_st.cfg.sp_card[r][k] >= 0) nc += bv_profile_own_card(&g_prof, g_st.cfg.sp_card[r][k]);
                    }
            }
            free(cd); free(ch);
            if (g_st.pass) {
                for (int s = 0; s < g_st.pass->n_skins; s++) nsk += bv_profile_own_skin(&g_prof, s);
                for (int t = 0; t < g_st.pass->n; t++) ncl += bv_profile_mark_claimed(&g_prof, 9, g_opt.pass_season, t) + bv_profile_mark_claimed(&g_prof, 10, g_opt.pass_season, t);
            }
            if (g_prof.pass_tokens < 99999) g_prof.pass_tokens = 99999;
            if (g_prof.exp < 9999999) g_prof.exp = 9999999;   /* max player level */
            bv_log("profile: max: %d brawlers at rank 35 / power 10, %d cards, %d skins, %d pass tiers claimed", nb, nc, nsk, ncl);
        }
        profile_topup();
        if (bv_profile_mode) profile_save();
        g_st.profile = &g_prof;
        g_st.profile_save = profile_save;
        g_st.day = (int)((time(NULL) - 8 * 3600) / 86400);   /* update 103 (B12): the shop day starts at 08:00 UTC */
        if (!g_opt.avatar_tail_set) g_st.cfg.avatar_tail[1] = g_prof.gems;   /* gems (avatar+0x60) */
        g_st.cfg.coins = g_prof.coins;                                       /* coins (list-0 row 8) */
        if (g_opt.owned_skins >= 0) { g_st.cfg.own_skins = g_prof.skins; g_st.cfg.n_own_skins = g_prof.n_skins; } /* owned_skins=-1 still = all */
        g_st.cfg.own_cards = g_prof.cards; g_st.cfg.n_own_cards = g_prof.n_cards;   /* update 174 */
        g_st.cfg.offer_bought_mask = 0;
        for (int i = 0; i < BMSG_MAX_OFFERS; i++) if (bv_profile_is_bought(&g_prof, g_st.day, i)) g_st.cfg.offer_bought_mask |= 1 << i;
        g_st.cfg.pass_claimed_bits = g_opt.pass_claimed_bits;
        if (g_prof.selected_brawler > 0) g_st.cfg.brawler_row = g_prof.selected_brawler;
        if (g_prof.thumbnail >= 0) g_st.cfg.thumbnail_row = g_prof.thumbnail;   /* update 182 */
        if (g_prof.name_color >= 0) g_st.cfg.name_color_row = g_prof.name_color;
        if (g_st.cfg.brawler_row >= 0 && g_st.cfg.brawler_row < 96 && g_prof.skinsel[g_st.cfg.brawler_row] >= 0) g_st.cfg.sel_skin_row = g_prof.skinsel[g_st.cfg.brawler_row];
        /* update 109 (E3 / E6) map_rotation=1: maps rotate every 4 h 48 min, from the user's locations.csv */
        g_st.cfg.upcoming_events = g_opt.upcoming_events;
        { extern int bv_maprot_all, bv_all_maps; bv_maprot_all = bv_all_maps; }   /* update 127 */
        if (g_opt.map_rotation) {
            char *loc = read_text(BV_DATA_DIR "/assets/csv_logic/locations.csv");
            long now = (long)time(NULL);
            int wday = (int)(((now - 8 * 3600) / 86400 + 4) % 7);
            long r = now / 17280;
            static const char *const rot2[4] = {"AttackDefend", "BountyHunter", "RoboWars", "KingOfHill"};   /* update 127 (user): slot 5 cycles Heist / Bounty / Siege / Hot Zone */
               /* update 125 (A15): KingOfHill entries crashed the events screen / Play (run 145 dump: entry +0x14 map NULL at libg+0x475294) */
            /* update 144 (req. 17 / 18 / 20): visible slots = Gem Grab, Showdown (Duo Showdown shares its card: 'SOLO-DUO', run 149), Brawl Ball and
             * three ROTATING slots (Heist / Bounty / Siege / Hot Zone, never the same mode twice at once). All seven advance in ONE global queue (each
             * lasts 4 h 48 min; one event changes every 4h48m / 7 = 41 min, in queue order). Boss Fight (Raid) on weekends, sent FIRST so it gets the
             * top row. Slot ids stay 1..6 for the old six entries (the client knows them), the two extra rotating slots get 7 and 8. */
            static const char *const pool[4] = {"AttackDefend", "BountyHunter", "RoboWars", "KingOfHill"};
            /* slot ids as on the console since update 126 (run 149 events screen OK): gem grab 1, showdown 2, brawl ball 8, duo 4 (merged into the
             * showdown card), rotating 6 and 7, and slot 3 = the third rotating slot on weekdays / Boss Fight on weekends (shown third, its own card) */
            /* update 146 (user T12 / T13, run 168): Brawl Ball was in the wrong place (slot 8 = 'SOLO EVENTS') -> slot 3, the third card. Rotating
             * slots 6, 8 and 7; on weekends slot 7 (TID_EVENT_SLOT_NAME_7) is Boss Fight instead of the third rotating mode.
             * Queue: every 4 h 48 min the NEXT event in order gets a new map (user: 'one event in order should reset every 4h48m'), so with 7 events
             * each map stays 7 x 4h48m. */
            static const struct { const char *mode; int slot, pos, rot; } plan[7] = {
                {"CoinRush", 1, 0, -1}, {"BattleRoyale", 2, 1, -1}, {"LaserBall", 3, 2, -1}, {"BattleRoyaleTeam", 4, 1, -1},
                {NULL, 6, 3, 0}, {NULL, 8, 4, 1}, {NULL, 7, 5, 2}};
            /* update 148 (user, run 172): one step = 4 h, the six cards in order 1..6 (Duo Showdown shares the Showdown card, so it rotates with it) */
            const long period = 14400L * 6;
            int weekend = (wday == 6 || wday == 0);
            int n = 0;
            (void)r;
            if (weekend && g_st.cfg.events > 0) {
                int rows[64], idx = 0, left = 0, id = 0, nm = bv_maps_for_mode(loc, "Raid", rows, 64);
                if (nm > 0) {
                    bv_rotation(now, 7, nm, &idx, &left, &id);
                    long wkend_left = (long)(wday == 6 ? 2 : 1) * 86400L - ((now - 8 * 3600) % 86400);   /* until Monday 08:00 UTC */
                    g_st.cfg.event_slot[n] = 7;
                    g_st.cfg.event_map[n] = rows[idx];
                    g_st.cfg.event_left[n] = (int)wkend_left;
                    g_st.cfg.event_id[n] = (int)(((now - 8 * 3600) / 86400 / 7) % 100000) * 16 + 15;
                    g_st.cfg.next_map[n] = -1;
                    bv_log("events: entry %d (slot 7) Raid (Boss Fight, weekend, top row) map row %d of %d, %ld s left", n, rows[idx], nm, wkend_left);
                    n++;
                }
            }
            for (int i = 0; loc && g_st.cfg.events > 0 && i < 7 && n < 8; i++) {
                if (weekend && n > 0 && i == 6) continue;   /* weekend: slot 7 is Boss Fight */
                int left = 0;
                long k = bv_queue_rotation(now, plan[i].pos, 6, period, &left);
                const char *mode = plan[i].mode ? plan[i].mode : pool[bv_queue_mode(k, plan[i].rot, 3, 4)];
                const char *mode2 = plan[i].mode ? plan[i].mode : pool[bv_queue_mode(k + 1, plan[i].rot, 3, 4)];
                int rows[64], nm = bv_maps_for_mode(loc, mode, rows, 64);
                if (nm <= 0) { bv_log("events: slot %d mode %s: no maps in locations.csv", plan[i].slot, mode); continue; }
                int idx = (int)(((k + plan[i].slot * 3) % nm + nm) % nm);   /* the csv's map order, one step per rotation */
                g_st.cfg.event_slot[n] = plan[i].slot;
                g_st.cfg.event_map[n] = rows[idx];
                g_st.cfg.event_left[n] = left;
                g_st.cfg.event_id[n] = (int)((k % 100000L) * 16 + plan[i].slot);
                {
                    int rows2[64], n2 = bv_maps_for_mode(loc, mode2, rows2, 64);
                    g_st.cfg.next_map[n] = n2 > 0 ? rows2[(int)((((k + 1) + plan[i].slot * 3) % n2 + n2) % n2)] : -1;
                    g_st.cfg.next_id[n] = (int)(((k + 1) % 100000L) * 16 + plan[i].slot);
                }
                if (i == 0) g_st.cfg.map_row = rows[idx];   /* Gem Grab: the default selection */
                bv_log("events: entry %d (slot %d, queue %d) %s map row %d of %d, rotation %ld, %d s left", n, plan[i].slot, plan[i].pos, mode, rows[idx], nm, k, left);
                n++;
            }
            if (n > 0) { g_st.cfg.events = n; g_st.cfg.ev_maps_exact = 1; }
            free(loc);
        }
        /* update 104 (D3): quest day = 08:00 UTC days; Tue/Thu since the season start (needs season_timer for the start) */
        {
            long now = (long)time(NULL);
            g_st.cfg.quest_day = (int)((now - 8 * 3600) / 86400);
            g_st.cfg.quest_refresh_s = (int)(86400 - (now - 8 * 3600) % 86400);
            {   /* update 108 (D8): next Tuesday / Thursday at 08:00 UTC */
                int d = (int)((now - 8 * 3600) / 86400), add = 1;
                while (((d + add + 4) % 7) != 2 && ((d + add + 4) % 7) != 4) add++;
                g_st.cfg.quest_batch_s = g_st.cfg.quest_refresh_s + (add - 1) * 86400;
                bv_log("quests: daily refresh in %d s, next season batch in %d s", g_st.cfg.quest_refresh_s, g_st.cfg.quest_batch_s);
            }
        }
        /* update 103 (D4) season_timer=1: a season lasts season_days (70); its start is saved in the profile and rolls over */
        if (g_opt.season_timer) {
            long now = (long)time(NULL), len = (long)(g_opt.season_days > 0 ? g_opt.season_days : 70) * 86400L;
            int rolled = 0;
            int had_start = g_prof.season_start > 0 && g_prof.season_start <= now;   /* update 130: the first start is not a season end */
            if (g_prof.season_start <= 0 || g_prof.season_start > now) { g_prof.season_start = now; rolled = 1; }
            while (now - g_prof.season_start >= len) { g_prof.season_start += len; rolled = 1; }
            g_st.cfg.season_left = (int)(g_prof.season_start + len - now);
            {   /* update 104: Tuesdays + Thursdays (08:00 UTC days) since the season start, the start day counts as one */
                int d0 = (int)((g_prof.season_start - 8 * 3600) / 86400), d1 = (int)((now - 8 * 3600) / 86400), tt = 1;
                for (int d = d0 + 1; d <= d1; d++) { int wd = (d + 4) % 7; if (wd == 2 || wd == 4) tt++; }
                g_st.cfg.season_tt = tt;
            }
            if (rolled && g_opt.trophy_league && had_start) {   /* update 130 (D6): trophy league ends with the season */
                int lost = 0, paid = bv_profile_league_end(&g_prof, &lost);
                g_prof.trophy_delta -= lost;
                bv_log("trophy league: season end: %d trophies reset, %d star points paid (now %d)", lost, paid, g_prof.star_points);
            }
            if (rolled) profile_save();
            bv_log("season: started %ld, %d days long, %d s left (%d days)%s", g_prof.season_start, (int)(len / 86400), g_st.cfg.season_left, g_st.cfg.season_left / 86400,
                   rolled ? " (new season / start saved)" : "");
        }
        g_st.cfg.star_points = g_prof.star_points;   /* update 130 (D6) */
        g_st.cfg.exp_total = g_prof.exp;              /* update 152 */
        /* update 144 (req. 22): the home screen shows the map of the event the player selected last (its CURRENT map after rotations), not
         * always Gem Grab's. The slot is kept in the profile; an unknown slot (event gone) falls back to Gem Grab. */
        g_st.cfg.home_map = -1;
        if (g_prof.selected_slot > 0) {
            int m = bmsg_map_for_slot(&g_st.cfg, g_prof.selected_slot);
            if (m >= 0) { g_st.cfg.home_map = m; g_st.cfg.home_slot = g_prof.selected_slot; }
            bv_log("events: selected slot %d -> home map row %d%s", g_prof.selected_slot, m, m < 0 ? " (event not running: Gem Grab)" : "");
        }
        g_st.cfg.new_event_tokens = g_opt.new_event_tokens;   /* update 131 (E4) */
        g_st.cfg.battle_end_labels = g_opt.battle_end_labels;   /* update 132 (A11) */
        g_st.cfg.offer_title_slot = g_opt.offer_title_slot;   /* update 136 (B11) */
        g_st.cfg.battle_log = g_opt.battle_log;   /* update 140 (G7) */
        g_st.cfg.battle_log_sd = g_opt.battle_log_sd;   /* update 168b */
        for (int i = 0; i < 8; i++) g_st.cfg.ev_unclaimed[i] = g_st.cfg.event_id[i] > 0 && !bv_profile_event_claimed(&g_prof, g_st.cfg.event_id[i]);
        /* update 117 (E7) play_tokens=1: token pool from the profile -> key bar (home +0xa4 / +0xa8, config +0xac cap / +0xb0 step), pass tokens */
        if (g_opt.play_tokens) {
            long now = (long)time(NULL);
            int next = bv_tokens_refill(&g_prof, now);
            g_st.cfg.play_tokens = 1;
            g_st.cfg.home_a4 = g_prof.tokens;
            g_st.cfg.home_a8 = next;
            if (!g_opt.tok_cfg[1]) { g_st.cfg.tok_cfg[0] = 0; g_st.cfg.tok_cfg[1] = BV_TOKEN_CAP; g_st.cfg.tok_cfg[2] = BV_TOKEN_STEP; g_st.cfg.tok_cfg[3] = BV_TOKEN_PERIOD; }
            if (g_prof.pass_tokens > g_st.cfg.pass_tokens) g_st.cfg.pass_tokens = g_prof.pass_tokens;
            profile_save();
            bv_log("tokens: %d of %d available, next +%d in %d s, Brawl Pass tokens %d", g_prof.tokens, BV_TOKEN_CAP, BV_TOKEN_STEP, next, g_prof.pass_tokens);
        }
        /* cycle 7 (G3 / G4): gems set by config, skins given by name (e.g. BarkeepWizard = Wizard Barley without Supercell ID) */
        {
            int changed = 0;
            if (g_opt.profile_gems >= 0 && g_prof.gems != g_opt.profile_gems) {
                bv_log("profile: gems %d -> %d (profile_gems)", g_prof.gems, g_opt.profile_gems);
                g_prof.gems = g_opt.profile_gems;
                changed = 1;
            }
            if (g_opt.n_give_skin > 0) {
                const bv_passdata *pd = g_st.pass ? g_st.pass : load_pass_tables();
                for (int i = 0; i < g_opt.n_give_skin; i++) {
                    int row = pd ? bv_pass_skin_row(pd, g_opt.give_skin[i]) : -1;
                    if (row < 0) { bv_log("profile: give_skin %s: not in skins.csv", g_opt.give_skin[i]); continue; }
                    if (bv_profile_has_skin(&g_prof, row)) { bv_log("profile: give_skin %s (row %d): already owned", g_opt.give_skin[i], row); continue; }
                    bv_profile_own_skin(&g_prof, row);
                    bv_log("profile: give_skin %s (row %d): owned now", g_opt.give_skin[i], row);
                    changed = 1;
                }
            }
            if (changed) {
                profile_save();
                if (!g_opt.avatar_tail_set) g_st.cfg.avatar_tail[1] = g_prof.gems;
                if (g_opt.owned_skins >= 0) { g_st.cfg.own_skins = g_prof.skins; g_st.cfg.n_own_skins = g_prof.n_skins; }
            }
        }
        if (g_fresh) {   /* update 144: a real account's total = the sum of its brawlers' trophies */
            g_st.cfg.account_trophies = bv_profile_total_trophies(&g_prof);
            bv_log("profile: real account: %d brawler(s), %d trophies, %d gems, %d coins", g_prof.n, g_st.cfg.account_trophies, g_prof.gems, g_prof.coins);
        } else if (g_opt.battle_trophies && g_prof.trophy_delta) {   /* update 96: kept battle results */
            g_st.cfg.account_trophies += g_prof.trophy_delta;
            if (g_st.cfg.account_trophies < 0) g_st.cfg.account_trophies = 0;
            bv_log("profile: account trophies %d (config) %+d (battles) = %d", g_opt.account_trophies, g_prof.trophy_delta, g_st.cfg.account_trophies);
        }
        /* cycle 6 experiment profile_brawlers=1 (G2): per-brawler stats from the profile; a fresh set is generated once (varied power levels,
         * trophies adding up to about the account total) and saved */
        if (g_opt.profile_brawlers) {
            int rows[96];
            int nr = bmsg_owned_brawler_rows(&g_st.cfg, rows, 96), made = 0;
            int total = g_st.cfg.account_trophies > 0 ? g_st.cfg.account_trophies : 1000, share = nr > 0 ? total / nr : 0;
            for (int i = 0; i < nr; i++) {
                if (rows[i] < 0 || rows[i] >= 96) continue;
                bv_brawler *b = bv_profile_find(&g_prof, rows[i]);
                if (!g_fresh && b && b->power < 8 && b->power_points == 0 && b->trophies > 0) {   /* update 101 (B10): older profiles had no power points */
                    uint32_t y = (uint32_t)rows[i] * 40503u + 977u;
                    b->power_points = (int)((y ^ (y >> 7)) % (uint32_t)k_pp_cost[b->power]);
                    made++;
                }
                if (!g_fresh && (!b || (b->power == 0 && b->trophies == 0 && b->best_trophies == 0 && !b->power_points))) {   /* missing or never set (test account only) */
                    if (!b) b = bv_profile_unlock(&g_prof, rows[i]);
                    if (!b) continue;
                    uint32_t x = (uint32_t)rows[i] * 2654435761u + 0x1234567u;
                    x ^= x >> 13; x *= 0x5bd1e995u; x ^= x >> 15;
                    b->power = (int)(x % 9u);                                      /* level 1..9 */
                    b->trophies = share / 2 + (int)((x >> 8) % (uint32_t)(share + 1)) + b->power * 3;
                    b->best_trophies = b->trophies + (int)((x >> 16) % 40u);
                    if (b->power < 8) b->power_points = (int)((x >> 20) % (uint32_t)k_pp_cost[b->power]);   /* update 101: inside the level's range */
                    made++;
                }
            }
            for (int i = 0; i < g_prof.n; i++) {
                int r = g_prof.b[i].row;
                if (r < 0 || r >= 96) continue;
                g_st.cfg.br_has[r] = 1;
                g_st.cfg.br_tro[r] = (short)g_prof.b[i].trophies;
                g_st.cfg.br_best[r] = (short)g_prof.b[i].best_trophies;
                g_st.cfg.br_pow[r] = (signed char)(g_prof.b[i].power > 8 ? 8 : g_prof.b[i].power);
                g_st.cfg.br_pp[r] = (short)g_prof.b[i].power_points;
            }
            if (made) profile_save();
            bv_log("profile: per-brawler stats for %d brawlers (%d generated now); selected row %d: %d trophies, power %d", g_prof.n, made, g_st.cfg.brawler_row,
                   g_st.cfg.brawler_row >= 0 && g_st.cfg.brawler_row < 96 ? g_st.cfg.br_tro[g_st.cfg.brawler_row] : -1,
                   g_st.cfg.brawler_row >= 0 && g_st.cfg.brawler_row < 96 ? g_st.cfg.br_pow[g_st.cfg.brawler_row] + 1 : -1);
        }   /* cycle 6: the last selection (run 80: Shelly after a restart) */
        {   /* update 144 (req. 10): box drops by rarity; the rarity of each brawler comes from the user's csv_logic/cards.csv (Rarity of its _unlock card) */
            extern int bv_box_drops;
            g_st.cfg.box_drops = bv_box_drops;
            memset(g_st.cfg.br_rar, -1, sizeof g_st.cfg.br_rar);
            char *cards = bv_box_drops ? read_text(BV_DATA_DIR "/assets/csv_logic/cards.csv") : NULL;
            if (cards) {
                int found = bv_csv_rarities(cards, &g_st.cfg);
                free(cards);
                bv_log("boxes: 2020 drop rules on (box_drops=1), rarities of %d brawlers from cards.csv", found);
            }
            {   /* update 174: gadgets / star powers */
                extern int bv_box_cards;
                g_st.cfg.box_cards = bv_box_cards;
                char *cd = bv_box_cards ? read_text(BV_DATA_DIR "/assets/csv_logic/cards.csv") : NULL;
                char *ch = bv_box_cards ? read_text(BV_DATA_DIR "/assets/csv_logic/characters.csv") : NULL;
                if (cd && ch) bv_log("boxes: gadget / star power cards of %d slots from cards.csv (box_cards=1)", bv_csv_cards(cd, ch, &g_st.cfg));
                free(cd); free(ch);
            }
        }
        {   /* update 144 (req. 24): each brawler quest keeps the brawler it was given (random among the owned brawlers, in table order so the
             * selection never matters); a new pick happens only for a new quest seed, i.e. when the quest refreshes */
            int owned[96], no = bmsg_owned_brawler_rows(&g_st.cfg, owned, 96), made = 0, saved = 0;
            for (int i = 1; i < no; i++)   /* table order (the selected brawler comes first in that list) */
                for (int j = i; j > 0 && owned[j - 1] > owned[j]; j--) { int t = owned[j]; owned[j] = owned[j - 1]; owned[j - 1] = t; }
            for (int k = 0; k < 8; k++) g_st.cfg.quest_br[k] = -1;
            for (int k = 0, nq = bmsg_quest_count(&g_st.cfg); k < nq && k < 8; k++) {
                uint32_t x = bmsg_quest_seed(&g_st.cfg, k);
                if (!bmsg_quest_wants_brawler(x)) continue;
                g_st.cfg.quest_br[k] = bv_profile_quest_brawler(&g_prof, x, owned, no, &made);
                saved |= made;
                bv_log("quests: quest %d (seed %u) -> brawler row %d%s", k, (unsigned)x, g_st.cfg.quest_br[k], made ? " (new, saved)" : " (kept)");
            }
            if (saved) profile_save();
        }
        bv_profile_claimed_bits(&g_prof, 9, g_opt.pass_season, g_st.cfg.pass_bits[0]);
        bv_profile_claimed_bits(&g_prof, 10, g_opt.pass_season, g_st.cfg.pass_bits[1]);
        /* cycle 4 (B4 part 1): the daily skin offer from the user's skins.csv (needs the pass tables) */
        if (g_st.pass) {
            int price = 0, disc = 0;
            int row = bv_daily_skin(g_st.pass, g_st.day, g_prof.skins, g_prof.n_skins, &price, &disc);
            if (row >= 0) {
                g_st.cfg.skin_offer_row = row;
                g_st.cfg.skin_offer_price = price;
                bv_log("shop: daily skin (day %d) = row %d %s, %d gems (%d %% off %d)", g_st.day, row, g_st.pass->skins[row], price, disc, g_st.pass->skin_cost[row]);
            }
            /* cycle 5 experiment shop_layout=1 (PLAN.md section 6): special skin + 5 skins + 5 power-point offers + freebie */
            if (g_opt.shop_layout) {
                int rows[6], prices[6], br[64];
                int nb = bmsg_owned_brawler_rows(&g_st.cfg, br, 64);
                (void)rows; (void)prices;
                /* update 184 (user run 222): skins only for owned brawlers, exclusive skins too; seasonal skins only in their season and, like the
                 * skin of the day, as SPECIAL OFFERS: the client files a one-item skin offer under SKINS (0x2cf574 -> 0x8db68 = one item of type 4),
                 * so those carry a box item first (skin_special=1; offer_2_items = box big + offer_item_skin_small; 184b: a Mega Box, was 100 coins).
                 * 184b: shop_seasonal_all=1 lists every seasonal skin whatever the date (user test). */
                extern int bv_skin_special, bv_shop_date, bv_shop_seasonal_all;
                unsigned char have_br[96] = {0};
                for (int k = 0; k < nb; k++) if (br[k] >= 0 && br[k] < 96) have_br[br[k]] = 1;
                int mmdd = bv_shop_date;
                if (mmdd <= 0) { time_t tt = time(NULL); struct tm *tmv = gmtime(&tt); mmdd = tmv ? (tmv->tm_mon + 1) * 100 + tmv->tm_mday : 0; }
                int cat[10], sea[16];
                int ncat = bv_shop_skin_list(g_st.pass, g_st.day, mmdd, g_prof.skins, g_prof.n_skins, have_br, 0, cat, 10);
                int nsea = bv_shop_skin_list(g_st.pass, g_st.day, mmdd, g_prof.skins, g_prof.n_skins, have_br, bv_shop_seasonal_all ? 2 : 1, sea,
                                             bv_shop_seasonal_all ? 16 : 3);
                int ns = ncat > 1 ? ncat - 1 : 0;
                if (ns > 6) ns = 6;   /* update 186 (user): 6 skins in the SKINS section */
                bv_log("shop: date %04d: %d skins for sale (owned brawlers only), %d seasonal in season", mmdd, ncat, nsea);
                bmsg_offer *of = g_st.cfg.offers;
                int n = 0;
                const int gk = g_st.cfg.skin_offer_kind, ck = g_st.cfg.offer_cur;   /* gems / coins pricing kinds */
                /* cycle 6b (run 84 screenshot): offers WITH the +0x2c flag are shown under DAILY DEALS, without it in the top offers area.
                 * Special offer = no flag; daily deals = flag, rolled from the day number (same all day; bought state kept per day in the profile). */
                /* update 101 (cycle 7, B4): PLAN.md section 6 rotation, rolled from the day number (same all day) */
                uint32_t x = (uint32_t)g_st.day * 2654435761u ^ 0x5bd1e995u;
#define DRND(m) ((x = x * 1103515245u + 12345u), (int)((x >> 16) % (uint32_t)(m)))
                int wday = (int)((g_st.day + 4) % 7);   /* day 0 = 1970-01-01, a Thursday; 6 = Saturday */
                /* SPECIAL OFFERS (no flag) */
                /* update 102 (run 121: 'no special offer'): a skin is filed under SKINS whatever the flag (and a skin with the flag crashes, run 47), so the
                 * special area needs a non-skin offer every day: the pack rotates by weekday */
                {
                    static const struct { int type, amount, price, old; const char *t; } pk[7] = {
                        {10, 3, 119, 240, "MEGA PACK"}, {14, 5, 79, 150, "BIG BOX PACK"}, {10, 2, 89, 160, "DOUBLE MEGA"}, {6, 10, 49, 100, "BRAWL BOX BUNDLE"},
                        {10, 3, 119, 240, "MEGA PACK"}, {14, 4, 69, 120, "WEEKEND BIG BOXES"}, {10, 5, 179, 400, "MEGA BOX BONANZA"}};
                    bmsg_offer o = {pk[wday].type, pk[wday].amount, 0, gk, pk[wday].price, pk[wday].old, 0, pk[wday].t};
                    of[n++] = o;
                    int w2 = (wday + 3) % 7;   /* update 184b (user: more offers a day): a second pack */
                    if (strcmp(pk[w2].t, pk[wday].t)) { bmsg_offer o2 = {pk[w2].type, pk[w2].amount, 0, gk, pk[w2].price, pk[w2].old, 0, pk[w2].t}; of[n++] = o2; }
                }
                /* update 103: the pack first (the special area takes the first offer without the flag; a skin there is filed under SKINS -> run 122 'no special') */
                int special_skin = ncat > 0 ? cat[0] : -1;
                const int bonus = bv_skin_special ? 100 : 0;
                if (special_skin >= 0) {   /* update 184: the skin of the day = a special offer, discounted 25..60 % */
                    int coins_p = 0, full = bv_skin_shop_price(g_st.pass, special_skin, &coins_p);
                    int d = 25 + (int)((((unsigned)g_st.day * 2246822519u) >> 8) % 36u), pr = full * (100 - d) / 100;
                    if (pr < 1) pr = 1;
                    bmsg_offer o = {4, 1, special_skin, coins_p ? ck : gk, pr, full, 0, "SKIN OF THE DAY", bonus};
                    of[n++] = o;
                    g_st.cfg.skin_offer_row = special_skin;
                    g_st.cfg.skin_offer_price = pr;
                }
                for (int i = 0; i < nsea && n < BMSG_MAX_OFFERS; i++) {   /* update 184: seasonal skins, special offers in season only */
                    int coins_p = 0, full = bv_skin_shop_price(g_st.pass, sea[i], &coins_p);
                    bmsg_offer o = {4, 1, sea[i], coins_p ? ck : gk, full, full, 0, "SEASONAL SKIN", bonus};
                    of[n++] = o;
                }
                if (wday == 6) {                                                                                     /* every Saturday */
                    if (DRND(2)) { bmsg_offer o = {10, 1, 0, gk, 0, 80, 0, "SATURDAY GIFT"}; of[n++] = o; }          /* free Mega Box */
                    else { bmsg_offer o = {14, 1, 0, gk, 0, 30, 0, "SATURDAY GIFT"}; of[n++] = o; }                  /* free Big Box */
                }
                if (DRND(100) < 12) {                                                                                /* generous gift */
                    int g = DRND(4);
                    if (g == 1) { bmsg_offer o = {1, 250, 0, ck, 0, 0, 0, "GIFT"}; of[n++] = o; }
                    else if (g == 2 && nb > 0) { bmsg_offer o = {8, 200, br[DRND(nb)], ck, 0, 0, 0, "GIFT"}; of[n++] = o; }
                    else { bmsg_offer o = {14, 1, 0, gk, 0, 30, 0, "GIFT"}; of[n++] = o; }
                }
                /* update 146 (user T7, run 168): no gem offers at all (no daily gem pack, no gem gift) */
                /* DAILY DEALS (flag) */
                { bmsg_offer o = {6, 1, 0, ck, 0, 0, 1, "FREE"}; of[n++] = o; }                                   /* FREE Brawl Box */
                {
                    int deals = 0;
                    { bmsg_offer o = {10, 1, 0, gk, 60, 80, 1, "MEGA BOX"}; of[n++] = o; deals++; }   /* Mega Box 80 -> 60 (184b: every day) */
                    if (DRND(100) < 70) { bmsg_offer o = {14, 1, 0, gk, 20, 30, 1, "BIG BOX"}; of[n++] = o; deals++; }   /* 184b: Big Box 30 -> 20 */
                    if (DRND(100) < 50) { bmsg_offer o = {9, 1000, 0, gk, 50, 50, 1, "TOKEN DOUBLER"}; of[n++] = o; deals++; } /* B9: 1000 tokens */
                    static const int ppa[6] = {9, 18, 27, 45, 63, 90};
                    int elig[64], ne = 0;                                                                         /* cycle 6 (B7): below the maximum only */
                    for (int k = 0; k < nb; k++) {
                        int r = br[k], pw = (r >= 0 && r < 96 && g_st.cfg.br_has[r]) ? g_st.cfg.br_pow[r] : g_st.cfg.power_level;
                        if (pw < 8) elig[ne++] = r;
                    }
                    while (deals < 5 && ne > 0 && n < BMSG_MAX_OFFERS - ns) {   /* update 186 (user): 6 daily deals = FREE + 5 (184b had up to 9) */                                       /* power points, 2 coins each */
                        int a = ppa[DRND(6)];
                        bmsg_offer o = {8, a, elig[DRND(ne)], ck, a * 2, a * 2, 1, "POWER POINTS"};
                        of[n++] = o;
                        deals++;
                    }
                    /* no brawler deal: every brawler is owned (run 84 showed "Trophy Road reward / Obtained"); no star power until its DataRef is known */
                }
#undef DRND
                /* SKINS (update 184): up to 9 at full price, owned brawlers only, exclusive skins included, the skin of the day not repeated */
                for (int i = 1; i < ncat && i <= 6 && n < BMSG_MAX_OFFERS; i++) {   /* update 186: 6 skins */
                    int coins_p = 0, full = bv_skin_shop_price(g_st.pass, cat[i], &coins_p);
                    int excl = g_st.pass->skin_obtain[cat[i]] != 0 || g_st.pass->skin_cost[cat[i]] <= 0 || g_st.pass->skin_campaign[cat[i]] != 0;
                    bmsg_offer o = {4, 1, cat[i], coins_p ? ck : gk, full, full, 0, excl ? "EXCLUSIVE SKIN" : "DAILY SKIN"};
                    of[n++] = o;
                }
                g_st.cfg.n_offers = n;
                /* update 103 (B13/B14): bought marks by stable offer id, after the offers exist (was: by position, before generation) */
                g_st.cfg.offer_bought_mask = 0;
                for (int i = 0; i < n && i < BMSG_MAX_OFFERS; i++)
                    if (bv_profile_is_bought(&g_prof, g_st.day, bmsg_offer_id(&of[i]))) g_st.cfg.offer_bought_mask |= 1 << i;
                {   /* update 129 (B11) offer_tids=1: titles as text ids (after the bought marks: ids stay the same) */
                    extern int bv_offer_tids;
                    if (bv_offer_tids) for (int i = 0; i < n; i++) of[i].title = bv_offer_tid(of[i].title);
                }
                bv_log("shop: layout (cycle 7, day %d, weekday %d): %d offers (special: skin of the day + rolled packs/gifts, daily deals with the flag, 5 skins)", g_st.day, wday, n);
                for (int i = 0; i < n; i++) bv_log("shop:   offer %d: type %d x%d row %d, price %d (was %d) %s%s", i, of[i].type, of[i].amount, of[i].row, of[i].price,
                                                  of[i].old_price, of[i].promo ? "[daily deal] " : "[special] ", of[i].title ? of[i].title : "");
            }
        }
    }
    g_st.cfg.exp_level = 200; /* the original mod's values (meaning inferred) */
    /* update 91 experiment trophy_road_claimed=1: the 6th value is home+0x14 = trophy road claimed counter (last claimed Type 6 index + 2; agent report
     * trophy_road_agent.md, libg+0x24d078 / 0x396b04 / 0x2667e4). 200 = every row claimed. K + 2 = rows up to the current trophies claimed. */
    if (g_opt.trophy_road_claimed) {
        if (g_trophy_last < -1) load_pass_tables();
        {   /* update 144: the claimed counter follows the account's REAL trophies (config + battles, or a real account's brawlers), not the config value */
            char *ms = read_text(BV_DATA_DIR "/assets/csv_logic/milestones.csv");
            if (ms) { g_trophy_last = bv_trophy_road_last(ms, g_st.cfg.account_trophies); free(ms); }
        }
        if (g_trophy_last >= -1) g_st.cfg.exp_level = g_trophy_last + 2;
        bv_trophy_last_pub = g_trophy_last;
        bv_log("trophy road: last reached index %d for %d trophies -> claimed counter %d (trophy_road_claimed=1)", g_trophy_last, g_opt.account_trophies, g_st.cfg.exp_level);
    }
    g_st.cfg.exp_points = 99999;
    for (int i = 1; i < 8; i++)
        if (g_opt.event_map[i] >= 0) g_st.cfg.event_map[i] = g_opt.event_map[i];
    for (int i = 0; i < 8; i++)
        if (g_opt.event_slot[i] > 0) g_st.cfg.event_slot[i] = g_opt.event_slot[i];
    g_st.selected_map = g_opt.map_rotation ? g_st.cfg.map_row : g_opt.map_row;   /* update 109: the rotated gem grab map */
    if (g_st.cfg.home_map >= 0) g_st.selected_map = g_st.cfg.home_map;           /* update 144 (req. 22) */
    world_boot();   /* Phase 3 */
    bv_log("offline: ready (brawler row %d, map row %d, %d event(s), resend_home=%d, release_old=%d); hook goes in at the first connect to port %d",
           g_opt.brawler_row, g_opt.map_row, g_opt.events, g_opt.resend_home, g_opt.release_old, GAME_SERVER_PORT);
}

void bvb_hook_on_connect(uint16_t port_host) {
    if (!g_boot_ok || !g_opt.enabled) return;
    if (port_host != GAME_SERVER_PORT || g_hook_installed) return;
    g_hook_installed = 1;
    int ok = tramp_install();
    bv_log("offline: first connect to port %d: send-message function at libg+0x%x %s (original bytes %02x%02x%02x%02x %02x%02x%02x%02x)",
           GAME_SERVER_PORT, OFF_SEND_MESSAGE, ok ? "replaced" : "NOT replaced", g_orig[0], g_orig[1], g_orig[2], g_orig[3], g_orig[4], g_orig[5], g_orig[6],
           g_orig[7]);
}

uint32_t bvb_hook_messages(void) { return g_messages; }

/* update 162 (decode_bot_power.md): the power level (1..9) local bots get = the selected brawler's level */
int bvb_bot_level(void) {
    int r = g_st.cfg.brawler_row;
    int p = (r >= 0 && r < 96 && g_st.cfg.br_has[r]) ? g_st.cfg.br_pow[r] : g_st.cfg.power_level;
    p += 1;
    return p < 1 ? 1 : (p > 9 ? 9 : p);
}
