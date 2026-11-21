/* Extra shims needed by the game library (first test build): the long tail of libm, bionic ctype tables and a few libc calls.
 * Same ABI rules as shim.h: float/double cross the boundary as uint32_t/uint64_t.
 */
#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "../loader/log.h"
#include "shim.h"

#define D1(name) static uint64_t w_##name(uint64_t a) { return bv_d2u(name(bv_u2d(a))); }
#define D2(name) static uint64_t w_##name(uint64_t a, uint64_t b) { return bv_d2u(name(bv_u2d(a), bv_u2d(b))); }

D1(acos) D1(acosh) D1(asin) D1(asinh) D1(atan) D1(atanh) D1(cbrt) D1(cosh) D1(erf) D1(erfc) D1(exp) D1(exp2)
D1(expm1) D1(log10) D1(log1p) D1(logb) D1(rint) D1(sinh) D1(sqrt) D1(tan) D1(tanh) D1(tgamma) D1(nearbyint)
D1(lgamma)
D2(atan2) D2(hypot) D2(nextafter) D2(remainder)

static uint32_t w_nextafterf(uint32_t a, uint32_t b) { return bv_f2u(nextafterf(bv_u2f(a), bv_u2f(b))); }
static uint64_t w_remquo(uint64_t a, uint64_t b, int *q) { return bv_d2u(remquo(bv_u2d(a), bv_u2d(b), q)); }
static long long w_llrint(uint64_t a) { return llrint(bv_u2d(a)); }
static long w_lrint(uint64_t a) { return lrint(bv_u2d(a)); }
static uint64_t w_scalbnl(uint64_t a, int e) { return bv_d2u(scalbn(bv_u2d(a), e)); } /* long double == double on ARM */
static int w___fpclassifyd(uint64_t a) { return fpclassify(bv_u2d(a)); }
static int w___isfinite(uint64_t a) { return isfinite(bv_u2d(a)) != 0; }
static int w___signbit(uint64_t a) { return signbit(bv_u2d(a)) != 0; }

/* ---- bionic ctype: `_ctype_`, `_tolower_tab_`, `_toupper_tab_` are pointer variables (OpenBSD heritage) to tables
 * indexed by c + 1 so that EOF (-1) is valid. Flags: _U 1, _L 2, _N 4, _S 8, _P 0x10, _C 0x20, _X 0x40, _B 0x80. ---- */
static char g_ctype[257];
static int16_t g_lower[257], g_upper[257];
static const char *g_ctype_ptr = g_ctype;
static const int16_t *g_lower_ptr = g_lower;
static const int16_t *g_upper_ptr = g_upper;

void bv_shims_init(void) {
    for (int c = 0; c < 256; c++) {
        char f = 0;
        if (c >= 'A' && c <= 'Z') f = 0x01;
        else if (c >= 'a' && c <= 'z') f = 0x02;
        else if (c >= '0' && c <= '9') f = 0x04;
        else if (c == ' ') f = 0x08 | 0x80;
        else if (c >= 9 && c <= 13) f = 0x08 | 0x20;
        else if (c < 32 || c == 127) f = 0x20;
        else if (c < 127) f = 0x10;
        if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) f |= 0x40;
        g_ctype[c + 1] = f;
        g_lower[c + 1] = (int16_t)((c >= 'A' && c <= 'Z') ? c + 32 : c);
        g_upper[c + 1] = (int16_t)((c >= 'a' && c <= 'z') ? c - 32 : c);
    }
    g_ctype[0] = 0;
    g_lower[0] = -1;
    g_upper[0] = -1;
}

/* ---- libc odds and ends ---- */
extern uint8_t bv_sF[];
static int std_stream(const void *f) {
    uintptr_t p = (uintptr_t)f, b = (uintptr_t)bv_sF;
    return p >= b && p < b + 3 * 84 ? (int)((p - b) / 84) : -1;
}
static FILE *tf2(void *f) {
    int i = std_stream(f);
    return i < 0 ? (FILE *)f : (i == 0 ? stdin : i == 1 ? stdout : stderr);
}
static int w_fgetc(void *f) { return fgetc(tf2(f)); }
static uint32_t w_fputwc(uint32_t wc, void *f) {
    if (std_stream(f) >= 1) return wc;
    return (uint32_t)fputwc((wchar_t)wc, tf2(f));
}
static const char *w_gai_strerror(int e) { (void)e; return "name resolution failed"; }
static int w_inotify_rm_watch(int fd, int wd) { (void)fd; (void)wd; errno = ENOSYS; return -1; }
static int w_rename(const char *a, const char *b) {
    char ba[512], bb[512];
    return rename(bv_fix_path(a, ba, sizeof ba), bv_fix_path(b, bb, sizeof bb));
}
static const char *w_setlocale(int cat, const char *loc) { (void)cat; (void)loc; return "C"; }

const bv_shim bv_shims_extra[] = {
    {"_ctype_", (const void *)&g_ctype_ptr},
    {"_tolower_tab_", (const void *)&g_lower_ptr},
    {"_toupper_tab_", (const void *)&g_upper_ptr},
    SHIM(acos), SHIM(acosh), SHIM(asin), SHIM(asinh), SHIM(atan), SHIM(atanh), SHIM(cbrt), SHIM(cosh), SHIM(erf),
    SHIM(erfc), SHIM(exp), SHIM(exp2), SHIM(expm1), SHIM(log10), SHIM(log1p), SHIM(logb), SHIM(rint), SHIM(sinh),
    SHIM(sqrt), SHIM(tan), SHIM(tanh), SHIM(tgamma), SHIM(nearbyint), SHIM(lgamma),
    SHIM(atan2), SHIM(hypot), SHIM(nextafter), SHIM(remainder), SHIM(nextafterf), SHIM(remquo), SHIM(llrint),
    SHIM(lrint), SHIM(scalbnl), SHIM(__fpclassifyd), SHIM(__isfinite), SHIM(__signbit),
    SHIM(fgetc), SHIM(fputwc), SHIM(gai_strerror), SHIM(inotify_rm_watch), SHIM(rename), SHIM(setlocale),
    SHIM_PASS(wcscmp),
    SHIM_END};
