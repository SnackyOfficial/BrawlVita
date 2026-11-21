/* OpenGL ES 2 entry points of libg.so onto vitaGL.
 *
 * - Functions with only integer/pointer arguments have the same calling convention in the soft-float Android code and in the
 *   hard-float console code, so the vitaGL symbol is handed over directly (SHIM_PASS).
 * - Functions taking a GLfloat receive the bits in integer registers: they get a wrapper (uint32_t in, real float out).
 * - glCompressedTexImage2D transcodes ETC1 (which the GPU cannot sample) to DXT1 on the fly (gfx/etc1.c).
 * - glGetString answers what the game's feature checks look for.
 * UNVERIFIED ON HARDWARE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vitaGL.h>

#include "../gfx/etc1.h"
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include "../loader/log.h"
#include "../loader/stats.h"
#include "texshrink.h"
#include "shim.h"
extern int bv_gl_width, bv_gl_height;   /* update 167: screen_res (shim_egl.c) */

#define GL_ETC1_RGB8_OES 0x8D64

/* ---- frame-window trace (config gltrace_frame=N, 0 = off): every call of the wrapped set below is logged while the EGL swap counter is N or N+1, i.e. two whole frames,
 * so the game's real call order (viewport, framebuffers, blits, programs, matrices, draws) can be read from the log. Added for the upside-down 3D brawler of 2026-11-11
 * (rendered into a 512x384 texture FBO, shown flipped). At most 6000 lines. */
int bv_gltrace_frame;
extern volatile uint32_t bv_egl_swaps;
static volatile uint32_t g_win_lines;
static uint32_t g_win_span = 1, g_win_cap = 6000; /* update 90: crash_trace widens both */
static int win_active(void) {
    if (!bv_gltrace_frame) return 0;
    uint32_t s = bv_egl_swaps;
    return s >= (uint32_t)bv_gltrace_frame && s <= (uint32_t)bv_gltrace_frame + g_win_span && g_win_lines < g_win_cap;
}
static int win(void) {
    if (!win_active()) return 0;
    return __sync_add_and_fetch(&g_win_lines, 1) <= g_win_cap;
}
/* update 90 (ISSUES A5): config crash_trace=N; armed once by a game stdout line (shims/shim_io.c: 'prepareIntroSceneRenderer') */
int bv_crash_trace;
static volatile int g_ct_armed;
void bv_crash_trace_arm(const char *why) {
    if (bv_crash_trace <= 0 || __sync_lock_test_and_set(&g_ct_armed, 1)) return;
    g_win_span = (uint32_t)bv_crash_trace;
    g_win_cap = 30000;
    g_win_lines = 0;
    uint32_t s = bv_egl_swaps;
    bv_gltrace_frame = (int)(s ? s : 1);
    bv_log("crash_trace: armed by '%s' at swap %u: every traced GL call and texture / framebuffer op is logged for %d swaps (W[...] / R[...] lines)", why, (unsigned)s,
           bv_crash_trace);
}
#define WIN(...) do { if (win()) bv_log("W[%u] " __VA_ARGS__); } while (0)

static void w_glClearColor(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { glClearColor(bv_u2f(r), bv_u2f(g), bv_u2f(b), bv_u2f(a)); }
static void w_glTexParameterf(GLenum target, GLenum pname, uint32_t v) { glTexParameterf(target, pname, bv_u2f(v)); }
static void w_glUniform1f(GLint loc, uint32_t x) { glUniform1f(loc, bv_u2f(x)); }
static void fc_note_uniform2(GLint loc, float x, float y); /* update 59, below */
static void w_glUniform2f(GLint loc, uint32_t x, uint32_t y) { fc_note_uniform2(loc, bv_u2f(x), bv_u2f(y)); glUniform2f(loc, bv_u2f(x), bv_u2f(y)); }
static void w_glUniform2fv(GLint loc, GLsizei n, const GLfloat *v) { if (n >= 1 && v) fc_note_uniform2(loc, v[0], v[1]); glUniform2fv(loc, n, v); }
static void w_glUniform3f(GLint loc, uint32_t x, uint32_t y, uint32_t z) { glUniform3f(loc, bv_u2f(x), bv_u2f(y), bv_u2f(z)); }
static void w_glUniform4f(GLint loc, uint32_t x, uint32_t y, uint32_t z, uint32_t w) { glUniform4f(loc, bv_u2f(x), bv_u2f(y), bv_u2f(z), bv_u2f(w)); }

/* Build B imports more than Build A: two float-argument calls, and two ES3 calls vitaGL does not have. */
static void w_glDepthRangef(uint32_t n, uint32_t f) { glDepthRangef(bv_u2f(n), bv_u2f(f)); }
static void w_glPolygonOffset(uint32_t factor, uint32_t units) { glPolygonOffset(bv_u2f(factor), bv_u2f(units)); }
/* multisampled renderbuffers are not available: fall back to a single-sample one */
static void w_glRenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum fmt, GLsizei w, GLsizei h) {
    (void)samples;
    glRenderbufferStorage(target, fmt, w, h);
}
static void w_glInvalidateFramebuffer(GLenum target, GLsizei n, const GLenum *attachments) {
    (void)target; (void)n; (void)attachments; /* a hint only */
}

volatile uint32_t bv_st_etc1_n, bv_st_etc1_bytes, bv_st_etc1_us;

/* 2 = report OpenGL ES 2.0 (default), 3 = report OpenGL ES 3.0; set from config.txt (gles=3) before the game runs */
int bv_gles_major = 2;

/* ---- oversized RGBA textures (shims/texshrink.h): stored at 1/2^shift per side, uploads averaged down ---- */
static volatile GLuint g_bound2d; /* vitaGL has one global binding state, so one variable is the same state */

static void w_glBindTexture(GLenum target, GLuint tex) {
    if (target == GL_TEXTURE_2D) g_bound2d = tex;
    WIN("glBindTexture(0x%x, %u)", (unsigned)bv_egl_swaps, (unsigned)target, (unsigned)tex);
    glBindTexture(target, tex);
}

static void tex_note_height(GLuint id, int h); /* update 58, below */
static void fbo_note_tex(unsigned fbo, GLuint tex); /* update 58, below */
static void glr_note(char op, uint32_t id, uint32_t w, uint32_t h, uint32_t ty); /* update 52 tripwire, below */
void bv_gl_mem_check(const char *where);
static void w_glDeleteTextures(GLsizei n, const GLuint *ids) {
    for (GLsizei i = 0; i < n; i++) {
        ts_clear(ids[i]);
        t16_clear(ids[i]);
        rt_clear(ids[i]);
        if (ids[i] == g_bound2d) g_bound2d = 0;
    }
    glDeleteTextures(n, ids);
    for (GLsizei i = 0; i < n && i < 4; i++) glr_note('D', ids[i], 0, 0, 0);
    bv_gl_mem_check("glDeleteTextures");
}

/* ---- update 52: GPU-memory tripwire. Runs 33 and 36 both showed vitaGL's RAM pool at "2031/71 MB free" (free > total = its bookkeeping is broken) right
 * before the battle crashed, with no usable dump. A ring of the last texture / framebuffer operations is logged the first time free > total is seen. ---- */
static volatile uint32_t g_teximg_calls; /* (tentative declaration; defined with the texture counters below) */
#define GLR_N 48
static struct { uint32_t t_ms; char op; uint32_t id, w, h, ty; uint32_t ram_kb; } g_glr[GLR_N];
static volatile uint32_t g_glr_w;
static int g_glr_reported;
static void glr_note(char op, uint32_t id, uint32_t w, uint32_t h, uint32_t ty) {
    uint32_t i = __sync_fetch_and_add(&g_glr_w, 1) % GLR_N;
    g_glr[i].t_ms = (uint32_t)(sceKernelGetProcessTimeWide() / 1000);
    g_glr[i].op = op; g_glr[i].id = id; g_glr[i].w = w; g_glr[i].h = h; g_glr[i].ty = ty;
    g_glr[i].ram_kb = (op == 'F' || op == 'K') ? 0 : (uint32_t)(vglMemFree(VGL_MEM_RAM) >> 10); /* F / K: no pool query (frequent) */
    if (g_ct_armed && win()) bv_log("R[%u] %c id %u %ux%u 0x%x ram %u KB", (unsigned)bv_egl_swaps, op, (unsigned)id, (unsigned)w, (unsigned)h, (unsigned)ty,
                                  (unsigned)g_glr[i].ram_kb); /* update 90: live during crash_trace */
}
/* cycle 6: the ring on demand (status thread: no frame for 3 s). F = bind framebuffer (id = fbo), K = glClear (id = fbo, w = mask) */
void bv_gl_ring_dump(const char *why) {
    bv_log("GL STALL (%s): last texture / framebuffer operations (I teximage, C compressed, D delete, A attach, R RGBA8 again, B/U buffers, X delete buffers, "
           "M mipmap, Q/Y renderbuffer, F bind framebuffer, K clear):", why);
    uint32_t w = g_glr_w, n = w < GLR_N ? w : GLR_N;
    for (uint32_t k = w - n; k != w; k++) {
        uint32_t i = k % GLR_N;
        bv_log("  [%u ms] %c id %u %ux%u 0x%x ram %u KB", (unsigned)g_glr[i].t_ms, g_glr[i].op, (unsigned)g_glr[i].id, (unsigned)g_glr[i].w, (unsigned)g_glr[i].h,
               (unsigned)g_glr[i].ty, (unsigned)g_glr[i].ram_kb);
    }
}
/* also called once per second from the status line (loader/gameapp.c) */
void bv_gl_mem_check(const char *where) {
    if (g_glr_reported) return;
    /* vitaGL starts at the first eglGetDisplay; asking its pools before that crashed update 52 at 1 s (status line). Only after the first texture. */
    if (!g_teximg_calls) return;
    size_t f = vglMemFree(VGL_MEM_RAM), t = vglMemTotal(VGL_MEM_RAM);
    if (!t || f <= t) return;
    g_glr_reported = 1;
    bv_log("GPU MEMORY TRIPWIRE (%s): vitaGL RAM pool reports %u KB free of %u KB total: its bookkeeping is broken. Last texture / framebuffer operations "
           "(op: I teximage, C compressed, D delete tex, A attach, R re-made RGBA8, B/U buffer data/sub, X delete buffers, M mipmap, Q/Y renderbuffer storage/delete; ram = RAM pool free KB after it):", where, (unsigned)(f >> 10), (unsigned)(t >> 10));
    uint32_t w = g_glr_w, n = w < GLR_N ? w : GLR_N;
    for (uint32_t k = w - n; k != w; k++) {
        uint32_t i = k % GLR_N;
        bv_log("  [%u ms] %c tex %u %ux%u type 0x%x ram %u KB", (unsigned)g_glr[i].t_ms, g_glr[i].op, (unsigned)g_glr[i].id, (unsigned)g_glr[i].w, (unsigned)g_glr[i].h,
               (unsigned)g_glr[i].ty, (unsigned)g_glr[i].ram_kb);
    }
}

/* Free texture memory: the largest of vitaGL's pools (a texture takes its memory from one of them; the status line shows all three). */
static size_t gpu_free_max(void) {
    size_t a = vglMemFree(VGL_MEM_VRAM), b = vglMemFree(VGL_MEM_RAM), c = vglMemFree(VGL_MEM_PHYCONT);
    if (b > a) a = b;
    return c > a ? c : a;
}

