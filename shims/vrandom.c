#include "vrandom.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned char g_open[VR_MAX];
static unsigned char g_kind[VR_MAX]; /* 0 = random device, n = virtual file n */
static size_t g_pos[VR_MAX];
static vr_source_fn g_src;


static const char k_cpuinfo[] =
    "Processor\t: ARMv7 Processor rev 10 (v7l)\n"
    "processor\t: 0\nBogoMIPS\t: 888.00\n\n"
    "processor\t: 1\nBogoMIPS\t: 888.00\n\n"
    "processor\t: 2\nBogoMIPS\t: 888.00\n\n"
    "processor\t: 3\nBogoMIPS\t: 888.00\n\n"
    "Features\t: swp half thumb fastmult vfp edsp neon vfpv3 tls vfpd32 \n"
    "CPU implementer\t: 0x41\n"
    "CPU architecture: 7\n"
    "CPU variant\t: 0x2\n"
    "CPU part\t: 0xc09\n"
    "CPU revision\t: 10\n"
    "\n"
    "Hardware\t: PlayStation Vita\n"
    "Revision\t: 0000\n"
    "Serial\t\t: 0000000000000000\n";
static const char k_cpus[] = "0-3\n";
/* auxv: AT_PAGESZ = 4096, AT_HWCAP = swp|half|thumb|fastmult|vfp|edsp|neon|vfpv3|tls|vfpd32, AT_NULL */
static const unsigned char k_auxv[] = {6, 0, 0, 0, 0x00, 0x10, 0, 0, 16, 0, 0, 0, 0xd7, 0xb0, 0x08, 0, 0, 0, 0, 0, 0, 0, 0, 0};

static const struct { const char *path; const void *data; size_t len; } k_files[] = {
    {"/proc/cpuinfo", k_cpuinfo, sizeof k_cpuinfo - 1},
    {"/sys/devices/system/cpu/present", k_cpus, sizeof k_cpus - 1},
    {"/sys/devices/system/cpu/possible", k_cpus, sizeof k_cpus - 1},
    {"/proc/self/auxv", k_auxv, sizeof k_auxv},
};
#define NFILES ((int)(sizeof k_files / sizeof k_files[0]))

int vr_file_index(const char *path) {
    if (!path) return 0;
    for (int i = 0; i < NFILES; i++)
        if (!strcmp(path, k_files[i].path)) return i + 1;
    return 0;
}

const void *vr_file_data(int index, size_t *len) {
    if (index < 1 || index > NFILES) return NULL;
    if (len) *len = k_files[index - 1].len;
    return k_files[index - 1].data;
}

void vr_set_source(vr_source_fn fn) { g_src = fn; }

int vr_is_path(const char *path) { return path && (!strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random")); }

static int open_kind(int kind) {
    int fd = -1;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < VR_MAX; i++)
        if (!g_open[i]) {
            g_open[i] = 1;
            g_kind[i] = (unsigned char)kind;
            g_pos[i] = 0;
            fd = VR_FIRST_FD + i;
            break;
        }
    pthread_mutex_unlock(&g_mu);
    if (fd < 0) errno = EMFILE;
    return fd;
}

int vr_open(void) {
    if (!g_src) {
        errno = ENODEV;
        return -1;
    }
    return open_kind(0);
}

int vr_open_file(int index) {
    if (index < 1 || index > NFILES) {
        errno = ENOENT;
        return -1;
    }
    return open_kind(index);
}

int vr_is(int fd) {
    if (fd < VR_FIRST_FD || fd >= VR_FIRST_FD + VR_MAX) return 0;
    pthread_mutex_lock(&g_mu);
    int r = g_open[fd - VR_FIRST_FD];
    pthread_mutex_unlock(&g_mu);
    return r;
}

long vr_read(int fd, void *buf, size_t n) {
    if (!vr_is(fd)) {
        errno = EBADF;
        return -1;
    }
    int kind = g_kind[fd - VR_FIRST_FD];
    if (kind) { /* a virtual file: from its position, 0 at the end */
        size_t flen = k_files[kind - 1].len, pos = g_pos[fd - VR_FIRST_FD];
        size_t c = pos >= flen ? 0 : (flen - pos < n ? flen - pos : n);
        memcpy(buf, (const unsigned char *)k_files[kind - 1].data + pos, c);
        g_pos[fd - VR_FIRST_FD] = pos + c;
        return (long)c;
    }
    unsigned char *p = (unsigned char *)buf;
    size_t done = 0;
    while (done < n) { /* in chunks: the kernel call takes small sizes */
        size_t c = n - done > 256 ? 256 : n - done;
        if (g_src(p + done, c) != 0) {
            if (done) return (long)done; /* a short read is what read() may return */
            errno = EIO;
            return -1;
        }
        done += c;
    }
    return (long)done;
}

long vr_lseek(int fd, long off, int whence) {
    if (!vr_is(fd)) {
        errno = EBADF;
        return -1;
    }
    int kind = g_kind[fd - VR_FIRST_FD];
    if (!kind) return 0;
    long base = whence == 0 ? 0 : whence == 1 ? (long)g_pos[fd - VR_FIRST_FD] : whence == 2 ? (long)k_files[kind - 1].len : -1;
    if (base < 0 || base + off < 0) {
        errno = EINVAL;
        return -1;
    }
    g_pos[fd - VR_FIRST_FD] = (size_t)(base + off);
    return base + off;
}

int vr_close(int fd) {
    if (!vr_is(fd)) {
        errno = EBADF;
        return -1;
    }
    pthread_mutex_lock(&g_mu);
    g_open[fd - VR_FIRST_FD] = 0;
    pthread_mutex_unlock(&g_mu);
    return 0;
}
