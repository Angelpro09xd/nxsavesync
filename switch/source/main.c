// NX Save Sync -- sincroniza los savedata de la Switch con uno o varios PCs.
//
// Desarrollador: Angelpro09_Dev
//
// Aqui esta la logica: que se lee de la consola, que se le pide al PC y que se
// hace con la respuesta. El dibujo entero vive en screens.c, que no sabe nada
// de red ni de savedata. La separacion no es por gusto: asi la interfaz se
// puede compilar y *mirar* en el PC (ver tools/preview).

#include <switch.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "proto.h"
#include "net.h"
#include "ui.h"
#include "screens.h"
#include "audio.h"
#include "games.h"
#include "sync.h"
#include "settings.h"
#include "discovery.h"
#include "notify.h"

#define APP_VERSION "4.9"

#define LOG_LINES 10
#define VIEW_MAX  512

typedef enum { MODAL_NONE, MODAL_GAME, MODAL_PCCFG } modal_t;

// --------------------------------------------------------------------------
// estado
// --------------------------------------------------------------------------

static userlist_t g_users;
static size_t     g_user_sel;
static gamelist_t g_games;
static size_t     g_game_sel, g_grid_top;
static hostlist_t g_found;

static char g_server_name[128];
static char g_server_emu[64];

// Los emuladores que hay en el PC. El indice que devuelve el resumen por juego
// apunta a esta lista.
static emu_info_t g_emus[SYNC_MAX_EMUS];
static size_t     g_emus_n;

static nav_t   g_nav   = NAV_GAMES;
static modal_t g_modal = MODAL_NONE;

static int  g_row_sel;        // fila elegida en perfiles / PCs / ajustes
static int  g_set_top;
static char g_toast[160];
static u32  g_toast_until;
static float g_toast_in;

static color_t g_accent;

// Animaciones de entrada y salida. Una hoja no puede cerrarse en el acto: hay
// que seguir dibujandola mientras se va, asi que el cierre se pide y se cumple
// unos fotogramas despues.
static float g_modal_in;
static bool  g_modal_closing;
static float g_view_in = 1.0f;    // cambio de seccion
static float g_sheet_in;          // hojas de sincronizacion y dialogos

// registro de la sincronizacion
static char   g_log[LOG_LINES][160];
static int    g_log_n;
static char   g_progress_title[128];
static size_t g_progress_done, g_progress_total;
static bool   g_sync_finished;

// Nombre del emulador donde se jugo por ultima vez, o NULL si no se sabe.
static const char *emu_name_of(u8 idx)
{
    return idx < g_emus_n ? g_emus[idx].name : NULL;
}

// modelo de vista, rellenado cada fotograma
static scr_game_t g_vgames[VIEW_MAX];
static scr_user_t g_vusers[SET_MAX_USERS];
static scr_host_t g_vhosts[SET_MAX_HOSTS];
static scr_emu_t  g_vemus[SYNC_MAX_EMUS];

static void pccfg_fetch(void);
static void clona_perfil(void);
static const char *subtitle_for(void);

// --------------------------------------------------------------------------
// utilidades
// --------------------------------------------------------------------------

static void toast(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_toast, sizeof(g_toast), fmt, ap);
    va_end(ap);
    g_toast_until = ui_ticks() + 3200;
    g_toast_in = 0.0f;
}

static AccountUid current_uid(void)       { return g_users.v[g_user_sel].uid; }
static const char *current_user_name(void){ return g_users.v[g_user_sel].name; }

static const char *mode_label(void)
{
    return g_set.mode == MODE_AUTO ? "Automatico" : "Manual";
}

static const char *policy_label(u8 p)
{
    switch (p) {
    case POLICY_SWITCH: return "Gana la Switch";
    case POLICY_PC:     return "Gana el PC";
    case POLICY_SKIP:   return "No tocar nada";
    case POLICY_NEWEST: return "Gana el ultimo jugado";
    default:            return "Preguntar";
    }
}

// Recorre las politicas. `con_preguntar` solo donde tiene sentido (por juego,
// para heredar la general); en segundo plano no hay a quien preguntar.
static u8 policy_next(u8 p, bool con_preguntar)
{
    switch (p) {
    case POLICY_ASK:    return POLICY_SWITCH;
    case POLICY_SWITCH: return POLICY_PC;
    case POLICY_PC:     return POLICY_NEWEST;
    case POLICY_NEWEST: return POLICY_SKIP;
    default:            return con_preguntar ? POLICY_ASK : POLICY_SWITCH;
    }
}

// --------------------------------------------------------------------------
// modelo de vista
// --------------------------------------------------------------------------

static int build_games(void)
{
    int n = (int)(g_games.n < VIEW_MAX ? g_games.n : VIEW_MAX);
    for (int i = 0; i < n; i++) {
        game_t *g = &g_games.v[i];
        g_vgames[i].name      = g->name;
        g_vgames[i].author    = g->author;
        g_vgames[i].title_id  = g->application_id;
        g_vgames[i].state     = g->state;
        g_vgames[i].excluded  = settings_excluded(g->application_id);
        g_vgames[i].icon      = g->icon;
        g_vgames[i].icon_size = g->icon_size;
        g_vgames[i].emu       = emu_name_of(g->emu);
    }
    return n;
}

static int build_users(void)
{
    int n = (int)g_users.n;
    for (int i = 0; i < n; i++) {
        g_vusers[i].name      = g_users.v[i].name;
        g_vusers[i].icon      = g_users.v[i].icon;
        g_vusers[i].icon_size = g_users.v[i].icon_size;
        g_vusers[i].shared    = settings_profile_shared(g_users.v[i].uid);
    }
    return n;
}

static int build_emus(void)
{
    for (size_t i = 0; i < g_emus_n; i++) {
        g_vemus[i].name   = g_emus[i].name;
        g_vemus[i].path   = g_emus[i].path;
        g_vemus[i].active = g_emus[i].active;
    }
    return (int)g_emus_n;
}

static int build_hosts(void)
{
    int n = (int)g_set.host_count;
    for (int i = 0; i < n; i++) {
        g_vhosts[i].name = g_set.hosts[i].name;
        g_vhosts[i].ip   = g_set.hosts[i].ip;
        g_vhosts[i].emu  = g_set.hosts[i].emu;
        g_vhosts[i].port = g_set.hosts[i].port;
    }
    return n;
}

static scr_ctx_t view_ctx(void)
{
    const host_t *h = settings_host();

    scr_ctx_t c;
    memset(&c, 0, sizeof(c));
    c.accent    = g_accent;
    c.nav       = g_nav;
    c.user      = g_users.n ? &g_vusers[g_user_sel] : NULL;
    c.pc_name   = h ? (h->name[0] ? h->name : h->ip) : NULL;
    c.pc_emu    = g_server_emu[0] ? g_server_emu : NULL;
    c.mode      = mode_label();
    c.mode_auto = g_set.mode == MODE_AUTO;
    c.pc_live   = g_server_name[0] != '\0';
    c.version   = "v" APP_VERSION;
    return c;
}

