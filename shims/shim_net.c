/* Network entry points for the game: every socket is a virtual one served by the in-process fake server.
 * No real network is touched. bionic ABI (sockaddr_in, fd_set, pollfd, addrinfo, timeval are the Linux ARM layouts).
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/threadmgr.h>

#include "../loader/log.h"
#include "shim.h"
#include "vsock.h"

void bv_sleep_delay(int kind, uint32_t us); /* shims/shim_pthread.c, update 54 */

#define BV_GAME_SERVER_PORT 9339
#define AF_INET_ 2
#define SOCK_STREAM_ 1
#define MSG_PEEK_ 2
#define MSG_WAITALL_ 0x100
#define MSG_DONTWAIT_ 0x40
#define POLLIN_ 1
#define POLLOUT_ 4
#define POLLNVAL_ 32
#define SO_ERROR_ 4

static int fail(int linux_errno) {
    errno = linux_errno;
    return -1;
}

static int w_socket(int domain, int type, int proto) {
    (void)proto;
    if (domain != AF_INET_ || (type & 0xf) != SOCK_STREAM_) {
        BV_TRACE(1, "socket(domain %d type %d) -> refused", domain, type);
        return fail(97); /* EAFNOSUPPORT */
    }
    int fd = vs_socket();
    BV_TRACE(1, "socket() -> virtual fd %d", fd);
    if (fd < 0) errno = vs_last_errno();
    return fd;
}

typedef struct {
    uint16_t family;
    uint16_t port; /* big endian */
    uint32_t addr; /* big endian */
    uint8_t zero[8];
} sockaddr_in_;

static int w_connect(int fd, const sockaddr_in_ *sa, unsigned len) {
    if (!vs_is(fd)) return fail(9);
    if (!sa || len < 8) return fail(22);
    uint32_t a = sa->addr;
    BV_TRACE(1, "connect(fd %d, %u.%u.%u.%u:%u) -> virtual server", fd, a & 255, (a >> 8) & 255, (a >> 16) & 255, a >> 24,
             (unsigned)(((sa->port & 255) << 8) | (sa->port >> 8)));
    unsigned port = (unsigned)(((sa->port & 255) << 8) | (sa->port >> 8));
    if (port != BV_GAME_SERVER_PORT) {
        /* only the game server is simulated; any other TCP service (the original reaches the real network here) is unreachable */
        bv_log("connect(fd %d, port %u): refused (only port %d is virtual)", fd, port, BV_GAME_SERVER_PORT);
        return fail(111); /* ECONNREFUSED */
    }
    return vs_connect(fd, sa->addr, sa->port);
}

static long w_send(int fd, const void *buf, size_t len, int flags) {
    (void)flags;
    if (!vs_is(fd)) return fail(9);
    long r = vs_send(fd, buf, len);
    if (r < 0) errno = vs_last_errno();
    return r;
}
static long w_sendto(int fd, const void *buf, size_t len, int flags, const void *addr, unsigned alen) {
    (void)addr; (void)alen;
    return w_send(fd, buf, len, flags);
}

/* A blocking recv waits for data (all of `len` with MSG_WAITALL), returns what is pending when the peer shut down, and fails with EBADF if the
 * descriptor is closed while waiting. EAGAIN only for MSG_DONTWAIT. */
static long w_recv(int fd, void *buf, size_t len, int flags) {
    if (!vs_is(fd)) return fail(9);
    long r = vs_recv_wait(fd, buf, len, (flags & MSG_PEEK_) != 0, (flags & MSG_DONTWAIT_) != 0, (flags & MSG_WAITALL_) != 0);
    if (r < 0) errno = vs_last_errno();
    return r;
}
static long w_recvfrom(int fd, void *buf, size_t len, int flags, void *addr, unsigned *alen) {
    if (addr && alen) *alen = 0;
    return w_recv(fd, buf, len, flags);
}

