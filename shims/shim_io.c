/* stdio / file descriptor / directory shims. bionic's FILE (`__sF` streams), stat and dirent differ from newlib's. */
#define _GNU_SOURCE /* fopencookie */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../loader/log.h"
#include "shim.h"
#include "vrandom.h"
#include "vsock.h"
#include <psp2/kernel/rng.h>
#include <psp2/kernel/processmgr.h>
#include <stdint.h>

/* ---- paths ---- */
int bv_has_dotdot(const char *p) {
    const char *c = p;
    while (*c) {
        const char *e = c;
        while (*e && *e != '/') e++;
        if (e - c == 2 && c[0] == '.' && c[1] == '.') return 1;
        c = *e ? e + 1 : e;
    }
    return 0;
}

const char *bv_fix_path(const char *in, char *out, int n) {
    if (!in) {
        out[0] = 0;
        return out;
    }
    if (bv_has_dotdot(in)) {
        BV_TRACE(1, "path with '..' refused: %.100s", in);
        snprintf(out, (size_t)n, BV_DATA_DIR "/fs/_refused_");
        return out;
    }
    if (in[0] == '/') {
        snprintf(out, (size_t)n, BV_DATA_DIR "/fs%s", in);
    } else {
        snprintf(out, (size_t)n, "%s", in);
    }
    return out;
}

#define FIX(var, in) char var##_buf[512]; const char *var = bv_fix_path((in), var##_buf, sizeof var##_buf)

/* The console's mkdir/open do not create parents. Directories below our fs/ root are created on demand (like a phone's app sandbox,
 * where the framework has already made /data/data/<package>/{files,cache}). */
static void make_parents(const char *p) {
    static const char root[] = BV_DATA_DIR "/fs";
    size_t rl = sizeof root - 1;
    if (strncmp(p, root, rl) != 0 || p[rl] != '/') return;
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", p);
    for (size_t i = rl + 1; tmp[i]; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            mkdir(tmp, 0777);
            tmp[i] = '/';
        }
    }
}

static int mkdir_p(const char *p, mode_t mode) {
    int r = mkdir(p, mode);
    if (r != 0 && errno == ENOENT) {
        make_parents(p);
        r = mkdir(p, mode);
    }
    return r;
}

void bv_make_game_dirs(void) {
    static const char *const dirs[] = {
        BV_DATA_DIR "/fs/data/data/com.supercell.brawlstars/files",
        BV_DATA_DIR "/fs/data/data/com.supercell.brawlstars/cache",
        BV_DATA_DIR "/fs/sdcard/Android/data/com.supercell.brawlstars/files",
        BV_DATA_DIR "/fs/tmp",
    };
    mkdir(BV_DATA_DIR, 0777);
    mkdir(BV_DATA_DIR "/fs", 0777);
    for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; i++) mkdir_p(dirs[i], 0777);
}

/* ---- bionic FILE streams: stdin/stdout/stderr are &__sF[0..2], 84 bytes each ---- */
#define SF_SIZE 84
uint8_t bv_sF[3 * SF_SIZE];

static int std_index(const void *f) {
    uintptr_t p = (uintptr_t)f, b = (uintptr_t)bv_sF;
    if (p >= b && p < b + sizeof bv_sF) return (int)((p - b) / SF_SIZE);
    return -1;
}
static FILE *tf(void *f) {
    int i = std_index(f);
    return i < 0 ? (FILE *)f : (i == 0 ? stdin : i == 1 ? stdout : stderr);
}

void bv_crash_trace_arm(const char *why); /* shims/shim_gles.c, update 90 */
static void log_text(int stream, const char *s, size_t n) {
    char tmp[512];
    if (n >= sizeof tmp) n = sizeof tmp - 1;
    memcpy(tmp, s, n);
    while (n && (tmp[n - 1] == '\n' || tmp[n - 1] == '\r')) n--;
    tmp[n] = 0;
    if (n) bv_log("%s: %s", stream == 2 ? "stderr" : "stdout", tmp);
    if (n && strstr(tmp, "prepareIntroSceneRenderer")) bv_crash_trace_arm("prepareIntroSceneRenderer"); /* update 90 (A5), no-op unless crash_trace=N */
}

