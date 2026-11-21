#include "vsys.h"

#include <errno.h>
#include <string.h>

int bv_vsys_handle(long n, long a1, long a2, long a3, long tid, long *ret, int *err) {
    (void)a1;
    *err = 0;
    switch (n) {
    case BV_NR_GETTID:
        *ret = tid;
        return 1;
    case BV_NR_SCHED_SETAFFINITY: /* (pid, size, mask): accepted, nothing to pin on this system */
        if (a2 < 1 || !a3) {
            *ret = -1;
            *err = EINVAL;
        } else {
            *ret = 0;
        }
        return 1;
    case BV_NR_SCHED_GETAFFINITY: /* (pid, size, mask): the raw call returns the size of the kernel's mask (4 bytes for 4 cores) and fills it: cores 0-3 */
        if (a2 < 4 || !a3) {
            *ret = -1;
            *err = EINVAL;
        } else {
            memset((void *)a3, 0, (size_t)a2);
            *(unsigned char *)a3 = 0x0f;
            *ret = 4;
        }
        return 1;
    default:
        return 0;
    }
}
