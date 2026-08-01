#include "net.h"
#include "proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#ifndef NET_BUF_INITIAL
#define NET_BUF_INITIAL (256 * 1024)
#endif

#define WBUF_INITIAL NET_BUF_INITIAL
#define RBUF_INITIAL NET_BUF_INITIAL

static void set_err(net_t *n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(n->err, sizeof(n->err), fmt, ap);
    va_end(ap);
}

const char *net_error(net_t *n) { return n->err; }

bool net_is_dead(net_t *n) { return n->dead; }

// --------------------------------------------------------------------------
// conexion
// --------------------------------------------------------------------------

bool net_connect(net_t *n, const char *host, u16 port)
{
    memset(n, 0, sizeof(*n));
    n->fd = -1;

    n->wbuf = malloc(WBUF_INITIAL);
    n->rbuf = malloc(RBUF_INITIAL);
    if (!n->wbuf || !n->rbuf) {
        set_err(n, "sin memoria para los buffers de red");
        net_close(n);
        return false;
    }
    n->wcap = WBUF_INITIAL;
    n->rcap = RBUF_INITIAL;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        set_err(n, "IP invalida: %s", host);
        net_close(n);
        return false;
    }

    n->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (n->fd < 0) {
        set_err(n, "socket() fallo: %s", strerror(errno));
        net_close(n);
        return false;
    }

    if (connect(n->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        set_err(n, "no se pudo conectar a %s:%u (%s)", host, port, strerror(errno));
        net_close(n);
        return false;
    }

    // Sin esto, los mensajes de control pequenos se quedan esperando en el
    // buffer de Nagle y cada peticion suma ~40 ms.
    int one = 1;
    setsockopt(n->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    return true;
}

void net_close(net_t *n)
{
    if (n->fd >= 0) { close(n->fd); n->fd = -1; }
    free(n->wbuf); n->wbuf = NULL;
    free(n->rbuf); n->rbuf = NULL;
    n->wcap = n->wlen = 0;
    n->rcap = n->rlen = n->rpos = 0;
}

// --------------------------------------------------------------------------
// primitivas de socket
// --------------------------------------------------------------------------

static bool send_all(net_t *n, const void *data, size_t len)
{
    const u8 *p = (const u8 *)data;
    while (len > 0) {
        ssize_t k = send(n->fd, p, len, 0);
        if (k < 0) {
            if (errno == EINTR) continue;
            set_err(n, "send() fallo: %s", strerror(errno));
            n->dead = true;
            return false;
        }
        if (k == 0) {
            set_err(n, "conexion cerrada por el PC durante el envio");
            n->dead = true;
            return false;
        }
        p   += k;
        len -= k;
    }
    return true;
}

static bool recv_all(net_t *n, void *data, size_t len)
{
    u8 *p = (u8 *)data;
    while (len > 0) {
        ssize_t k = recv(n->fd, p, len, 0);
        if (k < 0) {
            if (errno == EINTR) continue;
            set_err(n, "recv() fallo: %s", strerror(errno));
            n->dead = true;
            return false;
        }
        if (k == 0) {
            set_err(n, "el PC cerro la conexion");
            n->dead = true;
            return false;
        }
        p   += k;
        len -= k;
    }
    return true;
}

void net_drain_raw(net_t *n, u64 count)
{
    static u8 sink[64 * 1024];
    while (count > 0) {
        size_t want = count < sizeof(sink) ? (size_t)count : sizeof(sink);
        if (!recv_all(n, sink, want)) return;
        count -= want;
    }
}

// --------------------------------------------------------------------------
// escritura
// --------------------------------------------------------------------------

static bool wreserve(net_t *n, size_t extra)
{
    if (n->wlen + extra <= n->wcap) return true;

    size_t cap = n->wcap ? n->wcap : WBUF_INITIAL;
    while (cap < n->wlen + extra) cap *= 2;
    if (cap > PROTO_MAX_FRAME) { n->overflow = true; return false; }

    u8 *nb = realloc(n->wbuf, cap);
    if (!nb) { n->overflow = true; return false; }
    n->wbuf = nb;
    n->wcap = cap;
    return true;
}

void net_begin(net_t *n, u8 op)
{
    n->wlen     = 0;
    n->overflow = false;
    // Reservamos la cabecera; el tamano se rellena al enviar.
    if (wreserve(n, 5)) {
        n->wbuf[0] = op;
        n->wlen    = 5;
    }
}

void net_w_u8(net_t *n, u8 v)
{
    if (!wreserve(n, 1)) return;
    n->wbuf[n->wlen++] = v;
}

void net_w_u16(net_t *n, u16 v)
{
    if (!wreserve(n, 2)) return;
    n->wbuf[n->wlen++] = (u8)(v);
    n->wbuf[n->wlen++] = (u8)(v >> 8);
}

void net_w_u32(net_t *n, u32 v)
{
    if (!wreserve(n, 4)) return;
    for (int i = 0; i < 4; i++) n->wbuf[n->wlen++] = (u8)(v >> (8 * i));
}

void net_w_u64(net_t *n, u64 v)
{
    if (!wreserve(n, 8)) return;
    for (int i = 0; i < 8; i++) n->wbuf[n->wlen++] = (u8)(v >> (8 * i));
}

void net_w_str(net_t *n, const char *s)
{
    size_t len = s ? strlen(s) : 0;
    if (len > 0xFFFF) { n->overflow = true; return; }
    net_w_u16(n, (u16)len);
    if (!wreserve(n, len)) return;
    memcpy(n->wbuf + n->wlen, s, len);
    n->wlen += len;
}

// Escribe el tamano de payload en la cabecera ya reservada.
static void patch_len(net_t *n, u64 payload_len)
{
    for (int i = 0; i < 4; i++)
        n->wbuf[1 + i] = (u8)(payload_len >> (8 * i));
}

bool net_send(net_t *n)
{
    if (n->overflow) {
        set_err(n, "mensaje demasiado grande");
        return false;
    }
    patch_len(n, n->wlen - 5);
    return send_all(n, n->wbuf, n->wlen);
}

bool net_send_streaming(net_t *n, u64 extra)
{
    if (n->overflow) {
        set_err(n, "mensaje demasiado grande");
        return false;
    }
    u64 payload = (n->wlen - 5) + extra;
    if (payload > PROTO_MAX_FRAME) {
        set_err(n, "payload de %llu bytes por encima del limite de trama", payload);
        return false;
    }
    patch_len(n, payload);
    return send_all(n, n->wbuf, n->wlen);
}

void net_w_bytes(net_t *n, const void *data, size_t len)
{
    if (!data || len == 0) return;
    if (!wreserve(n, len)) return;
    memcpy(n->wbuf + n->wlen, data, len);
    n->wlen += len;
}

bool net_send_raw(net_t *n, const void *data, size_t len)
{
    return send_all(n, data, len);
}

// --------------------------------------------------------------------------
// lectura
// --------------------------------------------------------------------------

bool net_recv_header(net_t *n, u8 *op, u32 *payload_len)
{
    u8 hdr[5];
    if (!recv_all(n, hdr, 5)) return false;

    *op = hdr[0];
    u32 len = (u32)hdr[1] | ((u32)hdr[2] << 8) | ((u32)hdr[3] << 16) | ((u32)hdr[4] << 24);
    if (len > PROTO_MAX_FRAME) {
        set_err(n, "trama de %u bytes por encima del limite", len);
        return false;
    }
    *payload_len = len;
    return true;
}

bool net_recv_raw(net_t *n, void *data, size_t len)
{
    return recv_all(n, data, len);
}

bool net_recv_body(net_t *n, u32 len)
{
    if (len > n->rcap) {
        size_t cap = n->rcap ? n->rcap : RBUF_INITIAL;
        while (cap < len) cap *= 2;
        u8 *nb = realloc(n->rbuf, cap);
        if (!nb) { set_err(n, "sin memoria para %u bytes", len); return false; }
        n->rbuf = nb;
        n->rcap = cap;
    }

    if (len > 0 && !recv_all(n, n->rbuf, len)) return false;

    n->rlen = len;
    n->rpos = 0;
    return true;
}

bool net_recv(net_t *n, u8 *op)
{
    u32 len;
    if (!net_recv_header(n, op, &len)) return false;
    return net_recv_body(n, len);
}

static bool rtake(net_t *n, void *out, size_t len)
{
    if (n->rpos + len > n->rlen) {
        set_err(n, "mensaje del PC truncado");
        return false;
    }
    memcpy(out, n->rbuf + n->rpos, len);
    n->rpos += len;
    return true;
}

bool net_r_u8(net_t *n, u8 *v)
{
    return rtake(n, v, 1);
}

bool net_r_u32(net_t *n, u32 *v)
{
    u8 b[4];
    if (!rtake(n, b, 4)) return false;
    *v = (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
    return true;
}

bool net_r_u64(net_t *n, u64 *v)
{
    u8 b[8];
    if (!rtake(n, b, 8)) return false;
    u64 r = 0;
    for (int i = 0; i < 8; i++) r |= (u64)b[i] << (8 * i);
    *v = r;
    return true;
}

bool net_r_str(net_t *n, char *out, size_t out_size)
{
    u8 b[2];
    if (!rtake(n, b, 2)) return false;
    size_t len = (size_t)b[0] | ((size_t)b[1] << 8);

    if (n->rpos + len > n->rlen) {
        set_err(n, "cadena truncada en el mensaje del PC");
        return false;
    }

    size_t copy = len < out_size - 1 ? len : out_size - 1;
    memcpy(out, n->rbuf + n->rpos, copy);
    out[copy] = '\0';
    n->rpos += len;

    return copy == len;
}