static unsigned bytes_per_texel(GLenum format, GLenum type) {
    if (type == 0x8033 || type == 0x8034 || type == 0x8363) return 2; /* UNSIGNED_SHORT_4_4_4_4 / 5_5_5_1 / 5_6_5 */
    if (format == 0x190a || format == 0x1906 || format == 0x1909) return format == 0x190a ? 2 : 1; /* LUMINANCE_ALPHA 2, ALPHA / LUMINANCE 1 */
    return 4; /* RGBA, and RGB too: the GPU stores it as 4 bytes */
}

#ifndef GL_UNSIGNED_SHORT_4_4_4_4
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033 /* (vitaGL.h has it; the host test's fake header may not) */
#endif
/* update 88: wall time inside ts_rgba8_to_4444 only (the tex line's total includes vitaGL) */
volatile uint32_t bv_st_t16_us;
#define T16_TIMED(call) do { uint64_t t16_0_ = sceKernelGetProcessTimeWide(); call; __sync_fetch_and_add(&bv_st_t16_us, (uint32_t)(sceKernelGetProcessTimeWide() - t16_0_)); } while (0)
static volatile uint32_t g_teximg_calls, g_teximg_failed, g_placeholders, g_tex16_n;
int bv_tex16_kb = 1024; /* config tex16=KB: RGBA8 textures of at least this size are stored as RGBA4444 (update 51); 0 = off */

/* Depth / depth-stencil textures: vitaGL cannot make them (its framebuffer attach takes a colour attachment only, and its glTexImage2D leaves such a texture without storage:
 * the game's own "Stage rendering error 1280" = GL_INVALID_ENUM, and the dump of 2026-11-03 ended right after a 1x1 GL_DEPTH_COMPONENT glTexImage2D). */
static int depth_format(GLint ifmt, GLenum fmt) {
    return fmt == 0x1902 || fmt == 0x84f9 || ifmt == 0x1902 || ifmt == 0x84f9 || (ifmt >= 0x81a5 && ifmt <= 0x81a7) || ifmt == 0x88f0;
}

/* A texture that must exist but cannot be made as asked becomes a 1x1 RGBA stand-in, remembered as such, so that no later call finds a texture without storage. */
static void make_placeholder(GLuint id, const char *why, int w, int h, GLenum fmt) {
    static const uint8_t zero[4] = {0, 0, 0, 0};
    ts_set(id, TS_PLACEHOLDER);
    uint32_t k = __sync_add_and_fetch(&g_placeholders, 1);
    if (k <= 30) bv_log("glTexImage2D: texture %u (%dx%d, fmt 0x%x) %s: replaced by a 1x1 RGBA stand-in, uploads into it are skipped (#%u)", (unsigned)id, w, h, (unsigned)fmt, why, (unsigned)k);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero);
}

/* The run of 2026-11-03 died inside vitaGL's glTexImage2D with video memory at 0 MB free (a NULL from an allocation, then a write through it). So vitaGL is only asked to
 * allocate what fits: an RGBA texture is shrunk to the memory that is left (3/4 of the largest pool), anything else that cannot fit is skipped, and every call is logged. */
static void tex_image_inner(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type,
                            const GLvoid *pixels);
static void tex_account(uint64_t t0, GLsizei w, GLsizei h, GLenum format, GLenum type);
static void w_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type,
                           const GLvoid *pixels) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    tex_image_inner(target, level, internalformat, width, height, border, format, type, pixels);
    if (pixels) tex_account(t0, width, height, format, type);
    if (level == 0) {
        {   /* update 58: stored height (a shrunk texture is 1/2^shift per side) */
            int sh = ts_get(g_bound2d);
            tex_note_height(g_bound2d, (sh > 0 && sh < TS_PLACEHOLDER) ? ((int)height >> sh) : (sh == TS_PLACEHOLDER ? 1 : (int)height));
        }
        glr_note('I', g_bound2d, (uint32_t)width, (uint32_t)height, t16_get(g_bound2d, NULL, NULL) ? 0x8033u : (uint32_t)type);
        bv_gl_mem_check("glTexImage2D");
    }
}
static void tex_image_inner(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type,
                            const GLvoid *pixels) {
    GLuint id = g_bound2d;
    size_t avail = gpu_free_max();
    uint64_t need = (uint64_t)(width > 0 ? width : 0) * (uint64_t)(height > 0 ? height : 0) * bytes_per_texel(format, type);
    uint32_t call = __sync_add_and_fetch(&g_teximg_calls, 1);
    if (call <= 60 || need >= (1u << 20))
        BV_TRACE(1, "glTexImage2D #%u: tex %u level %d %dx%d ifmt 0x%x fmt 0x%x type 0x%x %s = %u KB; largest free pool %u KB", call, (unsigned)id, (int)level, (int)width, (int)height,
                 (unsigned)internalformat, (unsigned)format, (unsigned)type, pixels ? "with data" : "no data", (unsigned)(need >> 10), (unsigned)(avail >> 10));
    if (target == GL_TEXTURE_2D && level == 0 && id && ts_get(id) == TS_PLACEHOLDER) ts_clear(id); /* the name is specified again */
    if (target == GL_TEXTURE_2D && level == 0 && id && depth_format(internalformat, format)) {
        make_placeholder(id, "is a depth texture, which vitaGL cannot make", (int)width, (int)height, format);
        return;
    }
    if (target == GL_TEXTURE_2D && level > 0 && id && ts_get(id) == TS_PLACEHOLDER) return;
    if (target == GL_TEXTURE_2D && level == 0 && need > 0 && need + need / 8 > avail && !(format == GL_RGBA && type == GL_UNSIGNED_BYTE)) {
        bv_log("glTexImage2D: %dx%d (fmt 0x%x) needs %u KB but only %u KB are free: NOT uploaded (vitaGL would crash)", (int)width, (int)height, (unsigned)format, (unsigned)(need >> 10),
               (unsigned)(avail >> 10));
        return;
    }
    if (target == GL_TEXTURE_2D && id && format == GL_RGBA && type == GL_UNSIGNED_BYTE) {
        int shift = ts_get(id);
        if (level > 0 && shift) { /* a mip level of a shrunk texture: the chain is not kept (the base level carries the picture) */
            BV_TRACE(1, "glTexImage2D: mip level %d (%dx%d) of shrunk texture %u skipped", (int)level, (int)width, (int)height, (unsigned)id);
            return;
        }
        /* update 51: large RGBA8 textures are stored as RGBA4444 (texshrink.h) */
        /* update 52: never for a texture that was attached to a framebuffer (run 36: the game re-specified render target 62 at a new size while it was
         * attached, and the RGBA4444 -> RGBA8 round trip re-allocated it twice) */
        int as16 = level == 0 ? (bv_tex16_kb > 0 && border == 0 && need >= (uint64_t)bv_tex16_kb << 10 && !rt_is(id)) : t16_get(id, NULL, NULL);
        if (level == 0 && !as16) t16_clear(id);
        int bpp = as16 ? 2 : 4;
        if (as16) need /= 2;
        if (level == 0) {
            uint64_t budget = (uint64_t)avail * 3 / 4;
            if (budget > TS_LIMIT_BYTES) budget = TS_LIMIT_BYTES;
            int want = ts_shift_for_budget((int)width, (int)height, bpp, budget);
            if (want < 0 && need > 0 && need + need / 8 > avail) {
                bv_log("glTexImage2D: %dx%d RGBA needs %u KB, only %u KB are free and it cannot be shrunk enough: NOT uploaded (vitaGL would crash)", (int)width, (int)height,
                       (unsigned)(need >> 10), (unsigned)(avail >> 10));
                return;
            }
            if (want > 0 && border == 0) {
                int w2 = (int)width >> want, h2 = (int)height >> want;
                if (!pixels) {
                    ts_set(id, want);
                    if (as16) t16_set(id, w2, h2);
                    bv_log("glTexImage2D: texture %u, %dx%d RGBA = %u MB does not fit the GPU memory: stored at %dx%d (1/%d per side)%s, uploads are averaged down", (unsigned)id,
                           (int)width, (int)height, (unsigned)(((uint64_t)width * height * 4) >> 20), w2, h2, 1 << want, as16 ? " as RGBA4444" : "");
                    glTexImage2D(target, level, internalformat, w2, h2, border, format, as16 ? GL_UNSIGNED_SHORT_4_4_4_4 : type, NULL);
                    return;
                }
                uint8_t *small = (uint8_t *)malloc((size_t)w2 * (size_t)h2 * 4);
                if (small) {
                    ts_box_down_rgba((const uint8_t *)pixels, (int)width, (int)height, want, small);
                    ts_set(id, want);
                    if (as16) t16_set(id, w2, h2);
                    bv_log("glTexImage2D: texture %u, %dx%d RGBA = %u MB does not fit the GPU memory: uploaded at %dx%d (1/%d per side)%s", (unsigned)id, (int)width, (int)height,
                           (unsigned)(((uint64_t)width * height * 4) >> 20), w2, h2, 1 << want, as16 ? " as RGBA4444" : "");
                    if (as16) T16_TIMED(ts_rgba8_to_4444(small, w2, h2, 0, 0, (uint16_t *)small)); /* in place: the 16-bit row k ends before the 32-bit row k+1 starts */
                    glTexImage2D(target, level, internalformat, w2, h2, border, format, as16 ? GL_UNSIGNED_SHORT_4_4_4_4 : type, small);
                    free(small);
                    return;
                }
                bv_log("glTexImage2D: %dx%d is too big and there is no memory to shrink it: passed on unchanged", (int)width, (int)height);
            } else if (want < 0) {
                bv_log("glTexImage2D: %dx%d RGBA cannot be halved evenly: passed on unchanged", (int)width, (int)height);
            } else {
                ts_clear(id); /* the name is reused for a normal size */
            }
        }
        if (as16) {
            uint16_t *p16 = NULL;
            if (pixels) {
                p16 = (uint16_t *)malloc((size_t)width * (size_t)height * 2);
                if (!p16) {
                    bv_log("glTexImage2D: no memory (%u KB) to convert texture %u to RGBA4444: stored as RGBA8", (unsigned)(need >> 10), (unsigned)id);
                    t16_clear(id);
                    goto rgba8;
                }
                T16_TIMED(ts_rgba8_to_4444((const uint8_t *)pixels, (int)width, (int)height, 0, 0, p16));
            }
            if (level == 0) {
                t16_set(id, (int)width, (int)height);
                uint32_t k = __sync_add_and_fetch(&g_tex16_n, 1);
                if (k <= 40) bv_log("glTexImage2D: texture %u, %dx%d RGBA8 = %u KB stored as RGBA4444 (%u KB) (#%u)", (unsigned)id, (int)width, (int)height, (unsigned)(need >> 9),
                                    (unsigned)(need >> 10), (unsigned)k);
            }
            glTexImage2D(target, level, internalformat, width, height, border, format, GL_UNSIGNED_SHORT_4_4_4_4, p16);
            free(p16);
            if (level == 0 && vglGetTexDataPointer(target) == NULL) {
                t16_clear(id);
                bv_log("glTexImage2D: vitaGL could NOT allocate %dx%d RGBA4444 (largest free pool was %u KB)", (int)width, (int)height, (unsigned)(avail >> 10));
                if (id) make_placeholder(id, "could not be allocated", (int)width, (int)height, format);
            }
            return;
        }
    }
rgba8:
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
    if (target == GL_TEXTURE_2D && level == 0 && vglGetTexDataPointer(target) == NULL) {
        uint32_t k = __sync_add_and_fetch(&g_teximg_failed, 1);
        if (k <= 25) bv_log("glTexImage2D: vitaGL could NOT allocate %dx%d (fmt 0x%x): the texture has no storage (#%u; largest free pool was %u KB)", (int)width, (int)height, (unsigned)format, (unsigned)k,
                            (unsigned)(avail >> 10));
        if (id) make_placeholder(id, "could not be allocated", (int)width, (int)height, format);
    }
}

