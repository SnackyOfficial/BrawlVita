#include "bv_server.h"

#include <string.h>

void bv_server_init(bv_server *s, const bv_server_ops *ops) {
    memset(s, 0, sizeof *s);
    if (ops) s->ops = *ops;
}

void bv_server_reset(bv_server *s) { (void)s; }

void bv_server_connected(bv_server *s, uint16_t port_host) {
    s->connects++;
    if (s->ops.log) s->ops.log(s->ops.ctx, "virtual socket connected to port %u (connect #%u)", (unsigned)port_host, s->connects);
    if (s->ops.on_connect) s->ops.on_connect(s->ops.ctx, port_host);
}

int bv_server_send(bv_server *s, const uint8_t *data, size_t len) {
    if (s->writes == 0 && len) {
        s->first_len = len < sizeof s->first ? len : sizeof s->first;
        memcpy(s->first, data, s->first_len);
        if (s->ops.log) {
            char hex[40] = {0};
            for (size_t i = 0; i < s->first_len && i < 12; i++) {
                static const char d[] = "0123456789abcdef";
                hex[2 * i] = d[s->first[i] >> 4];
                hex[2 * i + 1] = d[s->first[i] & 15];
            }
            s->ops.log(s->ops.ctx, "first write on a virtual socket: %u bytes, starts %s (dropped)", (unsigned)len, hex);
        }
    }
    s->writes++;
    s->bytes_written += len;
    return (int)len;
}

size_t bv_server_recv(bv_server *s, uint8_t *buf, size_t n) {
    (void)s; (void)buf; (void)n;
    return 0;
}
size_t bv_server_pending(const bv_server *s) {
    (void)s;
    return 0;
}
size_t bv_server_peek(const bv_server *s, uint8_t *buf, size_t n) {
    (void)s; (void)buf; (void)n;
    return 0;
}