// El acento sale del icono del juego elegido y tine la interfaz entera. Cambia
// con transicion, no de golpe: el fondo entero se mueve con el.
static void update_accent(void)
{
    color_t want = COL_ACCENT;
    if (g_games.n > 0 && g_game_sel < g_games.n) {
        game_t *g = &g_games.v[g_game_sel];
        if (g->icon) want = ui_image_color(g->icon, g->icon_size);
    }
    g_accent = ui_mix(g_accent, want, ui_approach(0.0f, 1.0f, 7.0f));
}

static const void *hero_icon(size_t *len)
{
    if (g_games.n > 0 && g_game_sel < g_games.n && g_games.v[g_game_sel].icon) {
        *len = g_games.v[g_game_sel].icon_size;
        return g_games.v[g_game_sel].icon;
    }
    *len = 0;
    return NULL;
}

// Fondo + dock + cabecera: el armazon que sale en todas las pantallas.
static int draw_frame(const ui_input_t *in, const scr_ctx_t *c,
                      const char *title, const char *sub, bool interactive)
{
    size_t hlen;
    const void *hero = hero_icon(&hlen);

    scr_backdrop(c, hero, hlen);
    int nav_hit = scr_dock(c, in, interactive);
    scr_topbar(c, title, sub);
    return nav_hit;
}

// Pide cerrar la hoja. Se cierra de verdad cuando termina de irse.
static void modal_close(void)
{
    if (g_modal == MODAL_NONE || g_modal_closing) return;
    g_modal_closing = true;
    audio_play(SND_BACK);
}

static void modal_open(modal_t m)
{
    g_modal = m;
    g_modal_closing = false;
    g_modal_in = 0.0f;
    g_row_sel = 0;
}

static void draw_toast(void)
{
    bool visible = ui_ticks() < g_toast_until && g_toast[0];
    g_toast_in = ui_approach(g_toast_in, visible ? 1.0f : 0.0f, 13.0f);
    scr_toast(g_toast, g_toast_in, g_accent);
}

// --------------------------------------------------------------------------
// hoja de sincronizacion
// --------------------------------------------------------------------------

static void log_push(const char *line)
{
    if (g_log_n < LOG_LINES) {
        snprintf(g_log[g_log_n++], 160, "%s", line);
    } else {
        memmove(g_log[0], g_log[1], (LOG_LINES - 1) * 160);
        snprintf(g_log[LOG_LINES - 1], 160, "%s", line);
    }
}

static void sync_redraw(void)
{
    ui_input_t in;
    if (!ui_frame_begin(&in)) return;

    scr_ctx_t c = view_ctx();
    draw_frame(&in, &c, "Juegos", "NX Save Sync  ·  Angelpro09_Dev", false);
    scr_hints("Sincronizando...");

    float prog = g_progress_total > 0
               ? (float)g_progress_done / (float)g_progress_total : -1.0f;
    if (g_sync_finished) prog = 1.0f;

    g_sheet_in = ui_approach(g_sheet_in, 1.0f, 15.0f);
    scr_sync(&c, g_sync_finished ? "Sincronizacion terminada" : "Sincronizando",
             g_progress_title, (const char (*)[160])g_log, g_log_n,
             prog, g_sync_finished, g_sheet_in);

    ui_ripples_draw();
    ui_debug_draw();
    ui_frame_end();
}

static void cb_log(void *ud, const char *fmt, ...)
{
    (void)ud;
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    log_push(line);
    sync_redraw();
}

static void cb_progress(void *ud, const char *title, size_t done, size_t total)
{
    (void)ud;
    snprintf(g_progress_title, sizeof(g_progress_title), "%s", title);
    g_progress_done  = done;
    g_progress_total = total;
    sync_redraw();
}

// --------------------------------------------------------------------------
// dialogos
// --------------------------------------------------------------------------

static int dialog(const char *heading, const char *name, const char *body,
                  const scr_choice_t *ch, color_t tint)
{
    audio_play(SND_WARN);

    float anim = 0.0f;
    int   elegido = -1;

    for (;;) {
        ui_input_t in;
        if (!ui_frame_begin(&in)) return 2;

        anim = ui_approach(anim, elegido < 0 ? 1.0f : 0.0f, 16.0f);

        scr_ctx_t c = view_ctx();
        draw_frame(&in, &c, "Juegos", "NX Save Sync  ·  Angelpro09_Dev", false);
        scr_hints("Elige una opcion");

        int hit = scr_dialog(&c, &in, heading, name, body, ch, tint, anim);

        ui_ripples_draw();
        ui_debug_draw();
        ui_frame_end();

        // Una vez elegido, el dialogo se va antes de devolver la respuesta.
        if (elegido >= 0) {
            if (anim < 0.02f) return elegido;
            continue;
        }

        if ((in.down & HidNpadButton_A) || hit == 0) { audio_play(SND_SELECT); elegido = 0; }
        if ((in.down & HidNpadButton_X) || hit == 1) { audio_play(SND_SELECT); elegido = 1; }
        if ((in.down & HidNpadButton_B) || hit == 2) { audio_play(SND_BACK);   elegido = 2; }
    }
}

static int cb_warning(void *ud, const char *title_name, u8 code, const char *message)
{
    (void)ud;

    scr_choice_t ch[3];
    const char *heading;

    switch (code) {
    case WARN_PC_EMPTY:
        heading = "La carpeta del PC esta vacia";
        ch[0] = (scr_choice_t){ "A", "Volver a subirla", "regenera en el PC" };
        ch[1] = (scr_choice_t){ "X", "Borrar tambien aqui", "deja la consola vacia" };
        break;
    case WARN_SWITCH_EMPTY:
        heading = "La consola aparece sin partida";
        ch[0] = (scr_choice_t){ "A", "Aceptar el borrado", "lo quita del PC" };
        ch[1] = (scr_choice_t){ "X", "Recuperarla del PC", "regenera en la consola" };
        break;
    case WARN_ROOT_CHANGED:
        heading = "La carpeta del PC cambio de sitio";
        ch[0] = (scr_choice_t){ "A", "Quedarme con la Switch", "lo de la consola" };
        ch[1] = (scr_choice_t){ "X", "Quedarme con el PC", "lo del emulador" };
        break;
    default:
        heading = "Aviso del PC";
        ch[0] = (scr_choice_t){ "A", "Manda la Switch", "" };
        ch[1] = (scr_choice_t){ "X", "Manda el PC", "" };
        break;
    }
    ch[2] = (scr_choice_t){ "B", "No tocar nada", "decidir mas tarde" };

    int r = dialog(heading, title_name, message, ch, COL_WARN);
    return r == 0 ? DEC_SWITCH : r == 1 ? DEC_PC : DEC_SKIP;
}

