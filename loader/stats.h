/* Counters for the per-second status line (levels 2 and 3): where does the time of the game's loading phase go? Written by the asset and GL shims (any thread),
 * read by the status line of loader/gameapp.c. Plain 32-bit counters, updated with __sync_fetch_and_add; they only inform, nothing depends on them. */
#ifndef BV_STATS_H
#define BV_STATS_H

#include <stdint.h>

extern volatile uint32_t bv_st_asset_opens, bv_st_asset_reads, bv_st_asset_bytes, bv_st_asset_us; /* AAssetManager_open calls, AAsset_read calls, bytes, time inside fread */
extern volatile uint32_t bv_st_etc1_n, bv_st_etc1_bytes, bv_st_etc1_us;                           /* ETC1 -> DXT1 uploads: count, compressed bytes, time (transcode + upload) */
extern char bv_st_last_asset[96];                                                                 /* the asset opened last (racy copy, for display only) */
extern volatile uint32_t bv_st_last_asset_ms;                                                     /* process time (ms) at which it was opened */

#endif
