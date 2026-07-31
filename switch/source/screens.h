#pragma once

// Todo el dibujo de la interfaz. Aqui no se lee nada de la consola ni se toca
// la red: entra el estado ya masticado y sale una pantalla.
//
// Esta separado por una razon muy concreta: asi el mismo codigo compila en el
// PC contra datos de mentira y se puede *mirar* el resultado. La version
// anterior acumulo seis solapamientos que hubo que cazar de uno en uno a base
// de fotos de la consola.

#include "ui.h"

typedef enum { NAV_GAMES, NAV_USERS, NAV_HOSTS, NAV_SETTINGS, NAV_COUNT } nav_t;

extern const char *SCR_NAV_NAMES[NAV_COUNT];

// --------------------------------------------------------------------------
// medidas
// --------------------------------------------------------------------------
//
// Una sola definicion por medida, y lo de abajo se deriva de lo de arriba. Es
// la mitad del trabajo para que nada se solape: si no hay dos numeros que
// digan lo mismo, no pueden discrepar.

#define DOCK_X      22
#define DOCK_Y      148
#define DOCK_W      80
#define DOCK_H      424
#define DOCK_R      40
#define DOCK_FIRST  (DOCK_Y + 58)
#define DOCK_STEP   86
#define DOCK_ITEM   68

#define CONTENT_X   126
#define CONTENT_R   1258
#define CONTENT_W   (CONTENT_R - CONTENT_X)

#define HEAD_Y      26
#define BODY_Y      118
#define BODY_B      620
#define BODY_H      (BODY_B - BODY_Y)

#define HINT_Y      648
#define HINT_H      48

#define GRID_COLS   4
#define GRID_ROWS   3
#define GRID_PAGE   (GRID_COLS * GRID_ROWS)
#define CARD_W      152
#define CARD_H      158
#define CARD_GAP    14
#define GRID_X      CONTENT_X
#define GRID_Y      BODY_Y
#define GRID_R      (GRID_X + GRID_COLS * CARD_W + (GRID_COLS - 1) * CARD_GAP)

#define DET_X       (GRID_R + 20)
#define DET_W       (CONTENT_R - DET_X)
#define DET_Y       BODY_Y
#define DET_H       BODY_H

// --------------------------------------------------------------------------
// datos
// --------------------------------------------------------------------------

typedef struct {
    const char *name;
    const char *author;
    u64   title_id;
    u8    state;          // SUM_*
    bool  excluded;
    const void *icon;
    size_t icon_size;
} scr_game_t;

typedef struct {
    const char *name;
    const void *icon;
    size_t icon_size;
    bool shared;
} scr_user_t;

typedef struct {
    const char *name, *ip, *emu;
    unsigned port;
} scr_host_t;

typedef struct {
    const char *label, *help, *value;
    color_t vcolor;
} scr_row_t;

typedef struct {
    color_t accent;
    nav_t   nav;
    const scr_user_t *user;     // perfil en uso, o NULL
    const char *pc_name;        // como se llama el PC en uso, o NULL
    const char *pc_emu;         // emulador que respondio, o NULL
    const char *mode;           // "Manual" / "Automatico"
    bool  mode_auto;
    bool  pc_live;              // ha contestado en esta sesion
    const char *version;
} scr_ctx_t;

// --------------------------------------------------------------------------
// armazon
// --------------------------------------------------------------------------

// El fondo vivo. Deja abierta la capa de fondo y la cierra el mismo.
void scr_backdrop(const scr_ctx_t *c, const void *hero, size_t hero_len);

// Devuelve la seccion tocada, o -1. `interactive` a false solo dibuja.
int  scr_dock(const scr_ctx_t *c, const ui_input_t *in, bool interactive);

void scr_topbar(const scr_ctx_t *c, const char *title, const char *sub);
void scr_hints(const char *hints);
void scr_toast(const char *msg, float in, color_t accent);

// --------------------------------------------------------------------------
// vistas
// --------------------------------------------------------------------------

// Rejilla de juegos + panel de detalle. Devuelve el indice tocado, o -1.
// `retap` se pone a true si el toque fue sobre el que ya estaba elegido.
int  scr_games(const scr_ctx_t *c, const ui_input_t *in,
               const scr_game_t *g, int n, int sel, int top,
               const char *policy_label, bool *retap,
               bool *hit_sync, bool *hit_opts);

void scr_games_empty(const scr_ctx_t *c);

// Listas. Devuelven la fila tocada, o -1.
int  scr_users(const scr_ctx_t *c, const ui_input_t *in,
               const scr_user_t *u, int n, int sel, int in_use);
int  scr_hosts(const scr_ctx_t *c, const ui_input_t *in,
               const scr_host_t *h, int n, int sel, int in_use);
int  scr_rows(const scr_ctx_t *c, const ui_input_t *in,
              const scr_row_t *r, int n, int sel, int top, int visible);

// --------------------------------------------------------------------------
// hojas
// --------------------------------------------------------------------------

// Oscurece el fondo y devuelve la caja de la hoja.
void scr_sheet(const scr_ctx_t *c, int w, int h, int *out_x, int *out_y);

typedef struct { const char *key, *title, *sub; } scr_choice_t;

// Dialogo de tres opciones. Devuelve 0/1/2 si se toca alguna, o -1.
int  scr_dialog(const scr_ctx_t *c, const ui_input_t *in,
                const char *heading, const char *name, const char *body,
                const scr_choice_t *ch, color_t tint);

// Devuelven la fila tocada, o -1.
int  scr_game_opts(const scr_ctx_t *c, const ui_input_t *in, const scr_game_t *g,
                   const char *policy, bool excluded, int sel);
int  scr_pc_cfg(const scr_ctx_t *c, const ui_input_t *in, const char *server,
                const scr_row_t *rows, int n, int sel, bool ok);

void scr_sync(const scr_ctx_t *c, const char *title, const char *now,
              const char (*log)[160], int log_n,
              float progress, bool finished);
