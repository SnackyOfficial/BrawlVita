/* cycle 8 (E3 / E6): event map rotation every 4 h 48 min (user), maps from the user's csv_logic/locations.csv (Name, Disabled, ..., GameMode).
 * Pure logic, host-tested (tests/host_maprot_test.c). */
#include "maprot.h"

#include <string.h>

/* one CSV field (quoted or not) into f; returns 1 if more fields follow on the line */
static int field(const char **ps, char *f, size_t n) {
    const char *s = *ps;
    size_t k = 0;
    int q = (*s == '"');
    if (q) s++;
    while (*s) {
        if (q && *s == '"' && s[1] == '"') { if (k + 1 < n) f[k++] = '"'; s += 2; continue; }
        if (q && *s == '"') { q = 0; s++; continue; }
        if (!q && (*s == ',' || *s == '\n' || *s == '\r')) break;
        if (k + 1 < n) f[k++] = *s;
        s++;
    }
    f[k] = 0;
    int more = (*s == ',');
    if (*s == ',') s++;
    *ps = s;
    return more;
}
static void next_line(const char **ps) {
    const char *s = *ps;
    while (*s && *s != '\n') s++;
    if (*s == '\n') s++;
    *ps = s;
}

int bv_maprot_all; /* update 127 (all_maps): disabled maps count too */
/* update 128 (run 148: 'Unable to find MovieClip with export name: event_retropolis_banner' -> crash): a disabled map is only usable when its event
 * banner (IconExportName + "_banner") exists in this version's sc/events.sc (19 banners, read from the unpacked cache) */
int bv_map_banner_ok(const char *icon) {
    static const char *const ok[] = {"event_arcade", "event_bazaar", "event_beach", "event_boss", "event_brawlball_arena", "event_brawlball", "event_canyon",
                                     "event_cny", "event_gemmine", "event_giftshop", "event_lone_star", "event_mortuary", "event_mortuary_showdown", "event_oasis",
                                     "event_robowars", "event_scity", "event_snow", "event_stormvalley", "event_takedown"};
    for (unsigned i = 0; icon && i < sizeof ok / sizeof ok[0]; i++)
        if (!strcmp(icon, ok[i])) return 1;
    return 0;
}
int bv_maps_for_mode(const char *csv, const char *mode, int *rows, int max) {
    const char *s = csv;
    char f[96];
    int c_dis = -1, c_mode = -1, c_icon = -1, col = 0, more = 1;
    while (*s && more) {
        more = field(&s, f, sizeof f);
        if (!strcmp(f, "Disabled")) c_dis = col;
        else if (!strcmp(f, "GameMode")) c_mode = col;
        else if (!strcmp(f, "IconExportName")) c_icon = col;
        col++;
    }
    next_line(&s);
    if (c_mode < 0) return -1;
    next_line(&s);   /* the types line */
    int n = 0, row = 0;
    while (*s) {
        int c = 0, dis = 0, match = 0, named = 0, banner = 0;
        more = 1;
        while (more) {
            more = field(&s, f, sizeof f);
            if (c == 0 && f[0]) named = 1;
            if (c == c_icon && bv_map_banner_ok(f)) banner = 1;
            if (c == c_dis && (f[0] == 't' || f[0] == 'T' || f[0] == '1')) dis = 1;
            if (c == c_mode && !strcmp(f, mode)) match = 1;
            c++;
        }
        next_line(&s);
        /* update 127 (A15 root cause): rows with an empty Name are continuation rows of the row above; the game's table (DataRef rows) skips them.
         * Counting them shifted every map after csv row 75 (row 128 Gem Grab -> client Siege5; Hot Zone row 324 past the client's 296 rows -> NULL) */
        if (!named) continue;
        if (match && (!dis || (bv_maprot_all && banner)) && n < max) rows[n++] = row;
        row++;
    }
    return n;
}

long bv_queue_rotation(long now, int pos, int npos, long period, int *left) {
    if (npos < 1) npos = 1;
    if (period < 1) period = 1;
    /* exact fractions: queue step g starts at ceil(g * period / npos) seconds, so every event lasts exactly `period` (host test) */
    long long g = (long long)now * npos / period;           /* global queue step */
    long long k = (g - pos) >= 0 ? (g - pos) / npos : -((pos - g + npos - 1) / npos);
    long long ahead = ((pos - g - 1) % npos + npos) % npos + 1;   /* steps until this position's next turn */
    long long next = ((g + ahead) * period + npos - 1) / npos;    /* first second of that step */
    if (left) *left = (int)(next - now);
    return (long)k;
}

int bv_queue_mode(long k, int j, int nslots, int npool) {
    if (npool <= 0) return 0;
    long v = k * nslots + j;
    return (int)(((v % npool) + npool) % npool);
}

int bv_rotation(long now, int slot, int nmaps, int *map_index, int *seconds_left, int *event_id) {
    const long period = 17280; /* 4 h 48 min */
    long r = now / period;
    if (seconds_left) *seconds_left = (int)(period - now % period);
    if (event_id) *event_id = (int)((r % 100000L) * 16 + slot);
    if (nmaps <= 0) return -1;
    int idx = (int)((r + (long)slot * 7) % nmaps);
    if (map_index) *map_index = idx;
    return idx;
}
