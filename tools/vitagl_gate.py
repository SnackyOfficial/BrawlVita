"""update 87 (ISSUES H1): runtime switch for the vitaGL shader cache. Run AFTER tools/vitagl_paircache.py (tools/build_vitagl.sh does both).
The cache is read and written only while the global vgl_bv_shader_cache != 0; gfx/gl_init.c sets it from config vgl_cache (default 0), so one
build runs with or without the cache. Off = stock vitaGL behaviour (no cache file is read or written).
Usage: python -I tools/vitagl_gate.py <vitaGL checkout>.  Idempotent."""
import sys

p = sys.argv[1] + "/source/custom_shaders.c"
t = open(p, encoding="utf-8").read()
GATE = "Brawl Vita (update 87): runtime switch vgl_bv_shader_cache"
if GATE in t:
    print("custom_shaders.c gate already patched")
    sys.exit(0)
reps = [
    ("char vgl_shader_cache_path[256];\n", "char vgl_shader_cache_path[256];\nint vgl_bv_shader_cache = 0; /* " + GATE + " */\n"),
    ("\t\tif (pair_ok) {\n", "\t\tif (pair_ok && vgl_bv_shader_cache) {\n"),
    ("\t\tif (pair_ok && !p->fshader->prog) {\n", "\t\tif (pair_ok && vgl_bv_shader_cache && !p->fshader->prog) {\n"),
    ("\tSceUID f = sceIoOpen(cache_fname, SCE_O_CREAT | SCE_O_WRONLY | SCE_O_TRUNC, 0777);\n\tsize_t sz;\n\tvoid *buf = serialize_shader(NULL, &sz, s, save_bindings);\n"
     "\tsceIoWrite(f, buf, sz);\n\tsceIoClose(f);\n\tvgl_free(buf);\n",
     "\tif (vgl_bv_shader_cache) {\n\tSceUID f = sceIoOpen(cache_fname, SCE_O_CREAT | SCE_O_WRONLY | SCE_O_TRUNC, 0777);\n\tsize_t sz;\n"
     "\tvoid *buf = serialize_shader(NULL, &sz, s, save_bindings);\n\tsceIoWrite(f, buf, sz);\n\tsceIoClose(f);\n\tvgl_free(buf);\n\t}\n"),
    ("\tSceUID f = sceIoOpen(fname, SCE_O_RDONLY, 0777);\n\tif (f >= 0) {\n\t\tvgl_free(s->source);",
     "\tSceUID f = vgl_bv_shader_cache ? sceIoOpen(fname, SCE_O_RDONLY, 0777) : -1;\n\tif (f >= 0) {\n\t\tvgl_free(s->source);"),
]
for a, b in reps:
    if t.count(a) != 1:
        print("gate pattern not found once:", a[:80])
        sys.exit(1)
    t = t.replace(a, b)
open(p, "w", encoding="utf-8", newline="").write(t)
print("custom_shaders.c gate patched (vgl_bv_shader_cache)")
