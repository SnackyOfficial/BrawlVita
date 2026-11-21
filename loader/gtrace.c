#include "gtrace.h"

#include <stdio.h>
#include <string.h>

int bv_gtrace_enabled;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

int bv_gtrace_find(const uint8_t *text, uint32_t size, uint32_t max_off, bv_gt_site *out, int max) {
    int n = 0;
    for (uint32_t a = 0; a + 8 <= size && n < max; a += 4) {
        uint32_t w = rd32(text + a), nx = rd32(text + a + 4);
        if (nx != 0xe12fff1eu) continue;                                   /* bx lr */
        if ((w & 0xfffff000u) != 0xe5900000u && (w & 0xfffff000u) != 0xe5d00000u) continue; /* ldr / ldrb r0, [r0, #+imm], cond AL */
        if ((w & 0xfffu) > max_off) continue;
        out[n].off = a;
        out[n].old = w;
        n++;
    }
    return n;
}

uint32_t bv_gtrace_branch(uint32_t from, uint32_t to) {
    int32_t d = (int32_t)(to - (from + 8)) >> 2;
    return 0xea000000u | ((uint32_t)d & 0x00ffffffu);
}

void bv_gtrace_stub(uint32_t out[BV_GT_STUB_WORDS], int idx, uint32_t orig_ldr, uint32_t handler) {
    uint32_t i = (uint32_t)idx & 0xffffu;
    out[0] = 0xe92d500fu;                                            /* push {r0-r3, r12, lr}   (24 bytes: sp stays 8-aligned) */
    out[1] = 0xe1a01000u;                                            /* mov r1, r0               (the object) */
    out[2] = 0xe1a0200eu;                                            /* mov r2, lr               (who called the getter) */
    out[3] = 0xe3000000u | ((i & 0xf000u) << 4) | (i & 0x0fffu);     /* movw r0, #idx */
    out[4] = 0xe59fc00cu;                                            /* ldr r12, [pc, #12]       (pc = stub+24 -> stub+36) */
    out[5] = 0xe12fff3cu;                                            /* blx r12 */
    out[6] = 0xe8bd500fu;                                            /* pop {r0-r3, r12, lr} */
    out[7] = orig_ldr;                                               /* the getter's own ldr / ldrb r0, [r0, #imm] */
    out[8] = 0xe12fff1eu;                                            /* bx lr */
    out[9] = handler;                                                /* .word bv_gtrace_hit */
}

/* ---- probes ---- */
int bv_probe_ok(uint32_t w) {
    if ((w >> 28) == 0xf) return 0;                    /* unconditional space */
    if (((w >> 25) & 7) == 5) return 0;               /* b / bl */
    if ((w & 0x0ffffff0u) == 0x012fff10u || (w & 0x0ffffff0u) == 0x012fff30u) return 0; /* bx / blx reg */
    if (((w >> 16) & 0xf) == 15 || ((w >> 12) & 0xf) == 15 || (w & 0xf) == 15) return 0; /* pc as a register (rough, on purpose) */
    if (((w >> 25) & 7) == 4 && (w & 0x8000u)) return 0; /* ldm/stm with pc in the list */
    return 1;
}

void bv_probe_stub(uint32_t out[BV_PROBE_WORDS], int id, uint32_t orig, uint32_t handler, uint32_t stub_addr, uint32_t back_addr) {
    uint32_t i = (uint32_t)id & 0xffffu;
    out[0] = 0xe92d500fu;                                        /* push {r0-r3, r12, lr} */
    out[1] = 0xe10f1000u;                                        /* mrs r1, apsr */
    out[2] = 0xe92d0006u;                                        /* push {r1, r2}            (32 bytes in all: sp stays 8-aligned) */
    out[3] = 0xe3000000u | ((i & 0xf000u) << 4) | (i & 0x0fffu); /* movw r0, #id */
    out[4] = 0xe59fc014u;                                        /* ldr r12, [pc, #20]       (pc = stub+24 -> stub+44) */
    out[5] = 0xe12fff3cu;                                        /* blx r12 */
    out[6] = 0xe8bd0006u;                                        /* pop {r1, r2} */
    out[7] = 0xe128f001u;                                        /* msr apsr_nzcvq, r1 */
    out[8] = 0xe8bd500fu;                                        /* pop {r0-r3, r12, lr} */
    out[9] = orig;                                               /* the probed instruction */
    out[10] = bv_gtrace_branch(stub_addr + 40u, back_addr);      /* b address + 4 */
    out[11] = handler;
}

