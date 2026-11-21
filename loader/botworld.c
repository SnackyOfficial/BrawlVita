/* Phase 3 (living world): the bot world. See botworld.h. Pure logic, host-tested (tests/host_botworld_test.c). */
#include "botworld.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- hashing (deterministic per bot / day / match, independent of call order) ---- */
static uint32_t mix(uint32_t h) {
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}
static uint32_t H(uint32_t seed, uint32_t a, uint32_t b, uint32_t c) {
    return mix(seed ^ mix(a * 0x9e3779b1u ^ mix(b * 0x85ebca77u ^ mix(c * 0xc2b2ae3du + 0x27d4eb2fu))));
}
#define PCT(h) ((int)((h) % 100u))

/* ---- regions (our own list: ISO codes + English names) ---- */
static const char *const k_reg_code[BW_REGIONS] = {"US", "GB", "DE", "FR", "ES", "IT", "BR", "MX", "JP", "KR",
                                                    "RU", "PL", "TR", "IN", "CA", "AU", "NL", "SE", "PT", "AR"};
static const char *const k_reg_name[BW_REGIONS] = {"United States", "United Kingdom", "Germany", "France", "Spain", "Italy", "Brazil", "Mexico",
                                                    "Japan", "South Korea", "Russia", "Poland", "Turkey", "India", "Canada", "Australia",
                                                    "Netherlands", "Sweden", "Portugal", "Argentina"};
static const signed char k_reg_tz[BW_REGIONS] = {-5, 0, 1, 1, 1, 1, -3, -6, 9, 9, 3, 1, 3, 5, -5, 10, 1, 1, 0, -3};
static const unsigned char k_reg_weight[BW_REGIONS] = {14, 7, 7, 6, 6, 5, 8, 5, 4, 3, 6, 3, 4, 5, 3, 2, 2, 2, 2, 2};
const char *bw_region_code(int r) { return r >= 0 && r < BW_REGIONS ? k_reg_code[r] : "US"; }
const char *bw_region_name(int r) { return r >= 0 && r < BW_REGIONS ? k_reg_name[r] : "United States"; }
int bw_region_index(const char *code) {
    if (!code) return -1;
    for (int i = 0; i < BW_REGIONS; i++)
        if ((code[0] | 32) == (k_reg_code[i][0] | 32) && (code[1] | 32) == (k_reg_code[i][1] | 32) && !code[2]) return i;
    return -1;
}

/* ---- names (our own word lists) ---- */
static const char *const k_pre[] = {"Dark", "Shadow", "Pro", "Mr", "Lil", "King", "Toxic", "Silent", "Crazy", "Ice", "Fire", "Neo", "Ultra", "Mega",
                                    "Epic", "Lucky", "Sly", "Swift", "Night", "Golden", "Super", "Hyper", "Turbo", "Royal", "Wild", "Tiny", "Big",
                                    "Lazy", "Angry", "Happy", "Sneaky", "Mad", "Cyber", "Retro", "Noob", "Sir", "Lady", "Captain", "Dr", "Baby"};
static const char *const k_core[] = {"Wolf", "Ninja", "Gamer", "Sniper", "Brawler", "Tiger", "Dragon", "Ghost", "Fox", "Hawk", "Panda", "Viper",
                                     "Storm", "Blade", "Knight", "Pixel", "Rocket", "Bunny", "Shark", "Bear", "Falcon", "Cobra", "Raven", "Lion",
                                     "Phoenix", "Taco", "Potato", "Cookie", "Muffin", "Noodle", "Pickle", "Waffle", "Beast", "Legend", "Hunter",
                                     "Wizard", "Samurai", "Pirate", "Robot", "Comet", "Thunder", "Frost", "Spark", "Bolt", "Echo", "Zero", "Ace"};
static const char *const k_first[] = {"Alex", "Sam", "Leo", "Max", "Mia", "Luca", "Noah", "Emma", "Liam", "Ava", "Ben", "Zoe", "Finn", "Ella",
                                      "Jonas", "Lena", "Marco", "Yuki", "Kai", "Sofia", "Hugo", "Iris", "Nils", "Ines", "Mateo", "Diego", "Rafa",
                                      "Tomas", "Aiko", "Elif", "Oskar", "Nova", "Ryo", "Mina", "Jin", "Omar", "Ivan", "Lars", "Pia", "Tim", "Joe",
                                      "Ana", "Eva", "Ali", "Sven", "Kim", "Lou", "Tess", "Remy", "Nico"};
static const char *const k_suf[] = {"99", "07", "123", "_YT", "XD", "TV", "2k", "_pro", "x", "777", "01", "_BS", "GG", "_ttv", "2020", "69", "42",
                                    "_1", "_OP", "HD"};
#define NA(a) ((uint32_t)(sizeof(a) / sizeof(a[0])))
static void make_name(char out[BW_NAME], uint32_t h) {
    const char *p = k_pre[mix(h + 1) % NA(k_pre)], *c = k_core[mix(h + 2) % NA(k_core)], *f = k_first[mix(h + 3) % NA(k_first)],
               *s = k_suf[mix(h + 4) % NA(k_suf)];
    char b[48];
    switch (h % 12u) {
    case 0: snprintf(b, sizeof b, "xX_%s_Xx", c); break;
    case 1: snprintf(b, sizeof b, "%s%s", p, c); break;
    case 2: snprintf(b, sizeof b, "%s%s", f, s); break;
    case 3: snprintf(b, sizeof b, "%s_%s", p, f); break;
    case 4: snprintf(b, sizeof b, "%s%u", c, (unsigned)(mix(h + 5) % 1000u)); break;
    case 5: snprintf(b, sizeof b, "%s%s%s", p, c, s); break;
    case 6: snprintf(b, sizeof b, "%s", f); break;
    case 7: snprintf(b, sizeof b, "The%s", c); break;
    case 8: snprintf(b, sizeof b, "%s.%s", f, c); for (char *q = b; *q; q++) if (*q >= 'A' && *q <= 'Z') *q = (char)(*q + 32); break;
    case 9: snprintf(b, sizeof b, "%s %s", f, c); break;
    case 10: snprintf(b, sizeof b, "%s%s", c, s); break;
    default: snprintf(b, sizeof b, "%s_%s", c, f); break;
    }
    b[BW_NAME - 1] = 0;   /* 15 characters at most */
    memcpy(out, b, BW_NAME);
}
static const char *const k_cadj[] = {"Night", "Golden", "Royal", "Savage", "Silent", "Crimson", "Frozen", "Electric", "Mystic", "Iron", "Lucky",
                                     "Toxic", "Wild", "Cosmic", "Shadow", "Brave", "Rapid", "Holy", "Chill", "Retro", "Neon", "Lazy", "Mighty"};