static int w_vfprintf(void *f, const char *fmt, va_list ap) {
    int i = std_index(f);
    if (i >= 1) {
        char buf[512];
        int n = vsnprintf(buf, sizeof buf, fmt, ap);
        log_text(i, buf, n < 0 ? 0 : (size_t)n);
        return n;
    }
    return vfprintf(tf(f), fmt, ap);
}
static int w_fprintf(void *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = w_vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}
static int w_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_text(1, buf, n < 0 ? 0 : (size_t)n);
    return n;
}
static int w_puts(const char *s) {
    log_text(1, s, strlen(s));
    return 1;
}
static int w_putchar(int c) {
    char ch = (char)c;
    log_text(1, &ch, 1);
    return c;
}
static int w_fputs(const char *s, void *f) {
    int i = std_index(f);
    if (i >= 1) {
        log_text(i, s, strlen(s));
        return 1;
    }
    return fputs(s, tf(f));
}
static int w_fputc(int c, void *f) {
    int i = std_index(f);
    if (i >= 1) {
        char ch = (char)c;
        log_text(i, &ch, 1);
        return c;
    }
    return fputc(c, tf(f));
}
/* The game's own file calls (not assets, not virtual files), for the status line (update 27). Run 11: the main game thread waited ~80 % of the loading
 * time on two kernel objects, most likely inside file-system calls; this shows how many calls, how many bytes and how much time they take. */
volatile uint32_t bv_st_io_calls, bv_st_io_kb_r, bv_st_io_kb_w, bv_st_io_us, bv_st_io_opens;
static volatile uint32_t g_io_b_r, g_io_b_w;
#define IO_T(bytes_r, bytes_w, expr)                                                                   \
    ({                                                                                                 \
        uint64_t t0_ = sceKernelGetProcessTimeWide();                                                  \
        __typeof__(expr) r_ = (expr);                                                                  \
        __sync_fetch_and_add(&bv_st_io_us, (uint32_t)(sceKernelGetProcessTimeWide() - t0_));          \
        __sync_fetch_and_add(&bv_st_io_calls, 1);                                                      \
        long v_ = (long)(intptr_t)r_; /* a count for read/write, unused for open/stat */               \
        if ((bytes_r) && v_ > 0) bv_st_io_kb_r = (__sync_add_and_fetch(&g_io_b_r, (uint32_t)(v_ * (long)(bytes_r))) >> 10); \
        if ((bytes_w) && v_ > 0) bv_st_io_kb_w = (__sync_add_and_fetch(&g_io_b_w, (uint32_t)(v_ * (long)(bytes_w))) >> 10); \
        r_;                                                                                            \
    })
static int sink_is(void *f);
int bv_sink_fast; /* config sink_fast (0; update 87 experiment H): sink writes return at once */
static size_t w_fwrite(const void *p, size_t sz, size_t n, void *f) {
    if (bv_sink_fast && sink_is(f)) return n;
    int i = std_index(f);
    if (i >= 1) {
        log_text(i, (const char *)p, sz * n);
        return n;
    }
    return IO_T(0, sz, fwrite(p, sz, n, tf(f)));
}
static size_t w_fread(void *p, size_t sz, size_t n, void *f) { return IO_T(sz, 0, fread(p, sz, n, tf(f))); }
static char *w_fgets(char *s, int n, void *f) { return fgets(s, n, tf(f)); }
static int w_fflush(void *f) { return f ? (std_index(f) >= 1 ? 0 : fflush(tf(f))) : fflush(NULL); }
static void sink_forget(void *f);
static int w_fclose(void *f) {
    sink_forget(f);
    return fclose(tf(f));
}
static int w_ferror(void *f) { return ferror(tf(f)); }
static int w_feof(void *f) { return feof(tf(f)); }
static int w_getc(void *f) { return getc(tf(f)); }
static int w_ungetc(int c, void *f) { return ungetc(c, tf(f)); }
static int w_fseek(void *f, long off, int whence) { return fseek(tf(f), off, whence); }
static long w_ftell(void *f) { return ftell(tf(f)); }
static int w_fseeko(void *f, long off, int whence) { return fseek(tf(f), off, whence); }
static long w_ftello(void *f) { return ftell(tf(f)); }
static void w_rewind(void *f) { rewind(tf(f)); }
static int w_fileno(void *f) { return fileno(tf(f)); }

