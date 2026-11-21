/* Android NDK asset and native-window functions over plain files below ux0:data/BrawlVita/assets/.
 * The files are the owner's own, copied from the APK's assets/ folder by tools/prep_m4.py.
 * 64-bit offsets (off64_t) are passed as AAPCS 64-bit integers; the layout matches int64_t in both ABIs.
 */
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>

#include "../loader/log.h"
#include "../loader/stats.h"
#include "shim.h"

#define ASSET_DIR BV_DATA_DIR "/assets/"
#include "../loader/botnames.h"
#include "../loader/csvpatch.h"
#include "../loader/maprot.h"
#include "../loader/offertitles.h"
int bv_bot_names; /* config bot_names (cycle 8 G6) */
/* update 111 (A13): config csv_patch=<asset path>,<row>,<column>,<value> */
#define BV_CSV_PATCH_MAX 64   /* update 174: 24 -> 64 (Snowy theme override needs 32) */
int bv_n_csv_patch;
int bv_all_maps; /* update 127 config all_maps */
int bv_offer_tids; /* update 129 config offer_tids (B11) */
int bv_settings_texts = 1; /* update 144 (req. 13): with settings_clean, the two captions without an instance name get an empty text */
int bv_no_highres_battle; /* update 134 config no_highres_battle (A14) */
extern uint32_t bv_battle_kind_addr; /* loader/touch_vita.c */
extern volatile int bv_battle_left __attribute__((weak));
char bv_csv_patch_cfg[BV_CSV_PATCH_MAX][4][64];
uint32_t bv_botname_seed;
int bv_n_asset_alias;
char bv_asset_alias[16][2][96];   /* update 174 */
int bv_theme_fallback = 1;        /* update 175: built-in winter fallback (download-only Snowy files) */
static const char *const k_builtin_alias[][2] = {{"sc3d/bgr_snowy_ground.scw", "sc3d/bgr_canyon_ground.scw"}, {"sc3d/bgr_snowy.scw", "sc3d/bgr_canyon.scw"}};
static int csv_patches_for(const char *name) {
    for (int i = 0; i < bv_n_csv_patch; i++)
        if (!strcmp(bv_csv_patch_cfg[i][0], name)) return 1;
    return 0;
}

/* Assets are read with sceIo directly (update 27). Through newlib's fread they came in at ~3.5 MB/s (run 7: 7642 KB in 2170 ms for one read). */
typedef struct {
    SceUID fd;
    int64_t size, pos;
    char name[96];
    uint8_t *mem; /* cycle 8 (G6): served from memory (texts.csv with bot names) */
} asset;

typedef struct {
    DIR *d;
    char last[256];
} asset_dir;

volatile uint32_t bv_st_asset_opens, bv_st_asset_reads, bv_st_asset_bytes, bv_st_asset_us;
char bv_st_last_asset[96];
volatile uint32_t bv_st_last_asset_ms;

/* Startup check of the asset folder, for the log: a folder that was copied again (FTP, a different tool) often arrives nested one level too deep, with files missing,
 * or with files of size 0, and the game then only says "File not found fingerprint.json". */
/* The game looks for a hot-update override first: <data>/update/fingerprint.json, and uses it instead of the one in assets. This port never downloads updates, so
 * the folder only ever holds something a previous run left behind (a file cut short by a crash makes every later start fail with "File not found fingerprint.json"
 * and then a NULL crash: log of 2026-10-28). It is moved aside, never deleted. */
static void quarantine_update_dir(void) {
    static const char dir[] = BV_DATA_DIR "/fs/data/data/com.supercell.brawlstars/update";
    struct stat st;
    if (stat(dir, &st) != 0) {
        bv_log("update folder: none (the game uses the assets copy of fingerprint.json)");
        return;
    }
    char fp[220];
    snprintf(fp, sizeof fp, "%s/fingerprint.json", dir);
    if (stat(fp, &st) == 0) bv_log("update folder: %s exists, %lld bytes (%s)", fp, (long long)st.st_size, st.st_size == 0 ? "EMPTY" : "left behind by an earlier run");
    else bv_log("update folder: %s exists (no fingerprint.json in it)", dir);
    /* The new name must not exist yet. It used to be "_stale_<ms since start>", and with the faster boot of update 28+ the same number came up twice:
     * run 16 (2026-11-11) "could NOT be moved aside (error 0x80010011)" (= exists), then the game read the left-over file, could not map it and crashed
     * at 8 s after "File not found fingerprint.json". Now: the wall-clock time, plus a counter if that name is taken too. */
    char to[240];
    int r = -1;
    for (int k = 0; k < 50; k++) {
        if (k == 0) snprintf(to, sizeof to, "%s_stale_%u", dir, (unsigned)time(NULL));
        else snprintf(to, sizeof to, "%s_stale_%u_%d", dir, (unsigned)time(NULL), k);
        r = sceIoRename(dir, to);
        if (r >= 0 || (unsigned)r != 0x80010011u) break; /* done, or an error other than "exists" */
    }
    if (r >= 0) bv_log("update folder: moved aside to %s", to);
    else bv_log("update folder: could NOT be moved aside (error 0x%08x): delete ux0:data/BrawlVita/fs/data/data/com.supercell.brawlstars/update in VitaShell", (unsigned)r);
}