static const char *const k_cnoun[] = {"Wolves", "Legends", "Ninjas", "Dragons", "Sharks", "Knights", "Pandas", "Titans", "Hawks", "Rebels",
                                      "Pirates", "Wizards", "Lions", "Ghosts", "Foxes", "Rockets", "Kings", "Vipers", "Owls", "Squad", "Crew",
                                      "Gang", "Army", "Family"};
static void make_club_name(char out[BW_CLUB_NAME], uint32_t h, int region) {
    const char *a = k_cadj[mix(h + 1) % NA(k_cadj)], *n = k_cnoun[mix(h + 2) % NA(k_cnoun)];
    char b[64];
    switch (h % 6u) {
    case 0: snprintf(b, sizeof b, "The %s %s", a, n); break;
    case 1: snprintf(b, sizeof b, "%s %s", a, n); break;
    case 2: snprintf(b, sizeof b, "%s %s", k_reg_code[region], n); break;
    case 3: snprintf(b, sizeof b, "%s%s", a, n); break;
    case 4: snprintf(b, sizeof b, "%s %s %u", a, n, (unsigned)(mix(h + 3) % 99u + 1)); break;
    default: snprintf(b, sizeof b, "%s Elite", n); break;
    }
    b[BW_CLUB_NAME - 1] = 0;
    memcpy(out, b, BW_CLUB_NAME);
}

/* ---- trophies ---- */
int bw_trophy_delta(int t, int kind, int result) {
    if (kind == 2) {   /* showdown place 1..10 */
        static const int win[5] = {10, 8, 6, 4, 2};
        int pl = result < 1 ? 1 : result > 10 ? 10 : result;
        if (pl <= 4) { int d = win[pl - 1] - t / 500; return d < 1 ? 1 : d; }
        if (pl == 5) return t < 300 ? 0 : -1;
        int base = (pl - 5) * 2;          /* 6th -2 .. 10th -10 */
        if (t < 50) return 0;
        if (t < 150) return -(base / 2);
        return -(base + t / 400);
    }
    if (result == 2) return 0;
    static const int lim[] = {50, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 1100};
    static const int w[] = {8, 8, 8, 8, 8, 8, 8, 8, 8, 7, 6, 5, 4};
    int k = 0;
    while (k < 12 && t >= lim[k]) k++;
    if (result == 0) return w[k];
    return k <= 8 ? -k : -(k + (k - 8));   /* 0, -1 .. -8, then -10, -12 .. */
}
static int trophy_reset(int t) {   /* the same rule as bv_trophy_reset (profile.c): above 500 */
    if (t <= 500) return t;
    if (t <= 524) return 500;
    if (t <= 549) return 524;
    if (t <= 574) return 549;
    if (t <= 599) return 574;
    if (t <= 624) return 599;
    if (t <= 649) return 624;
    if (t <= 674) return 649;
    if (t <= 699) return 674;
    if (t <= 724) return 699;
    if (t <= 749) return 724;
    if (t <= 799) return 749;
    if (t <= 899) return 799;
    if (t <= 999) return 899;
    if (t <= 1099) return 999;
    if (t <= 1199) return 1099;
    if (t <= 1299) return 1199;
    if (t <= 1399) return 1299;
    return 1399;
}

/* ---- world ---- */
int bw_valid(const bw_world *w) { return w && w->magic == BW_MAGIC && w->version == BW_VERSION && w->size == (uint32_t)sizeof *w && w->n_bots > 0; }

int bw_bot_total(const bw_world *w, int i) {
    const bw_bot *b = &w->bots[i];
    int t = 0;
    for (int k = 0; k < w->n_br; k++) if (b->pow[k]) t += b->tro[k];
    return t;
}
static int bot_owned(const bw_bot *b, int n_br) { int n = 0; for (int k = 0; k < n_br; k++) n += b->pow[k] != 0; return n; }
int bw_slot_of_row(const bw_world *w, int row) {
    for (int k = 0; k < w->n_br; k++) if (w->br_row[k] == row) return k;
    return -1;
}

void bw_chat_add(bw_world *w, uint32_t t, int bot, int club, int kind, const char *text) {
    bw_chat *c = &w->chat[w->chat_n % BW_CHAT];
    memset(c, 0, sizeof *c);
    c->t = t; c->bot = (int16_t)bot; c->club = (int16_t)club; c->kind = (uint8_t)kind;
    if (text) { strncpy(c->text, text, BW_CHAT_LEN - 1); c->text[BW_CHAT_LEN - 1] = 0; }
    w->chat_n++;
}

static int pick_region(uint32_t h) {
    int tot = 0;
    for (int i = 0; i < BW_REGIONS; i++) tot += k_reg_weight[i];
    int r = (int)(h % (uint32_t)tot);
    for (int i = 0; i < BW_REGIONS; i++) { if (r < k_reg_weight[i]) return i; r -= k_reg_weight[i]; }
    return 0;
}