static int w_shutdown(int fd, int how) {
    (void)how;
    return vs_is(fd) ? vs_shutdown(fd) : fail(9);
}
static int w_setsockopt(int fd, int level, int name, const void *val, unsigned len) {
    (void)level; (void)name; (void)val; (void)len;
    return vs_is(fd) ? 0 : fail(9);
}
static int w_getsockopt(int fd, int level, int name, void *val, unsigned *len) {
    (void)level;
    if (!vs_is(fd)) return fail(9);
    if (name == SO_ERROR_ && val && len && *len >= 4) {
        memset(val, 0, 4);
        *len = 4;
    }
    return 0;
}
static int fill_addr(int fd, sockaddr_in_ *sa, unsigned *len) {
    if (!vs_is(fd)) return fail(9);
    if (sa && len && *len >= 16) {
        memset(sa, 0, sizeof *sa);
        sa->family = AF_INET_;
        sa->addr = 0x0100007f; /* 127.0.0.1 */
        *len = 16;
    }
    return 0;
}
static int w_getsockname(int fd, sockaddr_in_ *sa, unsigned *len) { return fill_addr(fd, sa, len); }
static int w_getpeername(int fd, sockaddr_in_ *sa, unsigned *len) { return fill_addr(fd, sa, len); }

/* ---- readiness: bionic fd_set is 1024 bits in 32-bit words ---- */
#define FDSET_WORDS 32
static int fd_isset(const uint32_t *s, int fd) { return (s[fd >> 5] >> (fd & 31)) & 1; }
static void fd_clr(uint32_t *s, int fd) { s[fd >> 5] &= ~(1u << (fd & 31)); }

static int w_select(int nfds, uint32_t *rd, uint32_t *wr, uint32_t *ex, const int32_t *tv) {
    long long wait_us = tv ? (long long)tv[0] * 1000000LL + tv[1] : -1; /* -1 = forever */
    long long waited = 0;
    if (ex) memset(ex, 0, FDSET_WORDS * sizeof(uint32_t)); /* no exceptional conditions exist here: only ready bits may stay set */
    for (;;) {
        int ready = 0;
        uint32_t rd_copy[FDSET_WORDS], wr_copy[FDSET_WORDS];
        if (rd) memcpy(rd_copy, rd, sizeof rd_copy);
        if (wr) memcpy(wr_copy, wr, sizeof wr_copy);
        for (int fd = 0; fd < nfds && fd < 1024; fd++) {
            if (rd && fd_isset(rd_copy, fd)) {
                if (vs_is(fd) && vs_readable(fd)) ready++;
                else fd_clr(rd_copy, fd);
            }
            if (wr && fd_isset(wr_copy, fd)) {
                if (vs_is(fd) && vs_writable(fd)) ready++;
                else fd_clr(wr_copy, fd);
            }
        }
        if (ready || (wait_us >= 0 && waited >= wait_us)) {
            if (rd) memcpy(rd, rd_copy, sizeof rd_copy);
            if (wr) memcpy(wr, wr_copy, sizeof wr_copy);
            return ready;
        }
        bv_sleep_delay(3, 2000);
        waited += 2000;
    }
}

typedef struct {
    int fd;
    int16_t events;
    int16_t revents;
} pollfd_;

static int w_poll(pollfd_ *fds, unsigned n, int timeout_ms) {
    long long waited = 0, limit = timeout_ms < 0 ? -1 : (long long)timeout_ms * 1000;
    for (;;) {
        int ready = 0;
        for (unsigned i = 0; i < n; i++) {
            fds[i].revents = 0;
            if (!vs_is(fds[i].fd)) { fds[i].revents = POLLNVAL_; ready++; continue; }
            if ((fds[i].events & POLLIN_) && vs_readable(fds[i].fd)) fds[i].revents |= POLLIN_;
            if ((fds[i].events & POLLOUT_) && vs_writable(fds[i].fd)) fds[i].revents |= POLLOUT_;
            if (fds[i].revents) ready++;
        }
        if (ready || (limit >= 0 && waited >= limit)) return ready;
        bv_sleep_delay(4, 2000);
        waited += 2000;
    }
}

