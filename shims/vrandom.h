/* Virtual /dev/urandom and /dev/random. The game's C++ runtime (libc++ std::random_device) opens /dev/urandom and reads it; the Vita has no such file, so the open
 * failed with ENOENT, libc++ threw std::system_error and the game's render thread died before its first frame (log of 2026-10-26: "random_device failed to open
 * /dev/urandom", EGL swaps 0 for 60 s). The descriptors handed out here are above the virtual sockets (900+) and far above the real descriptors of the console. */
#ifndef BV_VRANDOM_H
#define BV_VRANDOM_H

#include <stddef.h>

#define VR_FIRST_FD 1000
#define VR_MAX 16

/* Fills buf with n random bytes; returns 0 on success. Set by the console glue (sceKernelGetRandomNumber); the host test sets its own. */
typedef int (*vr_source_fn)(void *buf, size_t n);
void vr_set_source(vr_source_fn fn);

int vr_is_path(const char *path);                /* "/dev/urandom" or "/dev/random" */

/* Virtual CPU-information files. Android's CPU detection (cpu-features, inside libfmod.so and the game) reads them; on the Vita they do not exist and FMOD then
 * never got initialised ("FMOD error! (67) ... System::init ... was not called" for every sound, then a NULL music object: log of 2026-10-28, the open of /proc/cpuinfo
 * is the first thing after System::setOutput). They describe what the Vita has: 4 cores, Cortex-A9, ARMv7 with NEON and VFPv3-D32. */
int vr_file_index(const char *path);             /* 1-based index of a virtual file for this path (/proc/cpuinfo, /sys/devices/system/cpu/present|possible, /proc/self/auxv), else 0 */
int vr_open_file(int index);                     /* like vr_open(), a descriptor for that file with its own read position */
const void *vr_file_data(int index, size_t *len);/* the bytes of the file (for fopen, via fmemopen) */
long vr_lseek(int fd, long off, int whence);     /* the new position, or -1 with errno EINVAL / EBADF (a random device has no position: 0) */
int vr_open(void);                               /* a new descriptor >= VR_FIRST_FD, or -1 with errno = EMFILE (or ENODEV without a source) */
int vr_is(int fd);                               /* is fd one of ours (open)? */
long vr_read(int fd, void *buf, size_t n);       /* n bytes, or -1 with errno EBADF / EIO */
int vr_close(int fd);                            /* 0, or -1 with errno EBADF */

#endif
