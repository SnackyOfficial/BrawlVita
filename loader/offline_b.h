/* Offline path of Build B: what happens when the game "sends" a message (docs/offline_server_spec.md sections 2 and 8).
 *
 * The game's send-message function (libg+0x2392f4) is replaced; the replacement ends up in bvb_handle_send(). Nothing is sent on a
 * network: the message is classified by its type and the answers are injected into the client's own incoming path.
 *
 * The logic is portable (no console headers) and talks to the game through `bvb_game_if`, so the PC test (tests/host_offline_test.c)
 * drives it with a fake game. The binding to the real libg.so is loader/hook_b.c. UNVERIFIED on hardware.
 */
#ifndef BV_OFFLINE_B_H
#define BV_OFFLINE_B_H

#include <stddef.h>
#include <stdint.h>

#include "bmsg.h"
#include "botworld.h"

typedef struct bvb_game_if {
    void *ctx;
    int (*msg_type)(void *ctx, void *msg);                       /* vtable slot 5 of the outgoing message */
    /* vtable slot 2 of the outgoing message = encode(): serialises its fields into the message's stream. Until it ran, the body
     * (length at msg+24, bytes at msg+36) is empty. The message is not freed afterwards (the original mod does not either). */
    void (*msg_encode)(void *ctx, void *msg);
    int (*msg_payload)(void *ctx, void *msg, const uint8_t **p, size_t *len); /* body of an ENCODED outgoing message; 0 if none */
    /* Builds a message of `type` in the client's own format and hands it to the client: create + fields +4/+28/+36, vtable slot 3
     * (decode) and the push into the client's receive queue (see bvb_ring_push). */
    void (*deliver)(void *ctx, int type, int flag, const uint8_t *data, size_t len);
    /* First message only: take the hook out, drop the old connection object (M+212), put the hook back; later `login_done` sets M+208 = 5. */
    void (*login_prepare)(void *ctx);
    void (*login_done)(void *ctx);
    void (*after_login)(void *ctx);                              /* post-login code patch */
    int (*table_rows)(void *ctx, int table);
    void (*log)(void *ctx, const char *fmt, ...);
    /* optional: serialise the handler between game threads (a recursive lock; the handler can be re-entered by a reply it delivers) */
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
} bvb_game_if;

struct bv_passdata; /* loader/passdata.h */
struct bv_profile;  /* loader/profile.h */
typedef struct bvb_state {
    bmsg_cfg cfg;
    bmsg_game game;
    int selected_map;     /* the "selected map" kept for 24124 (default row 7) */
    int logged_in;
    int depth;            /* nesting of bvb_handle_send (a delivered reply can make the client send again) */
    int resend_home;      /* 1 = answer every later message with 24101 + 24124 + 24125 + 24124 like the original mod does; 0 (default) = 24125 only (+ 24124 for 14353/14363) */
    uint32_t count[16];   /* per known type, see bvb_slot() */
    uint32_t unknown;
    /* per-type counts of the types not in the known list (so each one gets its first bodies logged); a full table counts in `unknown` only */
    uint32_t cmd_logged;  /* 14102 bodies with commands logged so far (update 36) */
    uint32_t boxes_opened; /* box deliveries sent (update 50) */
    uint32_t keepalives;   /* 10108 seen (cycle 1: mode_push) */
    const struct bv_passdata *pass; /* update 66: Brawl Pass reward tables (NULL = not loaded) */
    struct bv_profile *profile;     /* cycle 4: saved progress (NULL = off) */
    void (*profile_save)(void);     /* called after every profile change */
    int day;                        /* days since 1970 (bought offers belong to a day) */
    int mode_pushed;
    int other_type[32];
    uint32_t other_count[32];
    /* Phase 3 (living world): the bot world (NULL = off, config world=0) */
    struct bw_world *world;
    void (*world_save)(void);       /* writes the world file */
    int world_region;               /* the player's region (local leaderboards), bw_region_index */
    uint32_t world_tick, world_saved;   /* last live simulation / save (unix time) */
    bw_pick plan[BW_SLOTS];         /* the bots planned for the current battle by name slot 1..9 (world_match=1) */
    int plan_on;
} bvb_state;

/* The game's queues are small 32-bit structs: mutex slot at q-4, then {uint32 array; int cap; int count; int pad; int tail}, i.e.
 * array @q+0, capacity @q+4, count @q+8, tail @q+16 (the client's receive queue is at manager+60, its send queue at manager+84).
 * Memory is read and written through callbacks so that the push is testable on a 64-bit PC. */
typedef struct bvb_mem {
    void *ctx;
    uint32_t (*rd32)(void *ctx, uint32_t addr);
    void (*wr32)(void *ctx, uint32_t addr, uint32_t value);
} bvb_mem;
/* Appends `msg` (an address) like the game's own producer: array[tail] = msg; tail = (tail + 1) % cap; count++.
 * The caller holds the queue's mutex. Returns 1 when pushed, 0 when the queue is invalid or full. */
int bvb_ring_push(const bvb_mem *m, uint32_t q, uint32_t msg);

void bvb_init(bvb_state *st, const bvb_game_if *gi);
/* Returns what the replaced function returns (always 0). */
int bvb_handle_send(bvb_state *st, const bvb_game_if *gi, void *msg);

#endif
