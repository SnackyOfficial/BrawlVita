#include <string.h>

#include "../loader/elf_loader.h"
#include "shim.h"

/* `misc` is reserved for Android/EGL/GL shims added in later phases */
const bv_shim bv_shims_misc[] = {SHIM_END};

static const bv_shim *const k_tables[] = {bv_shims_libc, bv_shims_io, bv_shims_math, bv_shims_pthread, bv_shims_net,
                                          bv_shims_extra, bv_shims_asset, bv_shims_gles, bv_shims_fmod, bv_shims_egl, bv_shims_line,
                                          bv_shims_misc};

uint32_t bv_shim_lookup(const char *name) {
    for (unsigned t = 0; t < sizeof k_tables / sizeof k_tables[0]; t++)
        for (const bv_shim *s = k_tables[t]; s->name; s++)
            if (strcmp(s->name, name) == 0) return (uint32_t)(uintptr_t)s->addr;
    return 0;
}

#define MAX_MODULES 8
static so_module *g_modules[MAX_MODULES];
static int g_nmodules;

void bv_register_module(so_module *m) {
    if (g_nmodules < MAX_MODULES) g_modules[g_nmodules++] = m;
}

so_module *bv_module_by_name(const char *name) {
    for (int i = 0; i < g_nmodules; i++)
        if (strcmp(g_modules[i]->name, name) == 0) return g_modules[i];
    return NULL;
}

so_module *bv_module_for_pc(uint32_t pc) {
    for (int i = 0; i < g_nmodules; i++)
        if (so_contains(g_modules[i], pc)) return g_modules[i];
    return NULL;
}
