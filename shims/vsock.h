/* Virtual sockets: the game's TCP connection ends in a peer that accepts it and drops what is written (server/bv_server.c).
 * Portable (no console headers) so it can be tested on the PC; the bionic-ABI wrappers live in shims/shim_net.c.
 * Virtual descriptors are 900..963 (below FD_SETSIZE, never equal to a real file descriptor).
 */
#ifndef BV_VSOCK_H
#define BV_VSOCK_H

#include <stddef.h>
#include <stdint.h>

#include "../server/bv_server.h"

#define VS_FIRST_FD 900
#define VS_MAX 64
#define VS_PAIR_BUF 4096 /* bytes a socketpair end can hold unread */

typedef struct vs_ops {
    void (*sleep_us)(unsigned us);
    void (*lock)(void);
    void (*unlock)(void);
} vs_ops;

void vs_init(const vs_ops *ops, bv_server *server);
int vs_is(int fd);
int vs_socketpair(int out[2]);                 /* two connected descriptors that carry bytes to each other: the wake-up channel of the game's network loop (libevent-style) */
void vs_set_nonblock(int fd, int on);          /* O_NONBLOCK / FIONBIO: recv never waits (EAGAIN instead) */
int vs_get_nonblock(int fd);
int vs_socket(void);                           /* new unconnected descriptor, -1 if none left */
int vs_connect(int fd, uint32_t addr_be, uint16_t port_be);
long vs_send(int fd, const void *buf, size_t len);
long vs_recv(int fd, void *buf, size_t len, int peek, int dontwait); /* -1 with *err = 11 (EAGAIN) if nothing; 0 = closed */
int vs_close(int fd);
/* recv as a blocking socket does it: waits for data (for all `len` bytes with `waitall`), returns what is there after a shutdown (0 = EOF),
 * fails with EBADF if the descriptor is closed while waiting, with EAGAIN (11) only if `dontwait`. Returns -1 with vs_last_errno() set on failure. */
long vs_recv_wait(int fd, void *buf, size_t len, int peek, int dontwait, int waitall);
int vs_shutdown(int fd);                       /* no more data will come: readers get what is pending, then 0 (EOF) */
int vs_readable(int fd);                       /* data pending */
int vs_writable(int fd);                       /* connected */
int vs_last_errno(void);                       /* Linux errno value of the last failure (what the game expects) */
/* Waits until `fd` is readable (data pending or shut down) or `timeout_us` passed (0 = just check).
 * Returns 1 readable, 0 timeout, -1 if the descriptor was closed meanwhile. */
int vs_wait_readable(int fd, unsigned timeout_us);

#endif
