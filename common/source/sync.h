#pragma once

#include <switch.h>

#include "net.h"
#include "proto.h"

typedef struct {
    void (*log)(void *ud, const char *fmt, ...);
    // Devuelve WINNER_SWITCH, WINNER_PC, o -1 para saltarse el juego.
    int  (*ask_conflict)(void *ud, const char *title_name, size_t n_conflicts);
    // Aviso del PC ante una situacion ambigua. Devuelve DEC_*.
    int  (*ask_warning)(void *ud, const char *title_name, u8 code, const char *message);
    // El PC trae cambios para la consola. Si esta puesto, se pregunta con que
    // lado quedarse antes de aplicarlos. Devuelve DEC_*.
    int  (*ask_incoming)(void *ud, const char *title_name, size_t files, size_t deletes);
    // Progreso de una transferencia; puede ser NULL.
    void (*progress)(void *ud, const char *title_name, size_t done, size_t total);
    void *ud;
} sync_ui_t;

typedef struct {
    int  pulled;
    int  pushed;
    int  del_local;
    int  del_remote;
    int  conflicts;
    u8   warning;   // WARN_* si el PC aviso de algo
    bool skipped;   // conflicto o aviso sin resolver, o juego excluido
} sync_stats_t;

// --- ajustes del daemon del PC, editables desde aqui ---

#define CFG_MAX_ITEMS   16
#define CFG_MAX_OPTIONS 8

typedef struct {
    char key[64];
    u8   type;                              // CFG_*
    char label[96];
    char help[160];
    char value[128];
    char options[CFG_MAX_OPTIONS][64];
    u32  n_options;
} cfg_item_t;

bool sync_cfg_get(net_t *n, cfg_item_t *out, size_t max, size_t *count);
bool sync_cfg_set(net_t *n, const char *key, const char *value,
                  char *msg_out, size_t msg_size);

// Handshake. Rellena el nombre del PC y del emulador si se pasan.
bool sync_hello(net_t *n, sync_ui_t *ui, char *server_out, size_t server_size,
                char *emu_out, size_t emu_size);

// Sincroniza el savedata de un juego para un perfil concreto.
bool sync_title(net_t *n, AccountUid uid, const char *user_name,
                u64 app_id, const char *title_name,
                u8 mode, u8 policy, sync_ui_t *ui, sync_stats_t *st);

// Pide el estado de varios juegos sin sincronizar nada (para pintar la lista).
bool sync_summary(net_t *n, AccountUid uid, const u64 *title_ids, size_t count,
                  u8 *out_states);
