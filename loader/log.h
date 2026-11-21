#ifndef BV_LOG_H
#define BV_LOG_H

#include <stdarg.h>

#define BV_DATA_DIR "ux0:data/BrawlVita"

void bv_log_init(const char *path);
void bv_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void bv_logv(const char *fmt, va_list ap);

/* verbosity of the per-call API trace of the shims (0 = off, 1 = notable, 2 = everything) */
extern int bv_trace_level;

#define BV_TRACE(lvl, ...) do { if (bv_trace_level >= (lvl)) bv_log(__VA_ARGS__); } while (0)

#endif