// El PC trae cambios. Aunque la comparacion diga que manda el PC, bajarlos
// sobreescribe lo que hay en la consola, asi que se confirma.
static int cb_incoming(void *ud, const char *title_name, size_t files, size_t deletes)
{
    (void)ud;

    char body[320];
    if (deletes && files)
        snprintf(body, sizeof(body),
                 "El PC tiene %zu archivo(s) mas nuevos y %zu borrado(s) desde la "
                 "ultima vez. Elige con que version se queda la consola.",
                 files, deletes);
    else if (deletes)
        snprintf(body, sizeof(body),
                 "En el PC se han borrado %zu archivo(s) desde la ultima vez. "
                 "Elige con que version se queda la consola.", deletes);
    else
        snprintf(body, sizeof(body),
                 "El PC tiene %zu archivo(s) mas nuevos que la consola. "
                 "Elige con que version te quedas.", files);

    scr_choice_t ch[3] = {
        { "A", "Los del PC",       "baja lo del emulador" },
        { "X", "Los de la Switch", "sube lo de la consola" },
        { "B", "No tocar nada",    "decidir mas tarde" },
    };

    int r = dialog("El PC tiene cambios", title_name, body, ch, g_accent);
    return r == 0 ? DEC_PC : r == 1 ? DEC_SWITCH : DEC_SKIP;
}

static int cb_conflict(void *ud, const char *title_name, size_t n_conflicts)
{
    (void)ud;

    char body[256];
    snprintf(body, sizeof(body),
             "%zu archivo(s) cambiaron en los dos lados. Se queda una version entera: "
             "mezclarlas corrompe la partida en la mayoria de juegos.", n_conflicts);

    scr_choice_t ch[3] = {
        { "A", "Quedarme con la Switch", "lo de la consola" },
        { "X", "Quedarme con el PC",     "lo del emulador" },
        { "B", "Saltar este juego",      "no tocar nada" },
    };

    int r = dialog("Conflicto", title_name, body, ch, g_accent);
    return r == 0 ? WINNER_SWITCH : r == 1 ? WINNER_PC : -1;
}

// --------------------------------------------------------------------------
// red
// --------------------------------------------------------------------------

static bool connect_now(net_t *n, bool quiet)
{
    const host_t *h = settings_host();
    if (!h) {
        if (!quiet) toast("No hay ningun PC configurado");
        return false;
    }

    if (!net_connect(n, h->ip, h->port)) {
        if (!quiet) toast("No se pudo conectar a %s", h->ip);
        return false;
    }

    sync_ui_t ui = { .log = quiet ? NULL : cb_log };
    if (!sync_hello(n, &ui, g_server_name, sizeof(g_server_name),
                    g_server_emu, sizeof(g_server_emu))) {
        net_close(n);
        return false;
    }
    return true;
}

// Deja en la SD como estan las cosas, para que el overlay pueda avisar sobre el
// menu HOME sin hablar con nadie. Lo escribe tambien el sysmodule tras cada
// pasada, asi que el aviso sigue al dia aunque no abras la app.
static void publica_estado(void)
{
    estado_t e;
    memset(&e, 0, sizeof(e));

    for (size_t i = 0; i < g_games.n; i++) {
        game_t *g = &g_games.v[i];
        if (settings_excluded(g->application_id)) continue;
        if (g->state != SUM_PC_CHANGED) continue;

        e.pendientes++;
        if (e.n < ESTADO_MAX_JUEGOS) {
            snprintf(e.nombre[e.n], sizeof(e.nombre[0]), "%s", g->name);
            e.estado[e.n] = g->state;
            e.n++;
        }
    }

    estado_write(&e);
}

static void refresh_states(void)
{
    if (g_games.n == 0) return;

    net_t n;
    if (!connect_now(&n, true)) return;

    sync_emus(&n, g_emus, SYNC_MAX_EMUS, &g_emus_n);

    u64 *ids = malloc(g_games.n * sizeof(u64));
    u8  *st  = malloc(g_games.n);
    u8  *em  = malloc(g_games.n);
    u8  *ic  = malloc(g_games.n);
    if (ids && st && em && ic) {
        for (size_t i = 0; i < g_games.n; i++) ids[i] = g_games.v[i].application_id;
        if (sync_summary(&n, current_uid(), ids, g_games.n, st, em, ic)) {
            for (size_t i = 0; i < g_games.n; i++) {
                g_games.v[i].state = st[i];
                g_games.v[i].emu   = em[i];
            }

            // Las caratulas que al PC le falten. Solo una vez por juego: son
            // unos 100 KB cada una y no cambian nunca.
            for (size_t i = 0; i < g_games.n; i++) {
                if (ic[i] || !g_games.v[i].icon) continue;
                sync_send_icon(&n, g_games.v[i].application_id, g_games.v[i].name,
                               g_games.v[i].icon, g_games.v[i].icon_size);
                if (net_is_dead(&n)) break;
            }
        }
    }
    free(ids);
    free(st);
    free(em);
    free(ic);
    net_close(&n);

    publica_estado();
}

// Espera a que el usuario cierre la hoja de sincronizacion.
static void wait_dismiss(void)
{
    bool salir = false;

    for (;;) {
        ui_input_t in;
        if (!ui_frame_begin(&in)) break;

        scr_ctx_t c = view_ctx();
        draw_frame(&in, &c, "Juegos", "NX Save Sync  ·  Angelpro09_Dev", false);
        scr_hints("A   volver");
        g_sheet_in = ui_approach(g_sheet_in, salir ? 0.0f : 1.0f, 15.0f);
        scr_sync(&c, "Sincronizacion terminada", g_progress_title,
                 (const char (*)[160])g_log, g_log_n, 1.0f, true, g_sheet_in);

        ui_ripples_draw();
        ui_debug_draw();
        ui_frame_end();

        // Al aceptar, la hoja se va antes de devolver el control: cerrarla de
        // golpe cortaba la animacion a medias.
        if ((in.down & (HidNpadButton_A | HidNpadButton_B)) || in.tap) salir = true;
        if (salir && g_sheet_in < 0.02f) break;
    }
}

