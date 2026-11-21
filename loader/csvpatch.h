/* cycle 8 (A13): one-cell csv patch in memory (see csvpatch.c). */
#ifndef BV_CSVPATCH_H
#define BV_CSVPATCH_H

#include <stddef.h>

/* copy of the csv with the cell (row = first-column value, column = header name) set to `value` (written as given; quote it yourself if needed);
 * malloc'd, NULL if the row / column is not found (then *done = 0) or out of memory */
char *bv_csv_patch(const char *in, size_t len, const char *row, const char *column, const char *value, size_t *out_len, int *done);

/* update 127: copy of the csv where every row whose `mode_col` value is one of `modes` gets an empty `dis_col` cell (enabled); *count = rows changed.
 * malloc'd; NULL when a column is missing or out of memory */
char *bv_csv_enable_modes(const char *in, size_t len, const char *mode_col, const char *const *modes, int nmodes, const char *dis_col, size_t *out_len, int *count,
                          const char *filter_col, int (*filter_ok)(const char *));   /* update 128: optional filter on another column (unquoted text) */

/* update 175: copy of the csv where row `dst` takes every cell of row `src` except the first (name) column; NULL if a row is missing */
char *bv_csv_copy_row(const char *in, size_t len, const char *dst, const char *src, size_t *out_len);

#endif