void bv_assets_selfcheck(void) {
    quarantine_update_dir();
    DIR *d = opendir(ASSET_DIR);
    if (!d) {
        bv_log("assets: CANNOT OPEN " ASSET_DIR " (errno %d): the game's files must be directly inside ux0:data/BrawlVita/assets/", errno);
        return;
    }
    int n = 0;
    char first[160] = "";
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (n < 14 && strlen(first) + strlen(e->d_name) + 3 < sizeof first) {
            strcat(first, n ? ", " : "");
            strcat(first, e->d_name);
        }
        n++;
    }
    closedir(d);
    bv_log("assets: " ASSET_DIR " holds %d entries: %s%s", n, first, n > 14 ? ", ..." : "");
    static const char *const key[] = {"fingerprint.json", "defaultEnglish.strings", "cacert.pem", "deferred.yml", "supplierconfig.json", "particle_emitters.json"};
    for (unsigned i = 0; i < sizeof key / sizeof key[0]; i++) {
        char path[200];
        struct stat st;
        snprintf(path, sizeof path, ASSET_DIR "%s", key[i]);
        if (stat(path, &st) != 0) bv_log("assets:   %-24s MISSING (errno %d)", key[i], errno);
        else if (st.st_size == 0) bv_log("assets:   %-24s EMPTY (0 bytes): copy this file again", key[i]);
        else bv_log("assets:   %-24s %lld bytes", key[i], (long long)st.st_size);
    }
}

static int g_manager; /* the pointer the game gets for its AssetManager object */
static int g_window;  /* ... and for the ANativeWindow of the surface */

static void *w_AAssetManager_fromJava(void *env, void *obj) {
    (void)env; (void)obj;
    return &g_manager;
}