void bw_create(bw_world *w, const bw_params *p, uint32_t seed, uint32_t now) {
    memset(w, 0, sizeof *w);
    w->magic = BW_MAGIC; w->version = BW_VERSION; w->size = (uint32_t)sizeof *w; w->seed = seed ? seed : 1;
    w->n_bots = p->n_bots > 0 ? (p->n_bots > BW_MAX_BOTS ? BW_MAX_BOTS : p->n_bots) : 1200;
    w->n_clubs = p->n_clubs > 0 ? (p->n_clubs > BW_MAX_CLUBS ? BW_MAX_CLUBS : p->n_clubs) : 60;
    w->n_br = p->n_br > 0 ? (p->n_br > BW_NBR ? BW_NBR : p->n_br) : 1;
    for (int k = 0; k < w->n_br; k++) { w->br_row[k] = p->n_br > 0 ? p->br_row[k] : 0; w->br_rar[k] = p->n_br > 0 ? p->br_rar[k] : 0; }
    w->created = now; w->sim_to = now; w->player_club = -1; w->player_seen = now;
    int nb = p->n_badges > 0 ? p->n_badges : 1, nt = p->n_thumbs > 0 ? p->n_thumbs : 1, nc = p->n_colors > 0 ? p->n_colors : 1;
    for (int c = 0; c < w->n_clubs; c++) {
        bw_club *cl = &w->clubs[c];
        uint32_t h = H(w->seed, 0xc1ab, (uint32_t)c, 0);
        cl->region = (uint8_t)pick_region(mix(h + 9));
        make_club_name(cl->name, h, cl->region);
        static const unsigned short req[] = {0, 0, 0, 100, 250, 500, 750, 1000, 1500, 2000, 3000, 5000};
        cl->req = req[mix(h + 11) % NA(req)];
        cl->type = (uint8_t)(PCT(mix(h + 12)) < 70 ? 1 : PCT(mix(h + 13)) < 70 ? 2 : 3);
        cl->badge = (uint16_t)(mix(h + 14) % (uint32_t)nb);
        cl->created = now;
    }
    for (int i = 0; i < w->n_bots; i++) {
        bw_bot *b = &w->bots[i];
        uint32_t h = H(w->seed, 0xb07, (uint32_t)i, 0);
        for (int tries = 0; tries < 8; tries++) {   /* unique names */
            make_name(b->name, mix(h + 100 + (uint32_t)tries));
            int dup = 0;
            for (int j = 0; j < i && !dup; j++) dup = !strcmp(w->bots[j].name, b->name);
            if (!dup) break;
            if (tries == 7) { size_t L = strlen(b->name); if (L > 11) L = 11; snprintf(b->name + L, BW_NAME - L, "%u", (unsigned)(i % 10000)); }
        }
        /* hidden skill: roughly normal around 450 (sum of three uniforms) */
        int s = (int)(mix(h + 1) % 334u) + (int)(mix(h + 2) % 334u) + (int)(mix(h + 3) % 334u);
        b->skill = (uint16_t)(s > 1000 ? 1000 : s);
        b->region = (uint8_t)pick_region(mix(h + 4));
        b->persona = (uint8_t)(mix(h + 5) % BW_P_COUNT);
        static const unsigned char hours[] = {15, 16, 17, 18, 18, 19, 19, 19, 20, 20, 20, 21, 21, 22, 23, 12, 13};
        b->hour0 = hours[mix(h + 6) % NA(hours)];
        /* daily minutes: mostly 40..150, a few heavy players up to ~300 */
        int m = 25 + (int)(mix(h + 7) % 100u) + (int)(mix(h + 8) % 60u);
        if (PCT(mix(h + 9)) < 10) m += 120;
        b->daily_min = (uint16_t)m;
        b->thumb = (uint8_t)(mix(h + 10) % (uint32_t)nt);
        b->color = (uint8_t)(PCT(mix(h + 11)) < 75 ? 0 : mix(h + 12) % (uint32_t)nc);
        b->pow[0] = 1;   /* everyone starts with the first brawler */
        b->club = -1;
        b->last_play = now;
        /* clubs: 70 % are in one, usually of their own region */
        if (w->n_clubs > 0 && PCT(mix(h + 13)) < 70) {
            int pick = -1;
            for (int t = 0; t < 6 && pick < 0; t++) {
                int c = (int)(mix(h + 20 + (uint32_t)t) % (uint32_t)w->n_clubs);
                if (w->clubs[c].n_members >= 100) continue;
                if (t < 4 && w->clubs[c].region != b->region && PCT(mix(h + 30 + (uint32_t)t)) < 60) continue;
                pick = c;
            }
            if (pick >= 0) {
                b->club = (int16_t)pick;
                b->role = w->clubs[pick].n_members == 0 ? BW_ROLE_PRESIDENT
                          : PCT(mix(h + 14)) < 4 ? BW_ROLE_VICE : PCT(mix(h + 15)) < 12 ? BW_ROLE_SENIOR : BW_ROLE_MEMBER;
                w->clubs[pick].n_members++;
            }
        }
    }
}

void bw_set_season(bw_world *w, uint32_t start, uint32_t len) { w->season_start = start; w->season_len = len; }

/* the two play sessions of bot i on local day d: start times and match counts (deterministic) */
static int sessions(const bw_world *w, int i, int d, uint32_t st[2], int cnt[2]) {
    const bw_bot *b = &w->bots[i];
    cnt[0] = cnt[1] = 0;
    if (b->quit) return 0;
    uint32_t h = H(w->seed, (uint32_t)i, (uint32_t)d, 0x5e55);
    if (PCT(h) < 15) return 0;                                      /* a day off */
    int wd = (d + 4) % 7;                                           /* 0 = Sunday */
    int mins = b->daily_min * (wd == 0 || wd == 6 ? 16 : 10) / 10;
    mins = mins * (50 + PCT(mix(h + 1))) / 100;
    int m = mins / 3;
    if (m > 120) m = 120;
    if (m <= 0) return 0;
    long day0 = (long)d * 86400L - (long)k_reg_tz[b->region] * 3600L;   /* local midnight in unix time */
    int j0 = (int)(mix(h + 2) % 7200u) - 3600;
    if (m >= 12 && PCT(mix(h + 3)) < 45) {                          /* an afternoon session too */
        cnt[1] = m * 3 / 10;
        cnt[0] = m - cnt[1];
        long s1 = day0 + (long)(b->hour0 > 6 ? b->hour0 - 5 : b->hour0 + 2) * 3600L + (long)(mix(h + 4) % 3600u);
        st[1] = (uint32_t)(s1 > 0 ? s1 : 0);
    } else cnt[0] = m;
    long s0 = day0 + (long)b->hour0 * 3600L + j0;
    st[0] = (uint32_t)(s0 > 0 ? s0 : 0);
    return 1;
}
#define MATCH_S 180u

int bw_bot_online(const bw_world *w, int i, uint32_t now) {
    if (i < 0 || i >= w->n_bots) return 0;
    int tz = k_reg_tz[w->bots[i].region];
    int d = (int)(((long)now + (long)tz * 3600L) / 86400L);
    for (int dd = d - 1; dd <= d; dd++) {
        uint32_t st[2]; int cnt[2];
        if (!sessions(w, i, dd, st, cnt)) continue;
        for (int s = 0; s < 2; s++)
            if (cnt[s] && now >= st[s] && now < st[s] + (uint32_t)cnt[s] * MATCH_S + 600u) return 1;
    }
    return 0;
}