static void run_sync(size_t from, size_t count)
{
    if (!settings_profile_shared(current_uid())) {
        audio_play(SND_ERROR);
        toast("Este perfil esta marcado como no compartido");
        return;
    }

    audio_play(SND_START);

    g_log_n = 0;
    g_progress_title[0] = '\0';
    g_progress_done = g_progress_total = 0;
    g_sync_finished = false;
    g_sheet_in = 0.0f;

    net_t n;
    if (!connect_now(&n, false)) {
        log_push("No se pudo conectar con el PC.");
        log_push("Comprueba que nxsavesyncd.py esta corriendo y que estais en la misma red.");
        g_sync_finished = true;
        audio_play(SND_ERROR);
        wait_dismiss();
        return;
    }

    int pulled = 0, pushed = 0, deleted = 0, failed = 0, skipped = 0, excluded = 0, avisos = 0;

    for (size_t i = from; i < from + count && i < g_games.n; i++) {
        game_t *g = &g_games.v[i];

        if (settings_excluded(g->application_id)) { excluded++; continue; }

        log_push(g->name);

        sync_stats_t st;
        bool ok = sync_title(&n, current_uid(), current_user_name(),
                             g->application_id, g->name,
                             g_set.mode, settings_policy_for(g->application_id),
                             &(sync_ui_t){ .log = cb_log, .ask_conflict = cb_conflict,
                                           .ask_warning = cb_warning,
                                           // En automatico no se pregunta nada.
                                           .ask_incoming = (g_set.ask_incoming &&
                                                            g_set.mode == MODE_MANUAL)
                                                         ? cb_incoming : NULL,
                                           .progress = cb_progress },
                             &st);
        if (!ok) {
            failed++;
            if (net_is_dead(&n)) { log_push("Se perdio la conexion."); break; }
            continue;
        }

        if (st.skipped) skipped++;
        if (st.warning) avisos++;
        pulled  += st.pulled;
        pushed  += st.pushed;
        deleted += st.del_local + st.del_remote;
    }

    net_close(&n);

    char linea[160];
    log_push("");
    snprintf(linea, sizeof(linea), "Bajados %d   Subidos %d   Borrados %d",
             pulled, pushed, deleted);
    log_push(linea);
    if (skipped)  { snprintf(linea, sizeof(linea), "Saltados: %d", skipped); log_push(linea); }
    if (excluded) { snprintf(linea, sizeof(linea), "Excluidos: %d", excluded); log_push(linea); }
    if (avisos)   { snprintf(linea, sizeof(linea), "Con aviso del PC: %d", avisos); log_push(linea); }
    if (failed)   { snprintf(linea, sizeof(linea), "Con fallos: %d", failed); log_push(linea); }

    g_sync_finished = true;
    g_progress_done = g_progress_total = 1;
    audio_play(failed ? SND_ERROR : SND_DONE);

    wait_dismiss();

    refresh_states();
    toast("Bajados %d, subidos %d", pulled, pushed);
}

// --------------------------------------------------------------------------
// carga
// --------------------------------------------------------------------------

static void draw_busy(const char *what)
{
    ui_input_t in;
    if (!ui_frame_begin(&in)) return;

    scr_ctx_t c = view_ctx();
    draw_frame(&in, &c, SCR_NAV_NAMES[g_nav], "NX Save Sync  ·  Angelpro09_Dev", false);

    int cx = CONTENT_X + CONTENT_W / 2;
    ui_glow(cx, UI_H / 2 - 20, 140, 140, g_accent, 90);
    ui_ring(cx, UI_H / 2 - 20, 38, 5, -1.0f, g_accent, ui_alpha(COL_TEXT, 40));
    ui_text_clip_center(cx, UI_H / 2 + 40, 18, CONTENT_W - 80,
                        ui_alpha(COL_TEXT, 215), what);

    ui_debug_draw();
    ui_frame_end();
}

static void reload_games(void)
{
    for (int i = 0; i < 6; i++) draw_busy("Leyendo las partidas guardadas...");

    games_free(&g_games);
    ui_images_clear();          // las texturas apuntaban a los iconos liberados
    games_list(&g_games, current_uid(), true);

    g_game_sel = g_grid_top = 0;
    refresh_states();
}

// --------------------------------------------------------------------------
// vistas
// --------------------------------------------------------------------------

static void input_games(const ui_input_t *in)
{
    if (g_games.n == 0) return;

    size_t before = g_game_sel;

    if (in->down & HidNpadButton_Right) g_game_sel = (g_game_sel + 1) % g_games.n;
    if (in->down & HidNpadButton_Left)  g_game_sel = g_game_sel ? g_game_sel - 1 : g_games.n - 1;
    if (in->down & HidNpadButton_Down)
        g_game_sel = g_game_sel + GRID_COLS < g_games.n ? g_game_sel + GRID_COLS : g_games.n - 1;
    if (in->down & HidNpadButton_Up)
        g_game_sel = g_game_sel >= GRID_COLS ? g_game_sel - GRID_COLS : 0;

    if (g_game_sel != before) audio_play(SND_MOVE);

    if (g_game_sel < g_grid_top) g_grid_top = (g_game_sel / GRID_COLS) * GRID_COLS;
    if (g_game_sel >= g_grid_top + GRID_PAGE)
        g_grid_top = ((g_game_sel / GRID_COLS) - (GRID_ROWS - 1)) * GRID_COLS;

    if (in->down & HidNpadButton_A) { audio_play(SND_SELECT); run_sync(g_game_sel, 1); }
    if (in->down & HidNpadButton_Y) { audio_play(SND_SELECT); run_sync(0, g_games.n); }
    if (in->down & HidNpadButton_X) { audio_play(SND_SELECT); modal_open(MODAL_GAME); }
}

static void input_users(const ui_input_t *in)
{
    if (g_users.n == 0) return;

    int before = g_row_sel;
    if (in->down & HidNpadButton_Down) g_row_sel = (g_row_sel + 1) % (int)g_users.n;
    if (in->down & HidNpadButton_Up)   g_row_sel = g_row_sel ? g_row_sel - 1 : (int)g_users.n - 1;
    if (g_row_sel != before) audio_play(SND_MOVE);

    if (in->down & HidNpadButton_X) {
        AccountUid uid = g_users.v[g_row_sel].uid;
        bool now = !settings_profile_shared(uid);
        settings_set_profile(uid, now);
        settings_save();
        audio_play(SND_TOGGLE);
        toast(now ? "Perfil compartido" : "Perfil fuera de la sincronizacion");
    }

    if (in->down & HidNpadButton_Y) { audio_play(SND_SELECT); clona_perfil(); return; }

    if (in->down & HidNpadButton_A) {
        if ((size_t)g_row_sel != g_user_sel) {
            g_user_sel = (size_t)g_row_sel;
            audio_play(SND_SELECT);
            reload_games();
            toast("Perfil: %s", current_user_name());
            g_nav = NAV_GAMES;
            g_view_in = 0.0f;
        }
    }
}

// --------------------------------------------------------------------------
// clonar el perfil en el PC
// --------------------------------------------------------------------------
//
// Deja el perfil de la consola dentro del emulador: nombre y foto. Las partidas
// van despues por la via de siempre, juego a juego, que ya sabe hacer copias de
// seguridad y resolver conflictos. Meterlo todo en una sola operacion habria
// significado reimplementar eso, y peor.