/* ---- call trace of the state and framebuffer calls (the loader's only record of which GL call a crash happens in: the dump of 2026-11-04 stopped in vitaGL's glEnable, and
 * its saved pc is not the exact instruction). The first 150 calls are logged, then 200 more after the first stand-in texture was made. The line is written BEFORE the call. ---- */
static volatile uint32_t g_gltrace_n, g_gltrace_after;
static volatile uint32_t g_fbo_log_n;
static int fbo_log_ok(void) { return __sync_add_and_fetch(&g_fbo_log_n, 1) <= 400; } /* framebuffer calls are rare: they get their own, larger budget */
static int gl_trace_ok(void) {
    uint32_t n = __sync_add_and_fetch(&g_gltrace_n, 1);
    if (n <= 150) return 1;
    if (g_placeholders) return __sync_add_and_fetch(&g_gltrace_after, 1) <= 200;
    return 0;
}
static GLuint g_cur_prog;
/* update 59 (fragcoord_fix=4): programs containing a gl_FragCoord shader, their u_viewportOffset location and the game's last value */
#define FC_PROGS 1024
static uint8_t g_prog_fc[FC_PROGS], g_prog_off_set[FC_PROGS];
static GLint g_prog_off_loc[FC_PROGS]; /* update 60: vitaGL returns POINTERS as locations (run 53: 2082340384); int16_t truncated them */
static float g_prog_off[FC_PROGS][2];
static void fcy4_before_draw(void);
extern int bv_fragcoord_fix;        /* defined with the gl_FragCoord rewrite below */
static void fcy_before_draw(void);  /* update 49: sets the bv_fcy uniform for the current draw target */
static GLint g_vp[4];
static void w_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { g_vp[0] = x; g_vp[1] = y; g_vp[2] = w; g_vp[3] = h; WIN("glViewport(%d, %d, %d, %d) fbo %u", (unsigned)bv_egl_swaps, (int)x, (int)y, (int)w, (int)h, fbo_current()); glViewport(x, y, w, h); }
static void w_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { WIN("glScissor(%d, %d, %d, %d)", (unsigned)bv_egl_swaps, (int)x, (int)y, (int)w, (int)h); glScissor(x, y, w, h); }
static void w_glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter) {
    WIN("glBlitFramebuffer(src %d,%d-%d,%d -> dst %d,%d-%d,%d, mask 0x%x, filter 0x%x) fbo %u", (unsigned)bv_egl_swaps, (int)sx0, (int)sy0, (int)sx1, (int)sy1, (int)dx0, (int)dy0, (int)dx1,
        (int)dy1, (unsigned)mask, (unsigned)filter, fbo_current());
    glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
}
static void w_glUseProgram(GLuint p) { g_cur_prog = p; WIN("glUseProgram(%u)", (unsigned)bv_egl_swaps, (unsigned)p); glUseProgram(p); }
static void w_glCullFace(GLenum m) { WIN("glCullFace(0x%x)", (unsigned)bv_egl_swaps, (unsigned)m); glCullFace(m); }
static void w_glActiveTexture(GLenum t) { WIN("glActiveTexture(0x%x)", (unsigned)bv_egl_swaps, (unsigned)t); glActiveTexture(t); }
static void w_glBindBuffer(GLenum t, GLuint b) { WIN("glBindBuffer(0x%x, %u)", (unsigned)bv_egl_swaps, (unsigned)t, (unsigned)b); glBindBuffer(t, b); }
static void w_glUniformMatrix4fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat *v) {
    if (v) WIN("glUniformMatrix4fv(loc %d, n %d, tr %d) prog %u = [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]", (unsigned)bv_egl_swaps, (int)loc,
               (int)n, (int)tr, (unsigned)g_cur_prog, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]);
    glUniformMatrix4fv(loc, n, tr, v);
}
static void w_glUniform4fv(GLint loc, GLsizei n, const GLfloat *v) {
    if (v) WIN("glUniform4fv(loc %d, n %d) prog %u = %.4f %.4f %.4f %.4f", (unsigned)bv_egl_swaps, (int)loc, (int)n, (unsigned)g_cur_prog, v[0], v[1], v[2], v[3]);
    glUniform4fv(loc, n, v);
}
static void w_glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *p) {
    WIN("glVertexAttribPointer(%u, size %d, type 0x%x, norm %d, stride %d, %p)", (unsigned)bv_egl_swaps, (unsigned)i, (int)size, (unsigned)type, (int)norm, (int)stride, p);
    glVertexAttribPointer(i, size, type, norm, stride, p);
}

int bv_fbo_stencil_off = 2, bv_stencil_probe;   /* update 171 (icons fix A); update 175: 2 by default (with ds_store=1, user pass) */
static int g_st_game;          /* what the game last asked for GL_STENCIL_TEST (0xb90) */
static int g_scissor_on;       /* update 184: GL_SCISSOR_TEST as the game set it */
static void stencil_sync(void);
static void w_glEnable(GLenum cap) {
    if (gl_trace_ok()) bv_log("glEnable(0x%x)", (unsigned)cap); else WIN("glEnable(0x%x)", (unsigned)bv_egl_swaps, (unsigned)cap);
    if (cap == 0xc11) g_scissor_on = 1;
    if (cap == 0xb90 && (bv_fbo_stencil_off || bv_stencil_probe)) { g_st_game = 1; stencil_sync(); return; }
    glEnable(cap);
}
static void w_glDisable(GLenum cap) {
    if (gl_trace_ok()) bv_log("glDisable(0x%x)", (unsigned)cap); else WIN("glDisable(0x%x)", (unsigned)bv_egl_swaps, (unsigned)cap);
    if (cap == 0xc11) g_scissor_on = 0;
    if (cap == 0xb90 && (bv_fbo_stencil_off || bv_stencil_probe)) { g_st_game = 0; stencil_sync(); return; }
    glDisable(cap);
}
static volatile uint32_t g_fbo_skipped;
/* ---- experiment fbo_flip_from=N (config, 0 = off): from EGL swap N on, when the game leaves a colour framebuffer it drew into, the drawn area (union of the draw
 * viewports, loader: fbo_note_draw) is flipped vertically in place: blit flipped into a scratch texture of that size, blit back. For the upside-down 3D brawler of
 * 2026-11-11 (drawn into tex 40 with viewport 3,3,185,375, then shown on a quad): if it is shown upright with the flip, the game expects the other row order. ---- */
int bv_fbo_flip_from;
/* One scratch framebuffer for every flip, SCRATCH_DIM x SCRATCH_DIM; a flip uses its lower-left w x h corner. (Run 5: one scratch per exact size ran out after 4 sizes,
 * because the drawn area of the brawler changes by a pixel or two every frame.) */
#define SCRATCH_DIM 512
static GLuint g_scratch_fbo, g_scratch_tex;
static int g_scratch_failed;
static volatile uint32_t g_flips, g_flip_fail;
static GLuint scratch_fbo(void) {
    if (g_scratch_fbo || g_scratch_failed) return g_scratch_fbo;
    GLint saved = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved);
    GLuint t = 0, f = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SCRATCH_DIM, SCRATCH_DIM, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    int ok = vglGetTexDataPointer(GL_TEXTURE_2D) != NULL;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, (GLuint)saved);
    if (!ok) {
        bv_log("fbo flip: no memory for the %dx%d scratch texture: flipping is off", SCRATCH_DIM, SCRATCH_DIM);
        g_scratch_failed = 1;
        return 0;
    }
    glGenFramebuffers(1, &f);
    if (!f) {
        bv_log("fbo flip: no framebuffer name for the scratch: flipping is off");
        glDeleteTextures(1, &t);
        g_scratch_failed = 1;
        return 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    g_scratch_fbo = f;
    g_scratch_tex = t;
    bv_log("fbo flip: scratch framebuffer %u with a %dx%d texture %u", (unsigned)f, SCRATCH_DIM, SCRATCH_DIM, (unsigned)t);
    return f;
}

/* At a screenshot frame (shim_egl.c shot_frame), each colour framebuffer the game leaves is read back whole and saved as shot_<N>_fbo<k>_<before|after>.tga,
 * before and (if a flip runs) after the flip: shows where the drawn picture really lies in the texture. Size from the union of the frame's viewports (x+w, y+h). */
extern int bv_shot_frames[4], bv_shot_n;
static int shot_frame_next(void) {
    for (int i = 0; i < bv_shot_n && i < 4; i++)
        if ((uint32_t)bv_shot_frames[i] == bv_egl_swaps + 1) return bv_shot_frames[i];
    return 0;
}
static int g_fbo_dumps;
static void dump_fbo(unsigned fb, int w, int h, int frame, const char *when) {
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || g_fbo_dumps >= 16) return;
    uint8_t *px = (uint8_t *)malloc((size_t)w * h * 4);
    if (!px) return;
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (size_t i = 0; i < (size_t)w * h; i++) { uint8_t t = px[4 * i]; px[4 * i] = px[4 * i + 2]; px[4 * i + 2] = t; }
    uint8_t hdr[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)h, (uint8_t)(h >> 8), 32, 8};
    char path[128];
    snprintf(path, sizeof path, BV_DATA_DIR "/shot_%d_fbo%d_%s.tga", frame, g_fbo_dumps / 2, when);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(hdr, 1, sizeof hdr, f);
        fwrite(px, 1, (size_t)w * h * 4, f);
        fclose(f);
        bv_log("fbo dump: framebuffer %u, %dx%d read back to %s", fb, w, h, path);
    }
    free(px);
    g_fbo_dumps++;
}

static void flip_drawn(unsigned fb) {
    int x, y, w, h;
    if (!fbo_take_drawn(fb, &x, &y, &w, &h)) return;
    int frame = shot_frame_next();
    int flip = bv_fbo_flip_from && bv_egl_swaps >= (uint32_t)bv_fbo_flip_from;
    if (frame) dump_fbo(fb, x + w, y + h, frame, "before");
    if (!flip) {
        if (frame) g_fbo_dumps++; /* keep the numbering of before/after pairs */
        return;
    }
    GLuint s = (w <= SCRATCH_DIM && h <= SCRATCH_DIM) ? scratch_fbo() : 0;
    if (!s) {
        if (__sync_add_and_fetch(&g_flip_fail, 1) <= 5) bv_log("fbo flip: framebuffer %u area %d,%d %dx%d NOT flipped (no scratch)", fb, x, y, w, h);
        return;
    }
    GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    if (sc) glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s);
    glBlitFramebuffer(x, y, x + w, y + h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb);
    glBlitFramebuffer(0, 0, w, h, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    if (sc) glEnable(GL_SCISSOR_TEST);
    if (frame) dump_fbo(fb, x + w, y + h, frame, "after");
    uint32_t k = __sync_add_and_fetch(&g_flips, 1);
    if (k <= 10 || !(k & (k - 1))) bv_log("fbo flip #%u: framebuffer %u area %d,%d %dx%d flipped vertically (swap %u)", (unsigned)k, fb, x, y, w, h, (unsigned)bv_egl_swaps);
}

