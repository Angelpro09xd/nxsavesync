#pragma once

#include <switch.h>
#include <stddef.h>

#include "proto.h"

typedef struct {
    char path[PROTO_MAX_PATH];  // relativa a la raiz del savedata, con '/'
    u64  size;
    u32  crc;
    u64  mtime;                 // hora de la consola; 0 si no se sabe
} mf_entry_t;

typedef struct {
    mf_entry_t *v;
    size_t      n, cap;
} manifest_t;

// Recorre `root` (ej. "save:") y calcula tamano + CRC32 de cada archivo.
// Puede tardar unos segundos en saves grandes: hay que leerlos enteros.
// `progress` puede ser NULL; si no, se llama con cada ruta ya procesada.
bool manifest_build(manifest_t *m, const char *root,
                    void (*progress)(const char *rel, size_t done, void *ud),
                    void *ud);

// Fecha del archivo mas reciente del manifiesto, en la hora de la consola.
// Devuelve 0 si el sistema de archivos no da fechas utiles.
u64 manifest_newest(const manifest_t *m);

void manifest_free(manifest_t *m);

// Une root y una ruta relativa en `out`. Devuelve false si no cabe.
bool mf_join(char *out, size_t out_size, const char *root, const char *rel);

// Crea los directorios padre de una ruta relativa dentro de root.
bool mf_make_parents(const char *root, const char *rel);

// Borra un archivo y, hacia arriba, los directorios que queden vacios.
bool mf_delete(const char *root, const char *rel);