// Elige a que emulador. Devuelve el indice, 0xFF para todos, o -1 si se sale.
static int elige_emulador(void)
{
    if (g_emus_n == 0) return -1;
    if (g_emus_n == 1) return 0;

    const char *op[SYNC_MAX_EMUS + 1];
    const char *det[SYNC_MAX_EMUS + 1];
    int n = 0;

    op[n] = "Todos los emuladores";
    det[n] = "el mismo perfil en todos los activos";
    n++;

    for (size_t i = 0; i < g_emus_n && n < SYNC_MAX_EMUS + 1; i++) {
        op[n]  = g_emus[i].name;
        det[n] = g_emus[i].path;
        n++;
    }

    int sel = 0;
    float anim = 0.0f;
    int elegido = -2;

    for (;;) {
        ui_input_t in;
        if (!ui_frame_begin(&in)) return -1;

        anim = ui_approach(anim, elegido == -2 ? 1.0f : 0.0f, 16.0f);

        scr_ctx_t c = view_ctx();
        draw_frame(&in, &c, "Perfiles", subtitle_for(), false);
        scr_hints("A elegir   arriba/abajo mover   B cancelar");

        int hit = scr_pick(&c, &in, "Clonar el perfil", "En que emulador quieres "
                           "el perfil de la consola", op, det, n, sel, anim);

        ui_ripples_draw();
        ui_debug_draw();
        ui_frame_end();

        if (elegido != -2) {
            if (anim < 0.02f) return elegido == -1 ? -1 : (elegido == 0 ? 0xFF : elegido - 1);
            continue;
        }

        if (hit >= 0 && hit != sel) { sel = hit; audio_play(SND_MOVE); continue; }

        int antes = sel;
        if (in.down & HidNpadButton_Down) sel = (sel + 1) % n;
        if (in.down & HidNpadButton_Up)   sel = sel ? sel - 1 : n - 1;
        if (sel != antes) audio_play(SND_MOVE);

        if ((in.down & HidNpadButton_A) || hit == sel) { audio_play(SND_SELECT); elegido = sel; }
        if (in.down & HidNpadButton_B) { audio_play(SND_BACK); elegido = -1; }
    }
}

static void clona_perfil(void)
{
    if (g_users.n == 0) return;

    if (g_emus_n == 0) {
        audio_play(SND_ERROR);
        toast("El PC no ha dicho que emuladores tiene");
        return;
    }

    int cual = elige_emulador();
    if (cual < 0) return;

    user_t *u = &g_users.v[g_row_sel];

    for (int i = 0; i < 4; i++) draw_busy("Clonando el perfil en el PC...");

    net_t n;
    if (!connect_now(&n, false)) { audio_play(SND_ERROR); toast("Sin conexion con el PC"); return; }

    char msg[256] = "";
    bool ok = sync_profile(&n, u->uid, u->name, u->icon, u->icon_size,
                           (u8)cual, msg, sizeof(msg));
    net_close(&n);

    audio_play(ok ? SND_DONE : SND_ERROR);
    toast("%s", msg[0] ? msg : (ok ? "Perfil clonado" : "No se pudo clonar"));

    // Y ahora las partidas, que es la otra mitad de "clonar el perfil".
    if (ok && (size_t)g_row_sel == g_user_sel && g_games.n)
        run_sync(0, g_games.n);
}

static void do_discovery(bool announce)
{
    for (int i = 0; i < 6; i++) draw_busy("Buscando PCs en la red...");

    discovery_scan(&g_found, 900);
    for (size_t i = 0; i < g_found.n; i++) settings_add_host(&g_found.v[i], false);

    // Si solo hay uno se usa y no se pregunta: molestar solo al tener que elegir.
    if (g_found.n == 1) {
        settings_add_host(&g_found.v[0], true);
        if (announce) { audio_play(SND_SELECT); toast("PC encontrado: %s", g_found.v[0].name); }
    } else if (g_found.n > 1) {
        if (announce) {
            audio_play(SND_WARN);
            toast("%zu PCs encontrados, elige uno", g_found.n);
            g_nav = NAV_HOSTS;
        }
    } else if (announce) {
        audio_play(SND_ERROR);
        toast("No se encontro ningun PC");
    }
    settings_save();
}

static void add_host_manual(void)
{
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return;

    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetGuideText(&kbd, "IP del PC, por ejemplo 192.168.1.50");
    swkbdConfigSetStringLenMax(&kbd, 39);

    const host_t *cur = settings_host();
    if (cur) swkbdConfigSetInitialText(&kbd, cur->ip);

    char out[48];
    if (R_SUCCEEDED(swkbdShow(&kbd, out, sizeof(out))) && out[0]) {
        host_t h;
        memset(&h, 0, sizeof(h));
        snprintf(h.ip, sizeof(h.ip), "%s", out);
        snprintf(h.name, sizeof(h.name), "%s", out);
        h.port = PROTO_DEFAULT_PORT;
        settings_add_host(&h, true);
        settings_save();
        audio_play(SND_SELECT);
        toast("PC anadido: %s", out);
    }
    swkbdClose(&kbd);
}

static void input_hosts(const ui_input_t *in)
{
    if (g_set.host_count > 0) {
        int before = g_row_sel;
        if (in->down & HidNpadButton_Down) g_row_sel = (g_row_sel + 1) % (int)g_set.host_count;
        if (in->down & HidNpadButton_Up)   g_row_sel = g_row_sel ? g_row_sel - 1
                                                                : (int)g_set.host_count - 1;
        if (g_row_sel != before) audio_play(SND_MOVE);

        if (in->down & HidNpadButton_A) {
            g_set.host_sel = (size_t)g_row_sel;
            settings_save();
            audio_play(SND_SELECT);
            g_server_emu[0] = '\0';
            g_server_name[0] = '\0';
            g_emus_n = 0;
            refresh_states();
            toast("PC en uso: %s", g_set.hosts[g_set.host_sel].name);
        }
    }

    if (in->down & HidNpadButton_Y) do_discovery(true);
    if (in->down & HidNpadButton_X) add_host_manual();
}

// --------------------------------------------------------------------------
// ajustes
// --------------------------------------------------------------------------

#define SET_ROWS    14
#define SET_VISIBLE 6

static const char *SET_LABELS[SET_ROWS] = {
    "Modo de sincronizacion",
    "Ante un conflicto",
    "Preguntar si el PC trae cambios",
    "Sincronizar todo al abrir",
    "Buscar PCs al arrancar",
    "Sonidos",
    "Musica de fondo",
    "Aviso en el menu HOME",
    "Segundo plano (sysmodule)",
    "Ante un conflicto en segundo plano",
    "Sincronizar cuando el PC avise",
    "Cada cuanto revisa en segundo plano",
    "Ajustes del PC",
    "Sincronizar todos los juegos ahora",
};

