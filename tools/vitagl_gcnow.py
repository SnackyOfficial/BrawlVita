"""update 113 (ISSUES A14, Showdown crash, run 128): add vgl_bv_gc_now() to vitaGL source/gxm.c. Run after tools/vitagl_rtpurge.py
(tools/build_vitagl.sh does all of them).
Evidence (run 128 log_1.txt, update 109, user: "Showdown crashes"): the Showdown arena pre-bake ran inside ONE frame of 57557 ms
(275 s -> 333 s, 28 rt_purge calls). During that frame no asset was opened, yet the vitaGL pools drained steadily (RAM 11 -> 0 MB,
phycont 25 -> 0 MB, vram 0 MB) and they came back the moment the frame ended (333.4 s: vram 5, RAM 15, phycont 13 MB). vitaGL never frees
replaced buffer / texture memory at once while the GPU may still use it: glBufferData / glDeleteTextures put the old block on the
frame purge list (mark_as_dirty) and the garbage collector frees it only after a swap. One 57 s frame therefore keeps every replaced block,
and the list (FRAME_PURGE_LIST_SIZE 16384 per frame) can overflow into the neighbour lists. 9 s later the RAM pool bookkeeping was broken
(GPU MEMORY TRIPWIRE: 2096661 KB free of 68608 KB) and the game died at 344.6 s.
vgl_bv_gc_now() takes every garbage-collector token (the GC thread finishes what it has), drains the GPU (sceGxmFinish), frees every
queued block and destroys every queued render target, then gives the tokens back. The loader calls it from its sceGxmBeginScene wrap (no
scene is open there) when a frame has queued many blocks and the pools run low. Returns the number of blocks freed.
Usage: python -I tools/vitagl_gcnow.py <vitaGL checkout>.  Idempotent."""
import sys

p = sys.argv[1] + "/source/gxm.c"
t = open(p, encoding="utf-8").read()
MARK = "Brawl Vita (update 113): vgl_bv_gc_now"
if MARK in t:
    print("gxm.c gc_now already patched")
    sys.exit(0)
t = t.rstrip("\n") + """

/* """ + MARK + """: run the garbage collector now, inside a long frame (see tools/vitagl_gcnow.py). */
int vgl_bv_gc_now(void) {
#ifndef HAVE_SINGLE_THREADED_GC
	sceKernelWaitSema(gc_mutex[1], FRAME_PURGE_FREQ, NULL);
#endif
	sceGxmFinish(gxm_context);
	int n = 0;
	for (int f = 0; f < FRAME_PURGE_FREQ; f++) {
		for (int i = 0; i < FRAME_PURGE_LIST_SIZE; i++) {
			if (frame_purge_list[f][i]) {
				vgl_free(frame_purge_list[f][i]);
				frame_purge_list[f][i] = NULL;
				n++;
			}
		}
		for (int i = 0; i < FRAME_PURGE_RENDERTARGETS_LIST_SIZE; i++) {
			if (frame_rt_purge_list[f][i]) {
				sceGxmDestroyRenderTarget(frame_rt_purge_list[f][i]);
				frame_rt_purge_list[f][i] = NULL;
			}
		}
	}
	frame_elem_purge_idx = 0;
	frame_rt_purge_idx = 0;
#ifndef HAVE_SINGLE_THREADED_GC
	sceKernelSignalSema(gc_mutex[1], FRAME_PURGE_FREQ);
#endif
	return n;
}
"""
open(p, "w", encoding="utf-8", newline="").write(t)
print("gxm.c patched (vgl_bv_gc_now)")
