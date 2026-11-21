/* NULL-safe calls of one libg accessor. Crash of 2026-10-27 (psp2core-1793092479): the game's loading state function (libg+0x1b9704) looks an item up in a list
 * (libg+0x39a934), gets NULL, and calls the accessor libg+0x43bcc4 (`ldr r0,[r0,#4]` ...) on it without a check: a read at address 4.
 * 41 places call that accessor, 27 of them right after `mov r0,rX`. Each of those 27 becomes `movs r0,rX` / `blne accessor`: unchanged for a real object,
 * and for NULL the call is skipped and r0 = 0 (the accessor's natural "nothing"). The flags a callee leaves are undefined by the ABI, so no caller can depend on them.
 * Found by shape: the accessor is recognised by its five words, never by its address. Uses at_patch / bv_antitamper_apply of antitamper.h. */
#ifndef BV_GAMEFIX_H
#define BV_GAMEFIX_H

#include "antitamper.h"

/* Plans the patches (2 per site). *sites = how many guardable call sites were found (0 when the accessor is not there: another libg build). */
int bv_gamefix_plan(const uint8_t *text, uint32_t size, at_patch *out, int max, int *sites);

/* Crash of 2026-10-30 (psp2core-1793311428, BRVT00001): the stage's page-change code (libg+0x390600) looks up the previous page (`r6`), compares it with the next one
 * (`cmp r5,r6 ; beq skip`) and then reads [r6+0x40] without a check. With no previous page r6 is NULL and the game dies. The skip is widened to "r5 == r6 OR r6 == NULL"
 * by redirecting the call just before the compare (libg+0x390614) to a 5-instruction helper in the zeroed end of the code block:
 *     push {r4,lr} ; bl <the original callee> ; cmp r6,#0 ; cmpne r5,r6 ; pop {r4,pc}
 * and the original `cmp r5,r6` becomes a nop, so the existing `beq` tests the flags the helper left. Found by shape (the 8 words of the site, in either the original or the accessor-patched form of the last two), never by address.
 * The result holds the helper's 5 patches first (*ncave of them, to be applied and checked before anything else), then the 2 patches of the site. 0 = site not found. */
int bv_gamefix_plan_pages(const uint8_t *text, uint32_t size, at_patch *out, int max, int *ncave);
/* The same, and when it returns 0 `why` (if not NULL) says what was missing: how many sites matched, or where the cave search stopped. */
int bv_gamefix_plan_pages_ex(const uint8_t *text, uint32_t size, at_patch *out, int max, int *ncave, char *why, unsigned whyn);
/* The same with the helper in a cave the caller provides instead of the block tail (update 16): cave_off = offset of the cave relative to libg's vaddr 0 (word aligned, >= size,
 * i.e. outside the scanned image, within +-32 MB of the site), cave_mem = the memory at that offset as the loader sees it now (5 words, must be zero). The helper patches then
 * write libg+cave_off (on the console host == load address, so at_patch.off reaches the loader's own block). cave_mem == NULL = search the block tail as before. The loader
 * uses this because nothing writes or guarantees the bytes between the end of the code (0x8cd238) and the end of the block (0x8ce000) on the console. */
int bv_gamefix_plan_pages_cave(const uint8_t *text, uint32_t size, uint32_t cave_off, const uint8_t *cave_mem, at_patch *out, int max, int *ncave, char *why, unsigned whyn);

/* Crash of 2026-11-04 (psp2core-1793796219, right after `socket(domain 16 type 3) -> refused`): the game has its own getifaddrs (libg+0x60d7f4) that asks the kernel for the
 * interface list over a netlink socket (socket(16,3,0), RTM_GETLINK). The loader cannot offer netlink; the function then takes its error exit WITHOUT clearing the caller's
 * list pointer, and both callers (libg+0x5b872c and libg+0x5c1560, which check whether 127.0.0.1 is a local address) ignore the result and walk stack garbage
 * (`ldrh r2,[r1]` with r1 = 0x3f800000). The function is replaced by `mov r1,#0 ; str r1,[r0] ; mov r0,#0 ; bx lr`: success with an empty list, which both callers handle.
 * Found by the shape of the callers (`mov r0,sp ; bl X ; ldr r6,[sp] ; cmp r6,#0 ; beq ; ldrh r0,[sp,#8] ; cmp r0,#2 ; beq`) and checked on X itself (it opens a
 * socket(0x10, 3, 0)). One patch of 4 words; *callers = how many call sites use it. 0 = not found. */
int bv_gamefix_plan_ifaddrs(const uint8_t *text, uint32_t size, at_patch *out, int max, int *callers);

/* Local bot battles (update 48; user: in the original offline APK, Play goes straight into a bot match). The game's start-game function (libg+0x67febc in
 * Build B; static reading notes/2026-11-10-page-guard/battles.md) takes the match kind in r3: != 3 builds the matchmake request 14103 and sends it to the
 * server (what Play does today), == 3 builds the battle's StartLoading message (20559, class libg+0x666bec) locally and starts the battle without a server.
 * The original mod forced r3 = 3 on every call. Patch: the prologue's `mov r4, r3` becomes `mov r4, #3`. With bots != 0 also args[6] (`ldr r0,[fp,#0x10]`
 * 0x44 bytes into the function) becomes `mov r0, #1` (the mod's "with bots" option; INFERRED). Found by the 7 prologue words, which must occur exactly once.
 * Returns the number of patches (0 = not found or not unique; *why says which). UNVERIFIED ON HARDWARE. */
int bv_gamefix_plan_localbattle(const uint8_t *text, uint32_t size, int bots, at_patch *out, int max, uint32_t *func, char *why, unsigned whyn);

#endif
