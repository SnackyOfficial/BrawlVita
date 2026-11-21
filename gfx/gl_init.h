#ifndef BV_GL_INIT_H
#define BV_GL_INIT_H

#include <stddef.h>

/* Starts vitaGL at width x height (the only sizes the Vita display scaler is known to accept: 960x544, 720x408, 640x368).
 * Needs libshacccg.suprx in ur0:data. Returns 0 on success, -1 if the shader compiler module is missing, -2 for a bad size. */
int bv_gl_init(int width, int height);
void bv_gl_swap(void);
void bv_gl_mem_report(char *out, size_t n); /* "vram 90/96 MB, ram ..." (only after bv_gl_init succeeded) */

#endif
