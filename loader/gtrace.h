/* Getter tracing (update 40, diagnostics; config trace_getters=1, default off).
 *
 * Which fields of the home data does a screen check? The game reads object fields through tiny getters "ldr/ldrb r0, [r0, #imm] ; bx lr". Each such getter
 * (imm <= BV_GT_MAX_OFF) gets its ldr replaced by a branch to a stub in the loader's near code block. The stub saves r0-r3, r12, lr, calls
 * bv_gtrace_hit(index, r0), restores them, runs the original ldr and returns: the getter's result and registers are unchanged. bv_gtrace_hit counts the call
 * only when r0 is one of the four objects of the home data (home fields, config, avatar, holder) handed over by the offline hook after the login, and
 * bv_gtrace_report() logs the counts that changed. Old words are checked before every patch; the planner works on the pristine text.
 *
 * The planner and encoders are portable C (host-tested: tests/host_gtrace_test.c); the install lives in main.c (console only).
 */
#ifndef BV_GTRACE_H
#define BV_GTRACE_H
#include <stdint.h>

#define BV_GT_MAX_OFF 0x104u   /* the largest field offset traced (home fields object is 0x108 bytes) */
#define BV_GT_MAX 1400         /* getters at most (Build B has 1055 with imm <= 0x104) */
#define BV_GT_STUB_WORDS 10    /* 40 bytes per stub (update 41: + mov r2, lr) */
#define BV_GT_CALLERS 2048     /* (getter, caller) pairs counted (update 41) */

typedef struct bv_gt_site {
    uint32_t off;  /* libg offset of the getter's ldr */
    uint32_t old;  /* the ldr word (also executed by the stub) */
} bv_gt_site;

/* Finds getters in text[0, size): word pairs (ldr|ldrb r0, [r0, #imm], bx lr) with imm <= max_off, cond AL. Returns how many (<= max). */
int bv_gtrace_find(const uint8_t *text, uint32_t size, uint32_t max_off, bv_gt_site *out, int max);
/* The ARM B instruction at `from` that jumps to `to` (both run-time addresses; |to - from - 8| < 32 MB). */
uint32_t bv_gtrace_branch(uint32_t from, uint32_t to);
/* The 10 words of stub `idx` (0..65535): push {r0-r3,r12,lr}; mov r1, r0; mov r2, lr; movw r0, #idx; ldr r12, [pc, #12]; blx r12; pop {r0-r3,r12,lr};
 * <orig ldr>; bx lr; .word handler. Position independent. The handler gets (idx, object, return address of the getter's caller). */
void bv_gtrace_stub(uint32_t out[BV_GT_STUB_WORDS], int idx, uint32_t orig_ldr, uint32_t handler);

/* ---- code-path probes (update 47; config probes=ADDR,ADDR,... libg offsets, hex) ----
 * The instruction at a probe address is replaced by a branch to a 12-word stub: push {r0-r3,r12,lr}; mrs r1, apsr; push {r1,r2}; movw r0, #id;
 * ldr r12, [pc, #20]; blx r12; pop {r1,r2}; msr apsr_nzcvq, r1; pop {r0-r3,r12,lr}; <original instruction>; b <address + 4>; .word handler.
 * The flags are saved and restored around the counter, so conditional code keeps working. Only instructions that do not use the pc and are not
 * branches can be probed (bv_probe_ok). bv_probe_report logs "probe libg+ADDR xN" for the probes that ran since the last report. */
#define BV_PROBE_WORDS 12
#define BV_PROBE_MAX 64
int bv_probe_ok(uint32_t ins);
void bv_probe_stub(uint32_t out[BV_PROBE_WORDS], int id, uint32_t orig, uint32_t handler, uint32_t stub_addr, uint32_t back_addr);
int bv_probe_parse(const char *list, uint32_t *offs, int max); /* "af02c,2c691c" -> offsets; returns how many */
void bv_probe_set(const uint32_t *offs, int n);
void bv_probe_hit(int id);
void bv_probe_report(void (*log)(const char *line));

/* run time */
void bv_gtrace_set_sites(const bv_gt_site *sites, int n);                                  /* the installed getters, for the report */
void bv_gtrace_set_objects(uint32_t home, uint32_t config, uint32_t avatar, uint32_t holder); /* from the offline hook after the login */
void bv_gtrace_hit(int idx, uint32_t self, uint32_t caller);                                 /* called by the stubs */
void bv_gtrace_set_module(uint32_t load, uint32_t size);  /* libg's run-time base / code size: callers are logged as libg offsets */
void bv_gtrace_report(void (*log)(const char *line));                                        /* once per second: the counts that changed */
extern int bv_gtrace_enabled;                                                                /* config trace_getters */
#endif