int bv_probe_parse(const char *s, uint32_t *offs, int max) {
    int n = 0;
    while (s && *s && n < max) {
        while (*s == ' ' || *s == ',') s++;
        if (!*s) break;
        uint32_t v = 0;
        int digits = 0;
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
        for (; *s && *s != ','; s++) {
            char ch = *s;
            int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
            if (d < 0) break;
            v = v * 16u + (uint32_t)d;
            digits++;
        }
        while (*s && *s != ',') s++;
        if (digits && !(v & 3u)) offs[n++] = v;
    }
    return n;
}

static uint32_t g_poff[BV_PROBE_MAX];
static int g_np;
static volatile uint32_t g_pcnt[BV_PROBE_MAX];
static uint32_t g_plast[BV_PROBE_MAX];

void bv_probe_set(const uint32_t *offs, int n) {
    g_np = n > BV_PROBE_MAX ? BV_PROBE_MAX : n;
    for (int i = 0; i < g_np; i++) g_poff[i] = offs[i];
}

void bv_probe_hit(int id) {
    if ((unsigned)id < BV_PROBE_MAX) __sync_fetch_and_add(&g_pcnt[id], 1);
}

void bv_probe_report(void (*log)(const char *line)) {
    if (!g_np || !log) return;
    char line[600];
    int at = snprintf(line, sizeof line, "probes:"), shown = 0;
    for (int i = 0; i < g_np && at < (int)sizeof line - 32; i++) {
        uint32_t c = g_pcnt[i], d = c - g_plast[i];
        if (!d) continue;
        g_plast[i] = c;
        at += snprintf(line + at, sizeof line - (size_t)at, " %x x%u", (unsigned)g_poff[i], (unsigned)d);
        shown++;
    }
    if (shown) log(line);
}

/* ---- run time ---- */
static const bv_gt_site *g_sites;
static int g_nsites;
static volatile uint32_t g_obj[4];
static volatile uint32_t g_cnt[4][BV_GT_MAX];
static uint32_t g_last[4][BV_GT_MAX];
static const char *const k_obj_name[4] = {"home", "config", "avatar", "holder"};

void bv_gtrace_set_sites(const bv_gt_site *sites, int n) {
    g_sites = sites;
    g_nsites = n > BV_GT_MAX ? BV_GT_MAX : n;
}

void bv_gtrace_set_objects(uint32_t home, uint32_t config, uint32_t avatar, uint32_t holder) {
    g_obj[0] = home;
    g_obj[1] = config;
    g_obj[2] = avatar;
    g_obj[3] = holder;
}

/* (caller, object) pairs (update 41). The field reads themselves are mostly inline in the game's code (run 25: only 6 distinct getter reads, e.g.
 * holder->home 101964 times): who fetches the home / config pointer while a screen opens is what points at that screen's code.
 * key = 1 << 31 | object << 24 | (caller offset in libg >> 2) (22 bits); one call site calls one getter, so the key fixes the getter too. */
static uint32_t g_load, g_size;
static volatile uint32_t g_ckey[BV_GT_CALLERS];
static volatile uint16_t g_cidx[BV_GT_CALLERS];
static volatile uint32_t g_ccnt[BV_GT_CALLERS];
static uint32_t g_clast[BV_GT_CALLERS];
static volatile uint32_t g_cfull;

void bv_gtrace_set_module(uint32_t load, uint32_t size) {
    g_load = load;
    g_size = size;
}