static void *w_AAssetManager_open(void *mgr, const char *name, int mode) {
    (void)mgr; (void)mode;
    char path[320];
    if (!name || bv_has_dotdot(name) || snprintf(path, sizeof path, ASSET_DIR "%s", name) >= (int)sizeof path) {
        BV_TRACE(1, "AAsset: refused name %.100s", name ? name : "(null)");
        return NULL;
    }
    /* update 134 (A14 part 2, run 151): Showdown died at 790.7 s while the intro loaded ten brawlers' sc3d/background/*_tex_highres.ktx with
     * vram 3-6 MB / RAM 9 MB free (the pre-bake itself survived thanks to gc_now). config no_highres_battle=1: during a battle these files are
     * reported missing, so the game takes the normal-resolution texture - the same fallback it already uses for the many highres files that
     * do not exist. Home / shop / pass previews keep the high-res ones. */
    if (bv_no_highres_battle && strstr(name, "_tex_highres") && bv_battle_kind_addr && *(volatile int *)(uintptr_t)bv_battle_kind_addr != 0 &&
        !(&bv_battle_left && bv_battle_left)) {
        static unsigned said;
        if (said++ < 8) bv_log("AAsset: %s refused in battle (no_highres_battle=1): normal-resolution texture instead", name);
        return NULL;
    }
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {   /* update 174: asset_alias=<missing>,<stand-in> (download-only files such as the winter map ground, run 206: black battle ground) */
        for (int i = 0; i < bv_n_asset_alias; i++)
            if (!strcmp(bv_asset_alias[i][0], name) && snprintf(path, sizeof path, ASSET_DIR "%s", bv_asset_alias[i][1]) < (int)sizeof path) {
                fd = sceIoOpen(path, SCE_O_RDONLY, 0);
                static unsigned said;
                if (said++ < 16) bv_log("AAsset: %s missing -> %s instead (asset_alias)%s", name, bv_asset_alias[i][1], fd < 0 ? " - also missing" : "");
                break;
            }
    }
    if (fd < 0 && bv_theme_fallback)   /* update 175: built-in stand-ins for the download-only winter ground / environment */
        for (unsigned i = 0; i < sizeof k_builtin_alias / sizeof k_builtin_alias[0]; i++)
            if (!strcmp(k_builtin_alias[i][0], name) && snprintf(path, sizeof path, ASSET_DIR "%s", k_builtin_alias[i][1]) < (int)sizeof path) {
                fd = sceIoOpen(path, SCE_O_RDONLY, 0);
                static unsigned said;
                if (said++ < 8) bv_log("AAsset: %s missing -> %s instead (theme_fallback)", name, k_builtin_alias[i][1]);
                break;
            }
    if (fd < 0 && bv_theme_fallback && !strncmp(name, "sc3d/bgr_", 9) && strlen(name) > 13 && !strcmp(name + strlen(name) - 4, ".scw") && !strstr(name, "_decos")) {
        /* update 177: every download-only map ground / environment (run 213: Old Town) falls back to the canyon one */
        const char *alt = strstr(name, "_ground.scw") ? "sc3d/bgr_canyon_ground.scw" : "sc3d/bgr_canyon.scw";
        if (strcmp(name, alt) && snprintf(path, sizeof path, ASSET_DIR "%s", alt) < (int)sizeof path) {
            fd = sceIoOpen(path, SCE_O_RDONLY, 0);
            static unsigned said;
            if (said++ < 8) bv_log("AAsset: %s missing -> %s instead (theme_fallback)", name, alt);
        }
    }
    if (fd < 0) {
        BV_TRACE(1, "AAsset: %s not found", name);
        return NULL;
    }
    asset *a = (asset *)calloc(1, sizeof *a);
    if (!a) { sceIoClose(fd); return NULL; }
    a->size = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (a->size < 0) a->size = 0;
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    a->fd = fd;
    a->pos = 0;
    snprintf(a->name, sizeof a->name, "%s", name);
    /* cycle 8 (G6) config bot_names=1: texts.csv with TID_BOT_1..9 replaced by player-style names (loader/botnames.c), served from memory */
    int want_bots = bv_bot_names && !strcmp(name, "localization/texts.csv");
    int want_tids = bv_offer_tids && !strcmp(name, "localization/texts.csv");   /* update 129 */
    int want_maps = bv_all_maps && !strcmp(name, "csv_logic/locations.csv");   /* update 127 */
    int want_set = bv_settings_texts && !strcmp(name, "localization/texts.csv");   /* update 144 */
    int want_theme = bv_theme_fallback && !strcmp(name, "csv_logic/location_themes.csv");   /* update 175 */
    if ((want_bots || want_tids || want_maps || want_set || want_theme || csv_patches_for(name)) && a->size > 0 && a->size < (8 << 20)) {
        char *raw = (char *)malloc((size_t)a->size);
        if (raw && sceIoRead(fd, raw, (SceSize)a->size) == (int)a->size) {
            size_t ol = (size_t)a->size;
            int rep = 0;
            char *cur = raw;
            raw = NULL;
            if (want_theme) {   /* update 175: the Snowy theme takes the Default theme's props (its own files are download-only) */
                char *p = bv_csv_copy_row(cur, ol, "Snowy", "Default", &ol);
                if (p) { free(cur); cur = p; }
                bv_log("AAsset: %s: theme_fallback Snowy <- Default %s", name, p ? "applied" : "NOT FOUND");
            }
            if (want_bots) {
                bv_botname_seed = (uint32_t)time(NULL) / 3600u;   /* update 170: the battle log saves the same names */
                char *p = bv_botnames_patch(cur, ol, &ol, bv_botname_seed, &rep);
                if (p) {
                    free(cur);
                    cur = p;
                    bv_log("AAsset: %s: %d bot names (first: %s)", name, rep, bv_botname((uint32_t)time(NULL) / 3600u, 1));
                }
            }
            if (want_tids) {   /* update 129 (B11): TID_BV_OFFER_<n> rows for the shop offer titles */
                char *p = bv_offer_tids_patch(cur, ol, &ol);
                if (p) { free(cur); cur = p; bv_log("AAsset: %s: offer title texts appended (offer_tids=1)", name); }
            }
            if (want_maps) {   /* update 127 (all_maps): the rotated modes' maps all enabled */
                static const char *const modes[] = {"CoinRush", "BattleRoyale", "BattleRoyaleTeam", "LaserBall", "AttackDefend", "BountyHunter", "RoboWars", "KingOfHill", "Raid"};
                int cnt = 0;
                char *p = bv_csv_enable_modes(cur, ol, "GameMode", modes, (int)(sizeof modes / sizeof modes[0]), "Disabled", &ol, &cnt, "IconExportName", bv_map_banner_ok);   /* update 128 */
                if (p) { free(cur); cur = p; }
                bv_log("AAsset: %s: all_maps: %d disabled maps of the rotated modes enabled", name, cnt);
            }
            if (want_set) {   /* update 144 (req. 13): run 139 left the friend-request blocking and sign-in captions (their rows are hidden, the captions are not) */
                /* update 146 (user T13): event cards say NEW MAP, not NEW EVENT */
                static const char *const tids[] = {"TID_SETTINGS_BLOCK_FRIEND_REQUESTS", "TID_SETTINGS_GOOGLE", "TID_MODES_NEW_EVENT", "TID_EVENTS_NEW_EVENT",
                                                   "TID_MODES_NEW_EVENTS", "TID_NEW_EVENT_IN", "TID_NEW_EVENT_NOW", "TID_HOMEPAGE_GUIDE_NEW_EVENT",
                                                   "TID_CONNECTING_TO_SERVER"};   /* update 167: loading notice */
                static const char *const vals[] = {" ", " ", "NEW MAP!", "New Map!", "NEW MAPS!", "New Map in: <TIME>", "NEW MAP AVAILABLE!", "New Map Unlocked!",
                                            "Loading your profile..."};
                for (unsigned i = 0; i < sizeof tids / sizeof tids[0]; i++) {
                    int done = 0;
                    char *p = bv_csv_patch(cur, ol, tids[i], "EN", vals[i], &ol, &done);
                    if (p) { free(cur); cur = p; }
                    bv_log("AAsset: %s: text %s -> '%s' %s", name, tids[i], vals[i], done ? "set" : "NOT FOUND");
                }
            }
            for (int i = 0; i < bv_n_csv_patch; i++) {
                if (strcmp(bv_csv_patch_cfg[i][0], name)) continue;
                int done = 0;
                char *p = bv_csv_patch(cur, ol, bv_csv_patch_cfg[i][1], bv_csv_patch_cfg[i][2], bv_csv_patch_cfg[i][3], &ol, &done);
                bv_log("AAsset: %s: csv_patch %s.%s = %s %s", name, bv_csv_patch_cfg[i][1], bv_csv_patch_cfg[i][2], bv_csv_patch_cfg[i][3], done ? "applied" : "NOT FOUND");
                if (p) {
                    free(cur);
                    cur = p;
                }
            }
            a->mem = (uint8_t *)cur;
            a->size = (int64_t)ol;
        }
        free(raw);
        sceIoLseek(fd, 0, SCE_SEEK_SET);
    }
    __sync_fetch_and_add(&bv_st_asset_opens, 1);
    snprintf(bv_st_last_asset, sizeof bv_st_last_asset, "%s", name);
    bv_st_last_asset_ms = (uint32_t)(sceKernelGetProcessTimeWide() / 1000);
    BV_TRACE(2, "AAsset: open %s (%lld bytes)", name, (long long)a->size);
    if (a->size == 0) BV_TRACE(1, "AAsset: WARNING %s is an EMPTY file (0 bytes): the game will report it as not found", name);
    else if (bv_st_asset_opens <= 40) BV_TRACE(1, "AAsset: open %s (%lld bytes)", name, (long long)a->size);
    return a;
}

