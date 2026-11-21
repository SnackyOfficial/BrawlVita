/* libm wrappers: soft-float (integer registers) <-> hard-float. See shim.h. */
#include <math.h>
#include <stdlib.h>

#include "shim.h"

#define F1(name) static uint32_t w_##name(uint32_t a) { return bv_f2u(name(bv_u2f(a))); }
#define F2(name) static uint32_t w_##name(uint32_t a, uint32_t b) { return bv_f2u(name(bv_u2f(a), bv_u2f(b))); }
#define D1(name) static uint64_t w_##name(uint64_t a) { return bv_d2u(name(bv_u2d(a))); }
#define D2(name) static uint64_t w_##name(uint64_t a, uint64_t b) { return bv_d2u(name(bv_u2d(a), bv_u2d(b))); }

F1(acosf) F1(asinf) F1(atanf) F1(ceilf) F1(cosf) F1(floorf) F1(roundf) F1(sinf) F1(tanf)
F1(expf) F1(exp2f) F1(logf) F1(log10f) F1(rintf)
F2(atan2f) F2(fmaxf) F2(fminf) F2(fmodf) F2(powf)
D1(ceil) D1(floor) D1(round) D1(cos) D1(sin) D1(log)
D2(fmod) D2(pow)

static uint32_t w_ldexpf(uint32_t a, int e) { return bv_f2u(ldexpf(bv_u2f(a), e)); }
static uint64_t w_ldexp(uint64_t a, int e) { return bv_d2u(ldexp(bv_u2d(a), e)); }
static uint64_t w_scalbn(uint64_t a, int e) { return bv_d2u(scalbn(bv_u2d(a), e)); }
static uint64_t w_frexp(uint64_t a, int *e) { return bv_d2u(frexp(bv_u2d(a), e)); }
static uint64_t w_modf(uint64_t a, uint64_t *ip) {
    double i;
    double f = modf(bv_u2d(a), &i);
    *ip = bv_d2u(i);
    return bv_d2u(f);
}
static void w_sincosf(uint32_t a, uint32_t *s, uint32_t *c) {
    float x = bv_u2f(a);
    *s = bv_f2u(sinf(x));
    *c = bv_f2u(cosf(x));
}
static uint64_t w_strtod(const char *s, char **end) { return bv_d2u(strtod(s, end)); }
static uint64_t w_difftime(int32_t a, int32_t b) { return bv_d2u((double)a - (double)b); }

const bv_shim bv_shims_math[] = {
    SHIM(acosf), SHIM(asinf), SHIM(atanf), SHIM(ceilf), SHIM(cosf), SHIM(floorf), SHIM(roundf), SHIM(sinf), SHIM(tanf),
    SHIM(expf), SHIM(exp2f), SHIM(logf), SHIM(log10f), SHIM(rintf),
    SHIM(atan2f), SHIM(fmaxf), SHIM(fminf), SHIM(fmodf), SHIM(powf),
    SHIM(ceil), SHIM(floor), SHIM(round), SHIM(cos), SHIM(sin), SHIM(log), SHIM(fmod), SHIM(pow),
    SHIM(ldexpf), SHIM(ldexp), SHIM(scalbn), SHIM(frexp), SHIM(modf), SHIM(sincosf), SHIM(strtod), SHIM(difftime),
    SHIM_END};
