"""ISSUES H1 (cycle 4): patch vitaGL 2bdbe89 source/custom_shaders.c so the shader cache keys BOTH files of a program by the vertex + fragment PAIR.

Why (static reading of vitaGL, VGL_MODE_POSTPONED = the default): glLinkProgram translates the two GLSL shaders together
(glsl_translator_set_process) and they share one semantic bindings map (varyings / attribute registers). The stock cache stores each shader under
the hash of ITS OWN source, so a vertex shader linked with two different fragment shaders comes back from the cache with the bindings of the
first pairing (run 65: cached battle shaders drew broken geometry). With a pair key a hit always restores a vertex + fragment couple that was
compiled together. If one of the two is already compiled (a shader object reused), nothing is looked up (normal compile).
Usage: python -I tools/vitagl_paircache.py <vitaGL checkout>.  Idempotent; changes nothing else."""
import sys

p = sys.argv[1] + "/source/custom_shaders.c"
s = open(p, encoding="utf-8").read()
MARK = "Brawl Vita (ISSUES H1): pair-keyed shader cache"
if MARK in s:
    print("custom_shaders.c already patched")
    sys.exit(0)

old_v = """		if (!p->vshader->prog) {
			uint64_t hash = XXH3_64bits(p->vshader->source, p->vshader->size);
			uint8_t subdir_hash = hash >> 56;
			sprintf(vert_fname, "%s/v/%02X/%llX.gxp", vgl_shader_cache_path, subdir_hash, hash);
			SceUID f = sceIoOpen(vert_fname, SCE_O_RDONLY, 0777);"""
new_v = """		/* """ + MARK + """: both files are named by the hash of the vertex + fragment pair */
		GLboolean pair_ok = !p->vshader->prog && !p->fshader->prog && p->vshader->source && p->fshader->source;
		uint64_t pair_hash = pair_ok ? (XXH3_64bits(p->vshader->source, p->vshader->size) ^ (XXH3_64bits(p->fshader->source, p->fshader->size) * 0x9E3779B97F4A7C15ULL)) : 0;
		sprintf(vert_fname, "%s/v/%02X/P%llX.gxp", vgl_shader_cache_path, (uint8_t)(pair_hash >> 56), pair_hash);
		sprintf(frag_fname, "%s/f/%02X/P%llX.gxp", vgl_shader_cache_path, (uint8_t)(pair_hash >> 56), pair_hash);
		if (pair_ok) {
			SceUID f = sceIoOpen(vert_fname, SCE_O_RDONLY, 0777);"""
old_f = """		if (!p->fshader->prog) {
			uint64_t hash = XXH3_64bits(p->fshader->source, p->fshader->size);
			uint8_t subdir_hash = hash >> 56;
			sprintf(frag_fname, "%s/f/%02X/%llX.gxp", vgl_shader_cache_path, subdir_hash, hash);
			SceUID f = sceIoOpen(frag_fname, SCE_O_RDONLY, 0777);"""
new_f = """		if (pair_ok && !p->fshader->prog) {
			SceUID f = sceIoOpen(frag_fname, SCE_O_RDONLY, 0777);"""
for a, b in ((old_v, new_v), (old_f, new_f)):
    if s.count(a) != 1:
        print("pattern not found once:", a[:80])
        sys.exit(1)
    s = s.replace(a, b)
open(p, "w", encoding="utf-8", newline="").write(s)
print("custom_shaders.c patched (pair-keyed shader cache)")