/* ---- the game's unpacked-file cache (update 28) ----
 * The game unpacks every LZMA .sc file and writes the result to <data>/cache/swfcache/sc/<name>.sc + .info. Run 12 (2026-11-11): 209 MB in 3.59 million tiny
 * writes, 174 s inside the file calls, on EVERY boot, and 0 bytes ever read back from any game file: the cache is opened again on the next boot ("rb") and
 * then rewritten, so it is never used (most likely the game maps it with mmap, which this loader cannot do for files: 128 MB heap, ui_tex alone unpacks to
 * ~139 MB). With swfcache=0 (default) those writes go to a stream that drops them; swfcache=1 writes them as before. */
int bv_swfcache;
static volatile uint32_t g_sink_n;
static ssize_t sink_write(void *c, const char *b, size_t n) { (void)c; (void)b; return (ssize_t)n; }
static ssize_t sink_read(void *c, char *b, size_t n) { (void)c; (void)b; (void)n; return 0; }
static int sink_seek(void *c, off_t *pos, int whence) { (void)c; (void)whence; *pos = 0; return 0; }
static int sink_close(void *c) { (void)c; return 0; }
static const cookie_io_functions_t k_sink_io = {sink_read, sink_write, sink_seek, sink_close};
#define SINK_MAX 16
static void *volatile g_sinks[SINK_MAX];
static int sink_is(void *f) {
    for (int i = 0; i < SINK_MAX; i++)
        if (g_sinks[i] == f) return 1;
    return 0;
}
static void sink_remember(void *f) {
    for (int i = 0; i < SINK_MAX; i++)
        if (__sync_bool_compare_and_swap(&g_sinks[i], NULL, f)) return;
}
static void sink_forget(void *f) {
    if (!f) return;
    for (int i = 0; i < SINK_MAX; i++)
        if (g_sinks[i] == f) g_sinks[i] = NULL;
}

static void *w_fopen(const char *path, const char *mode) {
    int vf = vr_file_index(path);
    if (vf && mode[0] == 'r') { /* a read-only memory stream over the virtual file */
        size_t len = 0;
        const void *data = vr_file_data(vf, &len);
        FILE *mf = fmemopen((void *)data, len, "r");
        BV_TRACE(1, "fopen(%s, %s) -> %p (virtual file)", path, mode, (void *)mf);
        return mf;
    }
    FIX(p, path);
    if (!bv_swfcache && mode[0] == 'w' && strstr(p, "/cache/swfcache/")) {
        FILE *sink = fopencookie(NULL, mode, k_sink_io);
        if (sink) {
            uint32_t k = __sync_add_and_fetch(&g_sink_n, 1);
            if (k <= 12) bv_log("fopen(%s, %s): the game's unpacked-file cache is written to nowhere (swfcache=0; #%u)", p, mode, (unsigned)k);
            sink_remember(sink);
            return sink;
        }
    }
    FILE *f = IO_T(0, 0, fopen(p, mode));
    if (!f && errno == ENOENT && (mode[0] == 'w' || mode[0] == 'a' || strchr(mode, '+'))) {
        make_parents(p);
        f = fopen(p, mode);
    }
    if (__sync_add_and_fetch(&bv_st_io_opens, 1) <= 40) BV_TRACE(1, "fopen(%s, %s) -> %p (game file #%u)", p, mode, (void *)f, (unsigned)bv_st_io_opens);
    else BV_TRACE(2, "fopen(%s, %s) -> %p", p, mode, (void *)f);
    return f;
}
static void *w_fdopen(int fd, const char *mode) { return fdopen(fd, mode); }
static int w_remove(const char *path) {
    FIX(p, path);
    return remove(p);
}
static int w_ftruncate(int fd, long len) {
    (void)fd; (void)len;
    errno = ENOSYS;
    return -1;
}

