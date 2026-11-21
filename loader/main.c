#include <math.h>
#include <time.h>
#include "vpad.h"
/* Brawl Vita - stage m4 "offline": maps libfmod.so and libg.so (the user's own, from ux0:data/BrawlVita/lib), runs their
 * constructors and JNI_OnLoad through a logging fake JavaVM, optionally drives the game's Android lifecycle (createGameMain, start,
 * surface) with an offline message hook, virtual sockets, asset files and OpenGL ES on vitaGL, and writes everything to
 * ux0:data/BrawlVita/log.txt.
 *
 * UNVERIFIED ON HARDWARE. Nothing here has run on a Vita. See docs/USER_TEST_PLAN.md.
 */
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../jni/fake_jni.h"
#include "../server/bv_server.h"
#include "../shims/shim.h"
#include "../shims/vsock.h"
#include "antitamper.h"
#include "gamefix.h"
#include "gtrace.h"
#include "kubridge_api.h"
#include "elf_loader.h"
#include "gameapp.h"
#include "hook_b.h"
#include "botworld.h"
#ifdef BV_PUBLIC_CONFIG
#include "builtin_config.h"
#endif
#include "log.h"
#include "rawsys.h"
#include "vita_mem.h"

#define BV_BUILD "BrawlVita 0.1 beta"
#define LIB_DIR BV_DATA_DIR "/lib"
#define EXPECTED_LIBG_SIZE 13256860u
#define EXPECTED_LIBFMOD_SIZE 952092u

unsigned int _newlib_heap_size_user = 128u * 1024u * 1024u;

extern void bv_unresolved_entry(void);

static so_unresolved_tab g_unres;
static so_module g_fmod, g_libg;

/* cycle 4 (ISSUES E1): read-only probe of the team / lobby object the events window needs (mode_switch.md section 1, ISSUES E1). */
static int g_team_probe = 1; /* config team_probe */
static uint32_t g_heap_lo, g_heap_hi;
static int heap_ok(uint32_t p, uint32_t len) { return p && !(p & 3) && p >= g_heap_lo && p + len <= g_heap_hi; }
void bv_team_probe(void) {
    if (!g_team_probe || !g_libg.host) return;
    if (!g_heap_hi) {
        void *probe = malloc(16);
        SceKernelMemBlockInfo info;
        memset(&info, 0, sizeof info);
        info.size = sizeof info;
        if (probe && sceKernelGetMemBlockInfoByAddr(probe, &info) >= 0) {
            g_heap_lo = (uint32_t)info.mappedBase;
            g_heap_hi = g_heap_lo + (uint32_t)info.mappedSize;
        }
        free(probe);
        if (!g_heap_hi) { g_team_probe = 0; bv_log("team probe: heap block unknown, probe off"); return; }
    }
    static char last[200];
    char line[200];
    uint32_t s = *(volatile uint32_t *)(g_libg.host + 0xc86bf8);
    if (!s) snprintf(line, sizeof line, "singleton not created yet");
    else if (!heap_ok(s, 0x7c)) snprintf(line, sizeof line, "singleton 0x%08x outside the heap", (unsigned)s);
    else {
        uint32_t t = *(volatile uint32_t *)s;
        if (!t) snprintf(line, sizeof line, "singleton 0x%08x, team NULL (events window variant A: taps send nothing)", (unsigned)s);
        else if (!heap_ok(t, 0x34)) snprintf(line, sizeof line, "team 0x%08x outside the heap", (unsigned)t);
        else {
            volatile uint32_t *tw = (volatile uint32_t *)t;
            uint32_t list = tw[0x20 / 4], n = 0, m = 0;
            if (heap_ok(list, 12)) { n = ((volatile uint32_t *)list)[2]; uint32_t items = ((volatile uint32_t *)list)[0]; if (n && heap_ok(items, 4)) m = ((volatile uint32_t *)items)[0]; }
            int len = snprintf(line, sizeof line, "team 0x%08x: +0 %u, +8 %u, +0x1c map ref 0x%08x, %u member(s)", (unsigned)t, (unsigned)tw[0], (unsigned)tw[2],
                               (unsigned)tw[0x1c / 4], (unsigned)n);
            if (heap_ok(m, 0x3c)) {
                volatile uint8_t *mb = (volatile uint8_t *)m;
                uint32_t idp = ((volatile uint32_t *)m)[1];
                if (heap_ok(idp, 8))
                    snprintf(line + len, sizeof line - len, "; member 0: byte0 %u byte1 %u (needs 1 / 0), id %u-%u", mb[0], mb[1],
                             (unsigned)((volatile uint32_t *)idp)[0], (unsigned)((volatile uint32_t *)idp)[1]);
                else
                    snprintf(line + len, sizeof line - len, "; member 0: byte0 %u byte1 %u (needs 1 / 0), id word 0x%08x", mb[0], mb[1], (unsigned)idp);
            }
        }
    }
    if (strcmp(line, last)) { strcpy(last, line); bv_log("team probe: %s", line); }
}
static int g_max_ctor = -1;  /* config: stop after this constructor index (bisecting a crash) */
static int g_skip_ctors;     /* config: do not run constructors at all */
static int g_rawsys = 1;     /* config: 0 = leave the raw system call sites of libg alone */
static int g_antitamper = 2; /* config: 0 = leave the game's self-check alone (it ends in a deliberate crash on the console, see loader/antitamper.h), 1 = its two flag setters, 2 = + its random calls */
static int g_snapshot = 1;   /* config: write libg's data segment to a file right before GameApp.nOnCreate (for offline analysis on the PC) */
static int g_bringup = 1;    /* config: 0 = stop after JNI_OnLoad, 1 = + createGameMain, 2 = + start/resume (no surface), 3 = + surface */
static int g_run_seconds = 30; /* config: how long the game runs after level 2 / 3 */
static int g_play_event_map = 0; /* config play_event_map (update 97 experiment): Play uses the team's selected map */
static uint32_t g_start_game_orig, g_team_getter, g_home_getter;
/* update 118 (K3): settings_clean=1 -> the settings popup without Supercell ID / social logins / privacy / parents guide / help / friend requests */
static int g_settings_clean = 1;   /* update 144: default (VERIFIED run 139) */
static int g_scid_off = 1;   /* update 118 (K1); update 144: default (VERIFIED run 139) */
/* update 133 (H5) trophy_rows=N: trophy road rows only within N of the last reached index */
static int g_trophy_rows;
static int g_trophy_cull = 1;   /* update 162 (decode_trophyroad_rank.md A1): full track, rows outside the visible window not drawn */
static uint8_t *volatile g_tr_popup, *volatile g_tr_scroll;
static uint32_t g_scroll_draw_orig;
static int g_news_off = 1;   /* update 139 (K2, user: 'delete the news button'): the home NEWS button hidden; update 144: default (req. 15) */
extern int bv_trophy_last_pub;
static uint32_t g_row_index_fn;
static volatile uint32_t g_tr_built, g_tr_skipped;
int bv_trophy_row_filter(int season, uint8_t *popup, void *row) {
    if (!popup || season != *(int *)(popup + 0x120)) return 1;            /* the original test: another season */
    if (g_trophy_cull && popup != g_tr_popup) { g_tr_popup = popup; g_tr_scroll = *(uint8_t **)(popup + 0xfc); }   /* update 162: popup+0xfc set before the builder */
    int idx = g_row_index_fn ? ((int (*)(void *))(uintptr_t)g_row_index_fn)(row) : -1;
    int c = bv_trophy_last_pub >= 0 ? bv_trophy_last_pub : 0;
    if (g_trophy_rows > 0 && idx >= 0 && (idx < c - g_trophy_rows || idx > c + g_trophy_rows)) { g_tr_skipped++; return 1; }
    if (!(g_tr_built++ & 15)) bv_log("trophy_rows: building row %d (reached %d, window %d, %u skipped so far)", idx, c, g_trophy_rows, (unsigned)g_tr_skipped);
    return 0;
}
static int g_new_event_label = 1; /* update 124 (E5): 0 = no new-event label without an upcoming event */
static int g_ready_unblock;       /* update 187 (user: profile button does nothing): 1 = the 'blocked while ready' guard 0x2e5674 returns 0 */
static uint32_t g_settings_ctor, g_getclip;
static const char *const k_settings_hide[] = {"button_allow_friend_requests", "button_sc_id", "button_fb_connect", "button_line_connect", "button_google_connect",
                                              "button_kakao_connect", "button_privacy", "button_parentsguide", "button_faq", "title_friends_txt", "friends_bg", "friendrequest_info",
                                              "button_tos", "button_tos_alt", "button_terms", "button_privacy_alt", "button_privacy_children"};   /* update 146 (T17): terms / privacy buttons */
void *bv_settings_wrap(void *self, void *a1, void *a2, void *a3) {
    void *r = ((void *(*)(void *, void *, void *, void *))(uintptr_t)g_settings_ctor)(self, a1, a2, a3);
    void *rootc = self ? *(void **)((uint8_t *)self + 0x50) : NULL;
    int n = 0, nf = 0;
    /* update 119: the button objects the constructor keeps (+0xa4 faq, +0x98 facebook, +0xe4 privacy, +0xc0 parents guide, +0xbc google, +0xa0 line,
     * +0x9c kakao) are hidden like the constructor hides Kakao: visible +4 = 0, +0x34 = 0 */
    static const int fields[] = {0xa4, 0x98, 0xe4, 0xc0, 0xbc, 0xa0, 0x9c};
    for (unsigned i = 0; self && i < sizeof fields / sizeof fields[0]; i++) {
        uint8_t *o = *(uint8_t **)((uint8_t *)self + fields[i]);
        uintptr_t a = (uintptr_t)o;
        if (o && !(a & 3) && a >= 0x80000000u && a < 0xc0000000u) { o[4] = 0; o[0x34] = 0; nf++; }
    }
    {   /* update 122: the friend-request toggle object (+0x94, ctor libg+0x1056e0, added to the popup by libg+0x4bd724; its clip is no longer a named
         * child, run 138) -> visible +4 = 0 */
        uint8_t *o = self ? *(uint8_t **)((uint8_t *)self + 0x94) : NULL;
        uintptr_t a = (uintptr_t)o;
        if (o && !(a & 3) && a >= 0x80000000u && a < 0xc0000000u) { o[4] = 0; nf++; }
    }
    for (unsigned i = 0; rootc && g_getclip && i < sizeof k_settings_hide / sizeof k_settings_hide[0]; i++) {
        uint8_t *c = ((uint8_t *(*)(void *, const char *))(uintptr_t)g_getclip)(rootc, k_settings_hide[i]);
        if (!c) c = ((uint8_t *(*)(void *, const char *))(uintptr_t)(g_libg.load + 0x23f444u))(rootc, k_settings_hide[i]);   /* update 121: named-child lookup (the toggle, run 137) */
        uintptr_t a = (uintptr_t)c;
        if (c && !(a & 3) && a >= 0x80000000u && a < 0xc0000000u) { c[4] = 0; n++; }
        else { static int miss; if (miss++ < 16) bv_log("settings_clean: %s not found", k_settings_hide[i]); }
    }
    static int said;
    if (said++ < 3) bv_log("settings_clean: %d of %u settings items hidden by name, %d button objects", n, (unsigned)(sizeof k_settings_hide / sizeof k_settings_hide[0]), nf);
    {   /* update 160 (user T17: no empty vertical space): the popup root's children (clip +0x54 children, +0x58 names, int16 count +0x6a; layout of
         * the child finder libg+0x38ea94) keep x at +0x1c and y at +0x20 (floats, run 172). Everything below the hidden middle (bottom-row buttons AND
         * their unnamed caption texts) moves up so that it sits 60 units under the lowest visible element of the top part. */
        uint8_t *rc = (uint8_t *)rootc;
        uintptr_t ra = (uintptr_t)rc;
        if (rc && !(ra & 3) && ra >= 0x80000000u && ra < 0xc0000000u) {
            uint8_t **kids = *(uint8_t ***)(rc + 0x54);
            int nk = *(int16_t *)(rc + 0x6a);
            float top_max = -1e9f, bot_min = 1e9f;
            int moved = 0;
            #define KID_OK(k) ((k) && !((uintptr_t)(k) & 3) && (uintptr_t)(k) >= 0x80000000u && (uintptr_t)(k) < 0xc0000000u)
            #define KID_Y(k) (*(float *)((k) + 0x20))
            #define KID_SANE(k) (KID_Y(k) > -1000.0f && KID_Y(k) < 1000.0f && *(float *)((k) + 0x18) > 0.1f && *(float *)((k) + 0x18) < 10.0f)
            if (kids && !((uintptr_t)kids & 3) && nk > 0 && nk < 256) {
                for (int i = 0; i < nk; i++) {
                    uint8_t *k = kids[i];
                    if (!KID_OK(k) || !KID_SANE(k) || !k[4]) continue;   /* visible ones only */
                    float y = KID_Y(k);
                    if (y <= 20.0f) { if (y > top_max) top_max = y; } else if (y < bot_min) bot_min = y;
                }
                float delta = (bot_min < 1e8f && top_max > -1e8f) ? bot_min - top_max - 60.0f : 0.0f;
                if (delta > 1.0f) {
                    for (int i = 0; i < nk; i++) {
                        uint8_t *k = kids[i];
                        if (!KID_OK(k) || !KID_SANE(k) || KID_Y(k) <= 20.0f) continue;
                        KID_Y(k) -= delta;
                        moved++;
                    }
                }
                static int said_gap;
                if (said_gap++ < 3) bv_log("settings_clean: %d children, top part ends at y %g, bottom part starts at y %g -> %d moved up by %g", nk, top_max, bot_min, moved, delta);
            }
            #undef KID_OK
            #undef KID_Y
            #undef KID_SANE
        }
    }
    return r;
}
typedef int (*bv_start_fn)(void *, void *, int, int, int, int, int, int, int);
/* called instead of libg's start-game 0x67febc from the solo Play path; same 9 arguments (4 in registers, 5 on the stack) */
/* update 162 (decode_bot_power.md, CONFIRMED): every local bot's loadout comes from libg+0x51f9a4 with the literal power level `mov r1,#9` at
 * libg+0x51f9e8 (e3a01009). config bot_power=1 (default): at each Play the literal becomes the played brawler's level (1..9). */
static int g_bot_power = 1;
static void bv_set_bot_level(int lvl);
int bvb_bot_level(void);
int bv_start_game_wrap(void *a0, void *loc, int a2, int a3, int s0, int s1, int s2, int s3, int s4) {
    { extern volatile int bv_tv_watching; if (bv_tv_watching) { bv_tv_watching = 0; *(volatile uint8_t *)(uintptr_t)(g_libg.load + 0xc8674cu) = 0; *(volatile uint8_t *)(uintptr_t)(g_libg.load + 0xc86bfcu) = 0; } }   /* update 171 */
    { void bv_tv_on_play(void); bv_tv_on_play(); }   /* update 184 */
    { void world_battle_start_pub(void); world_battle_start_pub(); }   /* Phase 3: opponents from the bot world */
    if (g_bot_power) bv_set_bot_level(bvb_bot_level());
    /* update 98: the argument is an EVENT ENTRY (+4 slot, +0x14 map; run 114 crashed when update 97 passed the map object itself). Pick the entry of
     * the home's event list (libg+0x6641cc() -> +0x48 entries, +0x50 count; the lookup 0x5ced04 walks the same list) whose map is the team's selected map. */
    void *orig = loc;
    void *map = NULL;
    int n = -1, found = -1;
    if (g_team_getter) {
        void **single = ((void **(*)(void))(uintptr_t)g_team_getter)();
        uint8_t *team = single ? (uint8_t *)single[0] : NULL;
        map = team ? *(void **)(team + 0x1c) : NULL;
        uint8_t *home = map && g_home_getter ? (uint8_t *)((void *(*)(void))(uintptr_t)g_home_getter)() : NULL;
        if (home) {
            void **ents = *(void ***)(home + 0x48);
            n = *(int *)(home + 0x50);
            for (int i = 0; ents && i < n && i < 64; i++) {
                uint8_t *e = (uint8_t *)ents[i];
                if (e && *(void **)(e + 0x14) == map) {
                    found = i;
                    loc = e;
                    break;
                }
            }
        }
    }
    bv_log("play_event_map: team map %p, %d event entries, match %d: start-game event entry %p (slot %d) -> %p (slot %d)", map, n, found, orig,
           orig ? *(int *)((uint8_t *)orig + 4) : -1, loc, loc ? *(int *)((uint8_t *)loc + 4) : -1);
    return ((bv_start_fn)(uintptr_t)g_start_game_orig)(a0, loc, a2, a3, s0, s1, s2, s3, s4);
}
static void bv_set_bot_level(int lvl) {
    if (lvl < 1) lvl = 1;
    if (lvl > 9) lvl = 9;
    uint32_t cur = 0;
    memcpy(&cur, g_libg.host + 0x51f9e8u, 4);
    if ((cur & 0xfffffff0u) != 0xe3a01000u) { bv_log("bot_power: libg+0x51f9e8 holds %08x, not mov r1,#n: not patched", (unsigned)cur); return; }
    if (cur == (0xe3a01000u | (uint32_t)lvl)) return;
    at_patch bp = {0x51f9e8u, cur, 0xe3a01000u | (uint32_t)lvl, "bots: power level"};
    int d = bv_antitamper_apply(&g_libg, &bp, 1);
    bv_log("bot_power: bots get power level %d (libg+0x51f9e8 %08x -> %08x) %s", lvl, (unsigned)cur, (unsigned)(0xe3a01000u | (uint32_t)lvl), d == 1 ? "" : "FAILED");
}
/* update 162 (decode_trophyroad_rank.md A1): the scroll class draw slot (vtable cell libg+0x8e96e8, original 0x2fcb08) is wrapped; for the trophy
 * road's scroll only, rows (popup+0x100) and dividers (popup+0x104) outside the visible window get visible byte 0, inside 1. The 5th argument is a
 * float passed on the stack in the original: declared uint32_t so it stays on the stack. */