static void level_and_unlock(bw_world *w, bw_bot *b, int k, uint32_t h) {
    if (b->pow[k] && b->pow[k] < 9 && mix(h + 40) % 1000u < 1000u / (6u * b->pow[k] + 4u)) b->pow[k]++;
    else if (b->pow[k] == 9 && mix(h + 41) % 150u == 0) b->pow[k] = 10;
    int owned = bot_owned(b, w->n_br);
    if (owned >= w->n_br) return;
    /* trophy road: the n-th trophy road brawler at a total-trophy threshold */
    static const int road[] = {0, 10, 60, 250, 500, 1000, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 12000, 14000, 16000};
    int total = 0;
    for (int q = 0; q < w->n_br; q++) if (b->pow[q]) total += b->tro[q];
    int nroad = 0;
    for (int q = 0; q < w->n_br; q++) {
        if (w->br_rar[q] != 0) continue;
        if (nroad < (int)NA(road) && total >= road[nroad] && !b->pow[q]) { b->pow[q] = 1; return; }
        nroad++;
    }
    /* boxes: a new brawler now and then, rarer ones less often */
    uint32_t r = mix(h + 42) % 10000u;
    uint32_t chance = 150u * (uint32_t)(w->n_br - owned) / (uint32_t)w->n_br + 20u;
    if (r >= chance) return;
    static const int wt[6] = {0, 60, 30, 18, 9, 4};
    int cand[BW_NBR], cw[BW_NBR], nc = 0, tw = 0;
    for (int q = 0; q < w->n_br; q++) {
        if (b->pow[q] || w->br_rar[q] == 0) continue;
        int rr = w->br_rar[q] < 1 || w->br_rar[q] > 5 ? 1 : w->br_rar[q];
        cand[nc] = q; cw[nc] = wt[rr]; tw += wt[rr]; nc++;
    }
    if (!nc || tw <= 0) return;
    int x = (int)(mix(h + 43) % (uint32_t)tw);
    for (int q = 0; q < nc; q++) { if (x < cw[q]) { b->pow[cand[q]] = 1; return; } x -= cw[q]; }
}

static void play_match(bw_world *w, int i, uint32_t t, uint32_t h) {
    bw_bot *b = &w->bots[i];
    int own[BW_NBR], no = 0;
    for (int k = 0; k < w->n_br; k++) if (b->pow[k]) own[no++] = k;
    if (!no) { b->pow[0] = 1; own[no++] = 0; }
    int k = own[mix(h + 1) % (uint32_t)no];
    if (b->pow[b->fav] && PCT(mix(h + 2)) < 40) k = b->fav;
    int tro = b->tro[k];
    int target = (int)b->skill * 11 / 10 * (45 + 55 * b->pow[k] / 10) / 100;   /* trophies this bot can hold with this brawler */
    int kind = PCT(mix(h + 3)) < 30 ? 2 : 0, result, won;
    if (kind == 0) {
        int p = 50 + (target - tro) * 100 / 700;
        if (p < 15) p = 15;
        if (p > 85) p = 85;
        int r = PCT(mix(h + 4));
        result = r < 4 ? 2 : (PCT(mix(h + 5)) < p ? 0 : 1);
        won = result == 0;
    } else {
        int e10 = 55 - (target - tro) * 10 / 120;                  /* expected place * 10 */
        e10 += (int)(mix(h + 6) % 61u) - 30;
        result = (e10 + 5) / 10;
        if (result < 1) result = 1;
        if (result > 10) result = 10;
        won = result == 1;
        if (won) b->sd_wins++;
    }
    int d = bw_trophy_delta(tro, kind, result);
    tro += d;
    if (tro < 0) tro = 0;
    if (tro > 60000) tro = 60000;
    b->tro[k] = (uint16_t)tro;
    if (tro > b->best[k]) b->best[k] = (uint16_t)tro;
    b->battles++;
    if (kind == 0 && won) b->wins++;
    b->exp += kind == 2 ? (uint32_t)(10 - result < 1 ? 1 : 10 - result) : (uint32_t)(result == 0 ? 8 : result == 1 ? 4 : 6);
    if (kind == 0) b->streak = (int8_t)(won ? (b->streak > 0 ? b->streak + 1 : 1) : result == 1 ? (b->streak < 0 ? b->streak - 1 : -1) : 0);
    if (b->streak > 20) b->streak = 20;
    if (b->streak < -20) b->streak = -20;
    if (tro >= b->tro[b->fav] || !b->pow[b->fav]) b->fav = (uint8_t)k;
    b->last_play = t;
    level_and_unlock(w, b, k, h);
    w->matches++;
}

static void season_reset(bw_world *w) {
    for (int i = 0; i < w->n_bots; i++)
        for (int k = 0; k < w->n_br; k++)
            if (w->bots[i].pow[k]) w->bots[i].tro[k] = (uint16_t)trophy_reset(w->bots[i].tro[k]);
}

/* once per (UTC) day: bots quit / come back, club moves (joins, leaves, kicks of inactive members, promotions) with chat lines */
static void world_day(bw_world *w, int d) {
    uint32_t t0 = (uint32_t)d * 86400u;
    for (int i = 0; i < w->n_bots; i++) {
        bw_bot *b = &w->bots[i];
        uint32_t h = H(w->seed, (uint32_t)i, (uint32_t)d, 0xda7);
        if (!b->quit && h % 4000u == 0) b->quit = 1;                       /* ~2.5 % stop each 100 days */
        else if (b->quit && h % 120u == 0) b->quit = 0;                    /* and some come back */
    }
    for (int c = 0; c < w->n_clubs; c++) {
        bw_club *cl = &w->clubs[c];
        uint32_t h = H(w->seed, (uint32_t)c, (uint32_t)d, 0xc1d);
        int me = c == w->player_club;
        /* a bot without a club joins */
        if (cl->n_members < 100 && PCT(h) < 35) {
            int j = (int)(mix(h + 1) % (uint32_t)w->n_bots);
            bw_bot *b = &w->bots[j];
            if (b->club < 0 && !b->quit && (cl->type == 1 || PCT(mix(h + 2)) < 30) && bw_bot_total(w, j) >= cl->req / 2) {
                b->club = (int16_t)c; b->role = BW_ROLE_MEMBER; cl->n_members++;
                if (me) bw_chat_add(w, t0 + mix(h + 3) % 86400u, j, c, 1, NULL);
            }
        }
        /* a member leaves */
        if (cl->n_members > 2 && PCT(mix(h + 4)) < 6) {
            int j = (int)(mix(h + 5) % (uint32_t)w->n_bots);
            bw_bot *b = &w->bots[j];
            if (b->club == c && b->role != BW_ROLE_PRESIDENT) {
                b->club = -1; b->role = 0; cl->n_members--;
                if (me) bw_chat_add(w, t0 + mix(h + 6) % 86400u, j, c, 2, NULL);
            }
        }
        /* the leaders kick an inactive member, promote an active one */
        if (PCT(mix(h + 7)) < 20) {
            for (int j = (int)(mix(h + 8) % (uint32_t)w->n_bots), n = 0; n < 60; n++, j = (j + 1) % w->n_bots) {
                bw_bot *b = &w->bots[j];
                if (b->club != c || b->role == BW_ROLE_PRESIDENT) continue;
                if (b->quit && b->last_play + 14u * 86400u < t0) {
                    b->club = -1; b->role = 0; cl->n_members--;
                    if (me) bw_chat_add(w, t0 + mix(h + 9) % 86400u, j, c, 3, NULL);
                    break;
                }
                if (b->role == BW_ROLE_MEMBER && b->battles > 300 && PCT(mix(h + 10)) < 25) {
                    b->role = BW_ROLE_SENIOR;
                    if (me) bw_chat_add(w, t0 + mix(h + 11) % 86400u, j, c, 4, NULL);
                    break;
                }
            }
        }
    }
}

