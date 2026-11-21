/* Portable loader for Android ARM32 shared objects (ET_DYN, EM_ARM).
 *
 * The core knows nothing about the Vita: all memory access goes through so_ops, so the same code is
 * unit-tested on the PC against the real libg.so / libfmod.so (tests/host_loader_test.c) and used
 * unchanged on the console (loader/vita_mem.c).
 *
 * Address model: the image is laid out exactly like the ELF (vaddr 0 = module base) because the code is
 * PC-relative into its data. `host` is where the loader reads/writes the bytes, `load` is the address the
 * code will see at run time. They are the same on the Vita.
 */
#ifndef BV_ELF_LOADER_H
#define BV_ELF_LOADER_H

#include <stddef.h>
#include <stdint.h>

#define SO_MAX_NEEDED 16

typedef struct so_ops {
    void *ctx;
    /* Reserve the image: [0,text_size) executable, [data_off,span) writable, contiguous.
     * Returns the host pointer of vaddr 0 and stores the run-time address of vaddr 0 in *load. */
    uint8_t *(*map)(void *ctx, size_t text_size, size_t data_off, size_t span, uint32_t *load);
    /* Write into the text part of the image (not directly writable on the console). */
    void (*write_text)(void *ctx, uint8_t *dst, const void *src, size_t n);
    void (*flush)(void *ctx, void *p, size_t n);
    /* Executable memory for unresolved-import thunks. */
    uint8_t *(*alloc_code)(void *ctx, size_t n, uint32_t *addr);
    /* Run-time address of the function every unresolved thunk jumps to (r12 = thunk id). */
    uint32_t unresolved_handler;
    /* Symbol lookup for imports: returns the run-time address or 0. */
    uint32_t (*resolve)(void *ctx, const char *name);
    void (*log)(void *ctx, const char *fmt, ...);
} so_ops;

typedef struct so_module {
    char name[32];
    uint8_t *host;        /* host pointer of vaddr 0 */
    uint32_t load;        /* run-time address of vaddr 0 */
    uint32_t text_size;   /* page aligned end of the executable part */
    uint32_t data_off;    /* page aligned start of the writable part */
    uint32_t span;        /* total image size, page aligned */

    const uint8_t *symtab; /* Elf32_Sym[] (read through host) */
    const char *strtab;
    uint32_t nsyms;
    const uint32_t *hash; /* DT_HASH */
    uint32_t init_func;
    uint32_t init_array, init_array_n;
    uint32_t fini_array, fini_array_n;
    uint32_t exidx, exidx_size;
    char needed[SO_MAX_NEEDED][32];
    int n_needed;

    /* statistics filled by so_load */
    uint32_t n_reloc[4];  /* RELATIVE, ABS32, GLOB_DAT, JUMP_SLOT */
    uint32_t n_unresolved;
    uint32_t id;          /* module id used in thunk ids */
} so_module;

/* Name table of imports that could not be resolved (thunk id -> name). Filled by so_load. */
#define SO_MAX_UNRESOLVED 1024
typedef struct so_unresolved_tab {
    char name[SO_MAX_UNRESOLVED][64];
    uint32_t module[SO_MAX_UNRESOLVED];
    uint32_t n;
} so_unresolved_tab;

/* Returns 0 on success, negative on error (message through ops->log). */
int so_load(so_module *m, const char *name, const uint8_t *file, size_t size, so_ops *ops,
            so_unresolved_tab *unres, uint32_t module_id);

/* Looks a defined symbol up through DT_HASH; returns the run-time address (Thumb bit kept) or 0. */
uint32_t so_find_symbol(const so_module *m, const char *name);

/* n-th defined dynamic symbol (exports). Returns 1 and fills name/addr, 0 when idx is past the end. */
int so_export(const so_module *m, int idx, const char **name, uint32_t *addr);

/* n-th constructor (DT_INIT first, then DT_INIT_ARRAY) as a run-time address in *addr.
 * Returns 1 for a callable entry, 2 for a 0/-1 placeholder (skip it), 0 when idx is past the end. */
int so_ctor(const so_module *m, int idx, uint32_t *addr);

/* True and sets exidx info if pc lies inside the module's mapped text. */
int so_contains(const so_module *m, uint32_t pc);

#endif
