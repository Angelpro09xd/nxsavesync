#pragma once

#include <switch.h>

#include "proto.h"

#define DISC_MAX_HOSTS 8

typedef struct {
    char ip[40];
    char name[64];      // hostname del PC
    char emu[48];       // emulador detectado ahi
    u16  port;
} host_t;

typedef struct {
    host_t v[DISC_MAX_HOSTS];
    size_t n;
} hostlist_t;

// Manda un broadcast y recoge respuestas durante `timeout_ms`.
// No es un error que no conteste nadie: la lista sale vacia.
bool discovery_scan(hostlist_t *out, int timeout_ms);