static void bv_trophy_cull(uint8_t *popup, uint8_t *scroll) {
    if (*(uint8_t **)(popup + 0xfc) != scroll) return;   /* stale popup */
    uint8_t *content = *(uint8_t **)(scroll + 0x54);
    if (!content) return;
    float xoff = *(float *)(content + 0x1c), vw = *(float *)(scroll + 0x48), zoom = *(float *)(scroll + 0x198);
    if (!(zoom > 0.1f && zoom < 10.0f)) zoom = 1.0f;
    float lo = -xoff / zoom - 400.0f, hi = (-xoff + vw) / zoom + 400.0f;
    for (int v = 0; v < 2; v++) {
        uint8_t *vec = *(uint8_t **)(popup + 0x100 + 4 * v);
        if (!vec) continue;
        uint8_t **items = *(uint8_t ***)vec;
        int n = *(int *)(vec + 8);
        for (int i = 0; items && i < n && i < 512; i++) {
            uint8_t *r = items[i];
            if (!r) continue;
            float x = *(float *)(r + 0x1c), w = v == 0 ? *(float *)(r + 0x128) : 0.0f;
            if (!(w > 0.0f && w < 2000.0f)) w = 300.0f;
            r[4] = (uint8_t)(x + w * 0.5f >= lo && x - w * 0.5f <= hi);
        }
    }
}
uint32_t bv_scroll_draw(void *self, void *m, void *c, uint32_t flags, uint32_t f5) {
    if (self && (uint8_t *)self == g_tr_scroll && g_tr_popup) bv_trophy_cull(g_tr_popup, (uint8_t *)self);
    return ((uint32_t (*)(void *, void *, void *, uint32_t, uint32_t))(uintptr_t)g_scroll_draw_orig)(self, m, c, flags, f5);
}
/* update 162 (decode_native_input.md): config input_probe=1 logs the touch manager's listener list (app = *(libg+0xc86770), mgr = app+0x5c,
 * listeners mgr+0x28 x count mgr+0x2c) once at home and once in a battle: the battle-only listener is the joystick logic to decode next. */
int bv_input_probe;
void bv_input_probe_tick(int in_battle) {
    static int done_home, done_battle;
    if (!bv_input_probe || (in_battle ? done_battle : done_home)) return;
    uint32_t app = *(volatile uint32_t *)(uintptr_t)(g_libg.load + 0xc86770u);
    if (!app || (app & 3) || app < 0x80000000u || app >= 0xc0000000u) return;
    uint32_t mgr = *(volatile uint32_t *)(uintptr_t)(app + 0x5c);
    if (!mgr || (mgr & 3) || mgr < 0x80000000u || mgr >= 0xc0000000u) return;
    uint32_t arr = *(volatile uint32_t *)(uintptr_t)(mgr + 0x28);
    int n = *(volatile int *)(uintptr_t)(mgr + 0x2c);
    if (!arr || n <= 0 || n > 64) return;
    if (in_battle) done_battle = 1; else done_home = 1;
    for (int i = 0; i < n; i++) {
        uint32_t l = *(volatile uint32_t *)(uintptr_t)(arr + 4 * i);
        uint32_t vt = (l && !(l & 3) && l >= 0x80000000u && l < 0xc0000000u) ? *(volatile uint32_t *)(uintptr_t)l : 0;
        uint32_t s0 = vt ? *(volatile uint32_t *)(uintptr_t)vt : 0, s1 = vt ? *(volatile uint32_t *)(uintptr_t)(vt + 4) : 0;
        bv_log("input_probe (%s): listener %d/%d %p vtable libg+0x%x began libg+0x%x moved libg+0x%x", in_battle ? "battle" : "home", i, n, (void *)(uintptr_t)l,
               vt ? (unsigned)(vt - g_libg.load) : 0u, s0 ? (unsigned)(s0 - g_libg.load) : 0u, s1 ? (unsigned)(s1 - g_libg.load) : 0u);
    }
}
/* update 163 (decode_brawl_tv.md): Brawl TV = a local bot match started from the next frame after 14104, with StartLoading +0x70 = 1 (spectator)
 * written by a cave at libg+0x680370 (`str r7,[r5,#0x54]` in the local builder) that copies bv_tv_spectator into +0x70 (0 for every normal Play). */
volatile int bv_tv_pending;
volatile int bv_tv_spectator;
volatile int bv_tv_watching;   /* update 171: an all-bot Brawl TV match is running (debug byte set) */
static int g_brawl_tv = 3;   /* update 175: all-bot watch match by default (user pass, run 208); 0 = off (24105), 1 = old spectator mode (crashed run 199) */
int bv_brawl_tv_on = 3;
static int tv_ok(uint32_t p) { return p >= 0x81000000u && p < 0xc0000000u && !(p & 3); }   /* update 180 */
/* ---- Phase 3 (living world): battles against world bots (world_match=1, world_ai=1) ----
 * Names: the battle name tag is TID_lookup("TID_BOT_" + n) (0x255d60 in 0x254ebc; n = the bot's name slot 1..9, run logs). At each battle start the
 * texts' String objects of TID_BOT_1..9 (returned by the TID lookup 0x590af4) get the planned world bots' names through the game's own String
 * assign 0x3c4960 (frees / allocates with the game's allocator, recounts the characters at +0).
 * Bots: the bot filler 0x58800c creates each bot with addPlayer 0x2c49d4(logic, id, hero row, loadout, ..., display data (name "n"), isBot, skin) at
 * 5 call sites; the wrapper swaps the hero for the world bot's brawler (row pointer from the characters table, vtable slots 0x10 count / 0x14 item),
 * rebuilds the loadout (0x51f9a4) with the bot's level (no gadget below 7, no star power below 10) and, with world_ai=1, sets the returned player's
 * tier override +0xd4 (init -1 at 0x7b61c8, read by 0x3b2244 for the bot brain / attack) from the bot's trophies and hidden skill. */
int bv_world_ai;
static uint32_t g_add_player_orig;
static int g_world_logs;
const bw_world *bvb_world(void);
const bw_pick *bvb_world_pick(int slot);
int bvb_world_plan(const char *names[BW_SLOTS]);
static void world_battle_start(void) {
    const char *names[BW_SLOTS];
    int got = bvb_world_plan(names);
    g_world_logs = 0;
    if (!got) return;
    uint32_t base = g_libg.load;
    uint32_t (*lookup)(const char *) = (uint32_t (*)(const char *))(uintptr_t)(base + 0x590af4u);
    void (*assign)(uint32_t, const char *) = (void (*)(uint32_t, const char *))(uintptr_t)(base + 0x3c4960u);
    int done = 0;
    for (int n = 1; n < BW_SLOTS; n++) {
        if (!names[n]) continue;
        char key[16];
        snprintf(key, sizeof key, "TID_BOT_%d", n);
        uint32_t s = lookup(key);
        if (!tv_ok(s)) continue;
        int len = *(volatile int *)(uintptr_t)(s + 4);
        const char *txt = len < 8 ? (const char *)(uintptr_t)(s + 8) : *(const char *const *)(uintptr_t)(s + 8);
        if (len <= 0 || len > 64 || !tv_ok((uint32_t)(uintptr_t)txt) || !strncmp(txt, "TID_BOT_", 8)) {
            if (n == 1) bv_log("world: %s not found in the texts (len %d): names not changed", key, len);
            break;
        }
        assign(s, names[n]);
        done++;
    }
    bv_log("world: %d battle names set (TID_BOT_1 = '%s')", done, names[1] ? names[1] : "-");
    /* L4 step 1 (battle.md 3.4, CONFIRMED words): the bots' aim error +-150 (0xf49f8/0xf49fc, 0xf4a14/0xf4a18 `mvn r1,#149; mov r2,#150`) is the
     * same for every tier; with world_ai=1 it follows the planned bots' strength: 150 - 25 * average tier (tier 0 = the game's 150, tier 4 = 50). */
    if (bv_world_ai) {
        int sum = 0, nt = 0;
        for (int n = 1; n < BW_SLOTS; n++) { const bw_pick *pk = bvb_world_pick(n); if (pk) { sum += pk->tier; nt++; } }
        int e = nt ? 150 - 25 * sum / nt : 150;
        if (e < 40) e = 40;
        if (e > 150) e = 150;
        static const uint32_t offs[4] = {0xf49f8u, 0xf49fcu, 0xf4a14u, 0xf4a18u};
        int ok = 0;
        for (int i = 0; i < 4; i++) {
            uint32_t cur = 0;
            memcpy(&cur, g_libg.host + offs[i], 4);
            uint32_t base_w = (i & 1) ? 0xe3a02000u : 0xe3e01000u, want = base_w | (uint32_t)((i & 1) ? e : e - 1);
            if ((cur & 0xffffff00u) != base_w) continue;
            if (cur == want) { ok++; continue; }
            at_patch ap = {offs[i], cur, want, "bots: aim error"};
            ok += bv_antitamper_apply(&g_libg, &ap, 1);
        }
        bv_log("world: bot aim error +-%d (average tier %d.%d of %d bots), %d of 4 words", e, nt ? sum / nt : 0, nt ? (sum * 10 / nt) % 10 : 0, nt, ok);
    }
}
/* the name slot n of a bot: its display data holds the String "n" (one or two digits) */
static int world_name_slot(uint32_t dd) {
    if (!tv_ok(dd)) return -1;
    for (uint32_t o = 0; o <= 0x14; o += 4) {
        int len = *(volatile int *)(uintptr_t)(dd + o + 4);
        if (len < 1 || len > 2) continue;
        const char *c = (const char *)(uintptr_t)(dd + o + 8);
        if (c[0] < '1' || c[0] > '9' || (len == 2 && (c[1] < '0' || c[1] > '9')) || c[len]) continue;
        return len == 1 ? c[0] - '0' : (c[0] - '0') * 10 + c[1] - '0';
    }
    return -1;
}
typedef uint32_t (*bv_add_fn)(uint32_t, uint32_t, uint32_t, uint32_t, int, int, uint32_t, int, int);
uint32_t bv_add_player_wrap(uint32_t logic, uint32_t id, uint32_t hero, uint32_t loadout, int s0, int s1, uint32_t dd, int isbot, int skin) {
    int slot = isbot ? world_name_slot(dd) : -1;
    const bw_pick *pk = slot > 0 ? bvb_world_pick(slot) : NULL;
    const bw_world *w = pk ? bvb_world() : NULL;
    uint32_t base = g_libg.load;
    const char *what = "kept";
    if (pk && w && tv_ok(hero)) {
        int want = w->br_row[pk->slot], cur = *(volatile int *)(uintptr_t)(hero + 0x10) - 16000000;
        uint32_t tbl = *(volatile uint32_t *)(uintptr_t)(hero + 8);
        uint32_t vt = tv_ok(tbl) ? *(volatile uint32_t *)(uintptr_t)tbl : 0;
        if (cur >= 0 && cur < 1000 && want != cur && tv_ok(vt)) {
            uint32_t fc = *(volatile uint32_t *)(uintptr_t)(vt + 0x10), fi = *(volatile uint32_t *)(uintptr_t)(vt + 0x14);
            uint32_t it = 0;
            if (fc - base < 0x900000u && fi - base < 0x900000u && want < ((int (*)(uint32_t))(uintptr_t)fc)(tbl))
                it = ((uint32_t (*)(uint32_t, int))(uintptr_t)fi)(tbl, want);
            int dup = 0;
            uint32_t arr = *(volatile uint32_t *)(uintptr_t)(logic + 0x48);
            int cnt = *(volatile int *)(uintptr_t)(logic + 0x3c);
            for (int i = 0; tv_ok(arr) && i < cnt && i < 12; i++) {
                uint32_t p = *(volatile uint32_t *)(uintptr_t)(arr + 4u * (uint32_t)i);
                if (tv_ok(p) && *(volatile uint32_t *)(uintptr_t)(p + 0x2c) == it) dup = 1;
            }
            if (tv_ok(it) && *(volatile int *)(uintptr_t)(it + 0x10) == 16000000 + want && !dup) {
                hero = it;
                if (loadout) loadout = ((uint32_t (*)(uint32_t, uint32_t))(uintptr_t)(base + 0x51f9a4u))(logic, hero);
                what = "swapped";
            } else what = dup ? "kept (already in the match)" : "kept (row not found)";
        }
        if (tv_ok(loadout)) {   /* loadout {level, star power card, gadget card} (classes.md 5.6) */
            int lv = pk->level < 1 ? 1 : pk->level > 9 ? 9 : pk->level;
            *(volatile int *)(uintptr_t)loadout = lv;
            if (pk->level < 10) *(volatile uint32_t *)(uintptr_t)(loadout + 4) = 0;
            if (pk->level < 7) *(volatile uint32_t *)(uintptr_t)(loadout + 8) = 0;
        }
    }
    uint32_t p = ((bv_add_fn)(uintptr_t)g_add_player_orig)(logic, id, hero, loadout, s0, s1, dd, isbot, skin);
    if (pk && w) {
        if (bv_world_ai && tv_ok(p) && *(volatile uint8_t *)(uintptr_t)(p + 0x31)) *(volatile int *)(uintptr_t)(p + 0xd4) = pk->tier;
        if (g_world_logs++ < 10)
            bv_log("world: bot slot %d = '%s': brawler row %d %s, level %d, tier %d%s", slot, w->bots[pk->bot].name, w->br_row[pk->slot], what, pk->level,
                   pk->tier, bv_world_ai ? "" : " (world_ai=0: game tier)");
    } else if (isbot && g_world_logs++ < 10 && bvb_world_pick(1)) bv_log("world: a bot without a plan (name slot %d)", slot);
    return p;
}
void world_battle_start_pub(void) { world_battle_start(); }

/* update 184 (user run 222):
 * - mode: a random event whose MODE (0x475288(entry+0x14), battle.md 1.1) is not Showdown (6 / 9), Tutorial (0xc) or Training (0xd), and not the event
 *   selected on the home screen when another one exists (run 222 picked slot 8 = a Showdown event -> 14110 + result screen, no exit)
 * - hero: HomeData+0xc0 (selected brawler, classes.md 2.2) is swapped for a random owned brawler for the start and put back once the match has its
 *   players (tv_random_hero=1)
 * - leave: Circle or a tap in the top-left corner (touch thread -> bv_tv_quit_req) calls the game's leave helper 0x2103f8(0x41330c(), 1, 0) = 14109,
 *   the in-battle quit path (battle_end_agent.md); the server answers with the home
 * - next match: a watched match that ended by itself starts the next one 3 s after the home is back (tv_next=1) until the player leaves
 * - home object: bv_home_obj or the popup manager's current screen with the home's vtable (shims/shim_egl.c bv_home_find) */