static void w_AAsset_close(void *p) {
    asset *a = (asset *)p;
    if (!a) return;
    sceIoClose(a->fd);
    free(a->mem);
    free(a);
}
static int w_AAsset_read(void *p, void *buf, size_t n) {
    asset *a = (asset *)p;
    if (a->mem) {   /* cycle 8: memory-backed asset */
        int64_t left = a->size - a->pos;
        size_t k = left <= 0 ? 0 : (n < (size_t)left ? n : (size_t)left);
        memcpy(buf, a->mem + a->pos, k);
        a->pos += (int64_t)k;
        return (int)k;
    }
    uint64_t t0 = sceKernelGetProcessTimeWide();
    size_t got = 0;
    while (got < n) { /* sceIoRead may return less than asked: keep going until n bytes or the end */
        int r = sceIoRead(a->fd, (uint8_t *)buf + got, (SceSize)(n - got));
        if (r <= 0) break;
        got += (size_t)r;
    }
    a->pos += (int64_t)got;
    __sync_fetch_and_add(&bv_st_asset_us, (uint32_t)(sceKernelGetProcessTimeWide() - t0));
    __sync_fetch_and_add(&bv_st_asset_reads, 1);
    __sync_fetch_and_add(&bv_st_asset_bytes, (uint32_t)got);
    return (int)got;
}
static int64_t w_AAsset_getLength64(void *p) { return ((asset *)p)->size; }
static int64_t w_AAsset_getRemainingLength64(void *p) {
    asset *a = (asset *)p;
    return a->size - a->pos;
}
/* AAsset_seek semantics: SEEK_SET / SEEK_CUR / SEEK_END (0 / 1 / 2, the same numbers as SCE_SEEK_*); -1 for a position outside the file */
static int64_t asset_seek(asset *a, int64_t off, int whence) {
    int64_t to = whence == 0 ? off : whence == 1 ? a->pos + off : whence == 2 ? a->size + off : -1;
    if (to < 0 || to > a->size) return -1;
    if (!a->mem && sceIoLseek(a->fd, to, SCE_SEEK_SET) < 0) return -1;
    a->pos = to;
    return to;
}
static int64_t w_AAsset_seek64(void *p, int64_t off, int whence) { return asset_seek((asset *)p, off, whence); }
/* The 32-bit forms (off_t): FMOD reads its sound banks through AAssetManager_open / AAsset_read / AAsset_seek, looked up with dlsym (console log 2026-11-11:
 * "dlsym(0x9, AAsset_seek) -> not found" once FMOD had a real output). */
