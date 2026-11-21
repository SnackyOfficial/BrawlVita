/* Oversized RGBA textures. The game's biggest UI atlas (sc/ui_tex.sc) is 4092x4096 RGBA8888 = 64 MB; the Vita's GPU memory pools (96 MB VRAM, 71 MB RAM, 26 MB
 * phycont, shared with every other texture) cannot give that much in one block, vitaGL's allocation fails, the texture has no storage, and the UI pieces that need it
 * are never created (log of 2026-10-28: all four upload strips "skipped", then NULL lookups in the UI code). A texture above TS_LIMIT_BYTES is therefore stored with
 * every side divided by 2^shift (4092x4096 -> 2046x2048 = 16 MB) and the uploads are box-filtered down. Texture coordinates are normalised, so the picture is the same,
 * only softer. */
#ifndef BV_TEXSHRINK_H
#define BV_TEXSHRINK_H

#include <stddef.h>
#include <stdint.h>

#define TS_LIMIT_BYTES (24u << 20)
#define TS_MAX_TRACKED 64
#define TS_PLACEHOLDER 99 /* ts_set value: this texture is only a 1x1 stand-in (unsupported format, or vitaGL could not allocate it); uploads into it are skipped */

/* Halvings per side (0 = fits as it is, 1..3) that bring width x height x bpp under TS_LIMIT_BYTES; -1 when the size is not divisible by the factor needed. */
int ts_shift_for(int width, int height, int bytes_per_pixel);

/* The same for a memory budget (bytes the stored texture may take, e.g. three quarters of the largest free GPU pool): halvings per side 0..3 so that
 * width*height*bytes_per_pixel / 4^shift <= budget; -1 when even 1/8 per side does not fit; -2 when the size is not divisible by the factor needed. */
int ts_shift_for_budget(int width, int height, int bytes_per_pixel, uint64_t budget);

/* Which texture names are stored shrunk, and by how much (a small table: only the few big textures are in it). */
void ts_set(unsigned id, int shift);
int ts_get(unsigned id); /* 0 when not shrunk */
void ts_clear(unsigned id);

/* Box filter: src is w x h RGBA8, tightly packed; dst receives (w >> shift) x (h >> shift). w and h must be divisible by 1 << shift. */
void ts_box_down_rgba(const uint8_t *src, int w, int h, int shift, uint8_t *dst);

/* ---- 16-bit storage (update 51). Run 33 (2026-11-12): the game asks for ~220 MB of RGBA8 textures, the GPU pools hold 96 + 71 + 26 MB; the RAM pool reached 0 at
 * 94 s on the home screen, after which brawlers, quests and the battle's bot textures were missing. Large RGBA8 textures are therefore stored as RGBA4444 (half the
 * memory); every upload into them is converted (ordered dither on colour, rounding on alpha). A texture that is attached to a framebuffer is made RGBA8 again. ---- */
#define T16_MAX 512
void t16_set(unsigned id, int w, int h);       /* texture `id` (stored size w x h) is RGBA4444 */
int t16_get(unsigned id, int *w, int *h);      /* 1 when it is; w / h may be NULL */
void t16_clear(unsigned id);
int t16_count(void);
/* update 52: textures that were ever attached to a framebuffer as colour (render targets); they are never stored as RGBA4444 */
void rt_mark(unsigned id);
int rt_is(unsigned id);
void rt_clear(unsigned id);
/* src: w x h RGBA8 tightly packed, at texel offset (x0, y0) of the texture (for a dither pattern that lines up across pieces); dst: w x h GL_UNSIGNED_SHORT_4_4_4_4
 * (R in bits 15..12, G 11..8, B 7..4, A 3..0). */
void ts_rgba8_to_4444(const uint8_t *src, int w, int h, int x0, int y0, uint16_t *dst);
extern int ts_lut; /* config t16_lut (0; update 88 experiment H): table-driven ts_rgba8_to_4444, identical output */

/* Framebuffer objects, as far as the shim needs to know them: which one is bound and which have a colour attachment. vitaGL's attach function takes COLOR_ATTACHMENT0 only
 * (anything else is GL_INVALID_ENUM), so a framebuffer the game builds with a depth attachment alone has no render target, and clearing or drawing into it is where
 * vitaGL died in the run of 2026-11-04 (glClear -> update_scissor_test). */
#define TS_MAX_FBO 64
void fbo_bind(unsigned id);               /* glBindFramebuffer: 0 = the screen */
unsigned fbo_current(void);
void fbo_set_color(unsigned id, int on);  /* a colour texture was attached (on = 1) or detached */
int fbo_has_color(unsigned id);
void fbo_forget(unsigned id);             /* glDeleteFramebuffers */
int fbo_unusable(void);                   /* a framebuffer object is bound and has no colour attachment: nothing may be cleared or drawn into it */
void fbo_note_draw(int x, int y, int w, int h); /* a draw call with this viewport went into the bound framebuffer (ignored for the screen) */
int fbo_take_drawn(unsigned id, int *x, int *y, int *w, int *h); /* 1 + the union of those viewports since the last take (then reset); 0 = nothing drawn */

#endif