static int g_tv_random_hero = 1, g_tv_next = 1;
volatile int bv_tv_quit_req, bv_tv_next_pending;
volatile int bv_tv_match;          /* 1 from a Brawl TV start until the next normal Play (offline_b.c: no trophies for it) */
static uint32_t g_tv_hd, g_tv_hd_saved;
static uint64_t g_tv_next_at;
static int g_tv_left, g_tv_wait;
int bvb_random_owned_brawler(unsigned seed, int not_row);
uint32_t bv_home_find(void) __attribute__((weak));
static void tv_hero_restore(void) {
    if (!g_tv_hd) return;
    *(volatile uint32_t *)(uintptr_t)(g_tv_hd + 0xc0) = g_tv_hd_saved;
    bv_log("brawl_tv: selected brawler put back (HomeData %p)", (void *)(uintptr_t)g_tv_hd);
    g_tv_hd = 0;
}
static void tv_hero_swap(void) {
    if (!g_tv_random_hero) return;
    uint32_t base = g_libg.load;
    uint32_t m4 = ((uint32_t (*)(void))(uintptr_t)(base + 0x2b4aecu))();
    uint32_t a = tv_ok(m4) ? *(volatile uint32_t *)(uintptr_t)(m4 + 0x1c) : 0;
    uint32_t lch = tv_ok(a) ? ((uint32_t (*)(uint32_t))(uintptr_t)(base + 0x419904u))(a) : 0;
    uint32_t hd = tv_ok(lch) ? ((uint32_t (*)(uint32_t))(uintptr_t)(base + 0x69353cu))(lch) : 0;
    uint32_t sel = tv_ok(hd) ? *(volatile uint32_t *)(uintptr_t)(hd + 0xc0) : 0;
    if (!tv_ok(sel)) { bv_log("brawl_tv: no selected brawler in HomeData: hero not changed"); return; }
    int cur = *(volatile int *)(uintptr_t)(sel + 0x10) - 16000000;
    uint32_t tbl = *(volatile uint32_t *)(uintptr_t)(sel + 8);
    uint32_t vt = tv_ok(tbl) ? *(volatile uint32_t *)(uintptr_t)tbl : 0;
    if (cur < 0 || cur > 255 || !tv_ok(vt)) { bv_log("brawl_tv: selected brawler row %d / table not usable: hero not changed", cur); return; }
    int row = bvb_random_owned_brawler((unsigned)sceKernelGetProcessTimeWide(), cur);
    if (row < 0) return;
    uint32_t fc = *(volatile uint32_t *)(uintptr_t)(vt + 0x10), fi = *(volatile uint32_t *)(uintptr_t)(vt + 0x14);
    if (fc - base >= 0x900000u || fi - base >= 0x900000u) { bv_log("brawl_tv: table vtable slots outside libg: hero not changed"); return; }
    int cnt = ((int (*)(uint32_t))(uintptr_t)fc)(tbl);
    uint32_t it = row < cnt ? ((uint32_t (*)(uint32_t, int))(uintptr_t)fi)(tbl, row) : 0;
    if (!tv_ok(it) || *(volatile int *)(uintptr_t)(it + 0x10) != 16000000 + row) { bv_log("brawl_tv: brawler row %d not found (%d rows): hero not changed", row, cnt); return; }
    g_tv_hd = hd;
    g_tv_hd_saved = sel;
    *(volatile uint32_t *)(uintptr_t)(hd + 0xc0) = it;
    bv_log("brawl_tv: watched hero = brawler row %d (the selected row %d comes back when the match runs)", row, cur);
}
void bv_tv_on_play(void) {   /* a normal Play: nothing of Brawl TV stays */
    tv_hero_restore();
    bv_tv_match = 0;
    g_tv_next_at = 0;
    bv_tv_next_pending = 0;
    bv_tv_quit_req = 0;
}
/* game thread, every frame while watching (bv_native_tick) */
static void tv_watch_frame(uint32_t gs) {
    if (g_tv_hd) {
        uint32_t host = *(volatile uint32_t *)(uintptr_t)(gs + 0x1c);
        if (tv_ok(host) && *(volatile int *)(uintptr_t)(host + 0x3c) > 0) tv_hero_restore();   /* the players exist: the hero was read */
    }
    if (bv_tv_quit_req) {
        bv_tv_quit_req = 0;
        uint32_t g = ((uint32_t (*)(void))(uintptr_t)(g_libg.load + 0x41330cu))();
        if (tv_ok(g)) {
            g_tv_left = 1;
            bv_log("brawl_tv: left the watched match (leave helper -> 14109)");
            ((void (*)(uint32_t, int, int))(uintptr_t)(g_libg.load + 0x2103f8u))(g, 1, 0);
        } else bv_log("brawl_tv: leave asked but no game screen yet");
    }
}
static void tv_match_ended(void) {
    tv_hero_restore();
    if (!g_tv_left && g_tv_next && g_brawl_tv >= 2) {
        g_tv_next_at = sceKernelGetProcessTimeWide() + 3000000u;
        bv_tv_next_pending = 1;
        bv_log("brawl_tv: match over - the next one starts in 3 s (Circle: stop)");
    }
    g_tv_left = 0;
}
void bv_tv_tick(void) {
    if (g_tv_next_at) {
        if (bv_tv_quit_req) { bv_tv_quit_req = 0; g_tv_next_at = 0; bv_tv_next_pending = 0; bv_log("brawl_tv: next match cancelled"); }
        else if (sceKernelGetProcessTimeWide() >= g_tv_next_at) { g_tv_next_at = 0; bv_tv_next_pending = 0; bv_tv_pending = 1; bv_log("brawl_tv: next match"); }
    }
    if (!bv_tv_pending) return;
    bv_tv_pending = 0;
    extern volatile uint32_t bv_home_obj;
    uint32_t h = bv_home_obj;
    if (!tv_ok(h) && bv_home_find) h = bv_home_find();
    if (g_brawl_tv && g_start_game_orig && !tv_ok(h) && ++g_tv_wait < 240) { bv_tv_pending = 1; return; }   /* wait up to ~10 s for the home */
    g_tv_wait = 0;
    if (!g_brawl_tv || !g_start_game_orig || !tv_ok(h)) { bv_log("brawl_tv: no home object / start function: not started"); return; }
    uint8_t *home = (uint8_t *)(uintptr_t)h;
    void *entry = *(void **)(home + 0x140), *selected = entry;
    {   /* random event by MODE: not showdown / tutorial / training, not the selected one if another exists (EventsData 0x114814(): +0x48 / +0x50) */
        uint32_t ev = ((uint32_t (*)(void))(uintptr_t)(g_libg.load + 0x114814u))();
        if (tv_ok(ev)) {
            uint32_t arr = *(volatile uint32_t *)(uintptr_t)(ev + 0x48);
            int n = *(volatile int *)(uintptr_t)(ev + 0x50), pick[16], np = 0, alt = -1;
            for (int i = 0; tv_ok(arr) && i < n && i < 16; i++) {
                uint32_t e = *(volatile uint32_t *)(uintptr_t)(arr + 4u * (uint32_t)i);
                uint32_t map = tv_ok(e) ? *(volatile uint32_t *)(uintptr_t)(e + 0x14) : 0;
                if (!tv_ok(map)) continue;
                int mode = ((int (*)(uint32_t))(uintptr_t)(g_libg.load + 0x475288u))(map);
                if (mode == 6 || mode == 9 || mode == 0xc || mode == 0xd || mode < 0) continue;
                if ((void *)(uintptr_t)e == selected) { alt = i; continue; }
                pick[np++] = i;
            }
            if (!np && alt >= 0) pick[np++] = alt;
            if (np) {
                int i = pick[(unsigned)sceKernelGetProcessTimeWide() % (unsigned)np];
                entry = (void *)(uintptr_t)*(volatile uint32_t *)(uintptr_t)(arr + 4u * (uint32_t)i);
                bv_log("brawl_tv: random event: entry %p (slot %d, mode %d) of %d candidates", entry, *(int *)((uint8_t *)entry + 4),
                       ((int (*)(uint32_t))(uintptr_t)(g_libg.load + 0x475288u))(*(uint32_t *)((uint8_t *)entry + 0x14)), np);
            } else bv_log("brawl_tv: no non-showdown event: the selected one is used");
        }
    }
    bv_tv_quit_req = 0;
    g_tv_left = 0;
    bv_tv_match = 1;
    if (g_brawl_tv >= 2) {   /* update 171 (notes/libg_map/brawltv_fix.md): a NORMAL local match (no spectator flag) with the debug byte libg 0xc8674c = 1:
                              * the local player's hero is a bot too (player+0x31), so all six brawlers are AI; the verified non-spectator path stays in use */
        tv_hero_swap();
        *(volatile uint8_t *)(uintptr_t)(g_libg.load + 0xc8674cu) = 1;
        if (g_brawl_tv == 3) *(volatile uint8_t *)(uintptr_t)(g_libg.load + 0xc86bfcu) = 1;   /* update 174: bots drive the human hero too */
        bv_tv_watching = 1;
        bv_log("brawl_tv: starting an all-bot local match to watch (home %p, event entry %p, debug byte 0xc8674c = 1)", (void *)home, entry);
        ((bv_start_fn)(uintptr_t)g_start_game_orig)(home, entry, 0, 3, *(int *)(home + 0x14c), *(int *)(home + 0x148), 0, 0, 1);
        return;
    }
    bv_log("brawl_tv: starting a watch-only local bot match (home %p, event entry %p, spectator flag 1)", (void *)home, entry);
    bv_tv_spectator = 1;
    ((bv_start_fn)(uintptr_t)g_start_game_orig)(home, entry, 0, 3, *(int *)(home + 0x14c), *(int *)(home + 0x148), 0, 0, 1);
    bv_tv_spectator = 0;
}
/* update 165: zone profiler. push(label) / pop() are called in matched pairs (nesting <= 4) by the game thread; times are inclusive per label. */
static int g_prof_zones;
static SceUID g_zone_thread = -1;
static struct { const char *label; uint64_t t0; } g_zstack[16];
static int g_zdepth;
#define ZONE_MAX 48
static struct { const char *label; uint32_t us, calls; } g_zone[ZONE_MAX];
static int g_nzone;
void bv_zone_push(const char *label) {
    SceUID me = sceKernelGetThreadId();
    if (g_zone_thread < 0) g_zone_thread = me;
    if (me != g_zone_thread) return;
    if (g_zdepth < 16) { g_zstack[g_zdepth].label = label; g_zstack[g_zdepth].t0 = sceKernelGetProcessTimeWide(); }
    g_zdepth++;
}
void bv_zone_pop(void) {
    if (sceKernelGetThreadId() != g_zone_thread || g_zdepth <= 0) return;
    g_zdepth--;
    if (g_zdepth >= 16) return;
    uint32_t dt = (uint32_t)(sceKernelGetProcessTimeWide() - g_zstack[g_zdepth].t0);
    const char *l = g_zstack[g_zdepth].label;
    int i = 0;
    while (i < g_nzone && g_zone[i].label != l) i++;
    if (i == g_nzone) { if (g_nzone >= ZONE_MAX) return; g_zone[g_nzone].label = l; g_zone[g_nzone].us = 0; g_zone[g_nzone].calls = 0; g_nzone++; }
    g_zone[i].us += dt;
    g_zone[i].calls++;
}
/* called once a second from the status line (loader/gameapp.c): top 8 zones of the last second, then reset */
void bv_zones_report(void) {
    if (!g_prof_zones || !g_nzone) return;
    char line[512];
    int at = snprintf(line, sizeof line, "    zones (ms in the last second, game thread):");
    for (int k = 0; k < 8; k++) {
        int best = -1;
        for (int i = 0; i < g_nzone; i++) if (g_zone[i].us && (best < 0 || g_zone[i].us > g_zone[best].us)) best = i;
        if (best < 0) break;
        const char *l = g_zone[best].label;
        uintptr_t la = (uintptr_t)l;
        at += snprintf(line + at, at < (int)sizeof line ? sizeof line - (size_t)at : 0, " %.40s %u/%u", (la >= 0x80000000u && la < 0xc0000000u) ? l : "?",
                       (unsigned)(g_zone[best].us / 1000), (unsigned)g_zone[best].calls);
        g_zone[best].us = 0;
        if (at >= (int)sizeof line) break;
    }
    for (int i = 0; i < g_nzone; i++) { g_zone[i].us = 0; g_zone[i].calls = 0; }
    bv_log("%s", line);
}
/* update 167 (fix 1, notes/libg_map/battle.md P2.1): native controls. The touch thread publishes the pad (bv_np_*); this runs on the game thread
 * (per swap, shims/shim_egl.c) and feeds the game's own input queue. Every pointer is range-checked; nothing runs outside a live, undecided battle. */
volatile long bv_match_t0;   /* update 180: unix time the current match started running (battle log length) */
int bv_native_input = 3;   /* update 175: mobile-style fingers + native gadget by default (user pass) */
int bv_box_cards = 1;      /* update 175 */
static int g_stick_ui;                  /* update 170 experiment */
static uint32_t g_bscreen_orig;         /* original battle screen update 0x37f2f0 */
static volatile uint32_t g_bscreen;     /* last battle screen object seen */
static volatile int g_np_mx, g_np_my;   /* move stick (-127..127) from bv_native_tick */
void bv_bscreen_update(void *self, uint32_t dt) {
    g_bscreen = (uint32_t)(uintptr_t)self;
    if (g_stick_ui && bv_native_input && self) {
        extern vpad_cfg bv_vpad_cfg;
        uint8_t *sc = (uint8_t *)self;
        static int down;
        int mx = g_np_mx, my = g_np_my;
        if (mx || my) {
            float ox = (float)bv_vpad_cfg.move_x, oy = (float)bv_vpad_cfg.move_y, r = (float)bv_vpad_cfg.radius;
            volatile float *f = (volatile float *)(sc + 0x190);
            f[0] = ox; f[1] = oy; f[2] = ox + r * (float)mx / 127.f; f[3] = oy + r * (float)my / 127.f;
            sc[0x5d0] = 1;
            if (!down) { down = 1; bv_log("stick_ui: move stick down (screen %p)", self); }
        } else if (down) { sc[0x5d0] = 0; down = 0; }
    }
    ((void (*)(void *, uint32_t))(uintptr_t)g_bscreen_orig)(self, dt);
}
volatile uint32_t bv_np_buttons;
volatile int bv_np_lx = 128, bv_np_ly = 128, bv_np_rx = 128, bv_np_ry = 128;
static int np_ok(uint32_t p) { return p >= 0x81000000u && p < 0xc0000000u && !(p & 3); }
static int np_axis(int v) { v -= 128; return (v > -40 && v < 40) ? 0 : v; }   /* dead zone ~30 % */
static void np_send(uint32_t inclient, int type, int x, int y, int absolute) {
    uint32_t base = g_libg.load;
    void *(*f_new)(uint32_t) = (void *(*)(uint32_t))(uintptr_t)((base + 0x82e67cu) | 1u);   /* operator new (Thumb, as in 0x6229b0) */
    void (*f_ctor)(void *, int) = (void (*)(void *, int))(uintptr_t)(base + 0x2f3f14u);
    void (*f_enq)(uint32_t, void *) = (void (*)(uint32_t, void *))(uintptr_t)(base + 0x5270ecu);
    uint8_t *it = (uint8_t *)f_new(0x20);
    if (!it) return;
    f_ctor(it, type);
    *(int *)(it + 0xc) = x;
    *(int *)(it + 0x10) = y;
    it[0x18] = (uint8_t)absolute;
    f_enq(inclient, it);   /* the input list owns the item now */
    static int logged;
    if (logged < 30) { logged++; bv_log("native_input: item type %d (%d, %d)%s", type, x, y, absolute ? " absolute" : " offset"); }
}
void bv_native_tick(void) {
    static uint32_t prev_b;
    static int moving, last_tx, last_ty, adx = 0, ady = -1000, mdx = 0, mdy = -1000;
    static uint64_t last_move_us;
    if (!bv_native_input || !g_libg.load) return;
    uint32_t base = g_libg.load;
    uint32_t gs = ((uint32_t (*)(void))(uintptr_t)(base + 0x4fcd58u))();
    {   /* update 180: battle length starts at the first frame with a running match */
        static int had_gs;
        if (np_ok(gs) && !had_gs) bv_match_t0 = (long)time(NULL);
        had_gs = np_ok(gs);
    }
    if (!np_ok(gs)) {
        moving = 0; prev_b = 0;
        /* update 172: only after the watched match existed (run 203 cleared the byte 0.15 s after the start, during loading -> a normal match) */
        if (bv_tv_watching == 2) { bv_tv_watching = 0; *(volatile uint8_t *)(uintptr_t)(base + 0xc8674cu) = 0; *(volatile uint8_t *)(uintptr_t)(base + 0xc86bfcu) = 0; bv_log("brawl_tv: match over, debug bytes cleared"); tv_match_ended(); }
        return;
    }
    if (bv_tv_watching) { if (bv_tv_watching == 1) { bv_tv_watching = 2; bv_log("brawl_tv: watched match is running"); } tv_watch_frame(gs); return; }   /* watching - no player input (update 184: hero back, leave) */
    uint32_t ctx = *(volatile uint32_t *)(uintptr_t)(gs + 0x18), host = *(volatile uint32_t *)(uintptr_t)(gs + 0x1c), inc = *(volatile uint32_t *)(uintptr_t)(gs + 0x2c);
    if (!np_ok(ctx) || !np_ok(host) || !np_ok(inc)) return;
    if (*(volatile int *)(uintptr_t)(ctx + 0xa8) != -1) return;          /* result decided */
    if (*(volatile uint8_t *)(uintptr_t)(host + 0x6f)) return;           /* spectator / replay (Brawl TV) */
    int slot = *(volatile int *)(uintptr_t)(ctx + 0x74), np = *(volatile int *)(uintptr_t)(host + 0x3c);
    uint32_t plist = *(volatile uint32_t *)(uintptr_t)(host + 0x48), hobj = *(volatile uint32_t *)(uintptr_t)host;
    if (slot < 0 || slot >= np || np > 16 || !np_ok(plist) || !np_ok(hobj)) return;
    uint32_t player = *(volatile uint32_t *)(uintptr_t)(plist + 4u * (uint32_t)slot);
    if (!np_ok(player)) return;
    uint32_t ent = ((uint32_t (*)(uint32_t, int))(uintptr_t)(base + 0x92e60u))(hobj, *(volatile int *)(uintptr_t)(player + 0x10));
    if (!np_ok(ent)) return;
    int ex = *(volatile int *)(uintptr_t)(ent + 0x1c), ey = *(volatile int *)(uintptr_t)(ent + 0x20);
    {   /* update 168b probe (read only, for the on-screen sticks): is the HUD [0x41330c()+0x160] the battle screen (update fn 0x37f2f0, stick
         * fields +0x5d0 down, +0x190/+0x194 origin, +0x198/+0x19c current)? Logged once per battle and then every 2 s while a stick is held. */
        static uint32_t said_hud; static uint64_t last_probe;
        uint32_t g = ((uint32_t (*)(void))(uintptr_t)(base + 0x41330cu))();
        uint32_t hud = np_ok(g) ? *(volatile uint32_t *)(uintptr_t)(g + 0x160) : 0;
        if (np_ok(hud) && hud != said_hud) {
            said_hud = hud;
            uint32_t vt = *(volatile uint32_t *)(uintptr_t)hud;
            int slot = -1;
            for (int k = 0; np_ok(vt) && k < 0x100 && slot < 0; k++) if (*(volatile uint32_t *)(uintptr_t)(vt + 4u * (uint32_t)k) == base + 0x37f2f0u) slot = k;
            bv_log("stick probe: hud %08x vtable libg+0x%x, battle-screen update 0x37f2f0 at slot %d", (unsigned)hud, (unsigned)(vt - base), slot);
        }
        uint64_t tnow = sceKernelGetProcessTimeWide();
        if (np_ok(hud) && (np_axis(bv_np_lx) || np_axis(bv_np_ly) || np_axis(bv_np_rx) || np_axis(bv_np_ry)) && tnow - last_probe > 2000000u) {
            last_probe = tnow;
            volatile float *f = (volatile float *)(uintptr_t)(hud + 0x190);
            volatile uint8_t *bb = (volatile uint8_t *)(uintptr_t)(hud + 0x5d0);
            bv_log("stick probe: +5d0 %d %d %d +5f6 %d origin %.0f,%.0f current %.0f,%.0f", bb[0], bb[1], bb[2], bb[0x26], (double)f[0], (double)f[1], (double)f[2], (double)f[3]);
        }
    }
    uint32_t b = bv_np_buttons;
    int mx = np_axis(bv_np_lx), my = np_axis(bv_np_ly), ax = np_axis(bv_np_rx), ay = np_axis(bv_np_ry);
    if (b & (VPAD_BTN_UP | VPAD_BTN_DOWN | VPAD_BTN_LEFT | VPAD_BTN_RIGHT)) {
        mx = (b & VPAD_BTN_RIGHT) ? 127 : (b & VPAD_BTN_LEFT) ? -127 : 0;
        my = (b & VPAD_BTN_DOWN) ? 127 : (b & VPAD_BTN_UP) ? -127 : 0;
    }
    uint64_t now = sceKernelGetProcessTimeWide();
    g_np_mx = mx; g_np_my = my;   /* update 170: for stick_ui */
    if (g_stick_ui || bv_native_input >= 2) { /* the game's own stick code moves the brawler (update 172: native_input=2 = a virtual finger on it) */ }
    else if (mx || my) {   /* update 168: a point 8 tiles (2400 units) ahead; every new move item restarts the path (one tick of standing, run 199 'moves
                       * slightly'), so resend only when the direction changed (> ~10 degrees) or half the way is done */
        float len = sqrtf((float)(mx * mx + my * my));
        int ndx = (int)(1000.0f * mx / len), ndy = (int)(1000.0f * my / len);
        int tx = ex + ndx * 12 / 5, ty = ey + ndy * 12 / 5;
        int turn = (ndx - mdx) * (ndx - mdx) + (ndy - mdy) * (ndy - mdy);   /* 1000-unit vectors: 175^2 ~ 10 degrees */
        int rx = ex - last_tx, ry = ey - last_ty;
        mdx = ndx; mdy = ndy;
        if (!moving || turn > 175 * 175 || rx * rx + ry * ry < 1200 * 1200) {
            np_send(inc, 2, tx, ty, 1);
            last_tx = tx; last_ty = ty; last_move_us = now; moving = 1;
        }
    } else if (moving) { np_send(inc, 2, ex, ey, 1); moving = 0; }   /* stick released: stop where we are */
    if (ax || ay) { float len = sqrtf((float)(ax * ax + ay * ay)); adx = (int)(1000.0f * ax / len); ady = (int)(1000.0f * ay / len); }
    else { adx = mdx; ady = mdy; }   /* no aim: shoot where we walk */
    uint32_t pressed = b & ~prev_b;
    /* update 168: absolute targets like the HUD's own producer (0x482584 sets +0x18 = 1); the game clamps them to the weapon range */
    if (bv_native_input != 3 && (pressed & (VPAD_BTN_R | VPAD_BTN_CROSS))) np_send(inc, 0, ex + adx * 3, ey + ady * 3, 1);
    if (bv_native_input != 3 && (pressed & VPAD_BTN_L)) np_send(inc, 1, ex + adx * 3, ey + ady * 3, 1);
    if (pressed & VPAD_BTN_TRIANGLE) np_send(inc, 8, ex, ey, 1);
    prev_b = b;
}
/* update 167 (fix 9): home card refresh after an event pick (notes/2026-11-10-page-guard/decode_home_card.md 7.2) */
static int g_home_card_refresh = 1;   /* update 175: default (user pass run 199) */
volatile int bv_home_refresh_pending;   /* set by the offline server on 14362 */
void bv_homecard_tick(void) {
    static int wait;
    static uint32_t home_vt;
    extern volatile uint32_t bv_home_obj;
    uint32_t h = bv_home_obj;
    if (!np_ok(h)) return;
    if (!home_vt) home_vt = *(volatile uint32_t *)(uintptr_t)h;
    if (!bv_home_refresh_pending) return;
    if (wait == 0) wait = 30;
    if (--wait > 0) return;
    bv_home_refresh_pending = 0;
    uint32_t base = g_libg.load;
    if (*(volatile uint32_t *)(uintptr_t)h != home_vt) { bv_log("homecard: home screen object changed: no refresh"); return; }
    if (np_ok(((uint32_t (*)(void))(uintptr_t)(base + 0x4fcd58u))())) return;   /* in a battle */
    uint32_t team = ((uint32_t (*)(void))(uintptr_t)(base + 0x7a284cu))();
    uint32_t tobj = np_ok(team) ? *(volatile uint32_t *)(uintptr_t)team : 0;
    uint32_t entry = *(volatile uint32_t *)(uintptr_t)(h + 0x140);
    bv_log("homecard: before: map shown %08x, entry %08x slot %d, team %08x (team map %08x)", (unsigned)*(volatile uint32_t *)(uintptr_t)(h + 0x144), (unsigned)entry,
           np_ok(entry) ? *(volatile int *)(uintptr_t)(entry + 4) : -1, (unsigned)tobj, np_ok(tobj) ? (unsigned)*(volatile uint32_t *)(uintptr_t)(tobj + 0x1c) : 0u);
    if (!g_home_card_refresh || !np_ok(tobj)) return;
    ((void (*)(uint32_t))(uintptr_t)(base + 0x613328u))(h);
    bv_log("homecard: refreshed (0x613328): map shown now %08x", (unsigned)*(volatile uint32_t *)(uintptr_t)(h + 0x144));
}
static int g_colette_min = 1;   /* update 175: default (user pass run 201) */
static int g_pass_button_rewards = 0; /* config pass_button_rewards (update 106 experiment D7): the home pass button opens the reward track */
static int g_team_play_solo = 0; /* config team_play_solo (cycle 6 experiment): Play starts the local bot match also inside a team */
static int g_local_battle = 1;  /* config local_battle (update 48): Play starts a local bot battle (gamefix.h) */
static int g_battle_bots = 1;   /* config battle_bots (update 48): the start-game "with bots" argument forced to 1 (inferred) */
static uint32_t g_probe_off[BV_PROBE_MAX]; /* config probes=ADDR,... (update 47): libg offsets of code-path probes */
static int g_probe_n;
static bvb_options g_opt;
static bv_server g_server;
static SceUID g_vs_mutex = -1;
extern int bv_gles_major;
extern int bv_gltrace_frame; /* shims/shim_gles.c: log two whole frames of GL calls from this EGL swap on (0 = off) */
extern int bv_fragcoord_fix;  /* shims/shim_gles.c: 1 (default) = gl_FragCoord.xy of the game's shaders is made bottom-up like OpenGL */
extern int bv_fmod_output;    /* shims/shim_fmod.c: experiment, FMOD output type instead of NOSOUND (0 = silent, -1 = what the game asks) */
extern uint32_t bv_skip_prog_hash; /* shims/shim_gles.c, cycle 6 experiment skip_prog=HASH */
extern int bv_cond_pool;      /* shims/shim_pthread.c, cycle 6 experiment: reuse destroyed condition variables */
extern int bv_sem_reaper;     /* shims/shim_pthread.c, cycle 5 experiment: sem_destroy on a background thread */
extern int bv_yield_us;       /* shims/shim_pthread.c, cycle 5: sleep per sched_yield (1000 = as before) */
extern int bv_thread_cores;   /* shims/shim_pthread.c: 0 = threads keep their affinity, 1 = FMOD on core 2 / game on 0+1, 2 = all cores */
extern int bv_rt_purge;        /* shims/shim_gxm.c, update 95 fix A5 (default 1) */
extern int bv_gc_now, bv_gc_low_kb; /* shims/shim_gxm.c, update 113 fix A14 (Showdown pre-bake memory) */
extern int bv_fbo_finish;      /* shims/shim_gles.c, update 93 experiment A5 */
#include "vpad.h"
extern int bv_vpad;           /* loader/touch_vita.c, cycle 7 J1 */
extern vpad_cfg bv_vpad_cfg;
extern uint32_t bv_battle_kind_addr;
extern int bv_bot_names;       /* shims/shim_asset.c, cycle 8 G6 */
extern int bv_n_csv_patch;     /* shims/shim_asset.c, update 111 */
extern char bv_csv_patch_cfg[64][4][64];       /* shims/shim_asset.c, cycle 8 G6 */
extern int bv_rt_fresh;        /* shims/shim_gles.c, update 92 experiment A5 */
extern int bv_fbo_scenes;      /* gfx/gl_init.c, update 92 experiment A5 */
extern int bv_crash_trace;     /* shims/shim_gles.c, update 90 diagnostic A5 */
extern int ts_lut;            /* shims/texshrink.c, update 88 experiment H */
extern int bv_file_mmap_kb;  /* shims/shim_io.c, update 87 experiment H */
extern int bv_sink_fast;      /* shims/shim_io.c, update 87 experiment H */
extern int bv_vgl_cache;      /* gfx/gl_init.c, update 87 experiment H1 */
extern int bv_swfcache;       /* shims/shim_io.c: 0 (default) = the game's never-used unpacked-file cache is not written, 1 = written as before */
extern int bv_game_stack_kb;   /* shims/shim_pthread.c: stack of the game main thread (update 53) */
extern int bv_depth_ext;       /* shims/shim_gles.c: 0 = GL_EXTENSIONS without depth textures (update 54 experiment) */
extern int bv_finish_exits;    /* jni/fake_jni.c: 1 = GameApp.requestFinish() exits the app (cycle 1) */
extern int bv_rt_mipmap_skip;  /* shims/shim_gles.c: 1 = no mipmaps for render targets (cycle 4) */
extern int bv_tex16_kb;       /* shims/shim_gles.c: RGBA8 textures of at least this many KB are stored as RGBA4444 (update 51); 0 = off */
extern int bv_etc1_native;    /* shims/shim_gles.c: 1 = ETC1 textures go to vitaGL unchanged (experiment), 0 = ETC1 -> DXT1 transcode */
extern int bv_fbo_flip_from;  /* shims/shim_gles.c: experiment, flip what was drawn into colour framebuffers from this swap on (0 = off) */
extern int bv_shot_frames[4], bv_shot_n; /* shims/shim_egl.c: screenshots at these swaps */

