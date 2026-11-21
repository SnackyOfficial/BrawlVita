/* cycle 8 (E3 / E6): event map rotation (see maprot.c). */
#ifndef BV_MAPROT_H
#define BV_MAPROT_H

/* rows (0-based data rows of locations.csv) of enabled maps whose GameMode equals `mode`; -1 if the header is not understood */
int bv_map_banner_ok(const char *icon); /* update 128 */
int bv_maps_for_mode(const char *csv, const char *mode, int *rows, int max);
/* rotation every 4 h 48 min: index into the slot's map list, seconds until the next map, a unique event id per rotation and slot */
int bv_rotation(long now, int slot, int nmaps, int *map_index, int *seconds_left, int *event_id);

/* update 144 (req. 17): ONE global rotation queue. `npos` events take turns: every period / npos seconds the next event in queue order
 * changes map (event 1, then event 2, ...), so each event still lasts `period` seconds but they never all change at once.
 * Returns the rotation count k of queue position `pos` at `now` (k grows by 1 each time that event rotates); *left = seconds until its next rotation. */
long bv_queue_rotation(long now, int pos, int npos, long period, int *left);
/* mode of rotating slot j (0..nslots-1) at its rotation count k from a pool of npool modes (npool > nslots): slots next to each other in the queue never
 * show the same mode at the same time (proof in tests/host_maprot_test.c) */
int bv_queue_mode(long k, int j, int nslots, int npool);
#endif