/* casual chat of the player's club: lines from members who are online at that moment */
static const char *const k_chat[] = {"gg", "anyone up for a game?", "nice win!", "who wants to push trophies?", "good morning club", "lol",
                                     "need one more for a team", "brb", "let's go!!", "any tips for bounty?", "almost at my next rank",
                                     "that last match was close", "who's online?", "invite me", "gg wp everyone", "so close to a new brawler",
                                     "just got a new brawler!!", "box luck today is crazy", "I'm on a win streak", "lost 5 in a row lol",
                                     "play with me?", "nice trophies", "hello everyone", "anyone doing the event?", "ty for the game",
                                     "we should push the club trophies", "night all", "this map is hard", "my teammates keep dying", "back"};
static void club_chat(bw_world *w, uint32_t from, uint32_t to) {
    int c = w->player_club;
    if (c < 0 || c >= w->n_clubs || to <= from) return;
    uint32_t first = (from + 1799u) / 1800u * 1800u;
    for (uint32_t t = first; t < to; t += 1800u) {   /* one chance every 30 minutes */
        uint32_t h = H(w->seed, (uint32_t)c, t, 0xca7);
        if (PCT(h) >= 40) continue;
        for (int n = 0, j = (int)(mix(h + 1) % (uint32_t)w->n_bots); n < 200; n++, j = (j + 7) % w->n_bots) {
            if (w->bots[j].club != c || !bw_bot_online(w, j, t)) continue;
            bw_chat_add(w, t + mix(h + 2) % 1800u, j, c, 0, k_chat[mix(h + 3) % NA(k_chat)]);
            break;
        }
    }
}

static void play_range(bw_world *w, uint32_t from, uint32_t to) {
    for (int i = 0; i < w->n_bots; i++) {
        int tz = k_reg_tz[w->bots[i].region];
        int d0 = (int)(((long)from + (long)tz * 3600L) / 86400L) - 1, d1 = (int)(((long)to + (long)tz * 3600L) / 86400L);
        for (int d = d0; d <= d1; d++) {
            uint32_t st[2]; int cnt[2];
            if (!sessions(w, i, d, st, cnt)) continue;
            for (int s = 1; s >= 0; s--) {   /* the afternoon session comes first */
                for (int m = 0; m < cnt[s]; m++) {
                    uint32_t t = st[s] + (uint32_t)m * MATCH_S;
                    if (t < from || t >= to) continue;
                    play_match(w, i, t, H(w->seed, (uint32_t)i, (uint32_t)d, (uint32_t)(s * 1000 + m)));
                }
            }
        }
    }
}
/* UTC-day chunks: bots are independent inside a chunk, the daily club step runs at each midnight, so splitting a range anywhere gives the same world */
static uint32_t sim_segment(bw_world *w, uint32_t from, uint32_t to) {
    uint32_t before = w->matches;
    for (uint32_t a = from; a < to;) {
        uint32_t dayend = (a / 86400u + 1u) * 86400u, b = dayend < to ? dayend : to;
        play_range(w, a, b);
        club_chat(w, a, b);
        if (b == dayend) world_day(w, (int)(dayend / 86400u));
        a = b;
    }
    return w->matches - before;
}

uint32_t bw_simulate(bw_world *w, uint32_t to, int catchup_days) {
    if (!bw_valid(w) || to <= w->sim_to) return 0;
    uint32_t lim = (uint32_t)(catchup_days > 0 ? catchup_days : 30) * 86400u;
    uint32_t from = w->sim_to;
    if (to - from > lim) from = to - lim;
    uint32_t n = 0;
    while (from < to) {
        uint32_t seg_end = to;
        uint32_t b = 0;
        if (w->season_len >= 86400u && w->season_start) {
            /* next season boundary after `from` */
            if (from < w->season_start) b = w->season_start;
            else b = w->season_start + ((from - w->season_start) / w->season_len + 1u) * w->season_len;
            if (b > from && b < seg_end) seg_end = b;
            else b = 0;
        }
        n += sim_segment(w, from, seg_end);
        if (b && seg_end == b) season_reset(w);
        from = seg_end;
    }
    w->sim_to = to;
    return n;
}

int bw_club_trophies(const bw_world *w, int c) {
    int t = 0;
    for (int i = 0; i < w->n_bots; i++) if (w->bots[i].club == c) t += bw_bot_total(w, i);
    return t;
}
int bw_club_online(const bw_world *w, int c, uint32_t now) {
    int n = 0;
    for (int i = 0; i < w->n_bots; i++) if (w->bots[i].club == c && bw_bot_online(w, i, now)) n++;
    return n;
}

static int cmp_entry(const void *a, const void *b) {
    const bw_entry *x = (const bw_entry *)a, *y = (const bw_entry *)b;
    if (x->score != y->score) return x->score < y->score ? 1 : -1;
    return x->who < y->who ? -1 : x->who > y->who;
}