/* called by the thunk of an unresolved import (loader/unresolved.S) */
uint32_t bv_unresolved(uint32_t id, uint32_t lr) {
    static uint32_t counts[SO_MAX_UNRESOLVED];
    if (id >= g_unres.n) return 0;
    if (++counts[id] > 3 && (counts[id] & (counts[id] - 1))) return 0; /* log 1,2,3, then powers of two */
    so_module *m = bv_module_for_pc(lr & ~1u);
    bv_log("UNRESOLVED import %s::%s called (#%u) from %s+0x%x", g_unres.module[id] ? "libg" : "libfmod", g_unres.name[id],
           counts[id], m ? m->name : "?", m ? (lr & ~1u) - m->load : lr);
    return 0;
}

static uint32_t resolve_cb(void *ctx, const char *name) {
    (void)ctx;
    uint32_t a = bv_shim_lookup(name);
    if (a) return a;
    so_module *mods[2] = {&g_fmod, &g_libg};
    for (int i = 0; i < 2; i++)
        if (mods[i]->host && (a = so_find_symbol(mods[i], name))) return a;
    return 0;
}

static uint8_t *read_file(const char *path, size_t *size) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        bv_log("cannot open %s (0x%08x)", path, (unsigned)fd);
        return NULL;
    }
    SceOff n = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) {
        bv_log("out of memory reading %s (%u bytes)", path, (unsigned)n);
        sceIoClose(fd);
        return NULL;
    }
    size_t got = 0;
    while (got < (size_t)n) {
        int r = sceIoRead(fd, buf + got, (SceSize)((size_t)n - got > (1u << 20) ? (1u << 20) : (size_t)n - got));
        if (r <= 0) break;
        got += (size_t)r;
    }
    sceIoClose(fd);
    if (got != (size_t)n) {
        bv_log("short read of %s (%u of %u)", path, (unsigned)got, (unsigned)n);
        free(buf);
        return NULL;
    }
    *size = (size_t)n;
    return buf;
}