static void w_glBindFramebuffer(GLenum t, GLuint fb) {
    unsigned prev = fbo_current();
    if (prev && prev != fb) flip_drawn(prev);
    if (fbo_log_ok()) bv_log("glBindFramebuffer(0x%x, %u)%s", (unsigned)t, (unsigned)fb, fb && !fbo_has_color(fb) ? "  [no colour attachment yet]" : "");
    fbo_bind(fb);
    glr_note('F', fb, 0, 0, 0); /* cycle 6 */
    WIN("glBindFramebuffer(%u)", (unsigned)bv_egl_swaps, (unsigned)fb);
    glBindFramebuffer(t, fb);
}
static void w_glDeleteFramebuffers(GLsizei n, const GLuint *ids) {
    for (GLsizei i = 0; i < n; i++) fbo_forget(ids[i]);
    glDeleteFramebuffers(n, ids);
}
static void w_glBindRenderbuffer(GLenum t, GLuint rb) { if (gl_trace_ok()) bv_log("glBindRenderbuffer(0x%x, %u)", (unsigned)t, (unsigned)rb); glBindRenderbuffer(t, rb); }
int bv_rt_fresh; /* config rt_fresh (0; update 92 experiment A5) */
static volatile uint32_t g_rt_fresh_n;
static GLuint fbo_tex_of(unsigned fbo); /* below */
extern int bv_depth_tex_rb;
static void w_glFramebufferTexture2D(GLenum t, GLenum att, GLenum tt, GLuint tex, GLint lvl) {
    unsigned fb = fbo_current();
    if (fbo_log_ok()) bv_log("glFramebufferTexture2D(0x%x, attachment 0x%x, 0x%x, tex %u, level %d) on fbo %u%s", (unsigned)t, (unsigned)att, (unsigned)tt, (unsigned)tex, (int)lvl, fb,
                             ts_get(tex) == TS_PLACEHOLDER ? "  [tex is a 1x1 stand-in]" : "");
    if (att != 0x8ce0) { /* depth / stencil / depth-stencil: vitaGL has no such attachment (GL_INVALID_ENUM); its framebuffers carry their own depth buffer */
        /* update 142 (F1, run 160): the shop tile previews render their model ONCE into a framebuffer whose depth is a depth TEXTURE; dropping that
         * attach left the framebuffer without depth and the model never appeared (tile targets read back empty), while the skin popup, which uses a
         * depth RENDERBUFFER, draws fine. depth_tex_rb=1: give such a framebuffer a depth renderbuffer instead (vitaGL sizes it to the framebuffer). */
        if (bv_depth_tex_rb && tex && fb && (att == 0x8d00 || att == 0x821a)) {
            static GLuint rb_of_fb[32][2];
            GLuint rb = 0;
            for (int i = 0; i < 32; i++) if (rb_of_fb[i][0] == fb) { rb = rb_of_fb[i][1]; break; }
            if (!rb) {
                glGenRenderbuffers(1, &rb);
                glBindRenderbuffer(GL_RENDERBUFFER, rb);
                glRenderbufferStorage(GL_RENDERBUFFER, 0x81a5, 512, 512);   /* GL_DEPTH_COMPONENT16; vitaGL makes the depth buffer at the framebuffer size */
                glBindRenderbuffer(GL_RENDERBUFFER, 0);
                for (int i = 0; i < 32; i++) if (!rb_of_fb[i][0]) { rb_of_fb[i][0] = fb; rb_of_fb[i][1] = rb; break; }
            }
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, 0x8d00, GL_RENDERBUFFER, rb);
            static int said;
            if (said++ < 12) bv_log("depth_tex_rb: fbo %u: depth texture %u -> depth renderbuffer %u", fb, (unsigned)tex, (unsigned)rb);
            return;
        }
        if (fbo_log_ok()) bv_log("  not forwarded: vitaGL only takes COLOR_ATTACHMENT0 (fbo %u %s a colour attachment)", fb, fbo_has_color(fb) ? "has" : "has NO");
        return;
    }
    int w16, h16;
    if (tex && att == 0x8ce0) rt_mark(tex); /* update 52: a render target from now on: never stored as RGBA4444 again */
    if (tex && t16_get(tex, &w16, &h16)) { /* update 51: a render target is kept RGBA8 (contents are drawn by the game, nothing to keep) */
        t16_clear(tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w16, h16, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindTexture(GL_TEXTURE_2D, g_bound2d);
        bv_log("glFramebufferTexture2D: texture %u (%dx%d) is a render target: made RGBA8 again (was RGBA4444)", (unsigned)tex, w16, h16);
        glr_note('R', tex, (uint32_t)w16, (uint32_t)h16, GL_UNSIGNED_BYTE);
    }
    glr_note('A', tex, 0, 0, 0);
    /* update 92 (ISSUES A5) experiment rt_fresh=1: a different texture on the same FBO first detaches the old one, so vitaGL drops the FBO's render
     * target (created with 1 scene per frame) and makes a new one: run 102 = 48 textures through one FBO inside one frame, then the GPU stopped */
    if (bv_rt_fresh && tex && fb) {
        GLuint prev = fbo_tex_of(fb);
        if (prev && prev != tex) {
            glFramebufferTexture2D(t, att, tt, 0, 0);
            uint32_t k = __sync_add_and_fetch(&g_rt_fresh_n, 1);
            if (k <= 8 || !(k & (k - 1))) bv_log("rt_fresh: fbo %u: texture %u -> %u, old render target dropped first (#%u)", fb, (unsigned)prev, (unsigned)tex, (unsigned)k);
        }
    }
    fbo_set_color(fb, tex != 0);
    fbo_note_tex(fb, tex); /* update 58 */
    glFramebufferTexture2D(t, att, tt, tex, lvl);
}
/* update 141 (F1, shop / pass previews blank; run 159: all seven tile render targets read back fully transparent black while the home brawler's target
 * has content). The preview target is the first one with a DEPTH renderbuffer (fmt 0x81a5). Experiments: preview_nodepth=1 skips that attach (vitaGL
 * then uses its own depth buffer); rt_probe=1 clears such a framebuffer to magenta so a screenshot shows whether the tile composite works at all. */
int bv_preview_nodepth, bv_rt_probe;
int bv_depth_tex_rb; /* update 142 (F1) config depth_tex_rb */
static GLuint g_rb_fbo[16];
static int rb_fbo_is(GLuint f) { for (int i = 0; i < 16; i++) if (f && g_rb_fbo[i] == f) return 1; return 0; }
static void w_glFramebufferRenderbuffer(GLenum t, GLenum att, GLenum rt, GLuint rb) {
    if (gl_trace_ok()) bv_log("glFramebufferRenderbuffer(0x%x, attachment 0x%x, 0x%x, rb %u)", (unsigned)t, (unsigned)att, (unsigned)rt, (unsigned)rb);
    if (att == 0x8d00 && rb) {   /* GL_DEPTH_ATTACHMENT */
        GLuint f = fbo_current();
        if (!rb_fbo_is(f)) for (int i = 0; i < 16; i++) if (!g_rb_fbo[i]) { g_rb_fbo[i] = f; break; }
        static int said;
        if (said++ < 4) bv_log("fbo %u gets a depth renderbuffer %u%s", (unsigned)f, (unsigned)rb, bv_preview_nodepth ? " - NOT attached (preview_nodepth=1)" : "");
        if (bv_preview_nodepth) return;
    }
    glFramebufferRenderbuffer(t, att, rt, rb);
}
static GLenum w_glCheckFramebufferStatus(GLenum t) {
    GLenum r = glCheckFramebufferStatus(t);
    if (gl_trace_ok()) bv_log("glCheckFramebufferStatus(0x%x) -> 0x%x", (unsigned)t, (unsigned)r);
    return r;
}
static void w_glGenFramebuffers(GLsizei n, GLuint *ids) {
    glGenFramebuffers(n, ids);
    if (gl_trace_ok()) bv_log("glGenFramebuffers(%d) -> %u", (int)n, n > 0 && ids ? (unsigned)ids[0] : 0u);
}
static void w_glGenRenderbuffers(GLsizei n, GLuint *ids) {
    glGenRenderbuffers(n, ids);
    if (gl_trace_ok()) bv_log("glGenRenderbuffers(%d) -> %u", (int)n, n > 0 && ids ? (unsigned)ids[0] : 0u);
}
static void w_glRenderbufferStorage(GLenum t, GLenum f, GLsizei w, GLsizei h) {
    if (gl_trace_ok()) bv_log("glRenderbufferStorage(0x%x, fmt 0x%x, %dx%d)", (unsigned)t, (unsigned)f, (int)w, (int)h);
    glRenderbufferStorage(t, f, w, h);
    glr_note('Q', 0, (uint32_t)w, (uint32_t)h, (uint32_t)f); /* update 66 */
    bv_gl_mem_check("glRenderbufferStorage");
}
static void w_glDepthMask(GLboolean m) { if (gl_trace_ok()) bv_log("glDepthMask(%d)", (int)m); glDepthMask(m); }
static int skip_unusable_fbo(const char *what) {
    if (!fbo_unusable()) return 0;
    uint32_t k = __sync_add_and_fetch(&g_fbo_skipped, 1);
    if (k <= 20) bv_log("%s skipped: framebuffer %u has no colour attachment (vitaGL would crash here) (#%u)", what, fbo_current(), (unsigned)k);
    return 1;
}
extern volatile uint32_t bv_gxm_rt_live __attribute__((weak)), bv_gxm_rt_made __attribute__((weak)), bv_gxm_scenes __attribute__((weak)), bv_gxm_errors __attribute__((weak)); /* shims/shim_gxm.c, update 94 (weak: host tests) */
#define GXMV(x) (&(x) ? (unsigned)(x) : 0u)
int bv_fbo_finish; /* config fbo_finish (0; update 93 experiment A5) */
static int g_st_applied = -1;  /* what vitaGL currently has */
static int g_disp_st_ok;       /* the display stencil plane still holds this scene sequence's mask */
static GLenum g_sf_func; static GLint g_sf_ref; static GLuint g_sf_mask; static GLboolean g_cm[4] = {1, 1, 1, 1};
extern uint32_t bv_battle_kind_addr __attribute__((weak));   /* loader/touch_vita.c: libg battle-kind global (0 = no battle) */
static void stencil_sync(void) {
    int want = g_st_game;
    /* update 173: menus only - in a battle the ground is drawn through stencil-tested offscreen passes (runs 203 / 204: black map with the test off) */
    int in_battle = &bv_battle_kind_addr && bv_battle_kind_addr && *(volatile int *)(uintptr_t)bv_battle_kind_addr != 0;
    if (want && bv_fbo_stencil_off && !in_battle) {
        if (fbo_current()) { if (bv_fbo_stencil_off != 3) want = 0; }            /* (a) FBO target without a stencil plane: GL says the test passes */
        else if (bv_fbo_stencil_off != 2 && !g_disp_st_ok) want = 0;             /* (b) display after an FBO excursion: the plane was reset */
    }
    if (want != g_st_applied) { g_st_applied = want; if (want) glEnable(0xb90); else glDisable(0xb90); }
}
static void w_glStencilFunc(GLenum f, GLint ref, GLuint mask) { g_sf_func = f; g_sf_ref = ref; g_sf_mask = mask; glStencilFunc(f, ref, mask); }
static void w_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { g_cm[0] = r; g_cm[1] = g; g_cm[2] = b; g_cm[3] = a; glColorMask(r, g, b, a); }
/* update 184 (user: the boot notice stays visible behind the Supercell logo): the game draws its first frames without clearing the display, so the
 * notice (drawn into all 3 display buffers, shims/shim_egl.c) showed through. Before the first display draw / clear of each of the game's first 3
 * frames the display is cleared to black once (scissor, colour mask and clear colour restored). */
