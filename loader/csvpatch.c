/* cycle 8 (A13 Colette balance and later tweaks): replace one cell of a game csv (first column = row name, header line = column names) in memory.
 * shims/shim_asset.c applies every config csv_patch=<asset path>,<row>,<column>,<value> to the file when the game opens it; the user's file on the
 * card is never changed. Pure logic, host-tested (tests/host_csvpatch_test.c). */
#include "csvpatch.h"

#include <stdlib.h>
#include <string.h>

/* bounds of field number `col` (0-based) on the line [ls, le): *fs .. *fe (quotes included); 0 if the line has fewer fields */
static int field_at(const char *ls, const char *le, int col, const char **fs, const char **fe) {
    const char *p = ls;
    for (int c = 0;; c++) {
        const char *s = p;
        int q = 0;
        while (p < le) {
            if (*p == '"') q = !q;
            else if (*p == ',' && !q) break;
            p++;
        }
        if (c == col) {
            *fs = s;
            *fe = p;
            if (*fe > *fs && (*fe)[-1] == '\r') (*fe)--;
            return 1;
        }
        if (p >= le) return 0;
        p++; /* past the comma */
    }
}

/* field text without quotes equals `want` */
static int field_is(const char *fs, const char *fe, const char *want) {
    if (fe - fs >= 2 && *fs == '"' && fe[-1] == '"') { fs++; fe--; }
    size_t n = strlen(want);
    return (size_t)(fe - fs) == n && !memcmp(fs, want, n);
}

static int header_col(const char *hl, const char *he, const char *name) {
    for (int c = 0; c < 400; c++) {
        const char *fs, *fe;
        if (!field_at(hl, he, c, &fs, &fe)) return -1;
        if (field_is(fs, fe, name)) return c;
    }
    return -1;
}

char *bv_csv_enable_modes(const char *in, size_t len, const char *mode_col, const char *const *modes, int nmodes, const char *dis_col, size_t *out_len, int *count,
                          const char *filter_col, int (*filter_ok)(const char *)) {
    if (count) *count = 0;
    const char *end = in + len, *he = memchr(in, '\n', len);
    if (!he) return NULL;
    int cm = header_col(in, he, mode_col), cd = header_col(in, he, dis_col);
    int cf = filter_col ? header_col(in, he, filter_col) : -1;
    if (cm < 0 || cd < 0 || (filter_col && cf < 0)) return NULL;
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    size_t o = 0;
    memcpy(out, in, (size_t)(he + 1 - in));
    o = (size_t)(he + 1 - in);
    for (const char *ls = he + 1; ls < end;) {
        const char *le = memchr(ls, '\n', (size_t)(end - ls));
        const char *next = le ? le + 1 : end;
        if (!le) le = end;
        const char *ms, *me, *ds, *de;
        int hit = 0;
        if (field_at(ls, le, cm, &ms, &me) && field_at(ls, le, cd, &ds, &de) && de > ds)
            for (int i = 0; i < nmodes; i++) if (field_is(ms, me, modes[i])) { hit = 1; break; }
        if (hit && filter_ok && cf >= 0) {
            const char *fs, *fe;
            char t[96];
            size_t k = 0;
            if (!field_at(ls, le, cf, &fs, &fe)) hit = 0;
            else {
                if (fe - fs >= 2 && *fs == '"' && fe[-1] == '"') { fs++; fe--; }
                k = (size_t)(fe - fs) < sizeof t - 1 ? (size_t)(fe - fs) : sizeof t - 1;
                memcpy(t, fs, k);
                t[k] = 0;
                hit = filter_ok(t);
            }
        }
        if (hit) {
            memcpy(out + o, ls, (size_t)(ds - ls));
            o += (size_t)(ds - ls);
            memcpy(out + o, de, (size_t)(next - de));
            o += (size_t)(next - de);
            if (count) (*count)++;
        } else {
            memcpy(out + o, ls, (size_t)(next - ls));
            o += (size_t)(next - ls);
        }
        ls = next;
    }
    out[o] = 0;
    if (out_len) *out_len = o;
    return out;
}