static void read_config(void) {
    size_t n;
    vpad_default(&bv_vpad_cfg); /* cycle 7 J1: layout defaults before the config keys */
    uint8_t *b = read_file(BV_DATA_DIR "/config.txt", &n);
#ifdef BV_PUBLIC_CONFIG
    /* update 188 (v0.1 release): the tested settings are built in (loader/builtin_config.h); from config.txt only the profile= line is used */
    size_t nb = sizeof k_builtin_config - 1;
    char *txt = (char *)malloc(nb + (b ? n : 0) + 2);
    memcpy(txt, k_builtin_config, nb);
    size_t o = nb;
    txt[o++] = '\n';
    for (size_t i = 0; b && i < n;) {
        size_t e = i;
        while (e < n && b[e] != '\n' && b[e] != '\r') e++;
        if (e - i > 8 && !memcmp(b + i, "profile=", 8)) { memcpy(txt + o, b + i, e - i); o += e - i; txt[o++] = '\n'; }
        i = e + 1;
    }
    txt[o] = 0;
    free(b);
    bv_log("config: built-in settings + the profile line of config.txt (public build)");
#else
    if (!b) return;
    char *txt = (char *)malloc(n + 1);
    memcpy(txt, b, n);
    txt[n] = 0;
    free(b);
#endif
    for (char *line = strtok(txt, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        int v, v2;
        if (sscanf(line, "trace=%d", &v) == 1) bv_trace_level = v;
        else if (sscanf(line, "max_ctor=%d", &v) == 1) g_max_ctor = v;
        else if (sscanf(line, "skip_ctors=%d", &v) == 1) g_skip_ctors = v;
        else if (sscanf(line, "bringup=%d", &v) == 1) g_bringup = v;
        else if (sscanf(line, "rawsys=%d", &v) == 1) g_rawsys = v;
        else if (sscanf(line, "antitamper=%d", &v) == 1) g_antitamper = v;
        else if (sscanf(line, "snapshot=%d", &v) == 1) g_snapshot = v;
        else if (sscanf(line, "run_seconds=%d", &v) == 1) g_run_seconds = v;
        else if (sscanf(line, "patches=%d", &v) == 1) g_opt.patches = v;
        else if (sscanf(line, "offline=%d", &v) == 1) g_opt.enabled = v;
        else if (sscanf(line, "resend_home=%d", &v) == 1) g_opt.resend_home = v;
        else if (sscanf(line, "brawler_row=%d", &v) == 1) g_opt.brawler_row = v;
        else if (sscanf(line, "map_row=%d", &v) == 1) g_opt.map_row = v;
        else if (sscanf(line, "events=%d", &v) == 1) g_opt.events = v;
        else if (sscanf(line, "unlock_all=%d", &v) == 1) g_opt.unlock_all = v;
        else if (sscanf(line, "trophies=%d", &v) == 1) g_opt.trophies = v;
        else if (sscanf(line, "account_trophies=%d", &v) == 1) g_opt.account_trophies = v;
        else if (sscanf(line, "power_level=%d", &v) == 1) g_opt.power_level = v;
        else if (sscanf(line, "home_flags=%d", &v) == 1) g_opt.home_flags = v;
        else if (sscanf(line, "shop_disabled=%d", &v) == 1) g_opt.shop_disabled = v;
        else if (sscanf(line, "offer_test=%d", &v) == 1) g_opt.offer_test = v;
        else if (sscanf(line, "tutorial_steps=%d", &v) == 1) g_opt.tutorial_steps = v;
        else if (sscanf(line, "box_coin_type=%d", &v) == 1) g_opt.box_coin_type = v;
        else if (sscanf(line, "box_unit=%d", &v) == 1) g_opt.box_unit_mode = v;
        else if (sscanf(line, "gems=%d", &v) == 1) g_opt.gems = v;
        else if (sscanf(line, "coins=%d", &v) == 1) g_opt.coins = v;
        else if (sscanf(line, "shop_offers=%d", &v) == 1) g_opt.shop_offers = v;
        else if (sscanf(line, "offer_cur=%d", &v) == 1) g_opt.offer_cur = v;
        else if (sscanf(line, "owned_skins=%d", &v) == 1) g_opt.owned_skins = v;
        else if (sscanf(line, "res_markers=%d", &v) == 1) g_opt.res_markers = v;
        else if (sscanf(line, "box_pp_random=%d", &v) == 1) g_opt.box_pp_random = v;
        else if (sscanf(line, "offer_unit=%d", &v) == 1) g_opt.offer_unit = v;
        else if (sscanf(line, "profile=%d", &v) == 1) g_opt.profile_on = v;
        else if (sscanf(line, "pass_claimed_bits=%d", &v) == 1) g_opt.pass_claimed_bits = v;
        else if (sscanf(line, "pass_claim_reply=%d", &v) == 1) g_opt.pass_claim_reply = v;
        else if (!strncmp(line, "mode_push=", 10)) { /* cycle 1 experiment: mode_push=ROW@N */
            int row = -1, after = 3;
            if (sscanf(line + 10, "%d@%d", &row, &after) >= 1) { g_opt.mode_push_map = row; g_opt.mode_push_after = after; }
            bv_log("config: mode_push experiment: 24124 with map row %d after keep-alive #%d", g_opt.mode_push_map, g_opt.mode_push_after);
        }
        else if (sscanf(line, "skin_offer_kind=%d", &v) == 1) g_opt.skin_offer_kind = v;
        else if (!strncmp(line, "avatar_tail=", 12)) { /* update 57: 12 values, missing ones keep the mod's default */
            static const int t[12] = {0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 2};
            for (int i = 0; i < 12; i++) g_opt.avatar_tail[i] = t[i];
            const char *q = line + 12;
            for (int i = 0; i < 12 && *q; i++) {
                g_opt.avatar_tail[i] = atoi(q);
                while (*q && *q != ',') q++;
                if (*q == ',') q++;
            }
            g_opt.avatar_tail_set = 1;
            bv_log("config: avatar_tail = %d %d %d %d %d %d %d %d %d %d %d %d (update 57)", g_opt.avatar_tail[0], g_opt.avatar_tail[1], g_opt.avatar_tail[2],
                   g_opt.avatar_tail[3], g_opt.avatar_tail[4], g_opt.avatar_tail[5], g_opt.avatar_tail[6], g_opt.avatar_tail[7], g_opt.avatar_tail[8],
                   g_opt.avatar_tail[9], g_opt.avatar_tail[10], g_opt.avatar_tail[11]);
        }
        else if (!strncmp(line, "offer_types=", 12)) {
            const char *q = line + 12;
            for (int i = 0; i < 16 && *q; i++) {
                g_opt.offer_type[i] = atoi(q);
                while (*q && *q != ',') q++;
                if (*q == ',') q++;
            }
        }
        else if (!strncmp(line, "config_keys=", 12)) { /* config feature switches for the home data: K:V,K:V,... (update 42) */
            const char *q = line + 12;
            g_opt.n_extra_keys = 0;
            while (*q && g_opt.n_extra_keys < 8) {
                int k = 0, val = 0;
                if (sscanf(q, "%d:%d", &k, &val) == 2) {
                    g_opt.extra_key[g_opt.n_extra_keys][0] = k;
                    g_opt.extra_key[g_opt.n_extra_keys][1] = val;
                    g_opt.n_extra_keys++;
                }
                while (*q && *q != ',') q++;
                if (*q == ',') q++;
            }
        }
        else if (sscanf(line, "brawl_pass=%d", &v) == 1) g_opt.brawl_pass = v;
        else if (sscanf(line, "pass_season=%d", &v) == 1) g_opt.pass_season = v;
        else if (sscanf(line, "pass_tokens=%d", &v) == 1) g_opt.pass_tokens = v;
        else if (sscanf(line, "pass_premium=%d", &v) == 1) g_opt.pass_premium = v;
        else if (!strncmp(line, "event_slots=", 12)) { /* slot ids of events 0..7, comma separated (update 37) */
            const char *q = line + 12;
            for (int i = 0; i < 8 && *q; i++) {
                g_opt.event_slot[i] = atoi(q);
                while (*q && *q != ',') q++;
                if (*q == ',') q++;
            }
        }
        else if (!strncmp(line, "event_maps=", 11)) { /* rows of the locations table for events 1..7, comma separated */
            const char *q = line + 11;
            for (int i = 1; i < 8 && *q; i++) {
                g_opt.event_map[i] = atoi(q);
                while (*q && *q != ',') q++;
                if (*q == ',') q++;
            }
        }
        else if (sscanf(line, "release_old=%d", &v) == 1) g_opt.release_old = v;
        else if (sscanf(line, "gles=%d", &v) == 1) bv_gles_major = v;
        else if (sscanf(line, "gltrace_frame=%d", &v) == 1) bv_gltrace_frame = v;
        else if (sscanf(line, "fbo_flip_from=%d", &v) == 1) bv_fbo_flip_from = v;
        else if (sscanf(line, "fmod_output=%d", &v) == 1) bv_fmod_output = v;
        else if (sscanf(line, "cores=%d", &v) == 1) bv_thread_cores = v;
        else if (!strncmp(line, "skip_prog=", 10)) { unsigned hx = 0; if (sscanf(line + 10, "%x", &hx) == 1) bv_skip_prog_hash = hx; bv_log("config: skip_prog=%08x (experiment: draws with that shader are skipped)", (unsigned)bv_skip_prog_hash); }
        else if (sscanf(line, "cond_pool=%d", &v) == 1) { bv_cond_pool = v; bv_log("config: cond_pool=%d (experiment: destroyed condition variables are reused)", v); }
        else if (sscanf(line, "sem_reaper=%d", &v) == 1) { bv_sem_reaper = v; bv_log("config: sem_reaper=%d (experiment: sem_destroy on a background thread)", v); }
        else if (sscanf(line, "yield_us=%d", &v) == 1) { bv_yield_us = v; bv_log("config: yield_us=%d (experiment: sleep per sched_yield, 1000 = default)", v); }
        else if (sscanf(line, "swfcache=%d", &v) == 1) bv_swfcache = v;
        else if (sscanf(line, "file_mmap_kb=%d", &v) == 1) { bv_file_mmap_kb = v; bv_log("config: file_mmap_kb=%d (experiment H: read-only file mmaps served from memory)", v); }
        else if (sscanf(line, "t16_lut=%d", &v) == 1) { ts_lut = v; bv_log("config: t16_lut=%d (experiment H: table-driven RGBA4444 conversion)", v); }
        else if (sscanf(line, "sink_fast=%d", &v) == 1) { bv_sink_fast = v; bv_log("config: sink_fast=%d (experiment H: swfcache sink writes return at once)", v); }
        else if (sscanf(line, "vgl_cache=%d", &v) == 1) { bv_vgl_cache = v; bv_log("config: vgl_cache=%d (experiment H1: pair-keyed vitaGL shader cache)", v); }
        else if (sscanf(line, "etc1_native=%d", &v) == 1) bv_etc1_native = v;
        else if (sscanf(line, "tex16=%d", &v) == 1) bv_tex16_kb = v;
        else if (sscanf(line, "rt_mipmap=%d", &v) == 1) bv_rt_mipmap_skip = !v; /* rt_mipmap=1 = old behaviour */
        else if (sscanf(line, "team_probe=%d", &v) == 1) g_team_probe = v;
        else if (sscanf(line, "shop_layout=%d", &v) == 1) g_opt.shop_layout = v;
        else if (sscanf(line, "pass_new_types=%d", &v) == 1) g_opt.pass_new_types = v;
        else if (sscanf(line, "profile_brawlers=%d", &v) == 1) g_opt.profile_brawlers = v;
        else if (sscanf(line, "member_view=%d", &v) == 1) g_opt.member_view = v;
        else if (!strncmp(line, "team_member=", 12)) {
            int a = -1, b = -1, c3 = -1, d = 3;
            if (sscanf(line + 12, "%d,%d,%d,%d", &a, &b, &c3, &d) >= 1) { g_opt.team_m10 = a; g_opt.team_m14 = b; g_opt.team_m18 = c3; g_opt.team_m1c = d; }
            bv_log("config: team_member=%d,%d,%d,%d (team member +0x10 / +0x14 / +0x18 / +0x1c in 24124)", g_opt.team_m10, g_opt.team_m14, g_opt.team_m18, g_opt.team_m1c);
        }
        else if (!strncmp(line, "team_fields=", 12)) {
            int a = 1, b = 1;
            if (sscanf(line + 12, "%d,%d", &a, &b) >= 1) { g_opt.team_f0 = a; g_opt.team_f8 = b; }
            bv_log("config: team_fields=%d,%d (experiment: team +0 / +8 in 24124)", g_opt.team_f0, g_opt.team_f8);
        }
        else if (sscanf(line, "trophy_road_claimed=%d", &v) == 1) { g_opt.trophy_road_claimed = v; bv_log("config: trophy_road_claimed=%d (experiment: trophy road claimed up to the current trophies)", v); }
        else if (sscanf(line, "battle_end_msg=%d", &v) == 1) { g_opt.battle_end_msg = v; bv_log("config: battle_end_msg=%d (A6: 14110 gets the result screen 23456)", v); }
        else if (sscanf(line, "battle_trophies=%d", &v) == 1) { g_opt.battle_trophies = v; bv_log("config: battle_trophies=%d (A6: trophy changes kept)", v); }
        else if (sscanf(line, "battle_win=%d", &v) == 1) g_opt.battle_win = v;
        else if (sscanf(line, "battle_draw=%d", &v) == 1) g_opt.battle_draw = v;
        else if (sscanf(line, "battle_lose=%d", &v) == 1) g_opt.battle_lose = v;
        else if (sscanf(line, "gc_now=%d", &v) == 1) { bv_gc_now = v; bv_log("config: gc_now=%d (A14: vitaGL garbage collector run inside long frames, 0 = off)", v); }
        else if (sscanf(line, "gc_low_kb=%d", &v) == 1) { bv_gc_low_kb = v; bv_log("config: gc_low_kb=%d", v); }
        else if (sscanf(line, "prof_zones=%d", &v) == 1) { g_prof_zones = v; bv_log("config: prof_zones=%d (update 165: the game's profiler zones logged per second)", v); }
        else if (sscanf(line, "screen_res=%d", &v) == 1) {   /* update 167 (fps B): Vita display modes 960x544 / 720x408 / 640x368 */
            extern int bv_screen_w, bv_screen_h;
            if (v == 720) { bv_screen_w = 720; bv_screen_h = 408; } else if (v == 640) { bv_screen_w = 640; bv_screen_h = 368; } else { bv_screen_w = 960; bv_screen_h = 544; }
            bv_log("config: screen_res=%d -> the game renders at %dx%d (hardware upscale to the panel)", v, bv_screen_w, bv_screen_h);
        }
        else if (sscanf(line, "home_card_refresh=%d", &v) == 1) { g_home_card_refresh = v; bv_log("config: home_card_refresh=%d (update 167: refresh the home card after an event pick)", v); }
        else if (sscanf(line, "loading_notice=%d", &v) == 1) { extern int bv_loading_notice; bv_loading_notice = v; }
        else if (sscanf(line, "stick_ui=%d", &v) == 1) { g_stick_ui = v; bv_log("config: stick_ui=%d (update 170: on-screen move stick follows the left stick)", v); }
        else if (sscanf(line, "colette_min=%d", &v) == 1) { g_colette_min = v; bv_log("config: colette_min=%d (update 167: Colette minimum damage 500)", v); }
        else if (sscanf(line, "native_input=%d", &v) == 1) { bv_native_input = v; bv_log("config: native_input=%d (update 167: buttons feed the game's input queue directly)", v); }
        else if (sscanf(line, "brawl_tv=%d", &v) == 1) { g_brawl_tv = v; bv_brawl_tv_on = v; bv_log("config: brawl_tv=%d (update 163: Brawl TV = watch-only local bot match)", v); }
        else if (sscanf(line, "bot_power=%d", &v) == 1) { g_bot_power = v; bv_log("config: bot_power=%d (update 162: bots at the played brawler's level, your brawler at its real level)", v); }
        else if (sscanf(line, "trophy_cull=%d", &v) == 1) { g_trophy_cull = v; bv_log("config: trophy_cull=%d (update 162: trophy road draws only the visible rows)", v); }
        else if (sscanf(line, "input_probe=%d", &v) == 1) { bv_input_probe = v; bv_log("config: input_probe=%d (update 162: touch listener list logged)", v); }
        else if (sscanf(line, "box_cards=%d", &v) == 1) { extern int bv_box_cards; bv_box_cards = v; bv_log("config: box_cards=%d (update 174: gadgets / star powers from boxes)", v); }
        else if (sscanf(line, "box_drops=%d", &v) == 1) { extern int bv_box_drops; bv_box_drops = v; bv_log("config: box_drops=%d (update 144: 2020 box rules)", v); }
        else if (sscanf(line, "rt_cache=%d", &v) == 1) { extern int bv_rt_cache; bv_rt_cache = v; bv_log("config: rt_cache=%d (update 144: render targets reused by size)", v); }
        else if (sscanf(line, "prof=%d", &v) == 1) { extern int bv_prof; bv_prof = v; bv_log("config: prof=%d (update 144: per-second perf line)", v); }
        else if (sscanf(line, "rt_purge=%d", &v) == 1) { bv_rt_purge = v; bv_log("config: rt_purge=%d (A5 fix: queued render targets destroyed when creation fails)", v); }
        else if (sscanf(line, "fbo_finish=%d", &v) == 1) { bv_fbo_finish = v; bv_log("config: fbo_finish=%d (experiment A5: glFinish every N FBO clears of a frame)", v); }
        else if (sscanf(line, "rt_fresh=%d", &v) == 1) { bv_rt_fresh = v; bv_log("config: rt_fresh=%d (experiment A5: new render target per FBO texture)", v); }
        else if (sscanf(line, "fbo_scenes=%d", &v) == 1) { bv_fbo_scenes = v; bv_log("config: fbo_scenes=%d (experiment A5)", v); }
        else if (sscanf(line, "crash_trace=%d", &v) == 1) { bv_crash_trace = v; bv_log("config: crash_trace=%d (diagnostic A5: GL trace from the intro-scene line)", v); }
        else if (!strncmp(line, "limits=", 7)) {   /* update 115 (E7): 13 comma-separated VInts */
            char *q = line + 7;
            g_opt.n_limits = 0;
            while (*q && g_opt.n_limits < 13) {
                g_opt.limits[g_opt.n_limits++] = (int)strtol(q, &q, 10);
                if (*q == ',') q++; else break;
            }
            bv_log("config: limits= %d values (first %d, last %d)", g_opt.n_limits, g_opt.limits[0], g_opt.limits[g_opt.n_limits > 0 ? g_opt.n_limits - 1 : 0]);
        }
        else if (sscanf(line, "token_cfg=%d,%d,%d,%d", &g_opt.tok_cfg[0], &g_opt.tok_cfg[1], &g_opt.tok_cfg[2], &g_opt.tok_cfg[3]) == 4)
            bv_log("config: token_cfg=%d,%d,%d,%d (E7: config +0xa8 / +0xac key bar maximum / +0xb0 / +0xb4)", g_opt.tok_cfg[0], g_opt.tok_cfg[1], g_opt.tok_cfg[2], g_opt.tok_cfg[3]);
        else if (sscanf(line, "battle_log_sd=%d", &v) == 1) { g_opt.battle_log_sd = v; bv_log("config: battle_log_sd=%d", v); }
        else if (sscanf(line, "battle_log=%d", &v) == 1) { g_opt.battle_log = v; bv_log("config: battle_log=%d (G7: 14114 -> 23458, 2 = labelling)", v); }
        else if (sscanf(line, "offer_title_slot=%d", &v) == 1) { g_opt.offer_title_slot = v; bv_log("config: offer_title_slot=%d (B11 experiment)", v); }
        else if (sscanf(line, "battle_end_labels=%d", &v) == 1) { g_opt.battle_end_labels = v; bv_log("config: battle_end_labels=%d (A11 labelling: 23456 +0x60..+0x90 = 400..1600)", v); }
        else if (sscanf(line, "new_event_tokens=%d", &v) == 1) { g_opt.new_event_tokens = v; bv_log("config: new_event_tokens=%d (E4: tokens on the first tap of a new event)", v); }
        else if (sscanf(line, "trophy_league=%d", &v) == 1) { g_opt.trophy_league = v; bv_log("config: trophy_league=%d (D6: season end resets trophies > 500, pays star points)", v); }
        else if (sscanf(line, "no_highres_battle=%d", &v) == 1) { extern int bv_no_highres_battle; bv_no_highres_battle = v; bv_log("config: no_highres_battle=%d (A14: normal-resolution brawler textures in battle)", v); }
        else if (sscanf(line, "tv_random_hero=%d", &v) == 1) { g_tv_random_hero = v; bv_log("config: tv_random_hero=%d (update 184: Brawl TV hero = a random owned brawler)", v); }
        else if (sscanf(line, "tv_next=%d", &v) == 1) { g_tv_next = v; bv_log("config: tv_next=%d (update 184: Brawl TV starts the next match by itself)", v); }
        else if (sscanf(line, "skin_special=%d", &v) == 1) { extern int bv_skin_special; bv_skin_special = v; bv_log("config: skin_special=%d (update 184: skin of the day / seasonal skins as special offers)", v); }
        else if (sscanf(line, "shop_seasonal_all=%d", &v) == 1) { extern int bv_shop_seasonal_all; bv_shop_seasonal_all = v; bv_log("config: shop_seasonal_all=%d (update 184b: every seasonal skin in the shop, test)", v); }
        else if (sscanf(line, "shop_date=%d", &v) == 1) { extern int bv_shop_date; bv_shop_date = v; bv_log("config: shop_date=%04d (update 184: shop season date MMDD, 0 = today)", v); }
        else if (sscanf(line, "audio_ring=%d", &v) == 1) { extern int bv_audio_ring; bv_audio_ring = v; bv_log("config: audio_ring=%d (update 184: own audio output thread + ring)", v); }
        else if (sscanf(line, "offer_tids=%d", &v) == 1) { extern int bv_offer_tids; bv_offer_tids = v; bv_log("config: offer_tids=%d (B11: offer titles as text ids)", v); }
        else if (sscanf(line, "all_maps=%d", &v) == 1) { extern int bv_all_maps; bv_all_maps = v; bv_log("config: all_maps=%d (every map of the rotated modes, Hot Zone in slot 5)", v); }
        else if (sscanf(line, "upcoming_events=%d", &v) == 1) { g_opt.upcoming_events = v; bv_log("config: upcoming_events=%d (E5: next map per slot -> New Event in <time>)", v); }
        else if (sscanf(line, "play_tokens=%d", &v) == 1) { g_opt.play_tokens = v; bv_log("config: play_tokens=%d (E7: battle token pool 200, +20 / 2h24m)", v); }
        else if (sscanf(line, "home_tokens=%d,%d", &v, &v2) == 2) { g_opt.home_a4 = v; g_opt.home_a8 = v2; bv_log("config: home_tokens=%d,%d (E7: home +0xa4 / +0xa8, key bar)", v, v2); }
        else if (sscanf(line, "profile_reply=%d", &v) == 1) { g_opt.profile_reply = v; bv_log("config: profile_reply=%d (G5: 14113 -> 24113 player profile)", v); }
        else if (sscanf(line, "home_on_14109=%d", &v) == 1) { g_opt.home_on_14109 = v; bv_log("config: home_on_14109=%d (experiment A: 14109 gets the home)", v); }
        else if (sscanf(line, "ack24125=%d", &v) == 1) { g_opt.ack24125 = v; bv_log("config: ack24125=%d (0 = experiment: no 24125 team-left acknowledgements)", v); }
        else if (sscanf(line, "finish_exits=%d", &v) == 1) bv_finish_exits = v;
        else if (sscanf(line, "game_stack_kb=%d", &v) == 1) bv_game_stack_kb = v;
        else if (sscanf(line, "depth_ext=%d", &v) == 1) bv_depth_ext = v;
        else if (sscanf(line, "trace_getters=%d", &v) == 1) bv_gtrace_enabled = v;
        else if (!strncmp(line, "probes=", 7)) g_probe_n = bv_probe_parse(line + 7, g_probe_off, BV_PROBE_MAX);
        else if (sscanf(line, "local_battle=%d", &v) == 1) g_local_battle = v;
        else if (sscanf(line, "team_play_solo=%d", &v) == 1) g_team_play_solo = v;
        else if (sscanf(line, "map_rotation=%d", &v) == 1) { g_opt.map_rotation = v; bv_log("config: map_rotation=%d (E3: maps rotate every 4h48m)", v); }
        else if (sscanf(line, "theme_fallback=%d", &v) == 1) { extern int bv_theme_fallback; bv_theme_fallback = v; }
        else if (!strncmp(line, "asset_alias=", 12)) {   /* update 174: asset_alias=<missing asset>,<stand-in asset> */
            extern int bv_n_asset_alias; extern char bv_asset_alias[16][2][96];
            const char *p = line + 12, *c = strchr(p, ',');
            if (c && bv_n_asset_alias < 16 && c - p < 96 && strlen(c + 1) < 96) {
                memcpy(bv_asset_alias[bv_n_asset_alias][0], p, (size_t)(c - p)); bv_asset_alias[bv_n_asset_alias][0][c - p] = 0;
                strcpy(bv_asset_alias[bv_n_asset_alias][1], c + 1);
                char *e = bv_asset_alias[bv_n_asset_alias][1] + strlen(bv_asset_alias[bv_n_asset_alias][1]);
                while (e > bv_asset_alias[bv_n_asset_alias][1] && (e[-1] == 13 || e[-1] == 10 || e[-1] == 32)) *--e = 0;
                bv_log("config: asset_alias %s -> %s", bv_asset_alias[bv_n_asset_alias][0], bv_asset_alias[bv_n_asset_alias][1]);
                bv_n_asset_alias++;
            }
        }
        else if (!strncmp(line, "csv_patch=", 10) && bv_n_csv_patch < 64) {   /* update 111: <asset path>,<row>,<column>,<value> */
            char *p = line + 10;
            int k = 0;
            for (; k < 4 && p; k++) {
                char *c = k < 3 ? strchr(p, ',') : NULL;
                size_t l = c ? (size_t)(c - p) : strlen(p);
                if (l > 63) l = 63;
                memcpy(bv_csv_patch_cfg[bv_n_csv_patch][k], p, l);
                bv_csv_patch_cfg[bv_n_csv_patch][k][l] = 0;
                p = c ? c + 1 : NULL;
            }
            if (k == 4) { bv_log("config: csv_patch %s %s.%s = %s", bv_csv_patch_cfg[bv_n_csv_patch][0], bv_csv_patch_cfg[bv_n_csv_patch][1], bv_csv_patch_cfg[bv_n_csv_patch][2], bv_csv_patch_cfg[bv_n_csv_patch][3]); bv_n_csv_patch++; }
        }
        else if (sscanf(line, "world=%d", &v) == 1) { extern int bv_world_on; bv_world_on = v; bv_log("config: world=%d (Phase 3: bot world, leaderboards, bot profiles)", v); }
        else if (sscanf(line, "world_match=%d", &v) == 1) { extern int bv_world_match; bv_world_match = v; bv_log("config: world_match=%d (Phase 3: battles against world bots)", v); }
        else if (sscanf(line, "world_ai=%d", &v) == 1) { bv_world_ai = v; bv_log("config: world_ai=%d (Phase 3: bot strength from rank + hidden skill)", v); }
        else if (sscanf(line, "world_bots=%d", &v) == 1) { extern int bv_world_bots; bv_world_bots = v; bv_log("config: world_bots=%d (a NEW world only)", v); }
        else if (sscanf(line, "world_days=%d", &v) == 1) { extern int bv_world_days; bv_world_days = v; bv_log("config: world_days=%d (offline catch-up limit)", v); }
        else if (!strncmp(line, "world_region=", 13)) { extern char bv_world_region[4]; bv_world_region[0] = line[13]; bv_world_region[1] = line[13] ? line[14] : 0; bv_world_region[2] = 0; bv_log("config: world_region=%s", bv_world_region); }
        else if (sscanf(line, "bot_names=%d", &v) == 1) { bv_bot_names = v; bv_log("config: bot_names=%d (G6: player-style bot names)", v); }
        else if (sscanf(line, "pass_button_rewards=%d", &v) == 1) { g_pass_button_rewards = v; bv_log("config: pass_button_rewards=%d (D7: pass button -> rewards tab)", v); }
        else if (!strncmp(line, "profile=", 8)) {   /* update 188 (v0.1): normal | modded | max, each its own save + bot world */
            extern char bv_profile_slot[24];
            extern int bv_profile_mode;
            const char *m = line + 8;
            bv_profile_mode = !strncmp(m, "max", 3) ? 2 : !strncmp(m, "modded", 6) ? 1 : 0;
            snprintf(bv_profile_slot, 24, "%s", bv_profile_mode == 2 ? "max" : bv_profile_mode == 1 ? "modded" : "normal");
            bv_log("config: profile=%s", bv_profile_slot);
        }
        else if (sscanf(line, "ready_unblock=%d", &v) == 1) { g_ready_unblock = v; bv_log("config: ready_unblock=%d (update 187: profile and other features open while the team is ready)", v); }
        else if (sscanf(line, "new_event_label=%d", &v) == 1) { g_new_event_label = v; bv_log("config: new_event_label=%d (E5)", v); }
        else if (!strncmp(line, "profile_slot=", 13) || !strncmp(line, "profile=", 8)) {   /* update 143: profile_slot=NAME; update 144: profile=personal|test (same) */
            extern char bv_profile_slot[24];
            int k = 0;
            for (const char *q = strchr(line, '=') + 1; *q && k < 23; q++)
                if ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_') bv_profile_slot[k++] = *q;
            bv_profile_slot[k] = 0;
            bv_log("config: profile_slot=%s", bv_profile_slot);
        }
        else if (sscanf(line, "ds_store=%d", &v) == 1) { extern int bv_ds_store; bv_ds_store = v; bv_log("config: ds_store=%d (update 172: icons fix B)", v); }
        else if (sscanf(line, "fbo_stencil_off=%d", &v) == 1) { extern int bv_fbo_stencil_off; bv_fbo_stencil_off = v; bv_log("config: fbo_stencil_off=%d (update 171: icons fix A)", v); }
        else if (sscanf(line, "stencil_probe=%d", &v) == 1) { extern int bv_stencil_probe; bv_stencil_probe = v; bv_log("config: stencil_probe=%d", v); }
        else if (sscanf(line, "depth_tex_rb=%d", &v) == 1) { extern int bv_depth_tex_rb; bv_depth_tex_rb = v; bv_log("config: depth_tex_rb=%d (F1: depth textures -> depth renderbuffer)", v); }
        else if (sscanf(line, "preview_nodepth=%d", &v) == 1) { extern int bv_preview_nodepth; bv_preview_nodepth = v; bv_log("config: preview_nodepth=%d (F1 experiment)", v); }
        else if (sscanf(line, "rt_probe=%d", &v) == 1) { extern int bv_rt_probe; bv_rt_probe = v; bv_log("config: rt_probe=%d (F1 probe: depth-RB targets cleared magenta)", v); }
        else if (sscanf(line, "news_off=%d", &v) == 1) { g_news_off = v; bv_log("config: news_off=%d (K2: home NEWS button hidden)", v); }
        else if (sscanf(line, "trophy_rows=%d", &v) == 1) { g_trophy_rows = v; bv_log("config: trophy_rows=%d (H5 experiment: trophy road rows near the reached index only)", v); }
        else if (sscanf(line, "scid_off=%d", &v) == 1) { g_scid_off = v; bv_log("config: scid_off=%d (K1: Supercell ID buttons hidden)", v); }
        else if (sscanf(line, "settings_clean=%d", &v) == 1) { extern int bv_settings_texts; g_settings_clean = v; bv_settings_texts = v; bv_log("config: settings_clean=%d (K3: settings without SCID / social / privacy / help)", v); }
        else if (sscanf(line, "play_event_map=%d", &v) == 1) { g_play_event_map = v; bv_log("config: play_event_map=%d (experiment: Play uses the selected event map)", v); }
        else if (sscanf(line, "battle_bots=%d", &v) == 1) g_battle_bots = v;
        else if (sscanf(line, "fragcoord_fix=%d", &v) == 1) bv_fragcoord_fix = v;
        else if (sscanf(line, "shot_frame=%d", &v) == 1) { if (bv_shot_n < 4) bv_shot_frames[bv_shot_n++] = v; }
        else if (sscanf(line, "touch=%d", &v) == 1) bv_touch_enabled = v;
        else if (!strncmp(line, "give_skin=", 10) && g_opt.n_give_skin < 4) { snprintf(g_opt.give_skin[g_opt.n_give_skin], 32, "%s", line + 10); bv_log("config: give_skin=%s", g_opt.give_skin[g_opt.n_give_skin]); g_opt.n_give_skin++; }
        else if (sscanf(line, "season_timer=%d", &v) == 1) { g_opt.season_timer = v; bv_log("config: season_timer=%d (D4: 70-day season timer)", v); }
        else if (sscanf(line, "season_days=%d", &v) == 1) g_opt.season_days = v;
        else if (sscanf(line, "quests=%d", &v) == 1) { g_opt.quests_on = v; bv_log("config: quests=%d (D3 labelling experiment: 4 real quests with markers)", v); }
        else if (sscanf(line, "profile_gems=%d", &v) == 1) { g_opt.profile_gems = v; bv_log("config: profile_gems=%d", v); }
        else if (sscanf(line, "vpad=%d", &v) == 1) { bv_vpad = v; bv_log("config: vpad=%d (J1 experiment: Vita sticks / buttons -> touches)", v); }
        else if (sscanf(line, "vpad_move=%d,%d", &v, &v2) == 2) { bv_vpad_cfg.move_x = v; bv_vpad_cfg.move_y = v2; }
        else if (sscanf(line, "vpad_attack=%d,%d", &v, &v2) == 2) { bv_vpad_cfg.attack_x = v; bv_vpad_cfg.attack_y = v2; }
        else if (sscanf(line, "vpad_super=%d,%d", &v, &v2) == 2) { bv_vpad_cfg.super_x = v; bv_vpad_cfg.super_y = v2; }
        else if (sscanf(line, "vpad_gadget=%d,%d", &v, &v2) == 2) { bv_vpad_cfg.gadget_x = v; bv_vpad_cfg.gadget_y = v2; }
        else if (sscanf(line, "vpad_radius=%d", &v) == 1) bv_vpad_cfg.radius = v;
        else if (sscanf(line, "vpad_aim_radius=%d", &v) == 1) bv_vpad_cfg.aim_radius = v;
        else if (sscanf(line, "vpad_rear=%d", &v) == 1) { bv_log("config: vpad_rear=%d IGNORED (update 144: rear touch shooting removed)", v); }
        else if (sscanf(line, "vpad_play=%d,%d", &v, &v2) == 2) { bv_vpad_cfg.play_x = v; bv_vpad_cfg.play_y = v2; }
        else if (sscanf(line, "vpad_dead=%d", &v) == 1) bv_vpad_cfg.dead = v;
        else if (sscanf(line, "buttons=%d", &v) == 1) bv_buttons_enabled = v;
        else if (sscanf(line, "back_exits=%d", &v) == 1) bv_back_exits = v;
        else if (!strncmp(line, "back=", 5)) bv_back_cross = !strncmp(line + 5, "cross", 5);
    }
    free(txt);
    bv_log("config: trace=%d max_ctor=%d skip_ctors=%d bringup=%d run_seconds=%d gles=%d gltrace_frame=%d fbo_flip_from=%d screenshots=%d fragcoord_fix=%d fmod_output=%d cores=%d swfcache=%d etc1_native=%d trace_getters=%d tex16=%d game_stack_kb=%d depth_ext=%d", bv_trace_level, g_max_ctor, g_skip_ctors,
           g_bringup, g_run_seconds, bv_gles_major, bv_gltrace_frame, bv_fbo_flip_from, bv_shot_n, bv_fragcoord_fix, bv_fmod_output, bv_thread_cores, bv_swfcache,
           bv_etc1_native, bv_gtrace_enabled, bv_tex16_kb, bv_game_stack_kb, bv_depth_ext);
    bv_log("config: unlock_all=%d trophies=%d account_trophies=%d power_level=%d home_flags=%d brawl_pass=%d pass_season=%d pass_tokens=%d pass_premium=%d "
           "shop_disabled=%d config_keys=%d (offline home data)", g_opt.unlock_all, g_opt.trophies, g_opt.account_trophies, g_opt.power_level, g_opt.home_flags,
           g_opt.brawl_pass, g_opt.pass_season, g_opt.pass_tokens, g_opt.pass_premium, g_opt.shop_disabled, g_opt.n_extra_keys);
    for (int i = 0; i < g_opt.n_extra_keys; i++) bv_log("config:   switch %d = %d", g_opt.extra_key[i][0], g_opt.extra_key[i][1]);
    if (g_opt.tutorial_steps != 20) bv_log("config: tutorial_steps=%d (experiment: the 0..N-1 list of the home data)", g_opt.tutorial_steps);
    bv_log("config: gems=%d coins=%d shop_offers=%d offer_cur=%d owned_skins=%d (update 56/61)", g_opt.gems, g_opt.coins, g_opt.shop_offers, g_opt.offer_cur, g_opt.owned_skins);
    if (g_opt.offer_test)
        bv_log("config: offer_test=%d (quest list entries, home +0x100), types (+0x30) %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d", g_opt.offer_test,
               g_opt.offer_type[0], g_opt.offer_type[1], g_opt.offer_type[2], g_opt.offer_type[3], g_opt.offer_type[4], g_opt.offer_type[5], g_opt.offer_type[6],
               g_opt.offer_type[7], g_opt.offer_type[8], g_opt.offer_type[9], g_opt.offer_type[10], g_opt.offer_type[11], g_opt.offer_type[12],
               g_opt.offer_type[13], g_opt.offer_type[14], g_opt.offer_type[15]);
    bv_log("config: offline=%d patches=%d resend_home=%d brawler_row=%d map_row=%d events=%d touch=%d buttons=%d back=%s back_exits=%d", g_opt.enabled, g_opt.patches,
           g_opt.resend_home, g_opt.brawler_row, g_opt.map_row, g_opt.events, bv_touch_enabled, bv_buttons_enabled, bv_back_cross ? "cross" : "circle",
           bv_back_exits);
}

/* libg's writable segment (.data, .got, .bss: the state the game's start-up code reads) as it is right before nOnCreate. A crash dump only holds the pages near
 * the registers; with this file the routines that fail can be replayed on the PC with exactly the values the console had. */
static void write_snapshot(void) {
    if (!g_snapshot || !g_libg.host) return;
    uint32_t off = g_libg.data_off, n = g_libg.span - g_libg.data_off;
    const char *path = BV_DATA_DIR "/libg_data_before_nOnCreate.bin";
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        bv_log("snapshot: cannot create %s (0x%08x)", path, (unsigned)fd);
        return;
    }
    uint32_t done = 0;
    while (done < n) {
        uint32_t chunk = n - done > (1u << 20) ? (1u << 20) : n - done;
        int w = sceIoWrite(fd, g_libg.host + off + done, chunk);
        if (w <= 0) break;
        done += (uint32_t)w;
    }
    sceIoClose(fd);
    bv_log("snapshot: libg data segment, vaddr 0x%x..0x%x (run-time 0x%08x), %u of %u bytes written to %s", off, off + n, g_libg.load + off, (unsigned)done,
           (unsigned)n, path);
}

