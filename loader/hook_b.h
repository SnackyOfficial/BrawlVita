/* Binding of the offline logic (offline_b.c) to the real libg.so of Build B: boot patches, the replacement of the send-message function
 * and the calls into the game (message creation, vtable slots, data tables). Offsets are libg virtual addresses of ARM code.
 * UNVERIFIED ON HARDWARE.
 */
#ifndef BV_HOOK_B_H
#define BV_HOOK_B_H

#include <stdint.h>

#include "elf_loader.h"

typedef struct bvb_options {
    int enabled;      /* config offline=1 (default) */
    int patches;      /* config patches=1: the two boot flag patches */
    int resend_home;  /* config resend_home=1 */
    int brawler_row;  /* config brawler_row */
    int map_row;      /* config map_row */
    int events;       /* config events (default 6 since update 33: one per mode) */
    int event_map[8]; /* config event_maps=a,b,...: locations rows for events 1..7 (event 0 = map_row); -1 = keep the default */
    int event_slot[8]; /* config event_slots=a,b,...: slot ids of events 0..7; 0 = keep the default (update 37) */
    int unlock_all;   /* config unlock_all (default 1 since update 34): every enabled brawler owned */
    int trophies;     /* config trophies (default 1000 since update 34): trophies of the selected brawler */
    int account_trophies; /* config account_trophies (default 1000, update 35): the account's trophies (trophy road, unlocks) */
    int power_level;  /* config power_level (default 8 = level 9, update 35) */
    int home_flags;   /* config home_flags (default 0, update 38: shop test; 4 = the mod's value) */
    int brawl_pass, pass_season, pass_tokens, pass_premium; /* config brawl_pass (default 1), pass_season (2), pass_tokens (0), pass_premium (1) */
    int shop_disabled; /* config shop_disabled (default 0 since update 42: config switch key 5) */
    int offer_test, offer_type[16]; /* config offer_test=N, offer_types=a,b,... (update 43/44, labelling experiment) */
    int tutorial_steps; /* config tutorial_steps=N (default 20, update 46 experiment) */
    int box_coin_type, box_unit_mode; /* config box_coin_type (7), box_unit (0) (update 50: box deliveries) */
    int gems, coins, shop_offers, offer_cur; /* config gems (1000), coins (5000), shop_offers (2), offer_cur (1) (update 56) */
    int avatar_tail[12], avatar_tail_set;
    int box_pp_random; /* config box_pp_random (0, update 63) */
    int offer_unit; /* config offer_unit (100, update 64) */
    int member_view; /* config member_view (0; cycle 6 experiment E2) */
    int profile_brawlers; /* config profile_brawlers (0; cycle 6 experiment G2) */
    int pass_new_types; /* config pass_new_types (0; cycle 6 experiment) */
    int shop_layout; /* config shop_layout (0; 1 = cycle 5 experiment: 1 special + 5 skins + 5 power points + freebie) */
    int profile_on, pass_claimed_bits; /* config profile (1 = saved progress in profile.txt), pass_claimed_bits (0, experiment) (cycle 4) */
    int pass_claim_reply; /* config pass_claim_reply (0, update 65 experiment) */
    int team_m10, team_m14, team_m18, team_m1c; /* config team_member=A,B,C,D (0,0,1,3) */
    int team_f0, team_f8; /* config team_fields=A,B (1,1) */
    int trophy_road_claimed; /* config trophy_road_claimed (0; update 91 experiment G) */
    char give_skin[4][32]; /* config give_skin=<Name> (cycle 7 G4), up to 4 */
    int n_give_skin;
    int profile_gems;      /* config profile_gems=N (-1 = keep; cycle 7 G3) */
    int quests_on;
    int map_rotation;      /* config map_rotation (update 109 E3) */
    int season_timer;      /* config season_timer (update 103 D4) */
    int season_days;       /* config season_days (70) */
    int home_on_14109;
    int profile_reply;  /* update 114 (G5) */
    int home_a4, home_a8; /* update 114 (E7) */
    int n_limits, limits[13]; /* update 115 (E7) */
    int tok_cfg[4]; /* update 116 (E7) */
    int play_tokens; /* update 117 (E7) */
    int upcoming_events; /* update 123 (E5) */
    int trophy_league; /* update 130 (D6) */
    int new_event_tokens; /* update 131 (E4) */
    int battle_end_labels; /* update 132 (A11) */
    int offer_title_slot; /* update 136 (B11) */
    int battle_log; /* update 140 (G7) */
    int battle_log_sd; /* update 168b */
    int battle_end_msg, battle_trophies; /* update 96 (A6), 0 */
    int battle_win, battle_draw, battle_lose; /* config home_on_14109 (0; update 89 experiment A) */
    int ack24125; /* config ack24125 (1; 0 = experiment, cycle 4 E1) */
    int mode_push_map, mode_push_after; /* config mode_push=ROW@N (off, cycle 1 experiment) */
    int res_markers, skin_offer_kind; /* config res_markers (0), skin_offer_kind (0) (update 62) */
    int owned_skins; /* config owned_skins (default 0 = only default skins, update 61) */ /* config avatar_tail=a,b,... (update 57) */
    int n_extra_keys, extra_key[8][2]; /* config config_keys=K:V,... (update 42) */
    int release_old;  /* config release_old=1: call vtable slot 2 on the connection object the first message replaces, like the original mod */
} bvb_options;

void bvb_hook_default_options(bvb_options *o);
void bvb_hook_configure(const so_module *libg, const bvb_options *o);
/* Right after libg is mapped: checks that this is the build the offsets belong to, applies the two flag patches. */
void bvb_hook_boot(void);
/* Called by the virtual sockets whenever the game connects; the first connect to the game server port installs the hook. */
void bvb_hook_on_connect(uint16_t port_host);
/* Counters for the periodic status line. */
uint32_t bvb_hook_messages(void);

#endif