volatile int bv_notice_wipe;
static void notice_wipe(void) {
    if (!bv_notice_wipe || fbo_current()) return;
    static uint32_t done = 0xffffffffu;
    uint32_t f = bv_egl_swaps;
    /* update 184b (run 225/226 screenshots: frames 2 / 5 / 8 still showed the notice - the game clears the display only in frame 1, one buffer
     * kept it): every frame for the first 400 frames (the logo phase), until the full-screen loading art covers the display */
    if (f >= 400) { bv_notice_wipe = 0; bv_log("boot notice: wiped from the game's first 400 frames"); return; }
    if (f == done) return;
    done = f;
    GLfloat cc[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    if (g_scissor_on) glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, bv_gl_width, bv_gl_height);   /* 184b: vitaGL's clear covers the viewport only (run 228: the game's small viewport kept the notice) */
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    glViewport(g_vp[0], g_vp[1], g_vp[2], g_vp[3]);
    glColorMask(g_cm[0], g_cm[1], g_cm[2], g_cm[3]);
    if (g_scissor_on) glEnable(GL_SCISSOR_TEST);
    if (f < 6) bv_log("boot notice: display wiped before the game's frame %u", (unsigned)f + 1);
}
/* update 184b (run 227: frames 2 / 5 / 8 still read back the notice although each was wiped first): right after the game's first swap, 4 black
 * frames are pushed through the display (clear + swap) so no display buffer can keep the notice. Game state restored. */
extern void bv_gl_swap(void) __attribute__((weak));   /* gfx/gl_init.c (weak: host tests) */
void bv_notice_flush(void) {
    if (!bv_gl_swap) return;
    unsigned prev = fbo_current();
    if (prev) glBindFramebuffer(GL_FRAMEBUFFER, 0);
    GLfloat cc[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    if (g_scissor_on) glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    for (int k = 0; k < 4; k++) { glViewport(0, 0, bv_gl_width, bv_gl_height); glClear(GL_COLOR_BUFFER_BIT); bv_gl_swap(); }
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    glViewport(g_vp[0], g_vp[1], g_vp[2], g_vp[3]);
    glColorMask(g_cm[0], g_cm[1], g_cm[2], g_cm[3]);
    if (g_scissor_on) glEnable(GL_SCISSOR_TEST);
    if (prev) glBindFramebuffer(GL_FRAMEBUFFER, prev);
    bv_log("boot notice: 4 black frames pushed through the display after the game's first frame");
}
static void stencil_draw_hook(void) {
    if (!bv_fbo_stencil_off && !bv_stencil_probe) return;
    if (fbo_current()) g_disp_st_ok = 0;
    stencil_sync();
    static int nf, n26;
    if (bv_stencil_probe && ((fbo_current() && nf < 24) || (g_cur_prog == 26 && n26 < 24))) {
        if (fbo_current()) nf++; else n26++;
        bv_log("stencil_probe: prog %u fbo %u game_stencil %d applied %d func 0x%x ref %d mask 0x%x colormask %d%d%d%d disp_ok %d", (unsigned)g_cur_prog, fbo_current(),
               g_st_game, g_st_applied, (unsigned)g_sf_func, (int)g_sf_ref, (unsigned)g_sf_mask, g_cm[0], g_cm[1], g_cm[2], g_cm[3], g_disp_st_ok);
    }
}
static void w_glClear(GLbitfield m) {
    notice_wipe();   /* update 184 */
    if (gl_trace_ok()) bv_log("glClear(0x%x)", (unsigned)m);
    else WIN("glClear(0x%x) fbo %u", (unsigned)bv_egl_swaps, (unsigned)m, fbo_current());
    if (skip_unusable_fbo("glClear")) return;
    if (bv_fbo_stencil_off || bv_stencil_probe) { if (!fbo_current()) { if (m & 0x400) g_disp_st_ok = 1; } else g_disp_st_ok = 0; stencil_sync(); }   /* update 171 */
    glr_note('K', fbo_current(), (uint32_t)m, 0, 0); /* cycle 6: before the call (run 81 died inside glClear) */
    if (bv_rt_probe && (m & GL_COLOR_BUFFER_BIT) && rb_fbo_is(fbo_current())) {   /* update 141 (F1) probe */
        GLfloat cc[4];
        glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
        glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
        glClear(m);
        glClearColor(cc[0], cc[1], cc[2], cc[3]);
        return;
    }
    /* update 93 (ISSUES A5): FBO clears since the last swap; experiment fbo_finish=N drains the GPU before every N-th */
    if (fbo_current()) {
        static uint32_t last_swap, n_fbo;
        uint32_t sw = bv_egl_swaps;
        if (sw != last_swap) { last_swap = sw; n_fbo = 0; }
        n_fbo++;
        if (n_fbo == 40 || n_fbo == 47 || n_fbo == 48 || n_fbo == 49 || n_fbo == 64 || n_fbo == 96 || n_fbo == 200)
            bv_log("fbo clears: #%u in swap %u (fbo %u); gxm: render targets live %u (made %u), scenes %u, errors %u", (unsigned)n_fbo, (unsigned)sw, fbo_current(),
                   GXMV(bv_gxm_rt_live), GXMV(bv_gxm_rt_made), GXMV(bv_gxm_scenes), GXMV(bv_gxm_errors));
        if (bv_fbo_finish > 0 && n_fbo % (uint32_t)bv_fbo_finish == 0) {
            uint64_t t0 = sceKernelGetProcessTimeWide();
            glFinish();
            uint32_t ms = (uint32_t)((sceKernelGetProcessTimeWide() - t0) / 1000);
            static volatile uint32_t nf;
            uint32_t k = __sync_add_and_fetch(&nf, 1);
            if (k <= 12 || !(k & (k - 1))) bv_log("fbo_finish: GPU drained before FBO clear #%u of swap %u in %u ms (#%u)", (unsigned)n_fbo, (unsigned)sw, (unsigned)ms, (unsigned)k);
        }
    }
    glClear(m);
}
volatile uint32_t bv_st_draws; /* update 53: draw calls (status line: per frame) */
volatile uint32_t bv_st_draw_us; /* update 144: microseconds inside vitaGL's draw calls (config prof=1) */
int bv_prof;
/* cycle 6 (ISSUES A5) experiment skip_prog=HASH: draws with a program containing that shader source hash are skipped */
uint32_t bv_skip_prog_hash = 0;
#define SKIP_PROGS 4096
static uint8_t g_prog_skip[SKIP_PROGS];
static volatile uint32_t g_prog_skipped;
static int skip_marked_prog(const char *what) {
    if (g_cur_prog >= SKIP_PROGS || !g_prog_skip[g_cur_prog]) return 0;
    uint32_t k = __sync_add_and_fetch(&g_prog_skipped, 1);
    if (k <= 5 || !(k & (k - 1))) bv_log("%s skipped: program %u is marked by skip_prog (#%u)", what, (unsigned)g_cur_prog, (unsigned)k);
    return 1;
}
static void w_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    bv_st_draws++;
    notice_wipe();   /* update 184 */
    WIN("glDrawArrays(0x%x, %d, %d) prog %u tex %u fbo %u", (unsigned)bv_egl_swaps, (unsigned)mode, (int)first, (int)count, (unsigned)g_cur_prog, (unsigned)g_bound2d, fbo_current());
    if (skip_unusable_fbo("glDrawArrays")) return;
    if (skip_marked_prog("glDrawArrays")) return;
    stencil_draw_hook();   /* update 171 */
    fbo_note_draw(g_vp[0], g_vp[1], g_vp[2], g_vp[3]);
    if (bv_fragcoord_fix == 4) fcy4_before_draw();
    else if (bv_fragcoord_fix >= 2) fcy_before_draw();
    uint64_t t0 = bv_prof ? sceKernelGetProcessTimeWide() : 0;
    glDrawArrays(mode, first, count);
    if (bv_prof) bv_st_draw_us += (uint32_t)(sceKernelGetProcessTimeWide() - t0);
}
static void w_glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *idx) {
    bv_st_draws++;
    notice_wipe();   /* update 184 */
    WIN("glDrawElements(0x%x, %d, 0x%x, %p) prog %u tex %u fbo %u", (unsigned)bv_egl_swaps, (unsigned)mode, (int)count, (unsigned)type, idx, (unsigned)g_cur_prog, (unsigned)g_bound2d, fbo_current());
    if (skip_unusable_fbo("glDrawElements")) return;
    if (skip_marked_prog("glDrawElements")) return;
    stencil_draw_hook();   /* update 171 */
    fbo_note_draw(g_vp[0], g_vp[1], g_vp[2], g_vp[3]);
    if (bv_fragcoord_fix == 4) fcy4_before_draw();
    else if (bv_fragcoord_fix >= 2) fcy_before_draw();
    uint64_t t0 = bv_prof ? sceKernelGetProcessTimeWide() : 0;
    glDrawElements(mode, count, type, idx);
    if (bv_prof) bv_st_draw_us += (uint32_t)(sceKernelGetProcessTimeWide() - t0);
}

/* glTexSubImage2D into a texture that has no storage would be a memcpy to address 0 inside vitaGL (the crash of 2026-10-26, ui_tex.sc): skip it and say so. */
static volatile uint32_t g_texsub_skipped;
static void tex_sub_inner(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type,
                          const GLvoid *pixels);
/* Load-time accounting (update 26): calls, bytes handed in and wall time inside glTexImage2D + glTexSubImage2D with data, shown on the "load:" status line.
 * Run 7 had a 114 s stretch after sc/ui_tex.sc was read with no file reads and no frames; this says whether texture uploads (vitaGL + our shrinking) are it. */
