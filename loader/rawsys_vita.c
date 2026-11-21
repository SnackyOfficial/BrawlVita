#include <string.h>

#include "../shims/shim.h"
#include "elf_loader.h"
#include "kubridge_api.h"
#include "log.h"
#include "rawsys.h"

int bv_rawsys_apply(const struct so_module *m, const rs_patch *p, int n) {
    int done = 0, gtod = 0, redirects = 0;
    for (int i = 0; i < n; i++) {
        uint8_t code[8];
        memcpy(code, p[i].bytes, p[i].len);
        if (p[i].shim[0]) {
            uint32_t a = bv_shim_lookup(p[i].shim);
            if (!a) {
                bv_log("rawsys: no shim named %s, stub at libg+0x%x left alone", p[i].shim, p[i].off);
                continue;
            }
            code[4] = (uint8_t)a;
            code[5] = (uint8_t)(a >> 8);
            code[6] = (uint8_t)(a >> 16);
            code[7] = (uint8_t)(a >> 24);
        }
        uint8_t *dst = m->host + p[i].off;
        int r = kuKernelCpuUnrestrictedMemcpy(dst, code, (SceSize)p[i].len);
        kuKernelFlushCaches(dst, (SceSize)p[i].len);
        if (r < 0 || memcmp(dst, code, p[i].len) != 0) {
            bv_log("rawsys: patch at libg+0x%x FAILED (0x%08x)", p[i].off, (unsigned)r);
            continue;
        }
        done++;
        if (p[i].shim[0]) redirects++;
        if (p[i].nr == 78) gtod++;
        else if (bv_trace_level >= 2) bv_log("rawsys: libg+0x%06x  nr %d  %s", p[i].off, p[i].nr, p[i].what);
    }
    bv_log("rawsys: %d of %d `svc #0` sites patched (%d gettimeofday, %d redirected)", done, n, gtod, redirects);
    return done;
}
