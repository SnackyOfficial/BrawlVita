/* The three functions of TheOfficialFloW/kubridge v0.1 that the loader uses.
 * Own declarations (written from the plugin's behaviour); the import stub is generated from
 * third_party/kubridge_stub/kubridge_nids.yml. kubridge.skprx must be loaded as a kernel plugin.
 */
#ifndef BV_KUBRIDGE_API_H
#define BV_KUBRIDGE_API_H

#include <psp2/kernel/sysmem.h>
#include <stdint.h>

/* Mirrors the 0x58-byte kernel option block of ksceKernelAllocMemBlock (see kubridge.h of that repo).
 * Only w[0] = size, w[2] = attr and w[3] (requested address when attr bit 0 is set) are used. */
typedef struct bv_ku_opt {
    uint32_t w[22];
} bv_ku_opt;

#define BV_KU_OPT_SIZE 0
#define BV_KU_OPT_ATTR 2
#define BV_KU_OPT_ADDR 3

SceUID kuKernelAllocMemBlock(const char *name, SceKernelMemBlockType type, SceSize size, bv_ku_opt *opt);
void kuKernelFlushCaches(const void *ptr, SceSize len);
int kuKernelCpuUnrestrictedMemcpy(void *dst, const void *src, SceSize len);

#endif