static void count_caller(int k, int idx, uint32_t caller) {
    uint32_t off = caller - g_load;
    if (caller < g_load || off >= g_size) return;
    uint32_t key = 0x80000000u | ((uint32_t)k << 24) | ((off >> 2) & 0x3fffffu);
    uint32_t h = (key * 2654435761u) >> 21; /* 11 bits */
    for (uint32_t probe = 0; probe < BV_GT_CALLERS; probe++) {
        uint32_t s = (h + probe) & (BV_GT_CALLERS - 1);
        uint32_t cur = g_ckey[s];
        if (cur == key) {
            __sync_fetch_and_add(&g_ccnt[s], 1);
            return;
        }
        if (cur == 0) {
            if (__sync_bool_compare_and_swap(&g_ckey[s], 0, key)) {
                g_cidx[s] = (uint16_t)idx;
                __sync_fetch_and_add(&g_ccnt[s], 1);
                return;
            }
            if (g_ckey[s] == key) { /* another thread inserted the same key just now */
                __sync_fetch_and_add(&g_ccnt[s], 1);
                return;
            }
        }
    }
    __sync_fetch_and_add(&g_cfull, 1);
}

void bv_gtrace_hit(int idx, uint32_t self, uint32_t caller) {
    if ((unsigned)idx >= BV_GT_MAX || !self) return;
    for (int k = 0; k < 4; k++)
        if (self == g_obj[k]) {
            __sync_fetch_and_add(&g_cnt[k][idx], 1);
            count_caller(k, idx, caller);
            return;
        }
}

void bv_gtrace_report(void (*log)(const char *line)) {
    if (!g_sites || !log) return;
    for (int k = 0; k < 4; k++) {
        if (!g_obj[k]) continue;
        char line[900];
        int at = snprintf(line, sizeof line, "trace %s (0x%08x):", k_obj_name[k], (unsigned)g_obj[k]);
        int shown = 0;
        for (int i = 0; i < g_nsites; i++) {
            uint32_t c = g_cnt[k][i], d = c - g_last[k][i];
            if (!d) continue;
            g_last[k][i] = c;
            if (at < (int)sizeof line - 48) {
                at += snprintf(line + at, sizeof line - (size_t)at, " %s+0x%x@%x x%u", (g_sites[i].old & 0x00400000u) ? "b" : "w", (unsigned)(g_sites[i].old & 0xfffu),
                               (unsigned)g_sites[i].off, (unsigned)d);
            }
            shown++;
            if (at >= (int)sizeof line - 48) { /* flush a full line, keep going */
                log(line);
                at = snprintf(line, sizeof line, "trace %s (cont.):", k_obj_name[k]);
            }
        }
        if (shown) log(line);
    }
    /* callers whose counts changed: "trace callers: home<-libg+0x123456(w+0xc@69353c) x5 ..." */
    char line[900];
    int at = snprintf(line, sizeof line, "trace callers:"), shown = 0;
    for (int s = 0; s < BV_GT_CALLERS; s++) {
        uint32_t key = g_ckey[s];
        if (!key) continue;
        uint32_t c = g_ccnt[s], d = c - g_clast[s];
        if (!d) continue;
        g_clast[s] = c;
        int k = (int)((key >> 24) & 3), idx = g_cidx[s];
        uint32_t coff = (key & 0x3fffffu) << 2;
        at += snprintf(line + at, sizeof line - (size_t)at, " %s<-%x(%s+0x%x@%x) x%u", k_obj_name[k], (unsigned)coff,
                       idx < g_nsites && (g_sites[idx].old & 0x00400000u) ? "b" : "w", idx < g_nsites ? (unsigned)(g_sites[idx].old & 0xfffu) : 0u,
                       idx < g_nsites ? (unsigned)g_sites[idx].off : 0u, (unsigned)d);
        shown++;
        if (at >= (int)sizeof line - 64) {
            log(line);
            at = snprintf(line, sizeof line, "trace callers (cont.):");
        }
    }
    if (shown) log(line);
    if (g_cfull) {
        log("trace callers: the (caller, object) table is full, some callers are not counted");
        g_cfull = 0;
    }
}
