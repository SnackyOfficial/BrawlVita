/* The few raw Linux system calls the game's libraries make through syscall(). FMOD pins its audio threads to cores with sched_getaffinity / sched_setaffinity
 * (log of 2026-10-29: syscall(242) -> -1, syscall(241) -> -1, right before the log ends): the loader answered every syscall with ENOSYS, so FMOD's core mask stayed
 * uninitialised. They now answer as a 4-core machine. Everything else is still ENOSYS. */
#ifndef BV_VSYS_H
#define BV_VSYS_H

#define BV_NR_GETTID 224
#define BV_NR_SCHED_SETAFFINITY 241
#define BV_NR_SCHED_GETAFFINITY 242

/* Returns 1 when syscall n is handled here: *ret is its result and *err an errno value (0 = none). tid is the caller's thread id. */
int bv_vsys_handle(long n, long a1, long a2, long a3, long tid, long *ret, int *err);

#endif
