#pragma once

#include <switch.h>

#include "proto.h"
#include "discovery.h"

#define CFG_DIR       "sdmc:/switch/nxsavesync"
#define CFG_PATH      CFG_DIR "/config.txt"
#define CFG_PERGAME   CFG_DIR "/juegos.txt"
#define CFG_PROFILES  CFG_DIR "/perfiles.txt"

#define SET_MAX_HOSTS   8
#define SET_MAX_PERGAME 256
#define SET_MAX_USERS   8

typedef struct {
    u64 title_id;
    u8  policy;    // POLICY_*
    u8  excluded;  // 1 = no sincronizar nunca este juego
} game_rule_t;

typedef struct {
    AccountUid uid;
    u8 shared;     // 1 = este perfil participa en la sincronizacion
} profile_rule_t;

typedef struct {
    // hosts conocidos; el 0 es el que se usa
    host_t hosts[SET_MAX_HOSTS];
    size_t host_count;
    size_t host_sel;

    u8 mode;            // MODE_MANUAL / MODE_AUTO
    u8 policy;          // politica global en auto
    u8 sync_on_open;    // sincronizar todo nada mas abrir la app
    u8 auto_discover;   // buscar hosts por broadcast al arrancar
    u8 sounds;          // efectos de sonido de la interfaz
    u8 music;           // musica ambiental de fondo
    u8 ask_incoming;    // preguntar cuando el PC trae cambios

    // Sysmodule. Apagado por defecto a proposito: es un proceso que corre
    // siempre y eso se activa a conciencia, no por sorpresa.
    u8  bg_enabled;
    u8  bg_policy;      // POLICY_* que aplica sin nadie delante
    u16 bg_interval;    // segundos entre repasos
    u8  bg_on_exit;     // sincronizar justo al cerrar un juego
    u8  bg_nudge;       // atender los avisos del PC

    game_rule_t    games[SET_MAX_PERGAME];
    size_t         game_count;

    profile_rule_t profiles[SET_MAX_USERS];
    size_t         profile_count;
} settings_t;

extern settings_t g_set;

void settings_load(void);
void settings_save(void);

const host_t *settings_host(void);
void settings_add_host(const host_t *h, bool select_it);

// Politica efectiva de un juego: la suya si tiene, si no la global.
u8 settings_policy_for(u64 title_id);
bool settings_excluded(u64 title_id);
void settings_set_game(u64 title_id, u8 policy, u8 excluded);

bool settings_profile_shared(AccountUid uid);
void settings_set_profile(AccountUid uid, bool shared);
