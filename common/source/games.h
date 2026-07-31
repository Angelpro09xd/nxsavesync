#pragma once

#include <switch.h>
#include <stddef.h>

typedef struct {
    u64   application_id;
    char  name[256];    // nombre del juego, o el title id si no se pudo leer
    char  author[128];
    u8   *icon;         // JPEG del icono; NULL si no se pudo leer
    size_t icon_size;
    u8    state;        // SUM_* que devolvio el PC
} game_t;

typedef struct {
    game_t *v;
    size_t  n, cap;
} gamelist_t;

typedef struct {
    AccountUid uid;
    char       name[64];
    u8        *icon;      // JPEG del avatar
    size_t     icon_size;
} user_t;

typedef struct {
    user_t v[8];
    size_t n;
} userlist_t;

// Lista los savedata de tipo cuenta de `uid`, con nombre e icono de cada juego.
// `with_icons` a false se salta los iconos, que es mucho mas rapido.
bool games_list(gamelist_t *g, AccountUid uid, bool with_icons);
void games_free(gamelist_t *g);

// Todos los perfiles de la consola, con su avatar.
bool users_list(userlist_t *u);
void users_free(userlist_t *u);

// Desactiva la consulta de nombres/iconos por 'ns' (para el sysmodule).
void games_set_metadata_enabled(bool on);

bool user_nickname(AccountUid uid, char *out, size_t out_size);
