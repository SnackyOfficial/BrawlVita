#include "vsock.h"

#include <string.h>

#define E_BADF 9
#define E_AGAIN 11
#define E_INVAL 22
#define E_NOTCONN 107
#define E_CONNREFUSED 111
#define E_PIPE 32

typedef struct {
    int used;
    int connected;
    int shut;
    uint32_t addr_be;
    uint16_t port_be;
    int nonblock;
    int is_pair, peer, peer_closed; /* socketpair end: slot index of the other end; it closed */
    unsigned rx_len;
    uint8_t rx[VS_PAIR_BUF];        /* bytes the other end sent, not read yet */
} vsock;

static vsock g_socks[VS_MAX];
static vs_ops g_ops;
static bv_server *g_server;
static int g_errno;

static void lock(void) { if (g_ops.lock) g_ops.lock(); }
static void unlock(void) { if (g_ops.unlock) g_ops.unlock(); }

void vs_init(const vs_ops *ops, bv_server *server) {
    memset(g_socks, 0, sizeof g_socks);
    g_ops = ops ? *ops : (vs_ops){0};
    g_server = server;
    g_errno = 0;
}

int vs_last_errno(void) { return g_errno; }

static vsock *get(int fd) {
    if (fd < VS_FIRST_FD || fd >= VS_FIRST_FD + VS_MAX) return NULL;
    vsock *s = &g_socks[fd - VS_FIRST_FD];
    return s->used ? s : NULL;
}

int vs_is(int fd) { return get(fd) != NULL; }

int vs_socket(void) {
    lock();
    for (int i = 0; i < VS_MAX; i++) {
        if (!g_socks[i].used) {
            memset(&g_socks[i], 0, sizeof g_socks[i]);
            g_socks[i].used = 1;
            unlock();
            return VS_FIRST_FD + i;
        }
    }
    unlock();
    g_errno = 24; /* EMFILE */
    return -1;
}

int vs_socketpair(int out[2]) {
    lock();
    int a = -1, b = -1;
    for (int i = 0; i < VS_MAX && b < 0; i++)
        if (!g_socks[i].used) {
            if (a < 0) a = i;
            else b = i;
        }
    if (a < 0 || b < 0) {
        unlock();
        g_errno = 24; /* EMFILE */
        return -1;
    }
    memset(&g_socks[a], 0, sizeof g_socks[a]);
    memset(&g_socks[b], 0, sizeof g_socks[b]);
    g_socks[a].used = g_socks[b].used = 1;
    g_socks[a].connected = g_socks[b].connected = 1;
    g_socks[a].is_pair = g_socks[b].is_pair = 1;
    g_socks[a].peer = b;
    g_socks[b].peer = a;
    unlock();
    out[0] = VS_FIRST_FD + a;
    out[1] = VS_FIRST_FD + b;
    return 0;
}

void vs_set_nonblock(int fd, int on) {
    lock();
    vsock *s = get(fd);
    if (s) s->nonblock = on != 0;
    unlock();
}

int vs_get_nonblock(int fd) {
    lock();
    vsock *s = get(fd);
    int r = s ? s->nonblock : 0;
    unlock();
    return r;
}

int vs_connect(int fd, uint32_t addr_be, uint16_t port_be) {
    lock();
    vsock *s = get(fd);
    if (!s) { unlock(); g_errno = E_BADF; return -1; }
    s->connected = 1;
    s->shut = 0;
    s->addr_be = addr_be;
    s->port_be = port_be;
    if (g_server) bv_server_reset(g_server);
    unlock();
    if (g_server) bv_server_connected(g_server, (uint16_t)(((port_be & 0xff) << 8) | (port_be >> 8)));
    return 0;
}

long vs_send(int fd, const void *buf, size_t len) {
    lock();
    vsock *s = get(fd);
    if (!s) { unlock(); g_errno = E_BADF; return -1; }
    if (!s->connected) { unlock(); g_errno = E_NOTCONN; return -1; }
    if (s->is_pair) {
        vsock *p = &g_socks[s->peer];
        if (s->peer_closed || !p->used || !p->is_pair || p->peer != (int)(s - g_socks) || s->shut) { unlock(); g_errno = E_PIPE; return -1; }
        size_t room = VS_PAIR_BUF - p->rx_len, n = len < room ? len : room;
        if (len && n == 0) { unlock(); g_errno = E_AGAIN; return -1; }
        memcpy(p->rx + p->rx_len, buf, n);
        p->rx_len += (unsigned)n;
        unlock();
        return (long)n;
    }
    int r = g_server ? bv_server_send(g_server, (const uint8_t *)buf, len) : -1;
    unlock();
    if (r < 0) { g_errno = E_PIPE; return -1; }
    return (long)len;
}