static long w_AAsset_seek(void *p, long off, int whence) { return (long)asset_seek((asset *)p, off, whence); }
static long w_AAsset_getLength(void *p) { return (long)((asset *)p)->size; }
static long w_AAsset_getRemainingLength(void *p) {
    asset *a = (asset *)p;
    return (long)(a->size - a->pos);
}
static int w_AAsset_openFileDescriptor64(void *p, int64_t *start, int64_t *length) {
    asset *a = (asset *)p;
    (void)start; (void)length;
    BV_TRACE(1, "AAsset_openFileDescriptor64(%s) -> -1 (compressed/unsupported)", a ? a->name : "?");
    return -1;
}

static void *w_AAssetManager_openDir(void *mgr, const char *name) {
    (void)mgr;
    char path[320];
    if (!name || bv_has_dotdot(name) || snprintf(path, sizeof path, ASSET_DIR "%s", name) >= (int)sizeof path) return NULL;
    DIR *d = opendir(path);
    if (!d) return NULL;
    asset_dir *ad = (asset_dir *)calloc(1, sizeof *ad);
    if (!ad) { closedir(d); return NULL; }
    ad->d = d;
    return ad;
}
static const char *w_AAssetDir_getNextFileName(void *p) {
    asset_dir *ad = (asset_dir *)p;
    struct dirent *e;
    while ((e = readdir(ad->d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(ad->last, sizeof ad->last, "%s", e->d_name);
        return ad->last;
    }
    return NULL;
}
static void w_AAssetDir_close(void *p) {
    asset_dir *ad = (asset_dir *)p;
    if (!ad) return;
    closedir(ad->d);
    free(ad);
}

static void *w_ANativeWindow_fromSurface(void *env, void *surface) {
    (void)env; (void)surface;
    BV_TRACE(1, "ANativeWindow_fromSurface -> fake window");
    return &g_window;
}
static void w_ANativeWindow_release(void *w) { (void)w; }

const bv_shim bv_shims_asset[] = {
    SHIM(AAssetManager_fromJava), SHIM(AAssetManager_open), SHIM(AAssetManager_openDir), SHIM(AAsset_close), SHIM(AAsset_read),
    SHIM(AAsset_getLength64), SHIM(AAsset_getRemainingLength64), SHIM(AAsset_seek64), SHIM(AAsset_openFileDescriptor64),
    SHIM(AAsset_seek), SHIM(AAsset_getLength), SHIM(AAsset_getRemainingLength),
    SHIM(AAssetDir_getNextFileName), SHIM(AAssetDir_close), SHIM(ANativeWindow_fromSurface), SHIM(ANativeWindow_release),
    SHIM_END};
