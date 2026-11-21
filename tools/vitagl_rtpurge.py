"""update 95 (ISSUES A5): add vgl_bv_purge_rts_now() to vitaGL source/gxm.c. Run after tools/vitagl_paircache.py and tools/vitagl_gate.py
(tools/build_vitagl.sh does all three).
Evidence (run 108, shims/shim_gxm.c wraps): Play's arena pre-bake re-creates framebuffer render targets inside ONE long frame; vitaGL only queues
the old ones (frame_rt_purge_list) and destroys them a few frames later, so they pile up: sceGxmCreateRenderTarget #52 failed with 0x805b0027
while 48 render targets were alive, vitaGL then began a scene with a NULL target (sceGxmBeginScene -> 0x805b0004) and the game hung.
vgl_bv_purge_rts_now() drains the GPU (sceGxmFinish) and destroys every queued render target at once; the loader calls it from its
sceGxmCreateRenderTarget wrap when creation fails and then retries. Safe there: scene_reset() ends the previous scene before it asks for a
render target, and queued targets are no longer attached to any framebuffer.
Usage: python -I tools/vitagl_rtpurge.py <vitaGL checkout>.  Idempotent."""
import sys

p = sys.argv[1] + "/source/gxm.c"
t = open(p, encoding="utf-8").read()
MARK = "Brawl Vita (update 95): vgl_bv_purge_rts_now"
if MARK in t:
    print("gxm.c rt purge already patched")
    sys.exit(0)
t = t.rstrip("\n") + """

/* """ + MARK + """: destroy every render target waiting in the purge lists now (GPU drained first).
 * Called by the loader when sceGxmCreateRenderTarget runs out of render targets inside one long frame; returns how many were destroyed. */
int vgl_bv_purge_rts_now(void) {
	sceGxmFinish(gxm_context);
	int n = 0;
	for (int f = 0; f < FRAME_PURGE_FREQ; f++) {
		for (int i = 0; i < FRAME_PURGE_RENDERTARGETS_LIST_SIZE; i++) {
			if (frame_rt_purge_list[f][i]) {
				sceGxmDestroyRenderTarget(frame_rt_purge_list[f][i]);
				frame_rt_purge_list[f][i] = NULL;
				n++;
			}
		}
	}
	frame_rt_purge_idx = 0;
	return n;
}
"""
open(p, "w", encoding="utf-8", newline="").write(t)
print("gxm.c patched (vgl_bv_purge_rts_now)")
