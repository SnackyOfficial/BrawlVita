/* The "server" of Build B is not a network peer: the game's send-message function is replaced (loader/hook_b.c) and answers are
 * injected into the client directly. The game still opens a TCP connection first, so the virtual sockets (shims/vsock.c) need a peer
 * that accepts the connection, swallows what is written and never has anything to read. This is that peer. Portable C11.
 */
#ifndef BV_SERVER_H
#define BV_SERVER_H

#include <stddef.h>
#include <stdint.h>

typedef struct bv_server_ops {
    void *ctx;
    void (*on_connect)(void *ctx, uint16_t port_host); /* the game connected a virtual socket to this port */
    void (*log)(void *ctx, const char *fmt, ...);
} bv_server_ops;

typedef struct bv_server {
    bv_server_ops ops;
    uint64_t bytes_written; /* what the game wrote to its sockets (dropped) */
    uint32_t writes, connects;
    uint8_t first[16];      /* first bytes of the first write, for the log */
    size_t first_len;
} bv_server;

void bv_server_init(bv_server *s, const bv_server_ops *ops);
void bv_server_reset(bv_server *s); /* a new connection (nothing is buffered here: nothing to do) */
void bv_server_connected(bv_server *s, uint16_t port_host);
/* Client -> server bytes are dropped; returns len. */
int bv_server_send(bv_server *s, const uint8_t *data, size_t len);
/* Server -> client: never anything. */
size_t bv_server_recv(bv_server *s, uint8_t *buf, size_t n);
size_t bv_server_pending(const bv_server *s);
size_t bv_server_peek(const bv_server *s, uint8_t *buf, size_t n);

#endif