static void report_crash_dumps(void) {
    SceUID d = sceIoDopen("ux0:data");
    if (d < 0) return;
    SceIoDirent e;
    memset(&e, 0, sizeof e);
    int found = 0;
    while (sceIoDread(d, &e) > 0) {
        size_t l = strlen(e.d_name);
        if (l > 8 && !strcmp(e.d_name + l - 8, ".psp2dmp")) {
            bv_log("crash dump from an earlier run: ux0:data/%s (%u bytes)", e.d_name, (unsigned)e.d_stat.st_size);
            found++;
        }
        memset(&e, 0, sizeof e);
    }
    sceIoDclose(d);
    if (found) bv_log("  (send the newest one together with this log)");
}

static int load_module(so_module *m, const char *name, uint32_t expected_size, uint32_t id, so_ops *ops) {
    char path[160];
    snprintf(path, sizeof path, LIB_DIR "/%s", name);
    size_t size;
    uint8_t *file = read_file(path, &size);
    if (!file) return -1;
    bv_log("%s: %u bytes read%s", name, (unsigned)size, size == expected_size ? "" : "  WARNING: size differs from Build B");
    int r = so_load(m, name, file, size, ops, &g_unres, id);
    free(file);
    if (r) {
        bv_log("%s: so_load failed (%d)", name, r);
        return r;
    }
    bv_register_module(m);
    return 0;
}

static int run_ctors(so_module *m) {
    for (int i = 0;; i++) {
        uint32_t a;
        int r = so_ctor(m, i, &a);
        if (r == 0) break;
        if (r == 2) continue;
        if (g_max_ctor >= 0 && m == &g_libg && i > g_max_ctor) {
            bv_log("%s: stopping before constructor %d (max_ctor)", m->name, i);
            return 1;
        }
        bv_log("%s: ctor %d at +0x%x (%s)", m->name, i, (a & ~1u) - m->load, (a & 1) ? "Thumb" : "ARM");
        ((void (*)(void))(uintptr_t)a)();
    }
    return 0;
}

static void vs_sleep(unsigned us) { sceKernelDelayThread(us); }
static void vs_lock(void) { sceKernelLockMutex(g_vs_mutex, 1, NULL); }
static void vs_unlock(void) { sceKernelUnlockMutex(g_vs_mutex, 1); }
static void srv_log(void *ctx, const char *fmt, ...) {
    (void)ctx;
    va_list ap;
    va_start(ap, fmt);
    bv_logv(fmt, ap);
    va_end(ap);
}
static void on_connect(void *ctx, uint16_t port) {
    (void)ctx;
    bvb_hook_on_connect(port);
}

