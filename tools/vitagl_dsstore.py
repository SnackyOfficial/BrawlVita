"""update 172 (icons fix B, notes/libg_map/icons_fix.md): keep the DISPLAY depth/stencil surface across scenes. vitaGL (no STORE_DEPTH_STENCIL)
loses the display stencil at every render-to-texture excursion, so the shop / Brawl Pass tile composite fails its stencil clip (blank tiles), and
turning the test off (fix A part b) breaks the battle ground and lets the tiles spill out of their frames. With vgl_bv_ds_store = 1 (set by the
loader before vglInitExtended) the display surface gets what STORE_DEPTH_STENCIL would give it: mask-bit init + force load/store. Other
surfaces (FBO depth buffers) are unchanged. Usage: python -I tools/vitagl_dsstore.py <vitaGL checkout>. Idempotent."""
import sys

p = sys.argv[1] + "/source/gxm.c"
t = open(p, encoding="utf-8").read()
MARK = "Brawl Vita (update 172): vgl_bv_ds_store"
if MARK in t:
    if "vgl_bv_ds_force" not in t:
        t = t.rstrip("\n") + """

/* Brawl Vita (update 174): switch the display surface's force load / store at run time (the loader keeps it off in battles) */
void vgl_bv_ds_force(int on) {
	sceGxmDepthStencilSurfaceSetForceLoadMode(&gxm_depth_stencil_surface, on ? SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED : SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED);
	sceGxmDepthStencilSurfaceSetForceStoreMode(&gxm_depth_stencil_surface, on ? SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED : SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED);
}
"""
        open(p, "w", encoding="utf-8", newline="\n").write(t)
        print("gxm.c ds_force added")
    else:
        print("gxm.c ds_store already patched")
    sys.exit(0)


def rep(a, b):
    global t
    assert t.count(a) == 1, (a[:70], t.count(a))
    t = t.replace(a, b)


rep("void init_depth_stencil_buffer(uint32_t w, uint32_t h, SceGxmDepthStencilSurface *surface, GLboolean has_stencil) {",
    "/* " + MARK + " */\nint vgl_bv_ds_store = 0;\nstatic int bv_force_store = 0;\n"
    "void init_depth_stencil_buffer(uint32_t w, uint32_t h, SceGxmDepthStencilSurface *surface, GLboolean has_stencil) {")
rep("""#ifdef STORE_DEPTH_STENCIL
	// Initializing mask update bit to 1
	vgl_memset(depth_buffer, 0x80, 4 * depth_stencil_samples);
#endif""", """#ifdef STORE_DEPTH_STENCIL
	// Initializing mask update bit to 1
	vgl_memset(depth_buffer, 0x80, 4 * depth_stencil_samples);
#else
	if (bv_force_store)
		vgl_memset(depth_buffer, 0x80, 4 * depth_stencil_samples);
#endif""")
rep("""#ifdef STORE_DEPTH_STENCIL
	sceGxmDepthStencilSurfaceSetForceLoadMode(surface, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
	sceGxmDepthStencilSurfaceSetForceStoreMode(surface, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
#endif""", """#ifdef STORE_DEPTH_STENCIL
	sceGxmDepthStencilSurfaceSetForceLoadMode(surface, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
	sceGxmDepthStencilSurfaceSetForceStoreMode(surface, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
#else
	if (bv_force_store) {
		sceGxmDepthStencilSurfaceSetForceLoadMode(surface, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
		sceGxmDepthStencilSurfaceSetForceStoreMode(surface, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
	}
#endif""")
rep("""	init_depth_stencil_buffer(DISPLAY_WIDTH, DISPLAY_HEIGHT, &gxm_depth_stencil_surface, GL_TRUE);
}""", """	bv_force_store = vgl_bv_ds_store;
	init_depth_stencil_buffer(DISPLAY_WIDTH, DISPLAY_HEIGHT, &gxm_depth_stencil_surface, GL_TRUE);
	bv_force_store = 0;
}""")
t = t.rstrip("\n") + """

/* Brawl Vita (update 174): switch the display surface's force load / store at run time (the loader keeps it off in battles) */
void vgl_bv_ds_force(int on) {
	sceGxmDepthStencilSurfaceSetForceLoadMode(&gxm_depth_stencil_surface, on ? SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED : SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED);
	sceGxmDepthStencilSurfaceSetForceStoreMode(&gxm_depth_stencil_surface, on ? SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED : SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED);
}
"""
open(p, "w", encoding="utf-8", newline="\n").write(t)
print("gxm.c ds_store patched")