/* ---- name resolution: every name is the local server ---- */
typedef struct addrinfo_ {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    unsigned ai_addrlen;
    char *ai_canonname;
    sockaddr_in_ *ai_addr;
    struct addrinfo_ *ai_next;
} addrinfo_;

static int w_getaddrinfo(const char *node, const char *service, const addrinfo_ *hints, addrinfo_ **res) {
    (void)hints;
    addrinfo_ *ai = (addrinfo_ *)calloc(1, sizeof *ai + sizeof(sockaddr_in_));
    if (!ai) return -10; /* EAI_MEMORY */
    sockaddr_in_ *sa = (sockaddr_in_ *)(ai + 1);
    unsigned port = service ? (unsigned)atoi(service) : 0;
    sa->family = AF_INET_;
    sa->port = (uint16_t)(((port & 255) << 8) | (port >> 8));
    /* The game's own TCP clients (libg+0x4f8edc and +0x5acbf0) refuse 10/8, 127/8, 172.16/12 and 192.168/16 before they call connect(), so
     * 127.0.0.1 would make them give up without a connection attempt. 203.0.113.10 is a documentation address (TEST-NET-3): public-looking,
     * routed nowhere; connect() is virtual anyway. */
    sa->addr = 0x0A7100CBu;
    ai->ai_family = AF_INET_;
    ai->ai_socktype = SOCK_STREAM_;
    ai->ai_protocol = 6;
    ai->ai_addrlen = sizeof *sa;
    ai->ai_addr = sa;
    *res = ai;
    BV_TRACE(1, "getaddrinfo(%s, %s) -> 203.0.113.10 (virtual server)", node ? node : "(null)", service ? service : "(null)");
    return 0;
}
static void w_freeaddrinfo(addrinfo_ *ai) { free(ai); }

/* things that do not apply (FMOD's network streaming, pairs, name lookups by index) */
static int w_bind(int fd, const void *sa, unsigned len) { (void)fd; (void)sa; (void)len; return fail(13); }
static int w_listen(int fd, int backlog) { (void)fd; (void)backlog; return fail(95); }
static int w_accept(int fd, void *sa, unsigned *len) { (void)fd; (void)sa; (void)len; return fail(95); }
/* The game's network loop makes a socketpair as its wake-up channel (one end is written to wake the select() that waits on the other). Refusing it (error 97,
 * "socketpair failed") left the loop with descriptor -1 in its fd_set: an out-of-range write and the crash of 2026-10-27 right after the first connect. */
static int w_socketpair(int d, int t, int p, int *sv) {
    (void)p;
    if ((d != 1 && d != AF_INET_) || (t & 0xf) != SOCK_STREAM_ || !sv) {
        BV_TRACE(1, "socketpair(domain %d type 0x%x) -> refused", d, t);
        return fail(97);
    }
    if (vs_socketpair(sv) < 0) return fail(vs_last_errno());
    if (t & 0x800) { /* SOCK_NONBLOCK */
        vs_set_nonblock(sv[0], 1);
        vs_set_nonblock(sv[1], 1);
    }
    BV_TRACE(1, "socketpair(domain %d type 0x%x) -> virtual fds %d, %d", d, t, sv[0], sv[1]);
    return 0;
}
static char *w_if_indextoname(unsigned idx, char *name) { (void)idx; (void)name; return NULL; }
static int w_setpriority(int which, int who, int prio) { (void)which; (void)who; (void)prio; return 0; }

const bv_shim bv_shims_net[] = {
    SHIM(socket), SHIM(connect), SHIM(send), SHIM(sendto), SHIM(recv), SHIM(recvfrom), SHIM(shutdown),
    SHIM(setsockopt), SHIM(getsockopt), SHIM(getsockname), SHIM(getpeername), SHIM(select), SHIM(poll),
    SHIM(getaddrinfo), SHIM(freeaddrinfo), SHIM(bind), SHIM(listen), SHIM(accept), SHIM(socketpair),
    SHIM(if_indextoname), SHIM(setpriority),
    SHIM_END};