static int worker(SceSize args, void *argp) {
    (void)args; (void)argp;
    bv_log("Brawl Vita %s (built %s %s)", BV_BUILD, __DATE__, __TIME__);
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    SceKernelFreeMemorySizeInfo fm;
    fm.size = sizeof fm;
    if (sceKernelGetFreeMemorySize(&fm) >= 0)
        bv_log("free memory: user %u KB, cdram %u KB, phycont %u KB", fm.size_user / 1024, fm.size_cdram / 1024, fm.size_phycont / 1024);
    report_crash_dumps();
    bvb_hook_default_options(&g_opt);
    read_config();

    bv_shims_init();
    so_ops ops;
    bv_mem_make_ops(&ops, (uint32_t)(uintptr_t)bv_unresolved_entry, resolve_cb);
    bv_make_game_dirs();

    if (load_module(&g_fmod, "libfmod.so", EXPECTED_LIBFMOD_SIZE, 0, &ops)) return 1;
    if (load_module(&g_libg, "libg.so", EXPECTED_LIBG_SIZE, 1, &ops)) return 1;
    bv_log("modules loaded: libfmod at 0x%08x, libg at 0x%08x; %u shim-less imports go to logging thunks", g_fmod.load, g_libg.load, g_unres.n);

    if (g_rawsys) {
        /* `svc #0` inside the game's own code would enter the console kernel with Linux register conventions (REVIEW_m5.md, applied to Build B) */
        static rs_patch plan[RS_MAX];
        rs_stats rs;
        int np = bv_rawsys_plan_b(g_libg.host, g_libg.text_size, plan, RS_MAX, &rs);
        bv_log("rawsys: %d `svc #0` words in libg, %d planned (%d to 0, %d to -ENOSYS)", rs.svc_words, np, rs.to_zero, rs.to_enosys);
        bv_rawsys_apply(&g_libg, plan, np);
    }
    if (g_antitamper) {
        static at_patch at[AT_MAX];
        int mismatch = 0;
        int na = bv_antitamper_plan_b(g_libg.host, g_libg.text_size, at, AT_MAX, &mismatch);
        bv_log("antitamper: %d patches of the game's self-check planned (%d table entries do not match this libg)", na, mismatch);
        int done = bv_antitamper_apply(&g_libg, at, na);
        int planned = na;
        if (g_antitamper >= 2) {
            static at_patch rc[AT_MAX];
            int sites = 0;
            int nr = bv_antitamper_plan_reactions_b(g_libg.host, g_libg.text_size, rc, AT_MAX, &sites);
            bv_log("antitamper: %d random-call blocks found by shape, %d patches planned", sites, nr);
            if (nr != 2 * sites) bv_log("antitamper: ERROR: only %d of %d random-call blocks fit the patch table (AT_MAX)", nr / 2, sites);
            done += bv_antitamper_apply(&g_libg, rc, nr);
            planned += nr;
            /* verification on the live image: the shape must be gone. A random call that is left is the crash of 2026-10-25 waiting to happen, so it is listed by address. */
            uint32_t live[AT_MAX];
            int nl = bv_antitamper_find_reactions_b(g_libg.host, g_libg.text_size, live, AT_MAX);
            bv_log("antitamper: verify: %d random-call site(s) still live after patching%s", nl, nl ? " (!)" : "");
            for (int i = 0; i < nl && i < AT_MAX; i++) bv_log("antitamper:   LIVE random call at libg+0x%06x", (unsigned)live[i]);
            /* the NULL-unsafe accessor of the crash of 2026-10-27 (see gamefix.h) */
            at_patch pg[8];
            int ncave = 0;
            char pg_why[160] = "";
            /* update 16: the helper goes into a cleared code block of our own at the top of libg's slot, not into the block tail (bytes the loader never writes);
             * the tail is only the fallback. at_patch.off is relative to libg's base, and on the console host == load, so host + off reaches the near block. */
            int np = 0;
            const char *pg_where = "block tail";
            uint32_t near_addr = 0;
            uint8_t *near = (uint32_t)(uintptr_t)g_libg.host == g_libg.load ? bv_mem_alloc_near(g_libg.load, g_libg.span, &near_addr) : NULL;
            if (near) {
                np = bv_gamefix_plan_pages_cave(g_libg.host, g_libg.text_size, near_addr - g_libg.load, near, pg, 8, &ncave, pg_why, sizeof pg_why);
                if (np) pg_where = "near code block";
                else bv_log("gamefix: page-change helper cannot use the near code block (%s), trying the block tail", pg_why);
            } else {
                bv_log("gamefix: no near code block for the page-change helper, trying the block tail");
            }
            if (np == 0) np = bv_gamefix_plan_pages_ex(g_libg.host, g_libg.text_size, pg, 8, &ncave, pg_why, sizeof pg_why);
            at_patch gf[AT_MAX];
            int gsites = 0;
            int ng = bv_gamefix_plan(g_libg.host, g_libg.text_size, gf, AT_MAX, &gsites);
            bv_log("gamefix: %d NULL-guardable call sites of the accessor, %d patches planned", gsites, ng);
            if (ng != 2 * gsites) bv_log("gamefix: ERROR: only %d of %d call sites fit the patch table", ng / 2, gsites);
            done += bv_antitamper_apply(&g_libg, gf, ng);
            planned += ng;
            /* the game's built-in getifaddrs needs a netlink socket, which does not exist here: it becomes "no interfaces" (crash of 2026-11-04) */
            at_patch ifp[4];
            int ifc = 0;
            int nif = bv_gamefix_plan_ifaddrs(g_libg.host, g_libg.text_size, ifp, 4, &ifc);
            if (nif == 0) {
                bv_log("gamefix: the game's netlink getifaddrs was not found, not patched");
            } else {
                planned += nif;
                done += bv_antitamper_apply(&g_libg, ifp, nif);
                bv_log("gamefix: getifaddrs at libg+0x%x (%d callers) now reports an empty interface list", (unsigned)ifp[0].off, ifc);
            }
            /* update 48: local bot battles (gamefix.h bv_gamefix_plan_localbattle): Play builds the battle locally instead of asking a server */
            if (g_local_battle) {
                at_patch lbp[4];
                uint32_t lbf = 0;
                char lbwhy[160] = "";
                int nlb = bv_gamefix_plan_localbattle(g_libg.host, g_libg.text_size, g_battle_bots, lbp, 4, &lbf, lbwhy, sizeof lbwhy);
                if (nlb == 0) {
                    bv_log("gamefix: local battle NOT patched: %s", lbwhy);
                } else {
                    planned += nlb;
                    int dlb = bv_antitamper_apply(&g_libg, lbp, nlb);
                    done += dlb;
                    bv_log("gamefix: local battle: start-game function libg+0x%x, %d of %d patches written (match kind -> 3%s)%s%s", (unsigned)lbf, dlb, nlb,
                           nlb > 1 ? ", with bots" : "", lbwhy[0] ? "; " : "", lbwhy);
                }
            } else {
                bv_log("gamefix: local battle disabled by configuration (local_battle=0): Play asks the offline server for a match");
            }
            /* cycle 6 (ISSUES A5/E1) experiment team_play_solo=1: the Play handler's "team present?" branch (libg+0x6e2fb0 beq -> b) always takes the solo
             * start-game path (= local bot match) instead of sending 14355 'ready' */
            if (g_team_play_solo && g_local_battle) {
                at_patch tp = {0x6e2fb0u, 0x0a00003eu, 0xea00003eu, "Play: solo path also in a team"};
                uint32_t cur = 0;
                memcpy(&cur, g_libg.host + tp.off, 4);
                int dtp = cur == tp.old_word ? bv_antitamper_apply(&g_libg, &tp, 1) : 0;   /* only the expected instruction is replaced */
                planned += 1;
                done += dtp;
                bv_log("gamefix: team_play_solo: libg+0x6e2fb0 %s", dtp == 1 ? "patched (Play inside a team starts the local bot match)" : "NOT patched (word differs)");
            }
            bv_battle_kind_addr = g_libg.load + 0xc86a10u;   /* update 112 (J3): battle kind global (battle_end_agent.md) */
            /* update 106 experiment pass_button_rewards=1 (D7): libg+0x6e3330 bne -> nop: the home pass button opens the REWARDS tab */
            if (g_pass_button_rewards) {
                /* mode 1 (update 106): nop = always 0x257444 -> user: still QUESTS (so 0x257444 = quests tab); mode 2 (update 107): b = never 0x257444 */
                at_patch pb = {0x6e3330u, 0x1a00005du, g_pass_button_rewards >= 2 ? 0xea00005du : 0xe320f000u, "pass button: rewards tab"};
                uint32_t cur = 0;
                memcpy(&cur, g_libg.host + pb.off, 4);
                int dpb = cur == pb.old_word ? bv_antitamper_apply(&g_libg, &pb, 1) : 0;
                planned += 1;
                done += dpb;
                bv_log("gamefix: pass_button_rewards: libg+0x6e3330 %s", dpb == 1 ? "patched (pass button -> rewards tab)" : "NOT patched (word differs)");
            }
            /* update 97 experiment play_event_map=1: the solo Play path's start-game call (libg+0x6e30e4) goes through bv_start_game_wrap (team's selected map) */
            if (g_play_event_map && g_local_battle && near) {
                uint32_t vo = near_addr - g_libg.load + 0xF000u;   /* last 4 KB of the near block: update 97 veneers */
                at_patch vp[2] = {{vo, 0, 0xe51ff004u, "veneer: ldr pc,[pc,#-4]"}, {vo + 4, 0, (uint32_t)(uintptr_t)bv_start_game_wrap, "veneer: -> bv_start_game_wrap"}};
                uint32_t cur = 0;
                memcpy(&cur, g_libg.host + 0x6e30e4u, 4);
                int dv = cur == 0xebfe7374u ? bv_antitamper_apply(&g_libg, vp, 2) : 0;
                int dc = 0;
                if (dv == 2) {
                    int32_t rel = (int32_t)(vo - (0x6e30e4u + 8u)) >> 2;
                    at_patch cp = {0x6e30e4u, 0xebfe7374u, 0xeb000000u | ((uint32_t)rel & 0xffffffu), "Play: start-game via bv_start_game_wrap"};
                    dc = bv_antitamper_apply(&g_libg, &cp, 1);
                }
                g_start_game_orig = g_libg.load + 0x67febcu;
                g_team_getter = g_libg.load + 0x7a284cu;
                g_home_getter = g_libg.load + 0x6641ccu;
                bv_log("gamefix: play_event_map: %s", dc == 1 ? "Play starts the battle on the team's selected map (libg+0x6e30e4 -> wrapper)" : "NOT patched (word differs or veneer failed)");
            }
            /* Phase 3 (world_match=1): the 5 addPlayer calls of the bot filler 0x58800c -> bv_add_player_wrap (veneer at near block +0xF200) */
            { extern int bv_world_match, bv_world_on; if (bv_world_on && bv_world_match && near) {
                static const uint32_t sites[5] = {0x588190u, 0x588340u, 0x5884ecu, 0x58877cu, 0x588854u};
                uint32_t vo = near_addr - g_libg.load + 0xF200u;
                at_patch vp[2] = {{vo, 0, 0xe51ff004u, "veneer: ldr pc,[pc,#-4]"}, {vo + 4, 0, (uint32_t)(uintptr_t)bv_add_player_wrap, "veneer: -> bv_add_player_wrap"}};
                int ok = 0, dv = 0;
                for (int i = 0; i < 5; i++) {
                    uint32_t cur = 0, want = 0xeb000000u | (((0x2c49d4u - (sites[i] + 8u)) >> 2) & 0xffffffu);
                    memcpy(&cur, g_libg.host + sites[i], 4);
                    ok += cur == want;
                }
                if (ok == 5) dv = bv_antitamper_apply(&g_libg, vp, 2);
                int dc = 0;
                for (int i = 0; dv == 2 && i < 5; i++) {
                    uint32_t want = 0xeb000000u | (((0x2c49d4u - (sites[i] + 8u)) >> 2) & 0xffffffu);
                    int32_t rel = (int32_t)(vo - (sites[i] + 8u)) >> 2;
                    at_patch cp = {sites[i], want, 0xeb000000u | ((uint32_t)rel & 0xffffffu), "bot filler: addPlayer via bv_add_player_wrap"};
                    dc += bv_antitamper_apply(&g_libg, &cp, 1);
                }
                g_add_player_orig = g_libg.load + 0x2c49d4u;
                bv_log("gamefix: world_match: %d of 5 bot creation calls wrapped (%d words matched)", dc, ok);
            } }
            /* update 187 (user: tapping the home profile does nothing): the player card handler (0x6e3198) and other features first call 0x2e5674,
             * which refuses (toast TID_TEAM_FEATURE_BLOCKED_WHILE_READY) while the local team member's ready Bool (+1, decoder S_51d288) is set.
             * Play marks the member ready locally and the offline team does not always clear it; Play goes straight into a bot match here, so the
             * guard has no use: ready_unblock=1 makes it return 0 (`mov r0,#0; bx lr` over its first two words, e92d4c10 e28db008). */
            if (g_ready_unblock) {
                uint32_t w0 = 0, w1 = 0;
                memcpy(&w0, g_libg.host + 0x2e5674u, 4);
                memcpy(&w1, g_libg.host + 0x2e5678u, 4);
                at_patch rp[2] = {{0x2e5674u, 0xe92d4c10u, 0xe3a00000u, "ready guard: mov r0,#0"}, {0x2e5678u, 0xe28db008u, 0xe12fff1eu, "ready guard: bx lr"}};
                int dr = (w0 == 0xe92d4c10u && w1 == 0xe28db008u) ? bv_antitamper_apply(&g_libg, rp, 2) : 0;
                bv_log("gamefix: ready_unblock: libg+0x2e5674 %s", dr == 2 ? "returns 0 (features open while ready)" : "NOT patched (words differ)");
            }
            /* update 124 (E5): new_event_label=0 -> the event card label stays empty when there is no upcoming event */
            if (!g_new_event_label) {
                at_patch ep = {0x5c3240u, 0xba00001fu, 0xba000025u, "event card: no NEW EVENT AVAILABLE label"};
                int de = bv_antitamper_apply(&g_libg, &ep, 1);
                bv_log("gamefix: new_event_label=0: libg+0x5c3240 %s", de == 1 ? "patched (empty label without an upcoming event)" : "NOT patched (word differs)");
            }
            /* update 118 (K1): scid_off=1 -> libg+0x2392a4 'Supercell ID available' returns 0 */
            if (g_scid_off) {
                at_patch sp2[2] = {{0x2392a4u, 0xe92d4800u, 0xe3a00000u, "scid: mov r0,#0"}, {0x2392a8u, 0xe1a0b00du, 0xe12fff1eu, "scid: bx lr"}};
                int ds = bv_antitamper_apply(&g_libg, sp2, 2);
                bv_log("gamefix: scid_off: libg+0x2392a4 %s", ds == 2 ? "returns 0 (Supercell ID hidden)" : "NOT patched (words differ)");
            }
            /* update 139 (K2): the home screen looks up button_news (libg+0x5c2ea0, 0x4f7af8) and calls a highlight effect 0x640208(button, on, 0) on it;
             * that call goes to a veneer that hides the button instead (visible +4 = 0, +0x34 = 0, like the game hides Kakao / SCID buttons). */
            if (g_news_off && near) {
                uint32_t vo = near_addr - g_libg.load + 0xF030u;
                /* update 146 (user T18, run 163: the button came back): the veneer also stores the button pointer in bv_news_btn; the swap hook
                 * (shims/shim_egl.c) hides it again on every home-screen frame */
                extern volatile uint32_t bv_news_btn;
                at_patch vp[8] = {{vo, 0, 0xe3500000u, "veneer: cmp r0,#0"}, {vo + 4, 0, 0x13a01000u, "veneer: movne r1,#0"},
                                  {vo + 8, 0, 0x15c01004u, "veneer: strbne r1,[r0,#4]"}, {vo + 12, 0, 0x15c01034u, "veneer: strbne r1,[r0,#0x34]"},
                                  {vo + 16, 0, 0xe59fc004u, "veneer: ldr ip,[pc,#4]"}, {vo + 20, 0, 0x158c0000u, "veneer: strne r0,[ip]"},
                                  {vo + 24, 0, 0xe12fff1eu, "veneer: bx lr"}, {vo + 28, 0, (uint32_t)(uintptr_t)&bv_news_btn, "veneer: &bv_news_btn"}};
                uint32_t cur = 0;
                memcpy(&cur, g_libg.host + 0x5c2eacu, 4);
                int dv = cur == 0xeb01f4d5u ? bv_antitamper_apply(&g_libg, vp, 8) : 0;
                int dc = 0;
                if (dv == 8) {
                    int32_t rel = (int32_t)(vo - (0x5c2eacu + 8u)) >> 2;
                    at_patch cp = {0x5c2eacu, 0xeb01f4d5u, 0xeb000000u | ((uint32_t)rel & 0xffffffu), "home: hide button_news"};
                    dc = bv_antitamper_apply(&g_libg, &cp, 1);
                }
                bv_log("gamefix: news_off: %s", dc == 1 ? "home NEWS button hidden (libg+0x5c2eac -> veneer)" : "NOT patched (word differs or veneer failed)");
                /* update 148 (user T18, run 172: still visible; the 0x5c2eac path never ran): the home builder creates the NEWS button at libg+0x5c455c and
                 * keeps it at home+0x1d4, then calls libg+0x4382d4 on it (0x5c4574 blne). That call goes through a veneer that stores the button in
                 * bv_news_btn first; the swap hook hides it on every frame. */
                {   /* update 149: the 0x5c4574 call is skipped (home+0x1d4 stays NULL, run 173) -> also capture the HOME object at the always-run
                     * `bl 0x44cff8` (libg+0x5c455c, r0 = home) and look button_news up by name from the swap hook (shims/shim_egl.c news_rehide) */
                    extern volatile uint32_t bv_home_obj, bv_fn_str_ctor, bv_fn_str_dtor, bv_fn_child;
                    bv_fn_str_ctor = g_libg.load + 0x4c793cu;   /* String(dst, const char *) */
                    bv_fn_str_dtor = g_libg.load + 0x54fe64u;   /* ~String */
                    bv_fn_child = g_libg.load + 0x4f7af8u;      /* child clip by name (container, String *) */
                    uint32_t v3 = near_addr - g_libg.load + 0xF070u, site3 = 0x5c455cu, fn3 = 0x44cff8u;
                    uint32_t want3 = 0xeb000000u | ((uint32_t)((int32_t)(fn3 - (site3 + 8u)) >> 2) & 0xffffffu), cur3 = 0;
                    memcpy(&cur3, g_libg.host + site3, 4);
                    at_patch w[6] = {{v3, 0, 0xe59fc008u, "veneer: ldr ip,[pc,#8]"}, {v3 + 4, 0, 0xe58c0000u, "veneer: str r0,[ip]"},
                                     {v3 + 8, 0, 0xe59ff004u, "veneer: ldr pc,[pc,#4]"}, {v3 + 12, 0, 0xe1a00000u, "veneer: nop"},
                                     {v3 + 16, 0, (uint32_t)(uintptr_t)&bv_home_obj, "veneer: &bv_home_obj"}, {v3 + 20, 0, (uint32_t)(g_libg.load + fn3), "veneer: -> libg+0x44cff8"}};
                    int d3 = cur3 == want3 ? bv_antitamper_apply(&g_libg, w, 6) : 0, c3 = 0;
                    if (d3 == 6) {
                        at_patch cp3 = {site3, want3, 0xeb000000u | ((uint32_t)((int32_t)(v3 - (site3 + 8u)) >> 2) & 0xffffffu), "home: builder -> bv_home_obj"};
                        c3 = bv_antitamper_apply(&g_libg, &cp3, 1);
                    }
                    bv_log("gamefix: news_off: home object capture %s", c3 == 1 ? "installed (libg+0x5c455c -> veneer)" : "NOT patched (word differs)");
                    { extern volatile uint32_t bv_fn_popup_mgr; bv_fn_popup_mgr = g_libg.load + 0x683decu; }   /* update 184: popup manager getter (ui.md 4) */
                }
                {
                    uint32_t v2 = near_addr - g_libg.load + 0xF050u, site = 0x5c4574u, fn = 0x4382d4u;
                    uint32_t want = 0x1b000000u | ((uint32_t)((int32_t)(fn - (site + 8u)) >> 2) & 0xffffffu), cur2 = 0;
                    memcpy(&cur2, g_libg.host + site, 4);
                    at_patch v[6] = {{v2, 0, 0xe59fc008u, "veneer: ldr ip,[pc,#8]"}, {v2 + 4, 0, 0xe58c0000u, "veneer: str r0,[ip]"},
                                     {v2 + 8, 0, 0xe59ff004u, "veneer: ldr pc,[pc,#4]"}, {v2 + 12, 0, 0xe1a00000u, "veneer: nop"},
                                     {v2 + 16, 0, (uint32_t)(uintptr_t)&bv_news_btn, "veneer: &bv_news_btn"}, {v2 + 20, 0, (uint32_t)(g_libg.load + fn), "veneer: -> libg+0x4382d4"}};
                    int d2 = cur2 == want ? bv_antitamper_apply(&g_libg, v, 6) : 0, c2 = 0;
                    if (d2 == 6) {
                        at_patch cp2 = {site, want, 0x1b000000u | ((uint32_t)((int32_t)(v2 - (site + 8u)) >> 2) & 0xffffffu), "home: NEWS button -> bv_news_btn"};
                        c2 = bv_antitamper_apply(&g_libg, &cp2, 1);
                    }
                    bv_log("gamefix: news_off: home builder %s", c2 == 1 ? "hands the NEWS button to the swap hook (libg+0x5c4574 -> veneer)" : "NOT patched (word differs)");
                }
            }
            /* update 133 (H5): trophy road row filter (libg+0x24423c..0x244244 -> veneer at near block +0xF010) */
            if (g_prof_zones && near) {   /* update 165: profiler stubs -> veneers (near block +0xF0B0 push, +0xF0B8 pop) */
                uint32_t vp = near_addr - g_libg.load + 0xF0B0u, vq = vp + 8u, w1 = 0, w2 = 0;
                memcpy(&w1, g_libg.host + 0x2c02e0u, 4);
                memcpy(&w2, g_libg.host + 0x60d340u, 4);
                at_patch zv[4] = {{vp, 0, 0xe51ff004u, "zone veneer: ldr pc,[pc,#-4]"}, {vp + 4, 0, (uint32_t)(uintptr_t)bv_zone_push, "-> bv_zone_push"},
                                  {vq, 0, 0xe51ff004u, "zone veneer: ldr pc,[pc,#-4]"}, {vq + 4, 0, (uint32_t)(uintptr_t)bv_zone_pop, "-> bv_zone_pop"}};
                int dz = (w1 == 0xe12fff1eu && w2 == 0xe12fff1eu) ? bv_antitamper_apply(&g_libg, zv, 4) : 0, cz = 0;
                if (dz == 4) {
                    at_patch zp[2] = {{0x2c02e0u, 0xe12fff1eu, 0xea000000u | ((uint32_t)((int32_t)(vp - (0x2c02e0u + 8u)) >> 2) & 0xffffffu), "profiler push -> bv_zone_push"},
                                      {0x60d340u, 0xe12fff1eu, 0xea000000u | ((uint32_t)((int32_t)(vq - (0x60d340u + 8u)) >> 2) & 0xffffffu), "profiler pop -> bv_zone_pop"}};
                    cz = bv_antitamper_apply(&g_libg, zp, 2);
                }
                bv_log("gamefix: prof_zones: profiler stubs %s (libg+0x2c02e0 %08x, libg+0x60d340 %08x)", cz == 2 ? "hooked" : "NOT patched", (unsigned)w1, (unsigned)w2);
            }
            if (g_colette_min) {   /* update 167 (A13, libg map): percent-damage minimum 200 / 190 + 10 x level at 0x93858 / 0x93878 -> 500 / 492 + 10 x level */
                /* update 168b: the same minimum also sits in the second percent-damage path (fn 0x471468, 0x472f34 / 0x472f44: user saw 250 after 167) */
                at_patch cm[4] = {{0x93858u, 0xe3a010c8u, 0xe3a01f7du, "colette: percent damage minimum 200 -> 500"},
                                  {0x93878u, 0x13a010beu, 0x13a01f7bu, "colette: levelled minimum 190 -> 492 (+10 per level)"},
                                  {0x472f34u, 0xe3a010c8u, 0xe3a01f7du, "colette (path 2): minimum 200 -> 500"},
                                  {0x472f44u, 0x13a010beu, 0x13a01f7bu, "colette (path 2): levelled minimum 190 -> 492"}};
                int dc = bv_antitamper_apply(&g_libg, cm, 4);
                bv_log("gamefix: colette_min: %d of 4 patched", dc);
            }
            if (g_brawl_tv && near) {   /* update 163: spectator cave at libg+0x680370 (near block +0xF090) */
                uint32_t vo = near_addr - g_libg.load + 0xF090u, site = 0x680370u, w = 0;
                memcpy(&w, g_libg.host + site, 4);
                at_patch tv[6] = {{vo, 0, 0xe5857054u, "tv cave: str r7,[r5,#0x54]"}, {vo + 4, 0, 0xe59f0008u, "tv cave: ldr r0,[pc,#8]"},
                                  {vo + 8, 0, 0xe5900000u, "tv cave: ldr r0,[r0]"}, {vo + 12, 0, 0xe5850070u, "tv cave: str r0,[r5,#0x70]"},
                                  {vo + 16, 0, 0xe12fff1eu, "tv cave: bx lr"}, {vo + 20, 0, (uint32_t)(uintptr_t)&bv_tv_spectator, "tv cave: &bv_tv_spectator"}};
                int dt = w == 0xe5857054u ? bv_antitamper_apply(&g_libg, tv, 6) : 0, ct = 0;
                if (dt == 6) {
                    at_patch cp = {site, 0xe5857054u, 0xeb000000u | ((uint32_t)((int32_t)(vo - (site + 8u)) >> 2) & 0xffffffu), "StartLoading +0x70 from bv_tv_spectator"};
                    ct = bv_antitamper_apply(&g_libg, &cp, 1);
                }
                bv_log("gamefix: brawl_tv: spectator flag cave %s (libg+0x680370 %08x)", ct == 1 ? "installed" : "NOT patched", (unsigned)w);
            }
            if (g_bot_power) {   /* update 162 (decode_bot_power.md): the local player at its REAL level (the game's own branch: beq -> b at 0x3e5978) */
                uint32_t w = 0;
                memcpy(&w, g_libg.host + 0x3e5978u, 4);
                at_patch hp = {0x3e5978u, 0x0a000500u, 0xea000500u, "battle: your brawler at its real level"};
                int dh = w == 0x0a000500u ? bv_antitamper_apply(&g_libg, &hp, 1) : 0;
                bv_log("gamefix: bot_power: your brawler's real level %s (libg+0x3e5978 %08x)", dh == 1 ? "used" : "NOT patched", (unsigned)w);
            }
            if (g_stick_ui) {   /* update 170: wrap the battle screen update slot (va 0x8e3744 = 0x37f2f0) */
                uint32_t cell = 0, want = (uint32_t)(g_libg.load + 0x37f2f0u);
                memcpy(&cell, (void *)(uintptr_t)(g_libg.load + 0x8e3744u), 4);
                if (cell == want) {
                    g_bscreen_orig = cell;
                    at_patch bp = {0x8e3744u, want, (uint32_t)(uintptr_t)bv_bscreen_update, "battle screen update -> stick_ui"};
                    int db = bv_antitamper_apply(&g_libg, &bp, 1);
                    bv_log("gamefix: stick_ui: battle screen update %s", db == 1 ? "wrapped (libg+0x8e3744)" : "NOT patched");
                } else bv_log("gamefix: stick_ui: vtable cell holds %08x, expected %08x: not patched", (unsigned)cell, (unsigned)want);
            }
            if (g_trophy_cull) {   /* update 162 (A1): wrap the scroll draw slot */
                uint32_t cell = 0, want = (uint32_t)(g_libg.load + 0x2fcb08u);
                memcpy(&cell, (void *)(uintptr_t)(g_libg.load + 0x8e96e8u), 4);
                if (cell == want) {
                    g_scroll_draw_orig = cell;
                    at_patch sp = {0x8e96e8u, want, (uint32_t)(uintptr_t)bv_scroll_draw, "scroll draw -> trophy road cull"};
                    int ds = bv_antitamper_apply(&g_libg, &sp, 1);
                    bv_log("gamefix: trophy_cull: scroll draw slot %s", ds == 1 ? "wrapped (libg+0x8e96e8)" : "NOT patched");
                } else bv_log("gamefix: trophy_cull: vtable cell holds %08x, expected %08x: not patched", (unsigned)cell, (unsigned)want);
            }
            if ((g_trophy_rows > 0 || g_trophy_cull) && near) {
                uint32_t vo = near_addr - g_libg.load + 0xF010u;
                at_patch vp[7] = {{vo, 0, 0xe92d4010u, "veneer: push {r4,lr}"}, {vo + 4, 0, 0xe1a02008u, "veneer: mov r2,r8"},
                                  {vo + 8, 0, 0xe59fc008u, "veneer: ldr ip,[pc,#8]"}, {vo + 12, 0, 0xe12fff3cu, "veneer: blx ip"},
                                  {vo + 16, 0, 0xe3500000u, "veneer: cmp r0,#0"}, {vo + 20, 0, 0xe8bd8010u, "veneer: pop {r4,pc}"},
                                  {vo + 24, 0, (uint32_t)(uintptr_t)bv_trophy_row_filter, "veneer: -> bv_trophy_row_filter"}};
                uint32_t w0 = 0, w1 = 0;
                memcpy(&w0, g_libg.host + 0x24423cu, 4);
                memcpy(&w1, g_libg.host + 0x244240u, 4);
                int dv = (w0 == 0xe5971120u && w1 == 0xe1500001u) ? bv_antitamper_apply(&g_libg, vp, 7) : 0;
                int dc = 0;
                if (dv == 7) {
                    int32_t rel = (int32_t)(vo - (0x244240u + 8u)) >> 2;
                    at_patch cp[2] = {{0x24423cu, 0xe5971120u, 0xe1a01007u, "trophy road: mov r1,r7 (popup)"},
                                      {0x244240u, 0xe1500001u, 0xeb000000u | ((uint32_t)rel & 0xffffffu), "trophy road: bl row filter"}};
                    dc = bv_antitamper_apply(&g_libg, cp, 2);
                }
                g_row_index_fn = g_libg.load + 0x43bcc4u;
                bv_log("gamefix: trophy_rows=%d: %s", g_trophy_rows, dc == 2 ? "trophy road builder filters rows (libg+0x244240 -> helper)" : "NOT patched (words differ or veneer failed)");
            }
            /* update 118 (K3): settings popup constructor call libg+0x122504 -> bv_settings_wrap (veneer at near block +0xF008) */
            if (g_settings_clean && near) {
                uint32_t vo = near_addr - g_libg.load + 0xF008u;
                at_patch vp[2] = {{vo, 0, 0xe51ff004u, "veneer: ldr pc,[pc,#-4]"}, {vo + 4, 0, (uint32_t)(uintptr_t)bv_settings_wrap, "veneer: -> bv_settings_wrap"}};
                uint32_t cur = 0;
                memcpy(&cur, g_libg.host + 0x122504u, 4);
                int dv = cur == 0xeb08140du ? bv_antitamper_apply(&g_libg, vp, 2) : 0;
                int dc = 0;
                if (dv == 2) {
                    int32_t rel = (int32_t)(vo - (0x122504u + 8u)) >> 2;
                    at_patch cp = {0x122504u, 0xeb08140du, 0xeb000000u | ((uint32_t)rel & 0xffffffu), "settings: constructor via bv_settings_wrap"};
                    dc = bv_antitamper_apply(&g_libg, &cp, 1);
                }
                g_settings_ctor = g_libg.load + 0x327540u;
                g_getclip = g_libg.load + 0x38ea94u;   /* update 119: the null-checked child find the constructor uses for button_google_connect (0x426da4 found 1 of 11 in run 135) */
                bv_log("gamefix: settings_clean: %s", dc == 1 ? "settings popup constructor (libg+0x122504) -> wrapper" : "NOT patched (word differs or veneer failed)");
            }
            /* the stage's page-change code with no previous page (crash of 2026-10-30): planned on the PRISTINE text, before the accessor patches change two words inside the site,
             * and installed helper-first, the redirect only if the helper was written completely */
            if (np == 0) {
                bv_log("gamefix: page-change site not found, not patched: %s", pg_why);
            } else {
                int dc = bv_antitamper_apply(&g_libg, pg, ncave);
                planned += np;
                done += dc;
                if (dc == ncave) {
                    done += bv_antitamper_apply(&g_libg, pg + ncave, np - ncave);
                    bv_log("gamefix: page-change guard installed (helper at libg+0x%x in the %s, redirect at libg+0x%x)", (unsigned)pg[0].off, pg_where, (unsigned)pg[ncave].off);
                } else {
                    bv_log("gamefix: ERROR: the page-change helper could not be written (%d of %d words): the redirect is NOT installed", dc, ncave);
                }
            }
            /* update 40, diagnostics (config trace_getters=1): every tiny field getter of libg goes through a counting stub in the near block (gtrace.h) */
            if (bv_gtrace_enabled && near) {
                static bv_gt_site sites[BV_GT_MAX];
                int ns = bv_gtrace_find(g_libg.host, g_libg.text_size, BV_GT_MAX_OFF, sites, BV_GT_MAX);
                const uint32_t stub0 = near_addr + 0x1000u; /* the page-change helper sits at the start of the block */
                int room = (int)((BV_NEAR_SIZE - 0x1000u) / (BV_GT_STUB_WORDS * 4));
                if (ns > room) ns = room;
                for (int i = 0; i < ns; i++) {
                    uint32_t st[BV_GT_STUB_WORDS];
                    bv_gtrace_stub(st, i, sites[i].old, (uint32_t)(uintptr_t)bv_gtrace_hit);
                    kuKernelCpuUnrestrictedMemcpy((void *)(uintptr_t)(stub0 + (uint32_t)i * BV_GT_STUB_WORDS * 4), st, sizeof st);
                }
                kuKernelFlushCaches((void *)(uintptr_t)stub0, (SceSize)ns * BV_GT_STUB_WORDS * 4);
                int ok = 0, skipped = 0;
                for (int i = 0; i < ns; i++) {
                    uint32_t *p = (uint32_t *)(g_libg.host + sites[i].off);
                    if (*p != sites[i].old) { skipped++; continue; } /* old word check: another patch changed it */
                    uint32_t b = bv_gtrace_branch(g_libg.load + sites[i].off, stub0 + (uint32_t)i * BV_GT_STUB_WORDS * 4);
                    kuKernelCpuUnrestrictedMemcpy(p, &b, 4);
                    kuKernelFlushCaches(p, 4);
                    if (*p == b) ok++;
                }
                bv_gtrace_set_sites(sites, ns);
                bv_gtrace_set_module(g_libg.load, g_libg.text_size);
                bv_log("gtrace: %d field getters (imm <= 0x%x) found, %d redirected to counting stubs at 0x%08x, %d skipped (word changed)", ns,
                       (unsigned)BV_GT_MAX_OFF, ok, (unsigned)stub0, skipped);
            } else if (bv_gtrace_enabled) {
                bv_log("gtrace: no near code block: getter tracing NOT installed");
            }
            /* update 47, diagnostics (config probes=ADDR,...): code-path probes, stubs in the last 4 KB of the near block (gtrace.h) */
            if (g_probe_n && near) {
                const uint32_t pstub0 = near_addr + BV_NEAR_SIZE - 0x1000u;
                int okp = 0;
                for (int i = 0; i < g_probe_n && i < BV_PROBE_MAX; i++) {
                    uint32_t off = g_probe_off[i];
                    uint32_t *p = (uint32_t *)(g_libg.host + off);
                    if (off + 4 > g_libg.text_size || !bv_probe_ok(*p)) {
                        bv_log("probe libg+0x%x: instruction %08x cannot be probed (pc-relative, branch or outside the code): skipped", (unsigned)off,
                               off + 4 <= g_libg.text_size ? (unsigned)*p : 0u);
                        g_probe_off[i] = 0;
                        continue;
                    }
                    uint32_t sa = pstub0 + (uint32_t)i * BV_PROBE_WORDS * 4;
                    uint32_t st[BV_PROBE_WORDS];
                    bv_probe_stub(st, i, *p, (uint32_t)(uintptr_t)bv_probe_hit, sa, g_libg.load + off + 4);
                    kuKernelCpuUnrestrictedMemcpy((void *)(uintptr_t)sa, st, sizeof st);
                    kuKernelFlushCaches((void *)(uintptr_t)sa, sizeof st);
                    uint32_t b = bv_gtrace_branch(g_libg.load + off, sa);
                    kuKernelCpuUnrestrictedMemcpy(p, &b, 4);
                    kuKernelFlushCaches(p, 4);
                    if (*p == b) okp++;
                }
                bv_probe_set(g_probe_off, g_probe_n);
                bv_log("probes: %d of %d installed (stubs at 0x%08x)", okp, g_probe_n, (unsigned)pstub0);
            }
        }
        bv_log("antitamper: %d of %d patches written%s", done, planned, done == planned ? "" : " (!) some patches FAILED, see above");
    } else {
        bv_log("antitamper: disabled by configuration");
    }
    bv_gameapp_set_pre_oncreate(write_snapshot);
    bvb_hook_configure(&g_libg, &g_opt);
    bvb_hook_boot();
    g_vs_mutex = sceKernelCreateMutex("bv_vsock", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
    bv_server_ops sops = {NULL, on_connect, srv_log};
    bv_server_init(&g_server, &sops);
    vs_ops vops = {vs_sleep, vs_lock, vs_unlock};
    vs_init(&vops, &g_server);

    if (!g_skip_ctors) {
        bv_log("running libfmod constructors");
        run_ctors(&g_fmod);
        bv_log("running libg constructors");
        if (run_ctors(&g_libg)) return 1;
        bv_log("constructors done");
    } else {
        bv_log("constructors skipped (config)");
    }

    /* On Android System.loadLibrary("fmod") runs libfmod's JNI_OnLoad(vm), which looks up org/fmod/{AudioDevice,MediaCodec,FMOD} and keeps the JavaVM; FMOD's System::init
     * needs that VM and returns error 28 (FMOD_ERR_INTERNAL) without it (libfmod+0x9d7b4: movs r0,#0x1c when [global+0x80] is NULL). Nobody called it here, so every sound
     * failed with error 67 and the music object came back NULL: the crash at the end of loading, logs of 2026-10-27..29. */
    uint32_t fjni = so_find_symbol(&g_fmod, "JNI_OnLoad");
    if (fjni) {
        bv_log("calling JNI_OnLoad at libfmod+0x%x", fjni - g_fmod.load);
        int fver = ((int (*)(void *, void *))(uintptr_t)fjni)(bv_jni_vm(), NULL);
        bv_log("libfmod JNI_OnLoad returned 0x%x%s", fver, fver == -1 ? "  <== FAILED: FMOD gets no Java VM and System::init will return 28" : " (FMOD has its Java VM)");
    } else {
        bv_log("libfmod has no JNI_OnLoad");
    }

    uint32_t jni = so_find_symbol(&g_libg, "JNI_OnLoad");
    if (!jni) {
        bv_log("libg has no JNI_OnLoad");
        return 1;
    }
    bv_log("calling JNI_OnLoad at libg+0x%x", jni - g_libg.load);
    int ver = ((int (*)(void *, void *))(uintptr_t)jni)(bv_jni_vm(), NULL);
    bv_log("JNI_OnLoad returned 0x%x", ver);
    bv_log("natives registered by libg: %d", bv_jni_native_count());
    bv_log("MILESTONE m3 reached: libg loaded and JNI_OnLoad returned");
    if (g_bringup > 0) {
        int r = bv_gameapp_run(g_bringup, g_run_seconds);
        if (r) {
            bv_log("bring-up level %d stopped early (%d)", g_bringup, r);
            return 0;
        }
        bv_log("bring-up level %d finished without a crash", g_bringup);
    }
    return 0;
}

int main(void) {
    bv_log_init(BV_DATA_DIR "/log.txt");
    { extern void bv_early_notice(void); bv_early_notice(); }   /* update 170: notice from the first second */
    SceUID th = sceKernelCreateThread("bv_main", worker, 0x40, 8 * 1024 * 1024, 0, 0, NULL);
    if (th < 0) {
        bv_log("cannot create the worker thread (0x%08x)", (unsigned)th);
        return 1;
    }
    sceKernelStartThread(th, 0, NULL);
    int ret = 0;
    sceKernelWaitThreadEnd(th, &ret, NULL);
    bv_log("worker finished with %d", ret);
    sceKernelDelayThread(2 * 1000 * 1000);
    sceKernelExitProcess(0);
    return 0;
}