/* ---- descriptors: virtual sockets (900+) are routed to the in-process server ---- */
static int w_close(int fd) {
    if (vr_is(fd)) return vr_close(fd);
    return vs_is(fd) ? vs_close(fd) : close(fd);
}
static long w_read(int fd, void *buf, size_t n) {
    if (vr_is(fd)) return vr_read(fd, buf, n);
    if (vs_is(fd)) {
        long r = vs_recv(fd, buf, n, 0, 0);
        if (r < 0) errno = vs_last_errno();
        return r;
    }
    return IO_T(1, 0, read(fd, buf, n));
}
static long w_write(int fd, const void *buf, size_t n) {
    if (vs_is(fd)) {
        long r = vs_send(fd, buf, n);
        if (r < 0) errno = vs_last_errno();
        return r;
    }
    return IO_T(0, 1, write(fd, buf, n));
}

/* ---- file descriptors ---- */
/* the console's random number generator for the virtual /dev/urandom */
static int rnd_source(void *buf, size_t n) { return sceKernelGetRandomNumber(buf, (SceSize)n) < 0 ? -1 : 0; }

static int w_open(const char *path, int flags, int mode) {
    int vf = vr_file_index(path);
    if (vf) { /* /proc/cpuinfo and friends */
        int vfd = vr_open_file(vf);
        BV_TRACE(1, "open(%s, 0x%x) -> %d (virtual file)", path, flags, vfd);
        return vfd;
    }
    if (vr_is_path(path)) {
        vr_set_source(rnd_source);
        int vfd = vr_open();
        BV_TRACE(1, "open(%s, 0x%x) -> %d (virtual random device)", path, flags, vfd);
        return vfd;
    }
    FIX(p, path);
    int nf = flags & 3; /* O_RDONLY 0 / O_WRONLY 1 / O_RDWR 2 are the same */
    if (flags & 0100) nf |= O_CREAT;
    if (flags & 0200) nf |= O_EXCL;
    if (flags & 01000) nf |= O_TRUNC;
    if (flags & 02000) nf |= O_APPEND;
    int fd = IO_T(0, 0, open(p, nf, mode));
    if (fd < 0 && errno == ENOENT && (nf & O_CREAT)) {
        make_parents(p);
        fd = open(p, nf, mode);
    }
    if (__sync_add_and_fetch(&bv_st_io_opens, 1) <= 40) BV_TRACE(1, "open(%s, 0x%x) -> %d (game file #%u)", p, flags, fd, (unsigned)bv_st_io_opens);
    else BV_TRACE(2, "open(%s, 0x%x) -> %d", p, flags, fd);
    return fd;
}
static int w_openat(int dirfd, const char *path, int flags, int mode) {
    (void)dirfd;
    return w_open(path, flags, mode);
}
static int w_mkdir(const char *path, int mode) {
    FIX(p, path);
    return mkdir_p(p, (mode_t)mode);
}
static int w_rmdir(const char *path) {
    FIX(p, path);
    return rmdir(p);
}
static int w_unlink(const char *path) {
    FIX(p, path);
    return unlink(p);
}
static long w_lseek(int fd, long off, int whence) {
    if (vr_is(fd)) return vr_lseek(fd, off, whence);
    return IO_T(0, 0, lseek(fd, off, whence));
}

