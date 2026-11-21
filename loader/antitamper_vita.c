#include <string.h>

#include "antitamper.h"
#include "elf_loader.h"
#include "kubridge_api.h"
#include "log.h"

int bv_antitamper_apply(const struct so_module *m, const at_patch *p, int n) {
    int done = 0;
    for (int i = 0; i < n; i++) {
        uint8_t code[4] = {(uint8_t)p[i].new_word, (uint8_t)(p[i].new_word >> 8), (uint8_t)(p[i].new_word >> 16), (uint8_t)(p[i].new_word >> 24)};
        uint8_t *dst = m->host + p[i].off;
        int r = kuKernelCpuUnrestrictedMemcpy(dst, code, 4);
        kuKernelFlushCaches(dst, 4);
        if (r < 0 || memcmp(dst, code, 4) != 0) {
            bv_log("antitamper: patch at libg+0x%x FAILED (0x%08x)", p[i].off, (unsigned)r);
            continue;
        }
        bv_log("antitamper: libg+0x%06x  %08x -> %08x  %s", p[i].off, p[i].old_word, p[i].new_word, p[i].what);
        done++;
    }
    return done;
}
