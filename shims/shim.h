/* Symbol shims: what the Android libraries (libg, libfmod) get when they import bionic/libc/Android names.
 *
 * ABI rules (docs/abi_notes.md):
 *  - The Android code uses the soft-float calling convention (floats and doubles travel in r0-r3), the Vita
 *    toolchain is hard-float. Any shim that takes or returns float/double therefore uses uint32_t/uint64_t at the
 *    boundary and converts with memcpy. Variadic functions need no wrapper (variadic calls are soft-float on both).
 *  - bionic structs (stat, dirent, tm, timespec, pthread_*_t, FILE) differ from newlib, so those calls are wrapped.
 */
#ifndef BV_SHIM_H
#define BV_SHIM_H

#include <stdint.h>
#include <string.h>

typedef struct bv_shim {
    const char *name;
    const void *addr;
} bv_shim;

#define SHIM(n) {#n, (const void *)&w_##n}
#define SHIM_PASS(n) {#n, (const void *)&n}
#define SHIM_END {NULL, NULL}

/* Looks a name up in every table; returns the run-time address (0 if unknown). */
uint32_t bv_shim_lookup(const char *name);

/* tables, one per file */
extern const bv_shim bv_shims_libc[];
extern const bv_shim bv_shims_io[];
extern const bv_shim bv_shims_math[];
extern const bv_shim bv_shims_pthread[];
extern const bv_shim bv_shims_misc[];
extern const bv_shim bv_shims_net[];
extern const bv_shim bv_shims_extra[];
extern const bv_shim bv_shims_asset[];
extern const bv_shim bv_shims_gles[];
extern const bv_shim bv_shims_fmod[];
extern const bv_shim bv_shims_egl[];
extern const bv_shim bv_shims_line[];

/* builds the bionic ctype tables; call once before any module is loaded */
void bv_shims_init(void);

/* helpers */
static inline float bv_u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t bv_f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline double bv_u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static inline uint64_t bv_d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

/* Android path -> console path. Returns out. A path with a '..' component is mapped to a name that does not exist. */
const char *bv_fix_path(const char *in, char *out, int n);
/* Creates the directories the game expects below ux0:data/<app>/fs (the console's mkdir is not recursive). */
void bv_make_game_dirs(void);
/* Logs the state of ux0:data/BrawlVita/assets: number of entries, and the size of the key files (missing / empty / ok). */
void bv_assets_selfcheck(void);
/* True when `p` has a '..' path component. */
int bv_has_dotdot(const char *p);

/* module registry used by dladdr/dlsym/exidx lookup */
struct so_module;
void bv_register_module(struct so_module *m);
struct so_module *bv_module_by_name(const char *name);
struct so_module *bv_module_for_pc(uint32_t pc);

#endif