static const char *SET_HELPS[SET_ROWS] = {
    "En automatico no pregunta: aplica la regla de abajo",
    "Que hacer cuando los dos lados han cambiado",
    "Confirmar antes de bajar cambios del emulador",
    "Se pone al dia sin que toques nada",
    "Descubrimiento por la red local",
    "Efectos de la interfaz",
    "Ambiente sintetizado, a juego con la interfaz",
    "Que juegos esperan algo, sin abrir nada (necesita el overlay)",
    "Sincroniza sin abrir la app, al cerrar cada juego",
    "Sin nadie delante no se puede preguntar",
    "Al terminar de jugar en el emulador, sin esperar al repaso",
    "Ademas del repaso al cerrar un juego y de los avisos",
    "Emuladores, replicado y vigilancia del daemon",
    "",
};

static int build_settings(scr_row_t *r, char (*vals)[96])
{
    snprintf(vals[0],  96, "%s", mode_label());
    snprintf(vals[1],  96, "%s", policy_label(g_set.policy));
    snprintf(vals[2],  96, "%s", g_set.ask_incoming  ? "Si" : "No");
    snprintf(vals[3],  96, "%s", g_set.sync_on_open  ? "Si" : "No");
    snprintf(vals[4],  96, "%s", g_set.auto_discover ? "Si" : "No");
    snprintf(vals[5],  96, "%s", audio_enabled()     ? "Si" : "No");
    snprintf(vals[6],  96, "%s", audio_music_enabled() ? "Si" : "No");
    snprintf(vals[7],  96, "%s", g_set.aviso ? "Si" : "No");
    snprintf(vals[8],  96, "%s", g_set.bg_enabled ? "Activado" : "Desactivado");
    snprintf(vals[9],  96, "%s", policy_label(g_set.bg_policy));
    snprintf(vals[10], 96, "%s", g_set.bg_nudge ? "Si" : "No");
    snprintf(vals[11], 96, "%u min", g_set.bg_interval / 60);
    snprintf(vals[12], 96, "%s", g_server_name[0] ? g_server_name : "conectar");
    snprintf(vals[13], 96, "%zu juegos", g_games.n);

    for (int i = 0; i < SET_ROWS; i++) {
        r[i].label  = SET_LABELS[i];
        r[i].help   = SET_HELPS[i];
        r[i].value  = vals[i];
        r[i].vcolor = (i == 8 && g_set.bg_enabled) ? COL_OK : g_accent;
    }
    return SET_ROWS;
}

static void input_settings(const ui_input_t *in)
{
    int before = g_row_sel;
    if (in->down & HidNpadButton_Down) g_row_sel = (g_row_sel + 1) % SET_ROWS;
    if (in->down & HidNpadButton_Up)   g_row_sel = g_row_sel ? g_row_sel - 1 : SET_ROWS - 1;
    if (g_row_sel != before) audio_play(SND_MOVE);

    if (g_row_sel < g_set_top)                g_set_top = g_row_sel;
    if (g_row_sel >= g_set_top + SET_VISIBLE) g_set_top = g_row_sel - SET_VISIBLE + 1;

    bool change = (in->down & (HidNpadButton_A | HidNpadButton_Right | HidNpadButton_Left)) != 0;
    if (!change) return;

    switch (g_row_sel) {
    case 0: g_set.mode = g_set.mode == MODE_AUTO ? MODE_MANUAL : MODE_AUTO; break;
    case 1: g_set.policy = policy_next(g_set.policy, false); break;
    case 2: g_set.ask_incoming  = !g_set.ask_incoming;  break;
    case 3: g_set.sync_on_open  = !g_set.sync_on_open;  break;
    case 4: g_set.auto_discover = !g_set.auto_discover; break;
    case 5:
        audio_set_enabled(!audio_enabled());
        g_set.sounds = audio_enabled();
        break;
    case 6:
        audio_set_music(!audio_music_enabled());
        g_set.music = audio_music_enabled();
        break;
    case 7:
        g_set.aviso = !g_set.aviso;
        toast(g_set.aviso
              ? "Aviso en el menu HOME activado (lo dibuja el overlay)"
              : "Aviso en el menu HOME desactivado");
        break;
    case 8:
        g_set.bg_enabled = !g_set.bg_enabled;
        toast(g_set.bg_enabled
              ? "Segundo plano activado (necesita el sysmodule instalado)"
              : "Segundo plano desactivado");
        break;
    case 9:
        // Sin nadie delante, "preguntar" no es una opcion: se salta el juego.
        g_set.bg_policy = policy_next(g_set.bg_policy, false);
        break;
    case 10: g_set.bg_nudge = !g_set.bg_nudge; break;
    case 11: {
        int min = g_set.bg_interval / 60;
        min = (in->down & HidNpadButton_Left) ? min - 5 : min + 5;
        if (min < 1)  min = 60;
        if (min > 60) min = 1;
        g_set.bg_interval = (u16)(min * 60);
        break;
    }
    case 12: audio_play(SND_SELECT); modal_open(MODAL_PCCFG); pccfg_fetch(); return;
    case 13:
        audio_play(SND_SELECT);
        if (g_games.n) run_sync(0, g_games.n);
        return;
    }

    audio_play(SND_TOGGLE);
    settings_save();
}

// --------------------------------------------------------------------------
// hoja: opciones de un juego
// --------------------------------------------------------------------------

static void modal_game(const ui_input_t *in, const scr_ctx_t *c)
{
    game_t *g = &g_games.v[g_game_sel];
    bool ex = settings_excluded(g->application_id);

    game_rule_t *r = NULL;
    for (size_t i = 0; i < g_set.game_count; i++)
        if (g_set.games[i].title_id == g->application_id) r = &g_set.games[i];

    int hit = scr_game_opts(c, in, &g_vgames[g_game_sel],
                            policy_label(r ? r->policy : POLICY_ASK), ex, g_row_sel,
                            g_modal_in);

    if (g_modal_closing) return;      // mientras se va no acepta ordenes

    if (hit >= 0 && hit != g_row_sel) { g_row_sel = hit; audio_play(SND_MOVE); }

    if (in->down & (HidNpadButton_Down | HidNpadButton_Up)) {
        g_row_sel = g_row_sel ? 0 : 1;
        audio_play(SND_MOVE);
    }

    bool act = (in->down & (HidNpadButton_A | HidNpadButton_Right | HidNpadButton_Left)) != 0
               || (hit >= 0 && hit == g_row_sel);

    if (act) {
        u8 pol = r ? r->policy : POLICY_ASK;
        u8 exc = r ? r->excluded : 0;

        if (g_row_sel == 0) pol = policy_next(pol, true);
        else                exc = !exc;

        settings_set_game(g->application_id, pol, exc);
        settings_save();
        audio_play(SND_TOGGLE);
    }

    if (in->down & (HidNpadButton_B | HidNpadButton_X)) modal_close();
}