volatile uint32_t bv_st_tex_n, bv_st_tex_kb, bv_st_tex_us;
static void tex_account(uint64_t t0, GLsizei w, GLsizei h, GLenum format, GLenum type) {
    __sync_fetch_and_add(&bv_st_tex_us, (uint32_t)(sceKernelGetProcessTimeWide() - t0));
    __sync_fetch_and_add(&bv_st_tex_n, 1);
    __sync_fetch_and_add(&bv_st_tex_kb, (uint32_t)(((uint64_t)(w > 0 ? w : 0) * (uint64_t)(h > 0 ? h : 0) * bytes_per_texel(format, type)) >> 10));
}
static void w_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type,
                              const GLvoid *pixels) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    tex_sub_inner(target, level, xoffset, yoffset, width, height, format, type, pixels);
    tex_account(t0, width, height, format, type);
}
static void tex_sub_inner(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type,
                          const GLvoid *pixels) {
    int shift = (target == GL_TEXTURE_2D && g_bound2d) ? ts_get(g_bound2d) : 0;
    if (shift == TS_PLACEHOLDER) return; /* a 1x1 stand-in: nothing to upload into */
    if (shift) { /* the texture is stored smaller: average the piece down and place it at the halved offset */
        int f = 1 << shift;
        if (level > 0 || format != GL_RGBA || type != GL_UNSIGNED_BYTE || (xoffset | yoffset | width | height) & (f - 1) || !pixels) {
            BV_TRACE(1, "glTexSubImage2D: piece %dx%d at %d,%d (level %d) of shrunk texture %u cannot be averaged down: skipped", (int)width, (int)height, (int)xoffset, (int)yoffset,
                     (int)level, (unsigned)g_bound2d);
            return;
        }
        int w2 = (int)width >> shift, h2 = (int)height >> shift;
        uint8_t *small = (uint8_t *)malloc((size_t)w2 * (size_t)h2 * 4);
        if (!small) {
            bv_log("glTexSubImage2D: no memory (%u KB) to average a piece down: skipped", (unsigned)(((size_t)w2 * h2 * 4) >> 10));
            return;
        }
        ts_box_down_rgba((const uint8_t *)pixels, (int)width, (int)height, shift, small);
        int s16 = t16_get(g_bound2d, NULL, NULL);
        if (s16) T16_TIMED(ts_rgba8_to_4444(small, w2, h2, (int)xoffset >> shift, (int)yoffset >> shift, (uint16_t *)small)); /* in place, see tex_image_inner */
        if (vglGetTexDataPointer(target) != NULL)
            glTexSubImage2D(target, 0, xoffset >> shift, yoffset >> shift, w2, h2, format, s16 ? GL_UNSIGNED_SHORT_4_4_4_4 : type, small);
        else bv_log("glTexSubImage2D: the shrunk texture has no storage either: piece skipped");
        free(small);
        return;
    }
    if (target == GL_TEXTURE_2D && vglGetTexDataPointer(target) == NULL) {
        uint32_t k = __sync_add_and_fetch(&g_texsub_skipped, 1);
        if (k <= 25) bv_log("glTexSubImage2D: the bound texture has no storage (%dx%d at %d,%d, level %d, format 0x%x): upload #%u skipped", (int)width, (int)height, (int)xoffset,
                            (int)yoffset, (int)level, (unsigned)format, (unsigned)k);
        return;
    }
    if (target == GL_TEXTURE_2D && g_bound2d && t16_get(g_bound2d, NULL, NULL)) { /* update 51: the texture is stored as RGBA4444 */
        if (format != GL_RGBA || type != GL_UNSIGNED_BYTE || !pixels) {
            static volatile uint32_t said;
            if (__sync_add_and_fetch(&said, 1) <= 10)
                bv_log("glTexSubImage2D: piece %dx%d (fmt 0x%x type 0x%x) into RGBA4444 texture %u cannot be converted: skipped", (int)width, (int)height, (unsigned)format,
                       (unsigned)type, (unsigned)g_bound2d);
            return;
        }
        uint16_t *p16 = (uint16_t *)malloc((size_t)width * (size_t)height * 2);
        if (!p16) {
            bv_log("glTexSubImage2D: no memory (%u KB) to convert a piece to RGBA4444: skipped", (unsigned)(((size_t)width * height * 2) >> 10));
            return;
        }
        T16_TIMED(ts_rgba8_to_4444((const uint8_t *)pixels, (int)width, (int)height, (int)xoffset, (int)yoffset, p16));
        glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, GL_UNSIGNED_SHORT_4_4_4_4, p16);
        free(p16);
        return;
    }
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
}

/* Update 33. The GPU has a native ETC1 format (SCE_GXM_TEXTURE_BASE_FORMAT_ETC1, needs the extended-format init flag) and this vitaGL lists
 * GL_OES_compressed_ETC1_RGB8_texture, so config etc1_native=1 hands ETC1 to vitaGL unchanged (experiment: vitaGL may also decode it to RGBA, 8x the
 * memory). Default 0 = our ETC1 -> DXT1 transcode. bv_st_etc1_conv_us = time in the transcode alone (the rest of bv_st_etc1_us is vitaGL's upload). */
int bv_etc1_native = 1;   /* update 177: default (run 213: home + battle textures right, ETC1 conversion 2.5 s -> 0) */
volatile uint32_t bv_st_etc1_conv_us;
static void compressed_inner(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border,
                                     GLsizei size, const void *data) {
    if (internalformat == GL_ETC1_RGB8_OES && bv_etc1_native) {
        uint64_t t0 = sceKernelGetProcessTimeWide();
        glCompressedTexImage2D(target, level, internalformat, width, height, border, size, data);
        __sync_fetch_and_add(&bv_st_etc1_n, 1);
        __sync_fetch_and_add(&bv_st_etc1_bytes, (uint32_t)size);
        __sync_fetch_and_add(&bv_st_etc1_us, (uint32_t)(sceKernelGetProcessTimeWide() - t0));
        static int said;
        if (!said++) bv_log("ETC1 texture %dx%d passed to vitaGL unchanged (etc1_native=1)", (int)width, (int)height);
        return;
    }
    if (internalformat != GL_ETC1_RGB8_OES) {
        glCompressedTexImage2D(target, level, internalformat, width, height, border, size, data);
        return;
    }
    size_t blocks = ((size_t)(width + 3) / 4) * (size_t)((height + 3) / 4);
    if (!data || (size_t)size != blocks * 8) {
        BV_TRACE(1, "ETC1 upload with unexpected size %d for %dx%d (level %d): skipped", size, width, height, level);
        return;
    }
    uint64_t t0 = sceKernelGetProcessTimeWide();
    uint8_t *dxt = (uint8_t *)malloc((size_t)size);
    if (!dxt) return;
    bv_etc1_to_dxt1((const uint8_t *)data, dxt, blocks);
    __sync_fetch_and_add(&bv_st_etc1_conv_us, (uint32_t)(sceKernelGetProcessTimeWide() - t0));
    BV_TRACE(2, "ETC1 -> DXT1 level %d %dx%d (%d bytes)", level, width, height, size);
    glCompressedTexImage2D(target, level, GL_COMPRESSED_RGB_S3TC_DXT1_EXT, width, height, border, size, dxt);
    free(dxt);
    __sync_fetch_and_add(&bv_st_etc1_n, 1);
    __sync_fetch_and_add(&bv_st_etc1_bytes, (uint32_t)size);
    __sync_fetch_and_add(&bv_st_etc1_us, (uint32_t)(sceKernelGetProcessTimeWide() - t0));
}

/* ---- gl_FragCoord (update 21, the upside-down 3D brawler of 2026-11-11): in OpenGL gl_FragCoord.y counts from the BOTTOM of the screen; through vitaGL it is the
 * GPU's window position, which counts from the TOP when drawing to the screen. The game's four "impostor" fragment shaders paste the brawler's render texture with
 *     uv = (gl_FragCoord.xy - u_viewportOffset) * u_viewportSizeInverse;
 * so the picture came out mirrored about the screen centre (measured on screenshots: texture row r shown at screen line 109 + r instead of 437 - r).
 * Every `gl_FragCoord.xy` in a shader source becomes `vec2(gl_FragCoord.x, <screen height>.0 - gl_FragCoord.y)`. Returns a new string (malloc) and the number of
 * replacements, or NULL when the source does not use it. Other spellings of gl_FragCoord are left alone and counted in *other. Meant for shaders that draw to the
 * screen (the impostor shaders do); inside a framebuffer object vitaGL's window position already runs like OpenGL's. ---- */
char *bv_fix_fragcoord(const char *src, int screen_h, int *replaced, int *other) {
    static const char pat[] = "gl_FragCoord.xy", base[] = "gl_FragCoord";
    int n = 0, o = 0;
    for (const char *q = strstr(src, base); q; q = strstr(q + 1, base)) {
        if (!strncmp(q, pat, sizeof pat - 1) && !(q[sizeof pat - 1] == '_' || (q[sizeof pat - 1] >= 'a' && q[sizeof pat - 1] <= 'z'))) n++;
        else o++;
    }
    if (replaced) *replaced = n;
    if (other) *other = o;
    if (!n) return NULL;
    char rep[80];
    int rl = snprintf(rep, sizeof rep, "vec2(gl_FragCoord.x, %d.0 - gl_FragCoord.y)", screen_h);
    size_t len = strlen(src);
    char *out = (char *)malloc(len + (size_t)n * (size_t)rl + 1);
    if (!out) return NULL;
    char *w = out;
    const char *r = src;
    for (const char *q = strstr(r, pat); q; q = strstr(r, pat)) {
        char after = q[sizeof pat - 1];
        memcpy(w, r, (size_t)(q - r));
        w += q - r;
        if (after == '_' || (after >= 'a' && after <= 'z')) { /* e.g. .xyz: not this pattern */
            memcpy(w, pat, sizeof pat - 1);
            w += sizeof pat - 1;
        } else {
            memcpy(w, rep, (size_t)rl);
            w += rl;
        }
        r = q + sizeof pat - 1;
    }
    strcpy(w, r);
    return out;
}

/* ---- update 49: the same rewrite with a uniform, so it is right for the screen AND for framebuffer objects. User report 2026-11-12: brawler and skin
 * icons (rendered off-screen) are empty; the fixed "544.0 - y" of update 21 is only right when the impostor shader draws to the screen.
 * `gl_FragCoord.xy` -> `vec2(gl_FragCoord.x, bv_fcy.x + bv_fcy.y * gl_FragCoord.y)` and `uniform mediump vec2 bv_fcy;` is inserted before the first
 * `void main` (or at the end of the leading # lines). Before each draw the shim sets bv_fcy = (544, -1) when drawing to the screen and (0, 1) inside a
 * framebuffer object (vitaGL's window position already runs like OpenGL's there). ---- */