char *bv_csv_patch(const char *in, size_t len, const char *row, const char *column, const char *value, size_t *out_len, int *done) {
    if (done) *done = 0;
    const char *end = in + len, *hl = in, *he = memchr(in, '\n', len);
    if (!he) return NULL;
    int col = -1;
    for (int c = 0; c < 400; c++) {
        const char *fs, *fe;
        if (!field_at(hl, he, c, &fs, &fe)) break;
        if (field_is(fs, fe, column)) { col = c; break; }
    }
    if (col < 0) return NULL;
    if (row[0] == '*' && !row[1]) {   /* update 167: every named data row */
        size_t vl = strlen(value), nl = 0;
        for (const char *q = in; q < end; q++) if (*q == '\n') nl++;
        char *out = (char *)malloc(len + (nl + 1) * vl + 1);
        if (!out) return NULL;
        size_t o = 0;
        int n = 0, line = 0;
        for (const char *ls = in; ls < end; line++) {
            const char *le = memchr(ls, '\n', (size_t)(end - ls));
            const char *next = le ? le + 1 : end;
            if (!le) le = end;
            const char *fs, *fe, *ns, *ne;
            if (line >= 2 && field_at(ls, le, 0, &ns, &ne) && ne > ns && !(ne - ns == 2 && ns[0] == '"' && ns[1] == '"') && field_at(ls, le, col, &fs, &fe) && fe > fs) {
                memcpy(out + o, ls, (size_t)(fs - ls)); o += (size_t)(fs - ls);
                memcpy(out + o, value, vl); o += vl;
                memcpy(out + o, fe, (size_t)(next - fe)); o += (size_t)(next - fe);
                n++;
            } else { memcpy(out + o, ls, (size_t)(next - ls)); o += (size_t)(next - ls); }
            ls = next;
        }
        out[o] = 0;
        if (out_len) *out_len = o;
        if (done) *done = n > 0;
        if (!n) { free(out); return NULL; }
        return out;
    }
    for (const char *ls = he + 1; ls < end;) {
        const char *le = memchr(ls, '\n', (size_t)(end - ls));
        if (!le) le = end;
        const char *fs, *fe, *ns, *ne;
        if (field_at(ls, le, 0, &ns, &ne) && field_is(ns, ne, row) && field_at(ls, le, col, &fs, &fe)) {
            size_t vl = strlen(value), ol = len - (size_t)(fe - fs) + vl;
            char *out = (char *)malloc(ol + 1);
            if (!out) return NULL;
            size_t a = (size_t)(fs - in);
            memcpy(out, in, a);
            memcpy(out + a, value, vl);
            memcpy(out + a + vl, fe, (size_t)(end - fe));
            out[ol] = 0;
            if (out_len) *out_len = ol;
            if (done) *done = 1;
            return out;
        }
        ls = le + 1;
    }
    return NULL;
}

/* update 175: row dst <- row src (all cells but the name) */
char *bv_csv_copy_row(const char *in, size_t len, const char *dst, const char *src, size_t *out_len) {
    const char *end = in + len, *ds = NULL, *de = NULL, *ss = NULL, *se = NULL;
    for (const char *ls = in; ls < end;) {
        const char *le = memchr(ls, '\n', (size_t)(end - ls));
        if (!le) le = end;
        const char *ns, *ne;
        if (field_at(ls, le, 0, &ns, &ne)) {
            if (field_is(ns, ne, dst)) { ds = ne; de = le; }
            else if (field_is(ns, ne, src)) { ss = ne; se = le; }
        }
        ls = le < end ? le + 1 : end;
    }
    if (!ds || !ss) return NULL;
    if (de > ds && de[-1] == '\r') de--;
    if (se > ss && se[-1] == '\r') se--;
    size_t ol = len - (size_t)(de - ds) + (size_t)(se - ss);
    char *out = (char *)malloc(ol + 1);
    if (!out) return NULL;
    size_t a = (size_t)(ds - in);
    memcpy(out, in, a);
    memcpy(out + a, ss, (size_t)(se - ss));
    memcpy(out + a + (size_t)(se - ss), de, (size_t)(end - de));
    out[ol] = 0;
    if (out_len) *out_len = ol;
    return out;
}