int bw_leaderboard(const bw_world *w, int type, int brawler_row, int region, int player_score, const char *player_name, bw_entry *out, int max,
                   int *own_rank) {
    (void)player_name;
    int n = 0, cap = (w->n_bots > w->n_clubs ? w->n_bots : w->n_clubs) + 1;
    bw_entry *all = (bw_entry *)malloc(sizeof(bw_entry) * (size_t)cap);
    if (own_rank) *own_rank = 0;
    if (!all) return 0;
    if (type == 2) {
        int *sum = (int *)calloc((size_t)w->n_clubs, sizeof(int));
        if (!sum) { free(all); return 0; }
        for (int i = 0; i < w->n_bots; i++) if (w->bots[i].club >= 0 && w->bots[i].club < w->n_clubs) sum[w->bots[i].club] += bw_bot_total(w, i);
        if (w->player_club >= 0 && w->player_club < w->n_clubs) sum[w->player_club] += player_score;
        for (int c = 0; c < w->n_clubs; c++) {
            if (region >= 0 && w->clubs[c].region != region) continue;
            if (!w->clubs[c].n_members && c != w->player_club) continue;
            all[n].who = c; all[n].score = sum[c]; n++;
        }
        free(sum);
    } else {
        int k = type == 0 ? bw_slot_of_row(w, brawler_row) : -1;
        for (int i = 0; i < w->n_bots; i++) {
            if (region >= 0 && w->bots[i].region != region) continue;
            if (type == 0 && (k < 0 || !w->bots[i].pow[k])) continue;
            all[n].who = i; all[n].score = type == 0 ? w->bots[i].tro[k] : bw_bot_total(w, i); n++;
        }
        all[n].who = -1; all[n].score = player_score; n++;   /* the player (region filter: the player's region is the one asked) */
    }
    qsort(all, (size_t)n, sizeof *all, cmp_entry);
    if (own_rank) {
        for (int i = 0; i < n; i++) {
            if ((type == 2 && all[i].who == w->player_club && w->player_club >= 0) || (type != 2 && all[i].who == -1)) { *own_rank = i + 1; break; }
        }
    }
    int m = n < max ? n : max;
    memcpy(out, all, sizeof(bw_entry) * (size_t)m);
    free(all);
    return m;
}

static int ai_tier(int tro, int skill) {
    int t = tro < 40 ? 0 : tro < 100 ? 1 : tro < 300 ? 2 : tro < 500 ? 3 : 4;   /* like the game's own tier steps (battle.md 3.2) */
    if (skill >= 700 && t < 4) t++;
    if (skill < 300 && t > 0) t--;
    return t;
}

int bw_match_plan(const bw_world *w, int trophies, int streak, int n, uint32_t seed, uint32_t now, bw_pick *plan) {
    if (!bw_valid(w) || n <= 0) return 0;
    if (n > BW_SLOTS - 1) n = BW_SLOTS - 1;
    int target = trophies + streak * 15;
    if (target < 0) target = 0;
    typedef struct { int bot, slot, cost; } cand_t;
    cand_t best[BW_SLOTS];
    int nb = 0;
    for (int i = 0; i < w->n_bots; i++) {
        const bw_bot *b = &w->bots[i];
        if (b->quit) continue;
        int bk = -1, bd = 1 << 30;
        uint32_t h = H(seed, (uint32_t)i, 0x3a7c, 0);
        for (int k = 0; k < w->n_br; k++) {
            if (!b->pow[k]) continue;
            int d = b->tro[k] - target;
            if (d < 0) d = -d;
            d += (int)(mix(h + (uint32_t)k) % 25u);
            if (d < bd) { bd = d; bk = k; }
        }
        if (bk < 0) continue;
        int cost = bd + (bw_bot_online(w, i, now) ? 0 : 60) + (int)(h % 40u);
        cost -= streak * ((int)b->skill - 500) / 20;   /* win streak: stronger bots; losses: weaker ones */
        cand_t c = {i, bk, cost};
        /* keep the n cheapest (insertion) */
        int pos = nb;
        while (pos > 0 && best[pos - 1].cost > cost) pos--;
        if (pos >= n) continue;
        if (nb < n) nb++;
        for (int q = nb - 1; q > pos; q--) best[q] = best[q - 1];
        best[pos] = c;
    }
    for (int q = 0; q < nb; q++) {
        const bw_bot *b = &w->bots[best[q].bot];
        plan[q + 1].bot = best[q].bot;
        plan[q + 1].slot = best[q].slot;
        plan[q + 1].level = b->pow[best[q].slot];
        plan[q + 1].tier = ai_tier(b->tro[best[q].slot], b->skill);
    }
    plan[0].bot = -1;
    return nb;
}

void bw_match_result(bw_world *w, const bw_pick *plan, const int *team, const int *used, int kind, int result, int my_team, uint32_t now) {
    if (!bw_valid(w)) return;
    int taken[11] = {0};
    if (kind == 2 && result >= 1 && result <= 10) taken[result] = 1;
    for (int n = 1; n < BW_SLOTS; n++) {
        if (!used[n] || plan[n].bot < 0 || plan[n].bot >= w->n_bots) continue;
        bw_bot *b = &w->bots[plan[n].bot];
        int k = plan[n].slot;
        if (k < 0 || k >= w->n_br || !b->pow[k]) continue;
        int r;
        if (kind == 2) {   /* a free place, better bots tend to place higher */
            uint32_t h = H(w->seed, (uint32_t)plan[n].bot, now, 0x5d);
            int want = 1 + (int)((1000 - b->skill) * 9 / 1000) + (int)(h % 3u) - 1;
            if (want < 1) want = 1;
            if (want > 10) want = 10;
            r = want;
            for (int s = 0; s < 10 && taken[r]; s++) r = r % 10 + 1;
            taken[r] = 1;
        } else {
            int same = team[n] == my_team;
            r = result == 2 ? 2 : same ? result : (result == 0 ? 1 : 0);
        }
        int tro = b->tro[k] + bw_trophy_delta(b->tro[k], kind, r);
        if (tro < 0) tro = 0;
        b->tro[k] = (uint16_t)tro;
        if (tro > b->best[k]) b->best[k] = (uint16_t)tro;
        b->battles++;
        if (kind == 0 && r == 0) b->wins++;
        if (kind == 2 && r == 1) b->sd_wins++;
        b->last_play = now;
    }
}