static int w_fcntl(int fd, int cmd, ...) {
    if (vs_is(fd)) { /* virtual sockets keep O_NONBLOCK: the game sets it on the wake-up pair and drains it with recv() until EAGAIN */
        if (cmd == 3) return 2 | (vs_get_nonblock(fd) ? 04000 : 0); /* F_GETFL: O_RDWR [| O_NONBLOCK] */
        if (cmd == 4) {                                              /* F_SETFL */
            va_list ap;
            va_start(ap, cmd);
            int arg = va_arg(ap, int);
            va_end(ap);
            vs_set_nonblock(fd, (arg & 04000) != 0);
        }
    }
    return 0;
}
static int w_ioctl(int fd, unsigned long req, ...) {
    if (vs_is(fd) && req == 0x5421) { /* FIONBIO */
        va_list ap;
        va_start(ap, req);
        int *on = va_arg(ap, int *);
        va_end(ap);
        if (on) vs_set_nonblock(fd, *on != 0);
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

/* bionic ARM struct stat, 104 bytes */
typedef struct {
    uint64_t st_dev;
    uint8_t pad0[4];
    uint32_t st_ino32;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint8_t pad3[4];
    int64_t st_size;
    uint32_t st_blksize;
    uint64_t st_blocks;
    uint32_t st_atime_s, st_atime_ns;
    uint32_t st_mtime_s, st_mtime_ns;
    uint32_t st_ctime_s, st_ctime_ns;
    uint64_t st_ino;
} bionic_stat;

static void conv_stat(bionic_stat *o, const struct stat *s) {
    memset(o, 0, sizeof *o);
    o->st_mode = s->st_mode;
    o->st_nlink = 1;
    o->st_size = s->st_size;
    o->st_blksize = 4096;
    o->st_blocks = (uint64_t)((s->st_size + 511) / 512);
    o->st_atime_s = (uint32_t)s->st_atime;
    o->st_mtime_s = (uint32_t)s->st_mtime;
    o->st_ctime_s = (uint32_t)s->st_ctime;
}
static int w_stat(const char *path, bionic_stat *o) {
    FIX(p, path);
    struct stat s;
    int r = IO_T(0, 0, stat(p, &s));
    if (r == 0) conv_stat(o, &s);
    return r;
}
static int w_fstat(int fd, bionic_stat *o) {
    if (vr_is(fd)) { /* a character device */
        memset(o, 0, sizeof *o);
        o->st_mode = 0020666;
        o->st_nlink = 1;
        o->st_blksize = 4096;
        return 0;
    }
    struct stat s;
    int r = fstat(fd, &s);
    if (r == 0) conv_stat(o, &s);
    return r;
}

/* bionic struct dirent (kernel dirent64), 280 bytes */
typedef struct {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
} bionic_dirent;
static bionic_dirent g_dirent;

static void *w_opendir(const char *path) {
    FIX(p, path);
    return opendir(p);
}
static void *w_readdir(void *d) {
    struct dirent *e = readdir((DIR *)d);
    if (!e) return NULL;
    memset(&g_dirent, 0, sizeof g_dirent);
    g_dirent.d_reclen = sizeof g_dirent;
    g_dirent.d_type = 0;
    snprintf(g_dirent.d_name, sizeof g_dirent.d_name, "%s", e->d_name);
    return &g_dirent;
}
static int w_closedir(void *d) { return closedir((DIR *)d); }
static int w_dirfd(void *d) { (void)d; return 3; }
static char *w_getcwd(char *buf, size_t n) {
    if (!buf) return NULL;
    snprintf(buf, n, "%s", BV_DATA_DIR);
    return buf;
}

/* ---- anonymous mmap only ---- */
#define MAP_FAILED_ ((void *)-1)
#define MAP_MAX 512
static struct { void *base; size_t len; int file; } g_maps[MAP_MAX];
static pthread_mutex_t g_map_lock = PTHREAD_MUTEX_INITIALIZER;
static void map_add(void *p, size_t len, int file) {
    pthread_mutex_lock(&g_map_lock);
    for (int i = 0; i < MAP_MAX; i++)
        if (!g_maps[i].base) {
            g_maps[i].base = p;
            g_maps[i].len = len;
            g_maps[i].file = file;
            break;
        }
    pthread_mutex_unlock(&g_map_lock);
}
/* frees the region if addr is exactly the base of a recorded mapping and len covers it; returns whether it did */
static int map_remove_whole(void *p, size_t len) {
    int found = 0, file = 0;
    pthread_mutex_lock(&g_map_lock);
    for (int i = 0; i < MAP_MAX; i++)
        if (g_maps[i].base == p && len >= g_maps[i].len) {
            g_maps[i].base = NULL;
            file = g_maps[i].file;
            found = 1;
            break;
        }
    pthread_mutex_unlock(&g_map_lock);
    if (found && file) {
        static volatile uint32_t nu;
        uint32_t k = __sync_add_and_fetch(&nu, 1);
        if (k <= 40) bv_log("munmap of a file mapping %p (%u bytes) -> memory freed (#%u)", p, (unsigned)len, (unsigned)k);
    }
    if (found) free(p);
    return found;
}
int bv_file_mmap_kb = 32768; /* config file_mmap_kb (default 32768 since update 92, runs 90-99; 0 = off; update 87): read-only file mappings up to this size are served from memory */
static void *file_map(size_t len, int fd, long off) {
    void *p = memalign(4096, len ? len : 1);
    if (!p) return NULL;
    off_t keep = lseek(fd, 0, SEEK_CUR);
    size_t got = 0;
    if (lseek(fd, off, SEEK_SET) == (off_t)off) {
        while (got < len) {
            ssize_t r = read(fd, (char *)p + got, len - got > (1u << 20) ? (1u << 20) : len - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
    }
    if (got < len) memset((char *)p + got, 0, len - got);
    lseek(fd, keep, SEEK_SET);
    return p;
}
static void *w_mmap(void *addr, size_t len, int prot, int flags, int fd, long off) {
    (void)addr; (void)prot; (void)off;
    if (!(flags & 0x20) || fd != -1) { /* MAP_ANONYMOUS */
        static volatile uint32_t nfile;
        uint32_t k = __sync_add_and_fetch(&nfile, 1);
        if (bv_file_mmap_kb > 0 && fd >= 0 && !(prot & 2) && (flags & 0x2) && len <= (size_t)bv_file_mmap_kb * 1024u) {   /* PROT_WRITE off, MAP_PRIVATE */
            uint64_t t0 = sceKernelGetProcessTimeWide();
            void *p = file_map(len, fd, off);
            if (p) {
                map_add(p, len, 1);
                if (k <= 40) bv_log("mmap of a file (fd %d, %u bytes at %ld, prot %d, flags 0x%x) -> %p, read into memory in %u ms (file_mmap_kb=%d; #%u)", fd, (unsigned)len, off, prot,
                                    flags, p, (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000), bv_file_mmap_kb, (unsigned)k);
                return p;
            }
            if (k <= 40) bv_log("mmap of a file (fd %d, %u bytes): no memory for a copy -> failed, ENOMEM (#%u)", fd, (unsigned)len, (unsigned)k);
            errno = ENOMEM;
            return MAP_FAILED_;
        }
        if (k <= 20) bv_log("mmap of a file (fd %d, %u bytes at %ld, prot %d, flags 0x%x) -> failed, ENODEV: file mappings are not supported (#%u)", fd, (unsigned)len, off, prot, flags,
                            (unsigned)k);
        errno = ENODEV;
        return MAP_FAILED_;
    }
    void *p = memalign(4096, len);
    if (!p) {
        errno = ENOMEM;
        return MAP_FAILED_;
    }
    memset(p, 0, len);
    map_add(p, len, 0);
    return p;
}
static int w_munmap(void *addr, size_t len) {
    /* only a whole region that mmap handed out can be returned to the heap; a partial unmap just keeps the memory */
    if (!map_remove_whole(addr, len)) BV_TRACE(1, "munmap(%p, %u): not a whole mapping, memory kept", addr, (unsigned)len);
    return 0;
}
static int w_mprotect(void *a, size_t l, int p) {
    static int n;
    if (n < 24) {
        n++;
        BV_TRACE(1, "mprotect(%p, %u, %d) -> 0 (ignored)", a, (unsigned)l, p);
    }
    return 0;
}
static int w_madvise(void *a, size_t l, int adv) { (void)a; (void)l; (void)adv; return 0; }

const bv_shim bv_shims_io[] = {
    {"__sF", (const void *)bv_sF},
    SHIM(vfprintf), SHIM(fprintf), SHIM(printf), SHIM(puts), SHIM(putchar), SHIM(fputs), SHIM(fputc), SHIM(fwrite),
    SHIM(fread), SHIM(fgets), SHIM(fflush), SHIM(fclose), SHIM(ferror), SHIM(feof), SHIM(getc), SHIM(ungetc), SHIM(fseek),
    SHIM(ftell), SHIM(fseeko), SHIM(ftello), SHIM(rewind), SHIM(fileno), SHIM(fopen), SHIM(fdopen), SHIM(remove),
    SHIM(ftruncate), SHIM(open), SHIM(openat), SHIM(mkdir), SHIM(rmdir), SHIM(unlink), SHIM(fcntl), SHIM(ioctl),
    SHIM(stat), SHIM(fstat), SHIM(opendir), SHIM(readdir), SHIM(closedir), SHIM(dirfd), SHIM(getcwd),
    SHIM(mmap), SHIM(munmap), SHIM(mprotect), SHIM(madvise),
    SHIM(close), SHIM(read), SHIM(write), SHIM(lseek),
    SHIM_END};