char *bv_fix_fragcoord_uniform(const char *src, int *replaced, int *other) {
    char *body = NULL;
    {
        /* reuse the counting / replacing of bv_fix_fragcoord with a marker height, then swap the marker text for the uniform expression */
        int n = 0, o = 0;
        char *tmp = bv_fix_fragcoord(src, 77777, &n, &o);
        if (replaced) *replaced = n;
        if (other) *other = o;
        if (!tmp) return NULL;
        static const char from[] = "vec2(gl_FragCoord.x, 77777.0 - gl_FragCoord.y)", to[] = "vec2(gl_FragCoord.x, bv_fcy.x + bv_fcy.y * gl_FragCoord.y)";
        body = (char *)malloc(strlen(tmp) + (size_t)n * (sizeof to - sizeof from) + 1);
        if (!body) { free(tmp); return NULL; }
        char *w = body;
        const char *r = tmp;
        for (const char *q = strstr(r, from); q; q = strstr(r, from)) {
            memcpy(w, r, (size_t)(q - r));
            w += q - r;
            memcpy(w, to, sizeof to - 1);
            w += sizeof to - 1;
            r = q + sizeof from - 1;
        }
        strcpy(w, r);
        free(tmp);
    }
    static const char decl[] = "uniform mediump vec2 bv_fcy;\n";
    const char *at = strstr(body, "void main");
    if (!at) { /* after the leading preprocessor lines */
        at = body;
        while (*at == '#') {
            const char *nl = strchr(at, '\n');
            if (!nl) break;
            at = nl + 1;
        }
    }
    size_t pre = (size_t)(at - body), len = strlen(body);
    char *out = (char *)malloc(len + sizeof decl);
    if (!out) { free(body); return NULL; }
    memcpy(out, body, pre);
    memcpy(out + pre, decl, sizeof decl - 1);
    memcpy(out + pre + sizeof decl - 1, body + pre, len - pre + 1);
    free(body);
    return out;
}

/* update 58: stored height of each texture (level 0, after shrinking) and the colour texture of each framebuffer object, for fragcoord_fix=3 */
#define TH_MAX 4096
static uint16_t g_tex_h[TH_MAX];
static void tex_note_height(GLuint id, int h) { if (id && id < TH_MAX && h > 0) g_tex_h[id] = (uint16_t)(h > 65535 ? 65535 : h); }
#define FT_MAX 128
static struct { unsigned fbo; GLuint tex; } g_fbo_tex[FT_MAX];
static void fbo_note_tex(unsigned fbo, GLuint tex) {
    int free_slot = -1;
    for (int i = 0; i < FT_MAX; i++) {
        if (g_fbo_tex[i].fbo == fbo) { g_fbo_tex[i].tex = tex; return; }
        if (!g_fbo_tex[i].fbo && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) { g_fbo_tex[free_slot].fbo = fbo; g_fbo_tex[free_slot].tex = tex; }
}
static GLuint fbo_tex_of(unsigned fbo) {
    for (int i = 0; i < FT_MAX; i++)
        if (g_fbo_tex[i].fbo == fbo) return g_fbo_tex[i].tex;
    return 0;
}
static int fcy_target_height(void) {
    unsigned fb = fbo_current();
    if (!fb) return bv_gl_height;
    for (int i = 0; i < FT_MAX; i++)
        if (g_fbo_tex[i].fbo == fb) {
            GLuint t = g_fbo_tex[i].tex;
            return (t && t < TH_MAX && g_tex_h[t]) ? g_tex_h[t] : bv_gl_height;
        }
    return bv_gl_height;
}

static void fc_note_uniform2(GLint loc, float x, float y) {
    GLuint p = g_cur_prog;
    if (p < FC_PROGS && g_prog_fc[p] && loc != -1 && loc == g_prog_off_loc[p]) { g_prog_off[p][0] = x; g_prog_off[p][1] = y; g_prog_off_set[p] = 1; }
}
static void fcy4_before_draw(void) {
    GLuint p = g_cur_prog;
    if (p >= FC_PROGS || !g_prog_fc[p] || g_prog_off_loc[p] == -1 || !g_prog_off_set[p]) return;
    int h = fcy_target_height();
    glUniform2f(g_prog_off_loc[p], g_prog_off[p][0], g_prog_off[p][1] + (float)(bv_gl_height - h));
    static int said;
    if (h != bv_gl_height && said < 20) {
        said++;
        bv_log("fragcoord_fix=4: program %u draws into framebuffer %u of height %d: u_viewportOffset.y %.1f -> %.1f (#%d)", (unsigned)p, fbo_current(), h,
               g_prog_off[p][1], g_prog_off[p][1] + (float)(bv_gl_height - h), said);
    }
}

/* per program: location of bv_fcy (-1 = none), looked up once; programs are small numbers in vitaGL */
#define FCY_PROGS 1024
static GLint g_fcy_loc[FCY_PROGS]; /* update 60: full pointer-sized location (was int16_t: the cause of runs 33/51/52 losing the brawler) */
static uint8_t g_fcy_known[FCY_PROGS];
static void fcy_before_draw(void) {
    GLuint p = g_cur_prog;
    if (!p || p >= FCY_PROGS) return;
    if (!g_fcy_known[p]) {
        g_fcy_known[p] = 1;
        GLint l = glGetUniformLocation(p, "bv_fcy");
        g_fcy_loc[p] = l;
    }
    if (g_fcy_loc[p] == -1) return;
    if (bv_fragcoord_fix == 3) {
        int h = fcy_target_height();
        glUniform2f(g_fcy_loc[p], (float)h, -1.0f);
        static int said;
        if (h != bv_gl_height && said < 20) { said++; bv_log("fragcoord_fix=3: draw into framebuffer %u: gl_FragCoord.y flipped by its height %d (#%d)", fbo_current(), h, said); }
        return;
    }
    if (fbo_current()) glUniform2f(g_fcy_loc[p], 0.0f, 1.0f);
    else glUniform2f(g_fcy_loc[p], (float)bv_gl_height, -1.0f);
}


/* ---- update 55: shader compile / link log + source dumps ---- */
#define SH_MAX 4096
static uint32_t g_sh_hash[SH_MAX], g_sh_len[SH_MAX];
static volatile uint32_t g_sh_compiles, g_sh_links, g_sh_dumped;
/* update 59 (fragcoord_fix=4): shaders rewritten by bv_fix_fragcoord, programs that contain one, their u_viewportOffset location and the game's value */
static uint8_t g_sh_fc[SH_MAX];
/* (update 59 program tables: declared next to g_cur_prog) */
static uint32_t fnv1a(const char *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)p[i]; h *= 16777619u; }
    return h;
}
static void shader_dump(uint32_t h, const char *src, size_t n) {
    static int dir_made;
    if (g_sh_dumped >= 800) return;
    if (!dir_made) { sceIoMkdir(BV_DATA_DIR "/shaders", 0777); dir_made = 1; }
    char path[96];
    snprintf(path, sizeof path, BV_DATA_DIR "/shaders/%08x.glsl", (unsigned)h);
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_EXCL, 0666); /* EXCL: each distinct source once */
    if (fd < 0) return;
    sceIoWrite(fd, src, (unsigned)n);
    sceIoClose(fd);
    __sync_fetch_and_add(&g_sh_dumped, 1);
}
/* the source that is really handed to vitaGL (after our rewrite), joined */
static void shader_note_source(GLuint shader, GLsizei count, const GLchar *const *str, const GLint *len) {
    if (!str || count <= 0) return;
    size_t total = 0;
    for (GLsizei i = 0; i < count; i++) total += str[i] ? ((len && len[i] >= 0) ? (size_t)len[i] : strlen(str[i])) : 0;
    char *all = (char *)malloc(total + 1);
    if (!all) return;
    size_t at = 0;
    for (GLsizei i = 0; i < count; i++) {
        size_t l = str[i] ? ((len && len[i] >= 0) ? (size_t)len[i] : strlen(str[i])) : 0;
        if (l) memcpy(all + at, str[i], l);
        at += l;
    }
    all[at] = 0;
    uint32_t h = fnv1a(all, at);
    if (shader < SH_MAX) { g_sh_hash[shader] = h; g_sh_len[shader] = (uint32_t)at; }
    shader_dump(h, all, at);
    free(all);
}
static void w_glCompileShader(GLuint shader) {
    uint32_t k = __sync_add_and_fetch(&g_sh_compiles, 1);
    uint32_t h = shader < SH_MAX ? g_sh_hash[shader] : 0, n = shader < SH_MAX ? g_sh_len[shader] : 0;
    bv_log("shader compile #%u: shader %u source %08x (%u bytes, shaders/%08x.glsl) ...", (unsigned)k, (unsigned)shader, (unsigned)h, (unsigned)n, (unsigned)h);
    uint64_t t0 = sceKernelGetProcessTimeWide();
    glCompileShader(shader);
    GLint ok = -1;
    glGetShaderiv(shader, 0x8B81 /* GL_COMPILE_STATUS */, &ok);
    bv_log("shader compile #%u: -> %s in %u ms", (unsigned)k, ok == 1 ? "ok" : (ok == 0 ? "FAILED" : "?"), (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000));
}
static void w_glLinkProgram(GLuint prog) {
    uint32_t k = __sync_add_and_fetch(&g_sh_links, 1);
    GLuint sh[4] = {0, 0, 0, 0};
    GLsizei nsh = 0;
    glGetAttachedShaders(prog, 4, &nsh, sh);
    uint32_t h0 = sh[0] < SH_MAX ? g_sh_hash[sh[0]] : 0, h1 = sh[1] < SH_MAX ? g_sh_hash[sh[1]] : 0;
    bv_log("program link #%u: program %u, shaders %u (%08x) + %u (%08x) ...", (unsigned)k, (unsigned)prog, (unsigned)sh[0], (unsigned)h0, (unsigned)sh[1], (unsigned)h1);
    if (bv_skip_prog_hash && prog < SKIP_PROGS && (h0 == bv_skip_prog_hash || h1 == bv_skip_prog_hash)) {
        g_prog_skip[prog] = 1;
        bv_log("skip_prog: program %u contains shader %08x: its draws will be skipped (experiment)", (unsigned)prog, (unsigned)bv_skip_prog_hash);
    }
    uint64_t t0 = sceKernelGetProcessTimeWide();
    glLinkProgram(prog);
    GLint ok = -1;
    glGetProgramiv(prog, 0x8B82 /* GL_LINK_STATUS */, &ok);
    if (prog < FC_PROGS && ((sh[0] < SH_MAX && g_sh_fc[sh[0]]) || (sh[1] < SH_MAX && g_sh_fc[sh[1]]))) {   /* update 59 */
        GLint l = glGetUniformLocation(prog, "u_viewportOffset");
        g_prog_fc[prog] = 1;
        g_prog_off_loc[prog] = l;
        g_prog_off_set[prog] = 0;
        bv_log("fragcoord: program %u uses gl_FragCoord; u_viewportOffset location %d", (unsigned)prog, (int)l);
    }
    bv_log("program link #%u: -> %s in %u ms", (unsigned)k, ok == 1 ? "ok" : (ok == 0 ? "FAILED" : "?"), (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000));
}

/* config fragcoord_fix: 0 shaders unchanged, 1 = fixed 544 - y (update 21, HARDWARE-VERIFIED run 7: brawler upright; DEFAULT again), 2 = uniform per draw target
 * (update 49, experiment only: with it on, run 33 had no brawlers and run 34 crashed in vitaGL's glLinkProgram while the home brawler's shader was linked). */
