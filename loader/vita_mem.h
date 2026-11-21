#ifndef BV_VITA_MEM_H
#define BV_VITA_MEM_H

#include "elf_loader.h"

/* A small executable block of the loader's own (cleared to zero) at the top of the 16 MB slot of the module loaded at module_load (span = its image size): every
 * instruction of the module can reach it with a plain `bl`. For code caves that must not depend on bytes the loader never wrote (gamefix page-change helper).
 * Returns the block (NULL on failure, logged) and its address in *addr. One block per call: call it once per module. */
#define BV_NEAR_SIZE 0x10000u
uint8_t *bv_mem_alloc_near(uint32_t module_load, uint32_t module_span, uint32_t *addr);

void bv_mem_make_ops(so_ops *ops, uint32_t unresolved_handler, uint32_t (*resolve)(void *, const char *));

#endif
