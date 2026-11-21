/* so_ops for the console: fixed-address, contiguous text(RX) + data(RW) blocks through kubridge.
 *
 * UNVERIFIED ON HARDWARE. The memblock types and the "fixed address" option follow the way existing Vita
 * so-loaders use kuKernelAllocMemBlock (attr bit 0 + requested address in the option block). Every attempt and
 * its result code is logged, so a failure on the device shows exactly which variant the kernel refused.
 */
#include "vita_mem.h"

#include <psp2/kernel/sysmem.h>
#include <stdlib.h>
#include <string.h>

#include "kubridge_api.h"
#include "log.h"

#define LOAD_BASE 0x98000000u
#define SLOT_SIZE 0x01000000u /* 16 MB per module slot (libg needs 12.6 MB) */
#define CODE_POOL_ADDR 0x9f000000u
#define CODE_POOL_SIZE (256u * 1024u)
#define PAGE 0x1000u

static uint32_t g_next_slot = LOAD_BASE;
static uint8_t *g_code_pool;
static uint32_t g_code_used;

/* Candidate memblock types, tried in order. */
static const uint32_t TEXT_TYPES[] = {0x1020D005u, 0x0C20D050u};
static const uint32_t DATA_TYPES[] = {SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 0x1020D006u};

static SceUID alloc_fixed(const char *name, const uint32_t *types, int ntypes, uint32_t addr, size_t size, void **base) {
    for (int i = 0; i < ntypes; i++) {
        bv_ku_opt opt;
        memset(&opt, 0, sizeof opt);
        opt.w[BV_KU_OPT_SIZE] = sizeof opt;
        opt.w[BV_KU_OPT_ATTR] = 1;
        opt.w[BV_KU_OPT_ADDR] = addr;
        SceUID id = kuKernelAllocMemBlock(name, types[i], (SceSize)size, &opt);
        bv_log("memblock %-10s type 0x%08x addr 0x%08x size 0x%x -> %d (0x%08x)", name, types[i], addr, (unsigned)size, id,
               (unsigned)id);
        if (id < 0) continue;
        void *b = NULL;
        int r = sceKernelGetMemBlockBase(id, &b);
        bv_log("  base %p (get base rc %d)", b, r);
        if (r < 0 || (uint32_t)(uintptr_t)b != addr) {
            bv_log("  block is not at the requested address, releasing it");
            sceKernelFreeMemBlock(id);
            continue;
        }
        *base = b;
        return id;
    }
    return -1;
}

static uint8_t *v_map(void *ctx, size_t text_size, size_t data_off, size_t span, uint32_t *load) {
    (void)ctx;
    uint32_t addr = g_next_slot;
    if (span > SLOT_SIZE) {
        bv_log("image of 0x%x bytes does not fit a module slot", (unsigned)span);
        return NULL;
    }
    g_next_slot += SLOT_SIZE;
    void *tb = NULL, *db = NULL;
    if (alloc_fixed("bv_text", TEXT_TYPES, 2, addr, text_size, &tb) < 0) return NULL;
    if (alloc_fixed("bv_data", DATA_TYPES, 2, addr + (uint32_t)data_off, span - data_off, &db) < 0) return NULL;
    memset(db, 0, span - data_off);
    *load = addr;
    return (uint8_t *)(uintptr_t)addr;
}

static void v_write_text(void *ctx, uint8_t *dst, const void *src, size_t n) {
    (void)ctx;
    kuKernelCpuUnrestrictedMemcpy(dst, src, (SceSize)n);
}

static void v_flush(void *ctx, void *p, size_t n) {
    (void)ctx;
    kuKernelFlushCaches(p, (SceSize)n);
}

static uint8_t *v_alloc_code(void *ctx, size_t n, uint32_t *addr) {
    (void)ctx;
    if (!g_code_pool) {
        void *b = NULL;
        if (alloc_fixed("bv_thunks", TEXT_TYPES, 2, CODE_POOL_ADDR, CODE_POOL_SIZE, &b) < 0) return NULL;
        g_code_pool = (uint8_t *)b;
    }
    if (g_code_used + n > CODE_POOL_SIZE) return NULL;
    uint8_t *p = g_code_pool + g_code_used;
    *addr = CODE_POOL_ADDR + g_code_used;
    g_code_used += (uint32_t)n;
    return p;
}

uint8_t *bv_mem_alloc_near(uint32_t module_load, uint32_t module_span, uint32_t *addr) {
    if (module_load < LOAD_BASE || module_span + BV_NEAR_SIZE > SLOT_SIZE) {
        bv_log("near code block: module at 0x%08x span 0x%x leaves no room in its slot", (unsigned)module_load, (unsigned)module_span);
        return NULL;
    }
    uint32_t slot = LOAD_BASE + ((module_load - LOAD_BASE) / SLOT_SIZE) * SLOT_SIZE;
    uint32_t at = slot + SLOT_SIZE - BV_NEAR_SIZE; /* the top of the module's own 16 MB slot: free (above the image), within +-32 MB of all its code */
    void *b = NULL;
    if (alloc_fixed("bv_near", TEXT_TYPES, 2, at, BV_NEAR_SIZE, &b) < 0) return NULL;
    /* the kernel does not promise zeroed memory: the block is cleared here, so a planner that expects zero words sees exactly what it expects */
    static const uint8_t zeros[1024];
    for (uint32_t o = 0; o < BV_NEAR_SIZE; o += sizeof zeros) kuKernelCpuUnrestrictedMemcpy((uint8_t *)b + o, zeros, sizeof zeros);
    kuKernelFlushCaches(b, BV_NEAR_SIZE);
    uint32_t nz = 0;
    for (uint32_t o = 0; o < BV_NEAR_SIZE; o++) nz += ((const uint8_t *)b)[o] != 0;
    bv_log("near code block at 0x%08x (module+0x%x, 0x%x bytes), %u non-zero bytes after clearing", (unsigned)at, (unsigned)(at - module_load), (unsigned)BV_NEAR_SIZE, (unsigned)nz);
    if (nz) return NULL;
    *addr = at;
    return (uint8_t *)b;
}

static void v_log(void *ctx, const char *fmt, ...) {
    (void)ctx;
    va_list ap;
    va_start(ap, fmt);
    bv_logv(fmt, ap);
    va_end(ap);
}

void bv_mem_make_ops(so_ops *ops, uint32_t unresolved_handler, uint32_t (*resolve)(void *, const char *)) {
    memset(ops, 0, sizeof *ops);
    ops->map = v_map;
    ops->write_text = v_write_text;
    ops->flush = v_flush;
    ops->alloc_code = v_alloc_code;
    ops->unresolved_handler = unresolved_handler;
    ops->resolve = resolve;
    ops->log = v_log;
}