int bv_fragcoord_fix = 1; /* update 58: 3 = per-target height flip (experiment, see fcy_target_height) */
static volatile uint32_t g_fragcoord_shaders;
static void w_glShaderSource(GLuint shader, GLsizei count, const GLchar *const *str, const GLint *len) {
    if (bv_fragcoord_fix && count > 0 && str) {
        size_t total = 0;
        for (GLsizei i = 0; i < count; i++) total += str[i] ? ((len && len[i] >= 0) ? (size_t)len[i] : strlen(str[i])) : 0;
        char *all = (char *)malloc(total + 1);
        if (all) {
            size_t at = 0;
            for (GLsizei i = 0; i < count; i++) {
                size_t l = str[i] ? ((len && len[i] >= 0) ? (size_t)len[i] : strlen(str[i])) : 0;
                if (l) memcpy(all + at, str[i], l);
                at += l;
            }
            all[at] = 0;
            int n = 0, other = 0;
            char *fixed = (bv_fragcoord_fix == 1 || bv_fragcoord_fix == 4) ? bv_fix_fragcoord(all, bv_gl_height, &n, &other) : bv_fix_fragcoord_uniform(all, &n, &other);
            if (other) bv_log("glShaderSource: shader %u uses gl_FragCoord in %d place(s) that are NOT rewritten", (unsigned)shader, other);
            free(all);
            if (fixed) {
                uint32_t k = __sync_add_and_fetch(&g_fragcoord_shaders, 1);
                bv_log("glShaderSource: shader %u: %d x gl_FragCoord.xy made bottom-up like OpenGL (%s) (#%u)", (unsigned)shader, n,
                       (bv_fragcoord_fix == 1 || bv_fragcoord_fix == 4) ? "544.0 - y" : "uniform bv_fcy: 544 - y on the screen, y in a framebuffer object", (unsigned)k);
                if (shader < SH_MAX) g_sh_fc[shader] = 1; /* update 59 */
                const GLchar *one = fixed;
                shader_note_source(shader, 1, &one, NULL);
                glShaderSource(shader, 1, &one, NULL);
                return; /* the string is kept: vitaGL may hold on to it */
            }
        }
    }
    shader_note_source(shader, count, str, len);
    glShaderSource(shader, count, str, len);
}

static void w_glShaderBinary(GLsizei n, const GLuint *shaders, GLenum fmt, const void *bin, GLsizei len) {
    (void)n; (void)shaders; (void)fmt; (void)bin; (void)len;
    BV_TRACE(1, "glShaderBinary ignored (source shaders only)");
}

int bv_depth_ext = 1; /* config depth_ext (update 54): 0 = do not report GL_OES_depth_texture / GL_OES_packed_depth_stencil */
static const char *w_glGetString(GLenum name) {
    switch (name) {
    case 0x1F00: return "BrawlVita";                                  /* GL_VENDOR */
    case 0x1F01: return "vitaGL (SGX543MP4+)";                        /* GL_RENDERER */
    case 0x1F02: return bv_gles_major >= 3 ? "OpenGL ES 3.0 vitaGL" : "OpenGL ES 2.0 vitaGL"; /* GL_VERSION */
    case 0x1F03: /* GL_EXTENSIONS. update 54 experiment (config depth_ext=0): without GL_OES_depth_texture the game may skip its full-screen depth texture
                  * (run 40: tex 55 1410x878 DEPTH_COMPONENT -> 1x1 stand-in, so grass and a bot read garbage depth). Default 1 = as before. */
        {
            static int said;
            if (!said++) bv_log("glGetString(GL_EXTENSIONS) asked: answering %s depth textures (depth_ext=%d)", bv_depth_ext ? "WITH" : "WITHOUT", bv_depth_ext);
        }
        return bv_depth_ext ? "GL_OES_compressed_ETC1_RGB8_texture GL_OES_depth_texture GL_OES_packed_depth_stencil" : "GL_OES_compressed_ETC1_RGB8_texture";
    case 0x8B8C: return bv_gles_major >= 3 ? "OpenGL ES GLSL ES 3.00" : "OpenGL ES GLSL ES 1.00"; /* GL_SHADING_LANGUAGE_VERSION */
    default: return "";
    }
}

/* GL names the game may ask for by string (extensions). Unknown names return NULL. */
static void *w_eglGetProcAddress(const char *name) {
    uint32_t a = bv_shim_lookup(name);
    BV_TRACE(1, "eglGetProcAddress(%s) -> %s", name, a ? "shim" : "NULL");
    return (void *)(uintptr_t)a;
}

/* ---- update 66 (ISSUES A1): more operations under the GPU-memory tripwire. Op letters: C compressed teximage, B buffer data, U buffer sub data,
 * X delete buffers, M generate mipmap, Q renderbuffer storage, Y delete renderbuffers. ---- */
static void w_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border,
                                     GLsizei size, const void *data) {
    compressed_inner(target, level, internalformat, width, height, border, size, data);
    glr_note('C', g_bound2d, (uint32_t)width, (uint32_t)height, (uint32_t)internalformat);
    bv_gl_mem_check("glCompressedTexImage2D");
}
static void w_glBufferData(GLenum t, GLsizeiptr n, const void *d, GLenum u) {
    glBufferData(t, n, d, u);
    glr_note('B', 0, (uint32_t)n, (uint32_t)t, (uint32_t)u);
    bv_gl_mem_check("glBufferData");
}
static void w_glBufferSubData(GLenum t, GLintptr off, GLsizeiptr n, const void *d) {
    glBufferSubData(t, off, n, d);
    glr_note('U', 0, (uint32_t)n, (uint32_t)off, (uint32_t)t);
    bv_gl_mem_check("glBufferSubData");
}
static void w_glDeleteBuffers(GLsizei n, const GLuint *ids) {
    glDeleteBuffers(n, ids);
    glr_note('X', n > 0 && ids ? ids[0] : 0, (uint32_t)n, 0, 0);
    bv_gl_mem_check("glDeleteBuffers");
}
int bv_rt_mipmap_skip = 1; /* config rt_mipmap (cycle 4, ISSUES A1): 1 = no glGenerateMipmap on render-target textures */
static void w_glGenerateMipmap(GLenum t) {
    /* cycle 4 (ISSUES A1): run 68 tripwire: vitaGL's RAM pool broke right after glGenerateMipmap on texture 72, a 16x16 RGBA texture the game had
     * just attached to a framebuffer (pattern every ~25 ms in battle: teximage 16x16, attach, mipmap); the battle crashed right after. A render
     * target needs no mip chain: skip it. */
    if (bv_rt_mipmap_skip && t == GL_TEXTURE_2D && g_bound2d && rt_is(g_bound2d)) {
        static volatile uint32_t skipped;
        uint32_t k = __sync_add_and_fetch(&skipped, 1);
        if (k <= 5 || !(k & (k - 1))) bv_log("glGenerateMipmap skipped on render-target texture %u (vitaGL broke its memory pool doing this, run 68) (#%u)", (unsigned)g_bound2d, (unsigned)k);
        return;
    }
    {   /* update 135: report a full render-target table (the run 152 cause) */
        extern volatile unsigned bv_rt_full;
        static unsigned said;
        if (bv_rt_full && said++ < 4) bv_log("glGenerateMipmap: render-target table was full %u times (RT_MAX) - texture %u may be a render target", bv_rt_full, (unsigned)g_bound2d);
    }
    glGenerateMipmap(t);
    glr_note('M', g_bound2d, 0, 0, (uint32_t)t);
    bv_gl_mem_check("glGenerateMipmap");
}
static void w_glDeleteRenderbuffers(GLsizei n, const GLuint *ids) {
    glDeleteRenderbuffers(n, ids);
    glr_note('Y', n > 0 && ids ? ids[0] : 0, (uint32_t)n, 0, 0);
    bv_gl_mem_check("glDeleteRenderbuffers");
}

const bv_shim bv_shims_gles[] = {
    SHIM(glClearColor), SHIM(glTexParameterf), SHIM(glUniform1f), SHIM(glUniform2f), SHIM(glUniform3f), SHIM(glUniform4f),
    SHIM(glCompressedTexImage2D), SHIM(glTexSubImage2D), SHIM(glShaderBinary), SHIM(glGetString), SHIM(eglGetProcAddress),
    SHIM(glDepthRangef), SHIM(glPolygonOffset), SHIM(glRenderbufferStorageMultisample), SHIM(glInvalidateFramebuffer),
    SHIM_PASS(glBindVertexArray), SHIM_PASS(glGenVertexArrays), SHIM_PASS(glDeleteVertexArrays), SHIM(glBlitFramebuffer),
    SHIM(glColorMask), SHIM(glCullFace), SHIM_PASS(glDrawElementsInstanced), SHIM_PASS(glFlush),
    SHIM_PASS(glGetAttribLocation), SHIM_PASS(glGetBooleanv), SHIM_PASS(glMapBufferRange), SHIM_PASS(glUnmapBuffer),
    SHIM(glStencilFunc), SHIM_PASS(glStencilMask), SHIM_PASS(glStencilOp), SHIM_PASS(glVertexAttrib4fv),
    SHIM_PASS(glVertexAttribDivisor),
    SHIM(glActiveTexture), SHIM_PASS(glAttachShader), SHIM_PASS(glBindAttribLocation), SHIM(glBindBuffer),
    SHIM(glBindFramebuffer), SHIM(glBindRenderbuffer), SHIM(glBindTexture), SHIM_PASS(glBlendEquation),
    SHIM_PASS(glBlendFunc), SHIM(glBufferData), SHIM(glBufferSubData), SHIM(glCheckFramebufferStatus),
    SHIM(glClear), SHIM_PASS(glClearStencil), SHIM(glCompileShader), SHIM_PASS(glCreateProgram),
    SHIM_PASS(glCreateShader), SHIM(glDeleteBuffers), SHIM(glDeleteFramebuffers), SHIM_PASS(glDeleteProgram),
    SHIM(glDeleteRenderbuffers), SHIM_PASS(glDeleteShader), SHIM(glDeleteTextures), SHIM(glDepthMask),
    SHIM(glDisable), SHIM_PASS(glDisableVertexAttribArray), SHIM(glDrawArrays), SHIM(glDrawElements),
    SHIM(glEnable), SHIM_PASS(glEnableVertexAttribArray), SHIM(glFramebufferRenderbuffer),
    SHIM(glFramebufferTexture2D), SHIM_PASS(glGenBuffers), SHIM(glGenFramebuffers), SHIM(glGenRenderbuffers),
    SHIM_PASS(glGenTextures), SHIM(glGenerateMipmap), SHIM_PASS(glGetError), SHIM_PASS(glGetIntegerv),
    SHIM_PASS(glGetProgramInfoLog), SHIM_PASS(glGetProgramiv), SHIM_PASS(glGetShaderInfoLog), SHIM_PASS(glGetShaderiv),
    SHIM_PASS(glGetUniformLocation), SHIM_PASS(glHint), SHIM_PASS(glIsEnabled), SHIM(glLinkProgram),
    SHIM_PASS(glPixelStorei), SHIM_PASS(glReadPixels), SHIM(glRenderbufferStorage), SHIM(glScissor),
    SHIM(glShaderSource), SHIM(glTexImage2D), SHIM_PASS(glTexParameteri),
    SHIM_PASS(glUniform1fv), SHIM_PASS(glUniform1i), SHIM(glUniform2fv), SHIM_PASS(glUniform3fv), SHIM(glUniform4fv),
    SHIM(glUniformMatrix4fv), SHIM(glUseProgram), SHIM(glVertexAttribPointer), SHIM(glViewport),
    SHIM_END};