/* ---- 24403 ---- */
void bw_leaderboard_msg(bv_bs *s, const bw_world *w, int type, int local, int brawler_row, int region, int player_score, const char *player_name,
                        int player_thumb, int player_color, int acc_hi, int acc_lo) {
    enum { MAXE = 200 };
    static bw_entry e[MAXE];
    int own = 0;
    int n = bw_valid(w) ? bw_leaderboard(w, type, brawler_row, local ? region : -1, player_score, player_name, e, MAXE, &own) : 0;
    bs_vint(s, type);                              /* +0x50: must equal the request's type (popup +0x84) */
    bs_vint(s, 0);                                 /* +0x54 */
    if (type == 0 && brawler_row >= 0) bs_dataref(s, 16, brawler_row); else bs_vint(s, 0);   /* +0x58: the request's brawler (popup +0x88) */
    bs_string(s, local ? bw_region_code(region) : NULL);    /* +0x5c: non-null = the local tab */
    bs_vint(s, n);
    for (int i = 0; i < n; i++) {
        int who = e[i].who;
        if (type == 2) {
            const bw_club *c = &w->clubs[who];
            bs_vint(s, BW_CLUB_ID_HI);
            bs_vint(s, 1 + who);
            bs_vint(s, i + 1);                     /* +8 rank */
            bs_vint(s, e[i].score);                /* +0xc score */
            /* no player + club: two ADJACENT Bools share one byte (bit 0, bit 1); two bytes crashed the club tab (update 185, run 233 'Too long
             * String') */
            bs_byte(s, 2);                         /* club: String name, VInt members, DataRef badge (0x452adc) */
            bs_string(s, c->name);
            bs_vint(s, c->n_members + (who == w->player_club ? 1 : 0));
            bs_dataref(s, 8, c->badge);
        } else {
            const bw_bot *b = who >= 0 ? &w->bots[who] : NULL;
            int club = b ? b->club : w->player_club;
            bs_vint(s, b ? BW_ID_HI : acc_hi);
            bs_vint(s, b ? 1 + who : acc_lo);
            bs_vint(s, i + 1);
            bs_vint(s, e[i].score);
            bs_bool(s, 1);                         /* player: String club name, display data (String name, 4 VInt) */
            bs_string(s, club >= 0 && club < w->n_clubs ? w->clubs[club].name : NULL);
            bs_string(s, b ? b->name : player_name);
            bs_vint(s, 100);
            bs_vint(s, 28000000 + (b ? b->thumb : (player_thumb > 0 ? player_thumb : 0)));
            bs_vint(s, 43000000 + (b ? b->color : (player_color > 0 ? player_color : 0)));
            bs_vint(s, 0);
            bs_bool(s, 0);                         /* no club object */
        }
    }
    bs_vint(s, player_score);                      /* +0x64 (INFERRED: your score) */
    bs_vint(s, own);                               /* +0x68 your rank (run 232: the bottom row showed this value as the rank) */
    bs_vint(s, 0);                                 /* +0x6c */
    bs_vint(s, 0);                                 /* +0x70: TID_REGION_0 = GLOBAL (global tab label) */
    bs_string(s, bw_region_name(region));         /* +0x74: <COUNTRY> of the local tab label */
}

/* ---- Phase 3 (L2): clubs + chat ---- */
int bw_sc_role(int role) { return role == BW_ROLE_PRESIDENT ? 2 : role == BW_ROLE_VICE ? 4 : role == BW_ROLE_SENIOR ? 3 : 1; }
static const char *const k_cdesc[] = {"Active club, play every day!", "Push trophies together", "Be nice, no toxic players", "Chill club, all welcome",
                                      "Friends from everywhere", "Tryhards only", "Daily players wanted", "Have fun and help each other",
                                      "Inactive members get kicked", "We want to be the best club"};
