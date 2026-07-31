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

// Guarda un aviso como imagen en el Album de la consola.
//
// Un proceso de fondo NO puede sacar un mensaje en pantalla: los servicios de
// applet, que son los que dibujan encima del juego, no le estan permitidos. Los
// unicos canales que le quedan son el LED del mando y esto. No es un pop-up,
// pero se ve al abrir el Album y no depende de abrir la app.
//
// `lineas` termina con NULL.
bool notify_album(const char *titulo, const char *const *lineas, notify_kind_t kind);

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