// --------------------------------------------------------------------------
// hoja: ajustes del PC
// --------------------------------------------------------------------------

static cfg_item_t g_cfg[CFG_MAX_ITEMS];
static size_t     g_cfg_n;
static int        g_cfg_sel;
static bool       g_cfg_ok;

static void pccfg_fetch(void)
{
    g_cfg_n = 0;
    g_cfg_ok = false;

    for (int i = 0; i < 4; i++) draw_busy("Pidiendo los ajustes al PC...");

    net_t n;
    if (!connect_now(&n, true)) { audio_play(SND_ERROR); toast("Sin conexion con el PC"); return; }

    g_cfg_ok = sync_cfg_get(&n, g_cfg, CFG_MAX_ITEMS, &g_cfg_n);
    net_close(&n);

    if (!g_cfg_ok) { audio_play(SND_ERROR); toast("El PC no devolvio los ajustes"); }
    if (g_cfg_sel >= (int)g_cfg_n) g_cfg_sel = 0;
}

// Cambiar un ajuste puede cambiar la lista entera (activar un emulador anade su
// perfil), asi que se relee despues de cada cambio.
static void pccfg_send(const char *key, const char *value)
{
    net_t n;
    if (!connect_now(&n, true)) { audio_play(SND_ERROR); toast("Sin conexion con el PC"); return; }

    char msg[128] = "";
    bool ok = sync_cfg_set(&n, key, value, msg, sizeof(msg));
    net_close(&n);

    audio_play(ok ? SND_TOGGLE : SND_ERROR);
    toast("%s", msg[0] ? msg : (ok ? "Guardado" : "No se pudo cambiar"));
    pccfg_fetch();
}

static const char *cfg_display(const cfg_item_t *it)
{
    switch (it->type) {
    case CFG_BOOL:   return it->value[0] == '1' ? "Si" : "No";
    case CFG_ACTION: return "Pulsa A";
    case CFG_CHOICE: {
        int idx = atoi(it->value);
        if (idx >= 0 && idx < (int)it->n_options) return it->options[idx];
        return it->value;
    }
    default: return it->value;
    }
}

static void modal_pccfg(const ui_input_t *in, const scr_ctx_t *c)
{
    scr_row_t rows[CFG_MAX_ITEMS];
    for (size_t i = 0; i < g_cfg_n; i++) {
        rows[i].label  = g_cfg[i].label;
        rows[i].help   = g_cfg[i].help;
        rows[i].value  = cfg_display(&g_cfg[i]);
        rows[i].vcolor = g_cfg[i].type == CFG_INFO ? ui_alpha(COL_DIM, 220) : g_accent;
    }

    int hit = scr_pc_cfg(c, in, g_server_name, rows, (int)g_cfg_n, g_cfg_sel, g_cfg_ok,
                         g_modal_in);

    if (g_modal_closing) return;

    if (!g_cfg_ok || g_cfg_n == 0) {
        if (in->down & HidNpadButton_Y) pccfg_fetch();
        if (in->down & HidNpadButton_B) modal_close();
        return;
    }

    if (hit >= 0 && hit != g_cfg_sel) { g_cfg_sel = hit; audio_play(SND_MOVE); }

    int before = g_cfg_sel;
    if (in->down & HidNpadButton_Down) g_cfg_sel = (g_cfg_sel + 1) % (int)g_cfg_n;
    if (in->down & HidNpadButton_Up)   g_cfg_sel = g_cfg_sel ? g_cfg_sel - 1 : (int)g_cfg_n - 1;
    if (g_cfg_sel != before) audio_play(SND_MOVE);

    if (in->down & HidNpadButton_B) { modal_close(); return; }
    if (in->down & HidNpadButton_Y) { pccfg_fetch(); return; }

    bool fwd  = (in->down & (HidNpadButton_A | HidNpadButton_Right)) != 0
                || (hit >= 0 && hit == before);
    bool back = (in->down & HidNpadButton_Left) != 0;
    if (!fwd && !back) return;

    cfg_item_t *it = &g_cfg[g_cfg_sel];
    char value[64];

    switch (it->type) {
    case CFG_BOOL:
        snprintf(value, sizeof(value), "%s", it->value[0] == '1' ? "0" : "1");
        break;
    case CFG_CHOICE: {
        if (it->n_options == 0) return;
        int idx = atoi(it->value);
        idx = fwd ? (idx + 1) % (int)it->n_options
                  : (idx ? idx - 1 : (int)it->n_options - 1);
        snprintf(value, sizeof(value), "%d", idx);
        break;
    }
    case CFG_INT: {
        int v = atoi(it->value) + (fwd ? 1 : -1);
        if (v < 1) v = 1;
        snprintf(value, sizeof(value), "%d", v);
        break;
    }
    case CFG_ACTION:
        if (!fwd) return;
        snprintf(value, sizeof(value), "1");
        break;
    default:
        return;   // CFG_INFO no se toca
    }

    pccfg_send(it->key, value);
}

// --------------------------------------------------------------------------

static const char *hints_for(void)
{
    if (g_modal == MODAL_GAME)  return "A cambiar   arriba/abajo elegir   B volver";
    if (g_modal == MODAL_PCCFG) return "A cambiar   Y refrescar   B volver";

    switch (g_nav) {
    case NAV_GAMES: return "A sincronizar   Y todos   X opciones   ZL/ZR perfil   "
                           "L/R menu   + salir";
    case NAV_USERS: return "A usar este perfil   Y clonar en el PC   X compartir o no   "
                           "L/R menu   + salir";
    case NAV_HOSTS: return "A usar este PC   Y buscar en la red   X anadir por IP   "
                           "L/R menu   + salir";
    default:        return "A cambiar   L/R menu   + salir";
    }
}

