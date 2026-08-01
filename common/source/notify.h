#pragma once

#include <switch.h>
#include <stddef.h>

#include "settings.h"

#define NOTIFY_PATH CFG_DIR "/ultima-sync.txt"

typedef enum {
    NOTIFY_OK = 0,      // sincronizado sin novedad
    NOTIFY_CHANGES,     // se movieron archivos
    NOTIFY_ATTENTION,   // algo necesita que decidas desde la app
    NOTIFY_FAIL,        // no se pudo
} notify_kind_t;

// Parpadea el LED del boton HOME de los mandos conectados. Es la unica via de
// aviso que tiene un sysmodule: los servicios de applet, que son los que sacan
// mensajes en pantalla, no estan disponibles para un proceso de fondo.
void notify_led(notify_kind_t kind);

// Deja constancia de la ultima sincronizacion de fondo para que la app la
// muestre la proxima vez que se abra.
void notify_record(notify_kind_t kind, int pulled, int pushed, int deleted,
                   int pending, const char *detail);

typedef struct {
    bool  valid;
    u8    kind;
    int   pulled, pushed, deleted, pending;
    char  detail[128];
    u64   when;        // segundos, reloj local de la consola
} notify_info_t;

// Lee el registro. `consume` lo borra tras leerlo, para no repetir el aviso.
bool notify_read(notify_info_t *out, bool consume);

// --------------------------------------------------------------------------
// estado para el aviso en pantalla
// --------------------------------------------------------------------------
//
// Un archivo aparte de `ultima-sync.txt`, y por un motivo: aquel se consume al
// leerlo (es un aviso de una vez), mientras que este describe *como estan las
// cosas ahora* y lo relee el overlay cada pocos segundos.
//
// Lo escriben tanto la app como el sysmodule, y lo lee el overlay para pintar
// el aviso sobre el menu HOME. Va por archivo y no por red a proposito: el
// overlay no habla el protocolo ni toca savedata, solo lee la SD.

#define ESTADO_PATH      CFG_DIR "/estado.txt"
#define ESTADO_MAX_JUEGOS 6

typedef struct {
    bool valid;
    int  pendientes;                       // cuantos esperan algo, en total
    int  n;                                // cuantos vienen nombrados aqui
    char nombre[ESTADO_MAX_JUEGOS][64];
    u8   estado[ESTADO_MAX_JUEGOS];        // SUM_*
    u64  when;
} estado_t;

void estado_write(const estado_t *e);
bool estado_read(estado_t *out);