static int club_members_trophies(const bw_world *w, int c, int player_total) {
    int t = 0;
    for (int i = 0; i < w->n_bots; i++) if (w->bots[i].club == c) t += bw_bot_total(w, i);
    if (c == w->player_club) t += player_total;
    return t;
}
void bw_club_header(bv_bs *s, const bw_world *w, int c, int player_total) {
    const bw_club *cl = &w->clubs[c];
    bs_int(s, BW_CLUB_ID_HI);                      /* LogicLong id */
    bs_int(s, 1 + c);
    bs_string(s, cl->name);
    bs_dataref(s, 8, cl->badge);                   /* +8 badge */
    bs_vint(s, cl->type ? cl->type : 1);           /* +0xc type 1 open / 2 invite only / 3 closed (0xfd850) */
    bs_vint(s, cl->n_members + (c == w->player_club ? 1 : 0));   /* +0x10 members */
    bs_vint(s, club_members_trophies(w, c, player_total));       /* +0x14 trophies */
    bs_vint(s, cl->req);                           /* +0x18 required trophies */
    bs_vint(s, 0);                                 /* +0x24 region: null */
    bs_string(s, k_cdesc[mix((uint32_t)c * 31u + w->seed) % NA(k_cdesc)]);   /* +0x28 description */
    bs_vint(s, 0);                                 /* +0x1c */
    bs_bool(s, 0);                                 /* +0x2c family friendly */
}
static void display_data(bv_bs *s, const char *name, int thumb, int color) {
    bs_string(s, name);
    bs_vint(s, 100);
    bs_vint(s, 28000000 + (thumb > 0 ? thumb : 0));
    bs_vint(s, 43000000 + (color > 0 ? color : 0));
    bs_vint(s, 0);
}
static int bot_status(const bw_world *w, int i, uint32_t now, int *ago) {
    const bw_bot *b = &w->bots[i];
    if (bw_bot_online(w, i, now)) { *ago = 0; return (mix(now / 180u + (uint32_t)i) & 1) ? 1 : 2; }   /* battling / in menus */
    *ago = (int)(now > b->last_play ? now - b->last_play : 0);
    return 0;
}
void bw_club_data_msg(bv_bs *s, const bw_world *w, int c, uint32_t now, const bw_player *pl) {
    bs_bool(s, c == w->player_club);               /* +0x50 */
    bw_club_header(s, w, c, pl ? pl->total : 0);
    bs_string(s, k_cdesc[mix((uint32_t)c * 31u + w->seed) % NA(k_cdesc)]);   /* S_68122c +8: the description the club screen shows (run 232) */
    int n = 0;
    for (int i = 0; i < w->n_bots; i++) n += w->bots[i].club == c;
    int me = c == w->player_club && pl;
    bs_vint(s, n + me);
    if (me) {
        bs_int(s, pl->hi); bs_int(s, pl->lo);
        bs_vint(s, bw_sc_role(w->player_role)); bs_vint(s, pl->total); bs_vint(s, 2); bs_vint(s, 0); bs_bool(s, 0);
        display_data(s, pl->name, pl->thumb, pl->color);
    }
    for (int i = 0; i < w->n_bots; i++) {
        const bw_bot *b = &w->bots[i];
        if (b->club != c) continue;
        int ago, st = bot_status(w, i, now, &ago);
        bs_int(s, BW_ID_HI); bs_int(s, 1 + i);
        bs_vint(s, bw_sc_role(b->role));           /* +0x24 role */
        bs_vint(s, bw_bot_total(w, i));            /* +0x28 trophies (0x5cc2f4) */
        bs_vint(s, st);                            /* +0x34 online status (TID_CLAN_MEMBER_ONLINE_STATUS_n, 0 = offline) */
        bs_vint(s, ago);                           /* +0x38 seconds since last online */
        bs_bool(s, 0);
        display_data(s, b->name, b->thumb, b->color);
    }
}
void bw_my_club_msg(bv_bs *s, const bw_world *w, uint32_t now, const bw_player *pl) {
    int c = w->player_club;
    if (c < 0 || c >= w->n_clubs) { bs_vint(s, 0); bs_bool(s, 0); return; }
    bs_vint(s, bw_club_online(w, c, now) + 1);     /* +0x50 (INFERRED: online members) */
    bs_bool(s, 1);
    bs_dataref(s, 25, bw_sc_role(w->player_role) - 1);   /* +0x54 role (alliance_roles row) */
    bw_club_header(s, w, c, pl ? pl->total : 0);
}
static int name_match(const char *name, const char *q) {
    if (!q || !q[0]) return 1;
    size_t L = strlen(q);
    for (const char *p = name; *p; p++) {
        size_t k = 0;
        while (k < L && p[k] && ((p[k] | 32) == (q[k] | 32))) k++;
        if (k == L) return 1;
    }
    return 0;
}
void bw_club_list_msg(bv_bs *s, const bw_world *w, int region, const char *query, int player_total) {
    int list[BW_MAX_CLUBS], n = 0;
    for (int c = 0; c < w->n_clubs && n < 50; c++) {
        if (!w->clubs[c].n_members && c != w->player_club) continue;
        if (query ? !name_match(w->clubs[c].name, query) : (region >= 0 && w->clubs[c].region != region && (mix((uint32_t)c) & 3))) continue;
        list[n++] = c;
    }
    if (query) { bs_string(s, query); bs_vint(s, 0); }
    bs_vint(s, n);
    for (int i = 0; i < n; i++) bw_club_header(s, w, list[i], player_total);
}
static int club_president(const bw_world *w, int c) {
    for (int i = 0; i < w->n_bots; i++) if (w->bots[i].club == c && w->bots[i].role == BW_ROLE_PRESIDENT) return i;
    return -1;
}
static void stream_entry(bv_bs *s, const bw_world *w, int line, uint32_t now, const bw_player *pl) {
    const bw_chat *e = &w->chat[line % BW_CHAT];
    int ev = e->kind == 1 ? 3 : e->kind == 2 ? 4 : e->kind == 3 ? 1 : e->kind == 4 ? 5 : e->kind == 5 ? 6 : 0;
    int who = e->bot, sender = who;
    if (ev == 1 || ev == 5 || ev == 6) sender = club_president(w, e->club);   /* kicked / promoted by the president */
    bs_vint(s, ev ? 4 : 2);                        /* entry type */
    bs_vint(s, 0); bs_vint(s, 1 + line);           /* entry id */
    if (sender >= 0) {
        bs_vint(s, BW_ID_HI); bs_vint(s, 1 + sender);
        bs_string(s, w->bots[sender].name);
        bs_vint(s, bw_sc_role(w->bots[sender].role));
    } else {
        bs_vint(s, pl ? pl->hi : 0); bs_vint(s, pl ? pl->lo : 1);
        bs_string(s, pl ? pl->name : "You");
        bs_vint(s, bw_sc_role(w->player_role));
    }
    bs_vint(s, now > e->t ? (int)(now - e->t) : 0);   /* age in seconds */
    bs_bool(s, 0);
    if (!ev) { bs_string(s, e->text); return; }
    bs_vint(s, ev);
    bs_bool(s, 1);
    if (who >= 0) { bs_vint(s, BW_ID_HI); bs_vint(s, 1 + who); bs_string(s, w->bots[who].name); }
    else { bs_vint(s, pl ? pl->hi : 0); bs_vint(s, pl ? pl->lo : 1); bs_string(s, pl ? pl->name : "You"); }
}
void bw_stream_msg(bv_bs *s, const bw_world *w, uint32_t now, const bw_player *pl) {
    int c = w->player_club, first = w->chat_n > BW_CHAT ? w->chat_n - BW_CHAT : 0, n = 0;
    for (int i = first; i < w->chat_n; i++) n += w->chat[i % BW_CHAT].club == c && w->chat[i % BW_CHAT].t <= now;
    bs_vint(s, n);
    for (int i = first; i < w->chat_n; i++)
        if (w->chat[i % BW_CHAT].club == c && w->chat[i % BW_CHAT].t <= now) stream_entry(s, w, i, now, pl);
}
void bw_stream_entry_msg(bv_bs *s, const bw_world *w, int line, uint32_t now, const bw_player *pl) { stream_entry(s, w, line, now, pl); }
int bw_player_join(bw_world *w, int c, uint32_t now) {
    if (c < 0 || c >= w->n_clubs) return 0;
    if (w->player_club == c) return 1;
    if (w->player_club >= 0) bw_player_leave(w, now);
    w->player_club = (int16_t)c;
    w->player_role = club_president(w, c) < 0 ? BW_ROLE_PRESIDENT : BW_ROLE_MEMBER;
    w->player_seen = now;
    bw_chat_add(w, now, -1, c, 1, NULL);
    return 1;
}
void bw_player_leave(bw_world *w, uint32_t now) {
    if (w->player_club < 0) return;
    bw_chat_add(w, now, -1, w->player_club, 2, NULL);
    w->player_club = -1;
    w->player_role = 0;
}
static const char *const k_reply[] = {"hi!", "hey", "lol", "gg", "yes!", "sure, invite me", "nice", "same", "haha", "lets play",
                                      "welcome!", "true", "ok", "omg", "good luck"};
int bw_chat_reply(bw_world *w, uint32_t now) {
    int c = w->player_club;
    if (c < 0) return 0;
    uint32_t h = H(w->seed, now, (uint32_t)w->chat_n, 0x4e9);
    if (PCT(h) >= 60) return 0;
    for (int n = 0, j = (int)(mix(h + 1) % (uint32_t)w->n_bots); n < w->n_bots; n++, j = (j + 1) % w->n_bots) {
        if (w->bots[j].club != c || !bw_bot_online(w, j, now)) continue;
        bw_chat_add(w, now, j, c, 0, k_reply[mix(h + 2) % NA(k_reply)]);
        return 1;
    }
    return 0;
}