long vs_recv(int fd, void *buf, size_t len, int peek, int dontwait) {
    (void)dontwait;
    lock();
    vsock *s = get(fd);
    if (!s) { unlock(); g_errno = E_BADF; return -1; }
    if (!s->connected) { unlock(); g_errno = E_NOTCONN; return -1; }
    size_t n = 0;
    if (s->is_pair) {
        n = len < s->rx_len ? len : s->rx_len;
        memcpy(buf, s->rx, n);
        if (!peek) {
            memmove(s->rx, s->rx + n, s->rx_len - n);
            s->rx_len -= (unsigned)n;
        }
        int eof = s->shut || s->peer_closed;
        unlock();
        if (n == 0) {
            if (eof) return 0;
            g_errno = E_AGAIN;
            return -1;
        }
        return (long)n;
    }
    if (g_server) {
        if (peek) n = bv_server_peek(g_server, (uint8_t *)buf, len);
        else n = bv_server_recv(g_server, (uint8_t *)buf, len);
    }
    unlock();
    if (n == 0) {
        if (s->shut) return 0; /* EOF */
        g_errno = E_AGAIN;
        return -1;
    }
    return (long)n;
}

long vs_recv_wait(int fd, void *buf, size_t len, int peek, int dontwait, int waitall) {
    size_t got = 0;
    if (!vs_is(fd)) {
        g_errno = E_BADF;
        return -1;
    }
    if (len == 0) return 0;
    if (vs_get_nonblock(fd)) dontwait = 1;
    for (;;) {
        long r = vs_recv(fd, (uint8_t *)buf + got, len - got, peek, dontwait);
        if (r > 0) {
            got += (size_t)r;
            if (peek || !waitall || got >= len) return (long)got;
            continue;
        }
        if (r == 0) return (long)got; /* EOF: what was read so far, or 0 */
        if (g_errno != E_AGAIN) return got ? (long)got : -1;
        if (dontwait) {
            if (got) return (long)got;
            g_errno = E_AGAIN;
            return -1;
        }
        if (vs_wait_readable(fd, 50000) < 0) { /* closed while waiting */
            if (got) return (long)got;
            g_errno = E_BADF;
            return -1;
        }
    }
}

int vs_shutdown(int fd) {
    lock();
    vsock *s = get(fd);
    if (!s) { unlock(); g_errno = E_BADF; return -1; }
    s->shut = 1;
    unlock();
    return 0;
}

int vs_close(int fd) {
    lock();
    vsock *s = get(fd);
    if (!s) { unlock(); g_errno = E_BADF; return -1; }
    if (s->is_pair) { /* the other end reads what is left, then EOF; its writes fail with EPIPE */
        vsock *p = &g_socks[s->peer];
        if (p->used && p->is_pair && p->peer == (int)(s - g_socks)) p->peer_closed = 1;
    }
    s->used = 0;
    unlock();
    return 0;
}

int vs_readable(int fd) {
    lock();
    vsock *s = get(fd);
    int r = s && s->connected && (s->is_pair ? (s->rx_len > 0 || s->shut || s->peer_closed) : ((g_server && bv_server_pending(g_server) > 0) || s->shut));
    unlock();
    return r;
}

int vs_writable(int fd) {
    lock();
    vsock *s = get(fd);
    int r = s && s->connected;
    unlock();
    return r;
}

int vs_wait_readable(int fd, unsigned timeout_us) {
    unsigned waited = 0;
    for (;;) {
        if (!vs_is(fd)) return -1; /* closed while waiting */
        if (vs_readable(fd)) return 1;
        if (waited >= timeout_us || !g_ops.sleep_us) return 0;
        unsigned step = timeout_us - waited < 2000 ? timeout_us - waited : 2000;
        g_ops.sleep_us(step);
        waited += step;
    }
}