static const char *subtitle_for(void)
{
    switch (g_nav) {
    case NAV_USERS: return "Cada perfil se sincroniza por separado. Con Y se clona entero "
                           "en el emulador: nombre, foto y partidas.";
    case NAV_HOSTS: return "Se buscan solos por la red. Tambien puedes anadir uno por IP.";
    case NAV_SETTINGS: return "Los de la consola, y tambien los del PC.";
    default:        return "NX Save Sync  ·  Angelpro09_Dev";
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    socketInitializeDefault();
    accountInitialize(AccountServiceType_Application);
    nsInitialize();

    if (!ui_init()) { ui_exit(); return 1; }

    g_accent = COL_ACCENT;
    settings_load();

    audio_init();
    audio_set_enabled(g_set.sounds != 0);
    audio_set_music(g_set.music != 0);
    audio_music_init();

    if (!users_list(&g_users)) {
        for (int i = 0; i < 200; i++) draw_busy("No hay perfiles en la consola.");
        goto done;
    }

    build_users();

    if (g_set.auto_discover) do_discovery(false);
    if (g_users.n > 1) g_nav = NAV_USERS;
    else if (g_set.host_count > 1) g_nav = NAV_HOSTS;

    reload_games();

    // Lo que hizo el sysmodule mientras la app estaba cerrada.
    {
        notify_info_t info;
        if (notify_read(&info, true) && info.valid) {
            int movidos = info.pulled + info.pushed + info.deleted;
            if (info.pending) {
                audio_play(SND_WARN);
                toast("Segundo plano: %d juego(s) esperan tu decision", info.pending);
            } else if (info.kind == NOTIFY_FAIL) {
                audio_play(SND_ERROR);
                toast("Segundo plano: la ultima sincronizacion fallo");
            } else if (movidos > 0) {
                audio_play(SND_DONE);
                toast("Segundo plano: %d archivo(s) sincronizados", movidos);
            }
        }
    }

    if (g_set.sync_on_open && g_games.n > 0 && settings_host() && g_nav == NAV_GAMES)
        run_sync(0, g_games.n);

    for (;;) {
        ui_input_t in;
        if (!ui_frame_begin(&in)) break;

        update_accent();
        audio_music_poll();

        g_view_in = ui_approach(g_view_in, 1.0f, 12.0f);

        // La hoja se va sola cuando se ha pedido cerrarla, y solo entonces deja
        // de existir. Cerrar en el acto cortaba la animacion por la mitad.
        g_modal_in = ui_approach(g_modal_in,
                                 (g_modal != MODAL_NONE && !g_modal_closing) ? 1.0f : 0.0f,
                                 16.0f);
        if (g_modal_closing && g_modal_in < 0.02f) {
            g_modal = MODAL_NONE;
            g_modal_closing = false;
            g_row_sel = 0;
        }

        int n_games = build_games();
        int n_users = build_users();
        int n_hosts = build_hosts();

        scr_ctx_t c = view_ctx();
        int nav_hit = draw_frame(&in, &c, SCR_NAV_NAMES[g_nav], subtitle_for(),
                                 g_modal == MODAL_NONE);

        // --- la vista ---
        //
        // Al cambiar de seccion entra fundiendose y subiendo un poco. Va a una
        // capa aparte porque asi la opacidad se aplica una vez al conjunto, en
        // vez de tener que dar alfa a cada panel, icono y texto por separado.
        int tapped = -1;
        bool retap = false, hit_sync = false, hit_opts = false;

        bool entrando = g_view_in < 0.995f;
        if (entrando) ui_layer_begin();

        switch (g_nav) {
        case NAV_GAMES:
            if (n_games == 0) scr_games_empty(&c);
            else tapped = scr_games(&c, &in, g_vgames, n_games,
                                    (int)g_game_sel, (int)g_grid_top,
                                    policy_label(settings_policy_for(
                                        g_games.v[g_game_sel].application_id)),
                                    &retap, &hit_sync, &hit_opts);
            break;
        case NAV_USERS:
            tapped = scr_users(&c, &in, g_vusers, n_users, g_row_sel, (int)g_user_sel);
            break;
        case NAV_HOSTS:
            tapped = scr_hosts(&c, &in, g_vhosts, n_hosts, g_row_sel, (int)g_set.host_sel,
                               g_vemus, build_emus());
            break;
        default: {
            scr_row_t rows[SET_ROWS];
            char vals[SET_ROWS][96];
            build_settings(rows, vals);
            tapped = scr_rows(&c, &in, rows, SET_ROWS, g_row_sel, g_set_top, SET_VISIBLE);
            break;
        }
        }

        if (entrando)
            ui_layer_end(g_view_in, 0.99f + 0.01f * g_view_in,
                         CONTENT_X + CONTENT_W / 2, BODY_Y + BODY_H / 2,
                         0, (int)((1.0f - g_view_in) * 18.0f));

        scr_hints(hints_for());
        draw_toast();

        if (g_modal_in > 0.005f) {
            if (g_modal == MODAL_GAME && g_games.n > 0) modal_game(&in, &c);
            else if (g_modal == MODAL_PCCFG)            modal_pccfg(&in, &c);
        }

        ui_ripples_draw();
        ui_debug_draw();
        ui_frame_end();

        if (g_modal != MODAL_NONE) continue;

        // --- lo que se ha tocado ---
        if (nav_hit >= 0) {
            g_nav = (nav_t)nav_hit;
            g_row_sel = 0;
            g_view_in = 0.0f;
            audio_play(SND_MOVE);
            continue;
        }

        if (tapped >= 0) {
            switch (g_nav) {
            case NAV_GAMES:
                if (retap) { audio_play(SND_SELECT); run_sync((size_t)tapped, 1); }
                else       { g_game_sel = (size_t)tapped; audio_play(SND_MOVE); }
                break;
            case NAV_USERS:
            case NAV_HOSTS:
            default:
                if (tapped != g_row_sel) { g_row_sel = tapped; audio_play(SND_MOVE); }
                break;
            }
            continue;
        }

        if (hit_sync) { audio_play(SND_SELECT); run_sync(g_game_sel, 1); continue; }
        if (hit_opts) { audio_play(SND_SELECT); modal_open(MODAL_GAME); continue; }

        switch (g_nav) {
        case NAV_GAMES: input_games(&in);    break;
        case NAV_USERS: input_users(&in);    break;
        case NAV_HOSTS: input_hosts(&in);    break;
        default:        input_settings(&in); break;
        }

        if (in.down & HidNpadButton_Plus) break;

        // L/R cambian de seccion; ZL/ZR de perfil, que es lo que mas se usa.
        if (in.down & (HidNpadButton_L | HidNpadButton_R)) {
            g_nav = (in.down & HidNpadButton_R)
                  ? (nav_t)((g_nav + 1) % NAV_COUNT)
                  : (nav_t)((g_nav + NAV_COUNT - 1) % NAV_COUNT);
            g_row_sel = 0;
            g_view_in = 0.0f;
            audio_play(SND_MOVE);
        }

        if ((in.down & (HidNpadButton_ZL | HidNpadButton_ZR)) && g_users.n > 1) {
            g_user_sel = (in.down & HidNpadButton_ZR)
                       ? (g_user_sel + 1) % g_users.n
                       : (g_user_sel ? g_user_sel - 1 : g_users.n - 1);
            audio_play(SND_SELECT);
            reload_games();
            toast("Perfil: %s", current_user_name());
        }
    }

done:
    settings_save();
    games_free(&g_games);
    users_free(&g_users);
    audio_exit();
    ui_exit();

    nsExit();
    accountExit();
    socketExit();
    return 0;
}
