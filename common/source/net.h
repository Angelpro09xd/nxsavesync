#pragma once

#include <switch.h>
#include <stddef.h>

// Capa de framing sobre TCP. Los mensajes de control se arman en un buffer en
// memoria; los cuerpos de archivo se transmiten en streaming para no cargar un
// save entero en RAM.
typedef struct {
    int    fd;
    u8    *wbuf;      // payload saliente en construccion
    size_t wcap, wlen;
    u8    *rbuf;      // payload entrante completo
    size_t rcap, rlen, rpos;
    bool   overflow;  // alguna escritura no cupo: send fallara a proposito
    bool   dead;      // fallo el socket: el flujo ya no es fiable
    char   err[256];
} net_t;

bool net_connect(net_t *n, const char *host, u16 port);
void net_close(net_t *n);
const char *net_error(net_t *n);

// True si el socket fallo. A partir de ahi el flujo esta desincronizado y no
// tiene sentido seguir hablando por esa conexion.
bool net_is_dead(net_t *n);

// Lee y descarta `count` bytes. Sirve para no dejar a medias el cuerpo de un
// mensaje cuyo procesado fallo por causas locales.
void net_drain_raw(net_t *n, u64 count);

// --- envio ---
void net_begin(net_t *n, u8 op);          // empieza un mensaje
void net_w_u8(net_t *n, u8 v);
void net_w_u16(net_t *n, u16 v);
void net_w_u32(net_t *n, u32 v);
void net_w_u64(net_t *n, u64 v);
void net_w_str(net_t *n, const char *s);
bool net_send(net_t *n);                  // manda cabecera + payload acumulado

// Para cuerpos grandes: manda la cabecera con un tamano ya conocido y luego
// los bytes en bruto. `extra` son los bytes que se enviaran con net_send_raw
// despues del payload acumulado.
bool net_send_streaming(net_t *n, u64 extra);
bool net_send_raw(net_t *n, const void *data, size_t len);

// --- recepcion ---
// Lee un mensaje completo a memoria. El opcode queda en *op.
bool net_recv(net_t *n, u8 *op);
// Lee solo la cabecera; el cuerpo se consume luego con net_recv_body (a
// memoria, para poder parsearlo) o con net_recv_raw (en streaming).
bool net_recv_header(net_t *n, u8 *op, u32 *payload_len);
bool net_recv_body(net_t *n, u32 payload_len);
bool net_recv_raw(net_t *n, void *data, size_t len);

bool net_r_u8(net_t *n, u8 *v);
bool net_r_u32(net_t *n, u32 *v);
bool net_r_u64(net_t *n, u64 *v);
// Copia como cadena terminada en NUL. Trunca si no cabe (y devuelve false).
bool net_r_str(net_t *n, char *out, size_t out_size);
