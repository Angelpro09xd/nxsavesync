// NX Save Sync -- sincroniza los savedata de la Switch con uno o varios PCs.
//
// Desarrollador: Angelpro09_Dev

#include <switch.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#include "proto.h"
#include "net.h"
#include "ui.h"
#include "audio.h"
#include "games.h"
#include "sync.h"
#include "settings.h"
#include "discovery.h"
#include "notify.h"

#define APP_VERSION "3.2"

// --------------------------------------------------------------------------
// medidas
// --------------------------------------------------------------------------

#define RAIL_W    96
#define TOPBAR_H  92
#define HINT_H    68
#define BODY_Y    TOPBAR_H
#define BODY_H    (UI_H - TOPBAR_H - HINT_H)

#define GRID_X    120
#define GRID_Y    (BODY_Y + 16)
#define CARD_W    160
#define CARD_H    156
#define CARD_GAP  12
#define GRID_COLS 4
#define GRID_ROWS 3

#define DETAIL_X  820
#define DETAIL_W  (UI_W - DETAIL_X - 28)

#define LOG_LINES 9

typedef enum { NAV_GAMES, NAV_USERS, NAV_HOSTS, NAV_SETTINGS, NAV_COUNT } nav_t;
typedef enum { MODAL_NONE, MODAL_GAME, MODAL_PCCFG } modal_t;

static const char *NAV_NAMES[NAV_COUNT] = { "Juegos", "Perfiles", "PCs", "Ajustes" };

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

static nav_t   g_nav   = NAV_GAMES;
static modal_t g_modal = MODAL_NONE;
static bool    g_syncing;

static int  g_row_sel;        // fila elegida en perfiles / PCs / ajustes
static char g_toast[160];
static u32  g_toast_until;

// animaciones
static float g_nav_y;             // pastilla del menu lateral
static float g_sel_x, g_sel_y;    // recuadro de seleccion en la rejilla
static float g_detail_in;         // 0..1, entrada del panel de detalle
static float g_toast_in;
static color_t g_accent;          // color sacado del icono elegido

// registro de la sincronizacion
static char   g_log[LOG_LINES][160];
static int    g_log_n;
static char   g_progress_title[128];
static size_t g_progress_done, g_progress_total;
static bool   g_sync_finished;

static void pccfg_fetch(void);

static void toast(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_toast, sizeof(g_toast), fmt, ap);
    va_end(ap);
    g_toast_until = ui_ticks() + 3000;
    g_toast_in = 0.0f;
}

static AccountUid current_uid(void)      { return g_users.v[g_user_sel].uid; }
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
    default:            return "Preguntar";
    }
}

static color_t state_color(u8 state, bool excluded)
{
    if (excluded) return COL_DIM;
    switch (state) {
    case SUM_SYNCED:     return COL_OK;
    case SUM_PC_CHANGED: return COL_WARN;
    case SUM_NO_DIR:     return COL_ERR;
    default:             return COL_DIM;
    }
}

static const char *state_text(u8 state, bool excluded)
{
    if (excluded) return "Excluido";
    switch (state) {
    case SUM_SYNCED:     return "Al dia";
    case SUM_PC_CHANGED: return "Cambios en el PC";
    case SUM_NO_DIR:     return "Sin carpeta en el PC";
    default:             return "Sin sincronizar";
    }
}

// --------------------------------------------------------------------------
// iconos del menu, dibujados a mano
// --------------------------------------------------------------------------
//
// Cuatro formas sencillas con rectangulos. Evita meter un atlas de imagenes en
// el binario solo para esto, y escalan sin verse borrosas.

static void glyph_games(int x, int y, int s, color_t c)
{
    int u = s / 2 - 2;
    ui_rect_round(x, y, u, u, 3, c);
    ui_rect_round(x + u + 4, y, u, u, 3, c);
    ui_rect_round(x, y + u + 4, u, u, 3, c);
    ui_rect_round(x + u + 4, y + u + 4, u, u, 3, c);
}

static void glyph_user(int x, int y, int s, color_t c)
{
    int head = s / 3;
    ui_rect_round(x + (s - head) / 2, y + 2, head, head, head / 2, c);
    ui_rect_round(x + 2, y + head + 6, s - 4, s - head - 8, (s - head) / 3, c);
}

static void glyph_pc(int x, int y, int s, color_t c)
{
    ui_rect_round(x, y + 2, s, s - 10, 3, c);
    ui_rect(x + s / 2 - 5, y + s - 8, 10, 4, c);
    ui_rect(x + s / 2 - 12, y + s - 4, 24, 3, c);
}

static void glyph_gear(int x, int y, int s, color_t c)
{
    // Barras tipo mezclador: se lee como "ajustes" mejor que un engranaje mal
    // dibujado a esta resolucion.
    int h = 4, gap = (s - 3 * h) / 4;
    for (int i = 0; i < 3; i++) {
        int yy = y + gap + i * (h + gap);
        ui_rect_round(x, yy, s, h, h / 2, ui_alpha(c, 140));
        int knob = (i == 1) ? s - 14 : (i == 0 ? 4 : s / 2);
        ui_rect_round(x + knob, yy - 3, 10, h + 6, 5, c);
    }
}

// --------------------------------------------------------------------------
// armazon: menu lateral, barra superior, pistas
// --------------------------------------------------------------------------

#define RAIL_TOP   146     // primer elemento
#define RAIL_STEP   88     // separacion entre elementos
#define RAIL_ITEM   76     // alto de la pastilla

static void draw_rail(const ui_input_t *in, bool interactive)
{
    ui_rect(0, 0, RAIL_W, UI_H, COL_PANEL);
    ui_rect(RAIL_W - 1, 0, 1, UI_H, ui_alpha(COL_BG, 120));

    // El logo, el mismo dibujo que el icono del homebrew.
    ui_logo(RAIL_W / 2, 46, 62, g_accent, ui_mix(g_accent, COL_BG, 0.45f), COL_TEXT);

    float target = (float)(RAIL_TOP + g_nav * RAIL_STEP);

    // Sin esto la pastilla arrancaba en y=0 y entraba volando desde arriba en
    // el primer fotograma, que se veia como un fallo.
    if (g_nav_y <= 0.0f) g_nav_y = target;
    g_nav_y = ui_approach(g_nav_y, target, 16.0f);

    int py = (int)g_nav_y - RAIL_ITEM / 2;
    ui_rect_round(10, py, RAIL_W - 20, RAIL_ITEM, 14, ui_alpha(g_accent, 38));
    ui_rect_round(0, py + 10, 4, RAIL_ITEM - 20, 2, g_accent);

    for (int i = 0; i < NAV_COUNT; i++) {
        int cy = RAIL_TOP + i * RAIL_STEP;
        bool on = i == (int)g_nav;
        color_t c = on ? g_accent : COL_DIM;

        int gs = 28, gx = RAIL_W / 2 - gs / 2, gy = cy - 22;
        switch (i) {
        case NAV_GAMES:    glyph_games(gx, gy, gs, c); break;
        case NAV_USERS:    glyph_user(gx, gy, gs, c);  break;
        case NAV_HOSTS:    glyph_pc(gx, gy, gs, c);    break;
        case NAV_SETTINGS: glyph_gear(gx, gy, gs, c);  break;
        }

        ui_text_center(RAIL_W / 2, cy + 12, 13, on ? COL_TEXT : COL_DIM,
                       "%s", NAV_NAMES[i]);

        // La zona tactil cubre el hueco entero, no solo la pastilla: antes
        // quedaban franjas muertas entre elementos.
        if (interactive && ui_hit(in, 0, cy - RAIL_STEP / 2, RAIL_W, RAIL_STEP) && !on) {
            g_nav = (nav_t)i;
            g_row_sel = 0;
            audio_play(SND_MOVE);
        }
    }

    // Estado del PC, abajo del todo.
    {
        bool have = settings_host() != NULL;
        bool live = g_server_name[0] != '\0';
        color_t c = !have ? COL_ERR : (live ? COL_OK : COL_WARN);
        int cy = UI_H - 56;

        ui_circle(RAIL_W / 2, cy, 5, c);
        ui_text_center(RAIL_W / 2, cy + 10, 12, ui_alpha(COL_DIM, 150),
                       !have ? "sin PC" : (live ? "listo" : "?"));
    }

    ui_text_center(RAIL_W / 2, UI_H - 24, 12, ui_alpha(COL_DIM, 100), "v" APP_VERSION);
}

static void draw_chip(int *x, int limit, const char *label, const char *value, color_t c)
{
    int lw = ui_text_w(14, label);
    int vw = ui_text_w(17, value);
    int w = (lw > vw ? lw : vw) + 28;

    if (*x - w - 12 < limit) return;   // no cabe: mejor omitirla que solaparla
    *x -= w + 12;
    ui_rect_round(*x, 26, w, 48, 12, COL_PANEL_HI);
    ui_text(*x + 14, 30, 14, COL_DIM, "%s", label);
    ui_text(*x + 14, 48, 17, c, "%s", value);
}

static void draw_topbar(void)
{
    ui_rect(RAIL_W, 0, UI_W - RAIL_W, TOPBAR_H, COL_BG);

    ui_text(RAIL_W + 28, 18, 30, COL_TEXT, "%s", NAV_NAMES[g_nav]);
    ui_text(RAIL_W + 28, 58, 15, ui_alpha(COL_DIM, 180), "NX Save Sync  ·  Angelpro09_Dev");

    // Las fichas se dibujan de derecha a izquierda; este es el limite para que
    // con un nombre de usuario o de PC largo no se metan sobre el titulo.
    int title_end = RAIL_W + 28 + ui_text_w(30, NAV_NAMES[g_nav]) + 24;
    int x = UI_W - 16;

    // perfil, con avatar
    if (g_users.n > 0) {
        user_t *u = &g_users.v[g_user_sel];
        int w = ui_text_w(17, u->name) + 48 + 28;
        if (w > 260) w = 260;          // nombres largos no empujan al resto
        x -= w + 12;
        ui_rect_round(x, 26, w, 48, 12, COL_PANEL_HI);
        SDL_Texture *av = ui_image(u->icon, u->icon_size);
        if (av) ui_image_draw_round(av, x + 8, 32, 36, 36, 8, COL_PANEL_HI);
        ui_text_clip(x + 52, 40, 17, w - 62, COL_TEXT, u->name);
    }

    const host_t *h = settings_host();
    draw_chip(&x, title_end, "PC", h ? (h->name[0] ? h->name : h->ip) : "sin PC",
              h ? COL_TEXT : COL_ERR);

    if (g_server_emu[0]) draw_chip(&x, title_end, "Emulador", g_server_emu, COL_TEXT);

    draw_chip(&x, title_end, "Modo", mode_label(),
              g_set.mode == MODE_AUTO ? COL_OK : COL_TEXT);
}

static void draw_hints(const char *hints)
{
    ui_rect(RAIL_W, UI_H - HINT_H, UI_W - RAIL_W, HINT_H, COL_PANEL);
    ui_text(RAIL_W + 28, UI_H - HINT_H + 24, 17, COL_DIM, "%s", hints);
}

static void draw_toast(void)
{
    bool visible = ui_ticks() < g_toast_until && g_toast[0];
    g_toast_in = ui_approach(g_toast_in, visible ? 1.0f : 0.0f, 14.0f);
    if (g_toast_in < 0.01f) return;

    int w = ui_text_w(19, g_toast) + 52;
    int x = RAIL_W + (UI_W - RAIL_W - w) / 2;
    int y = (int)(UI_H - HINT_H - 20 - 56 * g_toast_in);

    ui_rect_round(x + 3, y + 4, w, 52, 14, ui_alpha(COL_BG, 150));   // sombra
    ui_rect_round(x, y, w, 52, 14, COL_PANEL_HI);
    ui_rect_round(x, y, 4, 52, 2, g_accent);
    ui_text_center(x + w / 2, y + 15, 19, COL_TEXT, "%s", g_toast);
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

static void draw_sheet(int *out_x, int *out_y, int w, int h)
{
    int x = RAIL_W + (UI_W - RAIL_W - w) / 2;
    int y = (UI_H - h) / 2;

    ui_rect(0, 0, UI_W, UI_H, ui_alpha(COL_BG, 200));
    ui_rect_round(x, y, w, h, 18, COL_PANEL);
    ui_gradient_round_top(x, y, w, 70, 18, ui_alpha(g_accent, 60), ui_alpha(g_accent, 0));

    *out_x = x;
    *out_y = y;
}

static void draw_sync_sheet(void)
{
    int x, y, w = 940, h = 540;
    draw_sheet(&x, &y, w, h);

    ui_text(x + 40, y + 30, 26, COL_TEXT,
            g_sync_finished ? "Sincronizacion terminada" : "Sincronizando");

    // anillo de progreso
    int cx = x + w - 110, cy = y + 96;
    float prog = g_progress_total > 0
               ? (float)g_progress_done / (float)g_progress_total : -1.0f;
    if (g_sync_finished) prog = 1.0f;

    ui_ring(cx, cy, 42, 6, prog, g_accent, COL_PANEL_HI);
    if (prog >= 0.0f)
        ui_text_center(cx, cy - 11, 20, COL_TEXT, "%d%%", (int)(prog * 100));

    if (g_progress_title[0])
        ui_text_clip(x + 40, y + 70, 20, w - 240, g_accent, g_progress_title);

    ui_rect(x + 40, y + 110, w - 240, 1, ui_alpha(COL_DIM, 60));

    int ly = y + 128;
    for (int i = 0; i < g_log_n; i++) {
        // Las lineas mas viejas se van apagando, para que la vista se lea sola.
        u8 a = (u8)(110 + 145 * (i + 1) / (g_log_n > 0 ? g_log_n : 1));
        ui_text_clip(x + 40, ly, 18, w - 80, ui_alpha(COL_DIM, a), g_log[i]);
        ly += 27;
    }

    if (g_sync_finished) {
        const char *msg = "A  volver";
        int bw = ui_text_w(19, msg) + 60;
        ui_rect_round(x + (w - bw) / 2, y + h - 74, bw, 50, 12, g_accent);
        ui_text_center(x + w / 2, y + h - 61, 19, COL_BG, "%s", msg);
    } else {
        ui_spinner(x + 40 + 8, y + h - 48, 8, ui_alpha(g_accent, 200));
        ui_text(x + 70, y + h - 58, 17, COL_DIM, "No apagues la consola");
    }
}

static void sync_redraw(void)
{
    ui_input_t in;
    if (!ui_frame_begin(&in)) return;

    ui_clear(COL_BG);
    draw_rail(&in, false);
    draw_topbar();
    draw_hints("Sincronizando...");
    draw_sync_sheet();
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

// Botonera de un dialogo: tres opciones con su explicacion debajo.
typedef struct { const char *key, *title, *sub; } choice_t;

static int dialog(const char *heading, const char *name, const char *body,
                  const choice_t *ch, color_t tint)
{
    audio_play(SND_WARN);

    for (;;) {
        ui_input_t in;
        if (!ui_frame_begin(&in)) return 2;

        ui_clear(COL_BG);
        draw_rail(&in, false);
        draw_topbar();
        draw_hints("Elige una opcion");

        int x, y, w = 960, h = 400;
        draw_sheet(&x, &y, w, h);
        ui_rect_round(x, y, w, 5, 2, tint);

        ui_text_center(x + w / 2, y + 32, 27, tint, "%s", heading);
        if (name && name[0])
            ui_text_center(x + w / 2, y + 74, 20, COL_TEXT, "%s", name);

        // El cuerpo viene del PC: se parte en lineas que quepan.
        {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s", body ? body : "");
            int ly = y + 116;
            char line[200] = "";
            for (char *word = strtok(buf, " "); word; word = strtok(NULL, " ")) {
                char probe[240];
                snprintf(probe, sizeof(probe), "%s%s%s", line, line[0] ? " " : "", word);
                if (ui_text_w(18, probe) > w - 120 && line[0]) {
                    ui_text_center(x + w / 2, ly, 18, COL_DIM, "%s", line);
                    ly += 26;
                    snprintf(line, sizeof(line), "%s", word);
                } else {
                    snprintf(line, sizeof(line), "%s", probe);
                }
            }
            if (line[0]) ui_text_center(x + w / 2, ly, 18, COL_DIM, "%s", line);
        }

        int bw = 276, bh = 74, by = y + h - 106;
        int bx[3] = { x + 32, x + (w - bw) / 2, x + w - 32 - bw };

        for (int i = 0; i < 3; i++) {
            bool hot = (i == 0);
            ui_rect_round(bx[i], by, bw, bh, 14, hot ? ui_alpha(tint, 45) : COL_PANEL_HI);
            if (hot) ui_rect_round_outline(bx[i], by, bw, bh, 14, 2, tint);

            ui_text_center(bx[i] + bw / 2, by + 14, 19, COL_TEXT,
                           "%s  %s", ch[i].key, ch[i].title);
            if (ch[i].sub[0])
                ui_text_center(bx[i] + bw / 2, by + 42, 15, COL_DIM, "%s", ch[i].sub);
        }

        ui_frame_end();

        if ((in.down & HidNpadButton_A) || ui_hit(&in, bx[0], by, bw, bh)) { audio_play(SND_SELECT); return 0; }
        if ((in.down & HidNpadButton_X) || ui_hit(&in, bx[1], by, bw, bh)) { audio_play(SND_SELECT); return 1; }
        if ((in.down & HidNpadButton_B) || ui_hit(&in, bx[2], by, bw, bh)) { audio_play(SND_BACK);   return 2; }
    }
}

static int cb_warning(void *ud, const char *title_name, u8 code, const char *message)
{
    (void)ud;

    choice_t ch[3];
    const char *heading;

    switch (code) {
    case WARN_PC_EMPTY:
        heading = "La carpeta del PC esta vacia";
        ch[0] = (choice_t){ "A", "Volver a subirla", "regenera en el PC" };
        ch[1] = (choice_t){ "X", "Borrar tambien aqui", "deja la consola vacia" };
        break;
    case WARN_SWITCH_EMPTY:
        heading = "La consola aparece sin partida";
        ch[0] = (choice_t){ "A", "Aceptar el borrado", "lo quita del PC" };
        ch[1] = (choice_t){ "X", "Recuperarla del PC", "regenera en la consola" };
        break;
    case WARN_ROOT_CHANGED:
        heading = "La carpeta del PC cambio de sitio";
        ch[0] = (choice_t){ "A", "Quedarme con la Switch", "lo de la consola" };
        ch[1] = (choice_t){ "X", "Quedarme con el PC", "lo del emulador" };
        break;
    default:
        heading = "Aviso del PC";
        ch[0] = (choice_t){ "A", "Manda la Switch", "" };
        ch[1] = (choice_t){ "X", "Manda el PC", "" };
        break;
    }
    ch[2] = (choice_t){ "B", "No tocar nada", "decidir mas tarde" };

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

    choice_t ch[3] = {
        { "A", "Los del PC",     "baja lo del emulador" },
        { "X", "Los de la Switch", "sube lo de la consola" },
        { "B", "No tocar nada",  "decidir mas tarde" },
    };

    int r = dialog("El PC tiene cambios", title_name, body, ch, COL_ACCENT);
    return r == 0 ? DEC_PC : r == 1 ? DEC_SWITCH : DEC_SKIP;
}

static int cb_conflict(void *ud, const char *title_name, size_t n_conflicts)
{
    (void)ud;

    char body[256];
    snprintf(body, sizeof(body),
             "%zu archivo(s) cambiaron en los dos lados. Se queda una version entera: "
             "mezclarlas corrompe la partida en la mayoria de juegos.", n_conflicts);

    choice_t ch[3] = {
        { "A", "Quedarme con la Switch", "lo de la consola" },
        { "X", "Quedarme con el PC",     "lo del emulador" },
        { "B", "Saltar este juego",      "no tocar nada" },
    };

    int r = dialog("Conflicto", title_name, body, ch, COL_ACCENT);
    return r == 0 ? WINNER_SWITCH : r == 1 ? WINNER_PC : -1;
}

// --------------------------------------------------------------------------
// sincronizacion
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

static void refresh_states(void)
{
    if (g_games.n == 0) return;

    net_t n;
    if (!connect_now(&n, true)) return;

    u64 *ids = malloc(g_games.n * sizeof(u64));
    u8  *st  = malloc(g_games.n);
    if (ids && st) {
        for (size_t i = 0; i < g_games.n; i++) ids[i] = g_games.v[i].application_id;
        if (sync_summary(&n, current_uid(), ids, g_games.n, st))
            for (size_t i = 0; i < g_games.n; i++) g_games.v[i].state = st[i];
    }
    free(ids);
    free(st);
    net_close(&n);
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
    g_syncing = true;

    net_t n;
    if (!connect_now(&n, false)) {
        log_push("No se pudo conectar con el PC.");
        log_push("Comprueba que nxsavesyncd.py esta corriendo y que estais en la misma red.");
        g_sync_finished = true;
        audio_play(SND_ERROR);
        for (;;) {
            ui_input_t in;
            if (!ui_frame_begin(&in)) break;
            ui_clear(COL_BG);
            draw_rail(&in, false); draw_topbar(); draw_hints("A  volver");
            draw_sync_sheet();
            ui_frame_end();
            if ((in.down & (HidNpadButton_A | HidNpadButton_B)) || in.tap) break;
        }
        g_syncing = false;
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

    for (;;) {
        ui_input_t in;
        if (!ui_frame_begin(&in)) break;
        ui_clear(COL_BG);
        draw_rail(&in, false); draw_topbar(); draw_hints("A  volver");
        draw_sync_sheet();
        ui_frame_end();
        if ((in.down & (HidNpadButton_A | HidNpadButton_B)) || in.tap) break;
    }

    g_syncing = false;
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
    ui_clear(COL_BG);
    draw_rail(&in, false);
    draw_topbar();
    ui_ring(RAIL_W + (UI_W - RAIL_W) / 2, UI_H / 2 - 20, 34, 5, -1.0f, g_accent, COL_PANEL);
    ui_text_center(RAIL_W + (UI_W - RAIL_W) / 2, UI_H / 2 + 34, 19, COL_DIM, "%s", what);
    draw_hints("");
    ui_frame_end();
}

static void reload_games(void)
{
    for (int i = 0; i < 4; i++) draw_busy("Leyendo las partidas guardadas...");

    games_free(&g_games);
    ui_images_clear();          // las texturas apuntaban a los iconos liberados
    games_list(&g_games, current_uid(), true);

    g_game_sel = g_grid_top = 0;
    g_detail_in = 0.0f;
    refresh_states();
}

// --------------------------------------------------------------------------
// vista de juegos
// --------------------------------------------------------------------------

static void card_pos(size_t index, int *x, int *y)
{
    size_t k = index - g_grid_top;
    *x = GRID_X + (int)(k % GRID_COLS) * (CARD_W + CARD_GAP);
    *y = GRID_Y + (int)(k / GRID_COLS) * (CARD_H + CARD_GAP);
}

static void draw_card(game_t *g, int x, int y, bool selected)
{
    bool excluded = settings_excluded(g->application_id);

    const int pad  = 20;
    const int icon = CARD_W - 2 * pad;    // 120
    const int iy   = y + 12;              // icono: 12 .. 132
    const int ny   = y + CARD_H - 26;     // nombre: 130 .. 150, dentro de 156

    ui_rect_round(x, y, CARD_W, CARD_H, 14, selected ? COL_PANEL_HI : COL_PANEL);

    SDL_Texture *t = ui_image(g->icon, g->icon_size);
    if (t) ui_image_draw_round(t, x + pad, iy, icon, icon, 10,
                               selected ? COL_PANEL_HI : COL_PANEL);
    else {
        ui_rect_round(x + pad, iy, icon, icon, 10, COL_PANEL_HI);
        ui_text_center(x + CARD_W / 2, iy + icon / 2 - 14, 24, COL_DIM, "?");
    }

    if (excluded) {
        ui_rect_round(x + pad, iy, icon, icon, 10, ui_alpha(COL_BG, 190));
        ui_text_center(x + CARD_W / 2, iy + icon / 2 - 9, 16, COL_DIM, "excluido");
    }

    // Insignia de estado en la esquina del icono. El anillo oscuro debajo la
    // hace visible tambien sobre iconos claros.
    color_t sc = state_color(g->state, excluded);
    int bx = x + CARD_W - pad - 20, by = iy + 6;
    ui_rect_round(bx - 2, by - 2, 20, 20, 10, ui_alpha(COL_BG, 210));
    ui_rect_round(bx + 1, by + 1, 14, 14, 7, sc);

    ui_text_clip(x + 12, ny, 15, CARD_W - 24, COL_TEXT, g->name);
}

static void draw_detail(void)
{
    if (g_games.n == 0) return;

    game_t *g = &g_games.v[g_game_sel];
    bool ex = settings_excluded(g->application_id);

    g_detail_in = ui_approach(g_detail_in, 1.0f, 12.0f);
    int slide = (int)((1.0f - g_detail_in) * 24.0f);

    const int x = DETAIL_X + slide;
    const int y = BODY_Y + 16;
    const int h = BODY_H - 32;
    const int pad = 24;

    ui_rect_round(x, y, DETAIL_W, h, 16, COL_PANEL);
    ui_gradient_round_top(x, y, DETAIL_W, 150, 16,
                          ui_alpha(g_accent, 70), ui_alpha(g_accent, 0));

    // Los botones se anclan al fondo del panel y el contenido se corta antes de
    // llegar. Antes se colocaba todo hacia abajo con sumas sueltas y el title id
    // acababa por debajo del boton de sincronizar.
    const int btn_h = 48, btn2_h = 44, btn_gap = 10;
    const int btn_y = y + h - pad - btn2_h - btn_gap - btn_h;
    const int limit = btn_y - 16;

    int ty = y + 22;

    const int icon = 132;
    SDL_Texture *t = ui_image(g->icon, g->icon_size);
    if (t) ui_image_draw_round(t, x + (DETAIL_W - icon) / 2, ty, icon, icon, 16, COL_PANEL);
    else   ui_rect_round(x + (DETAIL_W - icon) / 2, ty, icon, icon, 16, COL_PANEL_HI);
    ty += icon + 20;

    ui_text_clip(x + pad, ty, 21, DETAIL_W - 2 * pad, COL_TEXT, g->name);
    ty += 30;

    if (g->author[0]) {
        ui_text_clip(x + pad, ty, 15, DETAIL_W - 2 * pad, COL_DIM, g->author);
        ty += 24;
    }

    ty += 6;
    {
        const char *st = state_text(g->state, ex);
        color_t c = state_color(g->state, ex);
        int w = ui_text_w(15, st) + 28;
        if (w > DETAIL_W - 2 * pad) w = DETAIL_W - 2 * pad;
        ui_rect_round(x + pad, ty, w, 32, 10, ui_alpha(c, 45));
        ui_rect_round(x + pad, ty, 4, 32, 2, c);
        ui_text_clip(x + pad + 14, ty + 8, 15, w - 24, c, st);
        ty += 44;
    }

    // Lo de abajo solo se pinta si queda hueco antes de los botones.
    if (ty + 24 < limit) {
        ui_rect(x + pad, ty, DETAIL_W - 2 * pad, 1, ui_alpha(COL_DIM, 50));
        ty += 14;
    }

    if (ty + 42 < limit) {
        ui_text(x + pad, ty, 14, COL_DIM, "Ante un conflicto");
        ui_text_clip(x + pad, ty + 18, 16, DETAIL_W - 2 * pad, COL_TEXT,
                     policy_label(settings_policy_for(g->application_id)));
        ty += 46;
    }

    if (ty + 42 < limit) {
        ui_text(x + pad, ty, 14, COL_DIM, "Title ID");
        ui_text(x + pad, ty + 18, 15, ui_alpha(COL_TEXT, 190), "%016lX", g->application_id);
    }

    ui_rect_round(x + pad, btn_y, DETAIL_W - 2 * pad, btn_h, 12, g_accent);
    ui_text_center(x + DETAIL_W / 2, btn_y + 13, 19, COL_BG, "A   Sincronizar");

    ui_rect_round(x + pad, btn_y + btn_h + btn_gap, DETAIL_W - 2 * pad, btn2_h, 12, COL_PANEL_HI);
    ui_text_center(x + DETAIL_W / 2, btn_y + btn_h + btn_gap + 12, 17, COL_TEXT,
                   "X   Opciones del juego");
}

static void view_games(const ui_input_t *in)
{
    if (g_games.n == 0) {
        ui_text_center(RAIL_W + (UI_W - RAIL_W) / 2, UI_H / 2 - 30, 22, COL_DIM,
                       "Este perfil no tiene partidas guardadas.");
        ui_text_center(RAIL_W + (UI_W - RAIL_W) / 2, UI_H / 2 + 4, 17, ui_alpha(COL_DIM, 150),
                       "Juega un rato y vuelve, o cambia de perfil.");
        return;
    }

    // El acento sigue al juego elegido, con transicion suave.
    color_t want = ui_image_color(g_games.v[g_game_sel].icon, g_games.v[g_game_sel].icon_size);
    g_accent = ui_mix(g_accent, want, ui_approach(0.0f, 1.0f, 8.0f));

    size_t per_page = GRID_COLS * GRID_ROWS;

    int sx, sy;
    card_pos(g_game_sel, &sx, &sy);
    g_sel_x = ui_approach(g_sel_x, (float)sx, 22.0f);
    g_sel_y = ui_approach(g_sel_y, (float)sy, 22.0f);

    for (size_t i = g_grid_top; i < g_grid_top + per_page && i < g_games.n; i++) {
        int x, y;
        card_pos(i, &x, &y);
        draw_card(&g_games.v[i], x, y, i == g_game_sel);

        if (in->tap && in->tap_x >= x && in->tap_x < x + CARD_W &&
            in->tap_y >= y && in->tap_y < y + CARD_H) {
            if (i == g_game_sel) { audio_play(SND_SELECT); run_sync(i, 1); }
            else { g_game_sel = i; g_detail_in = 0.0f; audio_play(SND_MOVE); }
            return;
        }
    }

    // El recuadro va encima de las tarjetas: si no, mientras se desliza queda
    // por debajo de la tarjeta vecina y parece que parpadea.
    ui_rect_round_outline((int)g_sel_x - 4, (int)g_sel_y - 4,
                          CARD_W + 8, CARD_H + 8, 16, 3, g_accent);

    if (g_games.n > per_page) {
        int pages = (int)((g_games.n + per_page - 1) / per_page);
        int page  = (int)(g_grid_top / per_page);
        for (int i = 0; i < pages; i++) {
            int w = i == page ? 22 : 8;
            ui_rect_round(GRID_X + i * 16, UI_H - HINT_H - 22, w, 6, 3,
                          i == page ? g_accent : ui_alpha(COL_DIM, 90));
        }
    }

    draw_detail();
}

static void input_games(const ui_input_t *in)
{
    if (g_games.n == 0) return;

    size_t per_page = GRID_COLS * GRID_ROWS;
    size_t before = g_game_sel;

    if (in->down & HidNpadButton_Right) g_game_sel = (g_game_sel + 1) % g_games.n;
    if (in->down & HidNpadButton_Left)  g_game_sel = g_game_sel ? g_game_sel - 1 : g_games.n - 1;
    if (in->down & HidNpadButton_Down)
        g_game_sel = g_game_sel + GRID_COLS < g_games.n ? g_game_sel + GRID_COLS : g_games.n - 1;
    if (in->down & HidNpadButton_Up)
        g_game_sel = g_game_sel >= GRID_COLS ? g_game_sel - GRID_COLS : 0;

    if (g_game_sel != before) {
        audio_play(SND_MOVE);
        g_detail_in = 0.0f;
    }

    if (g_game_sel < g_grid_top) g_grid_top = (g_game_sel / GRID_COLS) * GRID_COLS;
    if (g_game_sel >= g_grid_top + per_page)
        g_grid_top = ((g_game_sel / GRID_COLS) - (GRID_ROWS - 1)) * GRID_COLS;

    if (in->down & HidNpadButton_A) { audio_play(SND_SELECT); run_sync(g_game_sel, 1); }
    if (in->down & HidNpadButton_Y) { audio_play(SND_SELECT); run_sync(0, g_games.n); }
    if (in->down & HidNpadButton_X) { audio_play(SND_SELECT); g_modal = MODAL_GAME; g_row_sel = 0; }
}

// --------------------------------------------------------------------------
// filas reutilizables para las listas
// --------------------------------------------------------------------------

static bool row(const ui_input_t *in, int y, int h, bool selected,
                const char *title, const char *sub, const char *value, color_t vc)
{
    int x = RAIL_W + 28, w = UI_W - RAIL_W - 56;

    ui_rect_round(x, y, w, h, 13, selected ? COL_PANEL_HI : COL_PANEL);
    if (selected) ui_rect_round(x, y, 4, h, 2, g_accent);

    ui_text_clip(x + 24, y + (sub && sub[0] ? 12 : (h - 22) / 2), 20, w - 320, COL_TEXT, title);
    if (sub && sub[0]) ui_text_clip(x + 24, y + 40, 15, w - 320, COL_DIM, sub);

    if (value && value[0]) {
        int vmax = 280;
        int vw = ui_text_w(18, value);
        if (vw > vmax) vw = vmax;
        ui_text_clip(x + w - 24 - vw, y + (h - 22) / 2, 18, vmax, vc, value);
    }

    return ui_hit(in, x, y, w, h);
}

// --------------------------------------------------------------------------
// vista de perfiles
// --------------------------------------------------------------------------

static void view_users(const ui_input_t *in)
{
    ui_text(RAIL_W + 28, BODY_Y + 4, 16, COL_DIM,
            "Cada perfil se sincroniza por separado. Puedes dejar alguno fuera.");

    // Cuantas filas caben de verdad entre la cabecera y la barra de ayuda.
    const int row_h = 82, row_gap = 10;
    const int first_y = BODY_Y + 34;
    int visible = (UI_H - HINT_H - 12 - first_y) / (row_h + row_gap);
    if (visible < 1) visible = 1;

    int top = 0;
    if (g_row_sel >= visible) top = g_row_sel - visible + 1;

    int y = first_y;
    for (size_t i = (size_t)top; i < g_users.n && (int)i < top + visible; i++) {
        user_t *u = &g_users.v[i];
        bool sel = (int)i == g_row_sel;
        bool shared = settings_profile_shared(u->uid);
        int h = row_h;

        int x = RAIL_W + 28, w = UI_W - RAIL_W - 56;
        ui_rect_round(x, y, w, h, 13, sel ? COL_PANEL_HI : COL_PANEL);
        if (sel) ui_rect_round(x, y, 4, h, 2, g_accent);

        SDL_Texture *av = ui_image(u->icon, u->icon_size);
        if (av) ui_image_draw_round(av, x + 22, y + 13, 56, 56, 12,
                                    sel ? COL_PANEL_HI : COL_PANEL);
        else    ui_rect_round(x + 22, y + 13, 56, 56, 12, COL_PANEL_HI);

        ui_text(x + 96, y + 18, 21, COL_TEXT, "%s", u->name);
        ui_text(x + 96, y + 46, 16, shared ? COL_OK : COL_DIM,
                shared ? "Se sincroniza" : "No se comparte");

        if (i == g_user_sel) {
            const char *tag = "en uso";
            int tw = ui_text_w(15, tag) + 24;
            ui_rect_round(x + w - 24 - tw, y + 24, tw, 32, 10, ui_alpha(g_accent, 50));
            ui_text_center(x + w - 24 - tw / 2, y + 31, 15, g_accent, "%s", tag);
        }

        if (ui_hit(in, x, y, w, h)) { g_row_sel = (int)i; audio_play(SND_MOVE); }
        y += h + row_gap;
    }

    if ((int)g_users.n > visible)
        ui_text(UI_W - 120, UI_H - HINT_H - 26, 14, ui_alpha(COL_DIM, 150),
                "%d de %zu", g_row_sel + 1, g_users.n);
}

static void input_users(const ui_input_t *in)
{
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

    if (in->down & HidNpadButton_A) {
        if ((size_t)g_row_sel != g_user_sel) {
            g_user_sel = (size_t)g_row_sel;
            audio_play(SND_SELECT);
            reload_games();
            toast("Perfil: %s", current_user_name());
            g_nav = NAV_GAMES;
        }
    }
}

// --------------------------------------------------------------------------
// vista de PCs
// --------------------------------------------------------------------------

static void do_discovery(bool announce)
{
    for (int i = 0; i < 4; i++) draw_busy("Buscando PCs en la red...");

    discovery_scan(&g_found, 900);
    for (size_t i = 0; i < g_found.n; i++) settings_add_host(&g_found.v[i], false);

    // Si solo hay uno se usa y no se pregunta: molestar solo cuando hay que elegir.
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

static void view_hosts(const ui_input_t *in)
{
    ui_text(RAIL_W + 28, BODY_Y + 4, 16, COL_DIM,
            "Se buscan solos por la red. Tambien puedes anadir uno por IP.");

    const int row_h = 74, row_gap = 10;
    const int first_y = BODY_Y + 34;
    int visible = (UI_H - HINT_H - 12 - first_y) / (row_h + row_gap);
    if (visible < 1) visible = 1;

    int top = 0;
    if (g_row_sel >= visible) top = g_row_sel - visible + 1;

    int y = first_y;
    for (size_t i = (size_t)top; i < g_set.host_count && (int)i < top + visible; i++) {
        host_t *h = &g_set.hosts[i];
        char sub[160];
        snprintf(sub, sizeof(sub), "%s:%u%s%s", h->ip, h->port,
                 h->emu[0] ? "   ·   " : "", h->emu);

        if (row(in, y, row_h, (int)i == g_row_sel, h->name[0] ? h->name : h->ip, sub,
                i == g_set.host_sel ? "en uso" : "", g_accent)) {
            g_row_sel = (int)i;
            audio_play(SND_MOVE);
        }
        y += row_h + row_gap;
    }

    if (g_set.host_count == 0)
        ui_text(RAIL_W + 52, y + 10, 19, COL_DIM, "Ninguno todavia. Pulsa Y para buscar.");
    else if ((int)g_set.host_count > visible)
        ui_text(UI_W - 120, UI_H - HINT_H - 26, 14, ui_alpha(COL_DIM, 150),
                "%d de %zu", g_row_sel + 1, g_set.host_count);
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
            refresh_states();
            toast("PC en uso: %s", g_set.hosts[g_set.host_sel].name);
        }
    }

    if (in->down & HidNpadButton_Y) do_discovery(true);
    if (in->down & HidNpadButton_X) add_host_manual();
}

// --------------------------------------------------------------------------
// vista de ajustes
// --------------------------------------------------------------------------

#define SET_ROWS 12
#define SET_VISIBLE 7

static int g_set_top;

static void view_settings(const ui_input_t *in)
{
    const char *labels[SET_ROWS] = {
        "Modo de sincronizacion",
        "Ante un conflicto",
        "Preguntar si el PC trae cambios",
        "Sincronizar todo al abrir",
        "Buscar PCs al arrancar",
        "Sonidos",
        "Segundo plano (sysmodule)",
        "Ante un conflicto en segundo plano",
        "Sincronizar cuando el PC avise",
        "Cada cuanto revisa en segundo plano",
        "Ajustes del PC",
        "Sincronizar todos los juegos ahora",
    };
    const char *helps[SET_ROWS] = {
        "En automatico no pregunta: aplica la regla de abajo",
        "Que hacer cuando los dos lados han cambiado",
        "Confirmar antes de bajar cambios del emulador",
        "Se pone al dia sin que toques nada",
        "Descubrimiento por la red local",
        "Efectos de la interfaz",
        "Sincroniza sin abrir la app, al cerrar cada juego",
        "Sin nadie delante no se puede preguntar",
        "Al terminar de jugar en el emulador, sin esperar al repaso",
        "Ademas del repaso al cerrar un juego y de los avisos",
        "Emuladores, replicado y vigilancia del daemon",
        "",
    };
    char values[SET_ROWS][96];
    snprintf(values[0], 96, "%s", mode_label());
    snprintf(values[1], 96, "%s", policy_label(g_set.policy));
    snprintf(values[2], 96, "%s", g_set.ask_incoming ? "Si" : "No");
    snprintf(values[3], 96, "%s", g_set.sync_on_open ? "Si" : "No");
    snprintf(values[4], 96, "%s", g_set.auto_discover ? "Si" : "No");
    snprintf(values[5], 96, "%s", audio_enabled() ? "Si" : "No");
    snprintf(values[6], 96, "%s", g_set.bg_enabled ? "Activado" : "Desactivado");
    snprintf(values[7], 96, "%s", policy_label(g_set.bg_policy));
    snprintf(values[8], 96, "%s", g_set.bg_nudge ? "Si" : "No");
    snprintf(values[9], 96, "%u min", g_set.bg_interval / 60);
    snprintf(values[10], 96, "%s", g_server_name[0] ? g_server_name : "conectar");
    snprintf(values[11], 96, "%zu juegos", g_games.n);

    // La lista ya no cabe entera: se desplaza con la seleccion.
    if (g_row_sel < g_set_top)                    g_set_top = g_row_sel;
    if (g_row_sel >= g_set_top + SET_VISIBLE)     g_set_top = g_row_sel - SET_VISIBLE + 1;

    int y = BODY_Y + 8;
    for (int i = g_set_top; i < SET_ROWS && i < g_set_top + SET_VISIBLE; i++) {
        color_t vc = (i == 6 && g_set.bg_enabled) ? COL_OK : g_accent;
        if (row(in, y, 66, i == g_row_sel, labels[i], helps[i], values[i], vc)) {
            g_row_sel = i;
            audio_play(SND_MOVE);
        }
        y += 74;
    }

    if (SET_ROWS > SET_VISIBLE)
        ui_text(UI_W - 120, UI_H - HINT_H - 26, 14, ui_alpha(COL_DIM, 150),
                "%d de %d", g_row_sel + 1, SET_ROWS);
}

static void input_settings(const ui_input_t *in)
{
    int before = g_row_sel;
    if (in->down & HidNpadButton_Down) g_row_sel = (g_row_sel + 1) % SET_ROWS;
    if (in->down & HidNpadButton_Up)   g_row_sel = g_row_sel ? g_row_sel - 1 : SET_ROWS - 1;
    if (g_row_sel != before) audio_play(SND_MOVE);

    bool change = (in->down & (HidNpadButton_A | HidNpadButton_Right | HidNpadButton_Left)) != 0;
    if (!change) return;

    switch (g_row_sel) {
    case 0: g_set.mode = g_set.mode == MODE_AUTO ? MODE_MANUAL : MODE_AUTO; break;
    case 1:
        g_set.policy = g_set.policy == POLICY_SWITCH ? POLICY_PC
                     : g_set.policy == POLICY_PC     ? POLICY_SKIP
                                                     : POLICY_SWITCH;
        break;
    case 2: g_set.ask_incoming  = !g_set.ask_incoming;  break;
    case 3: g_set.sync_on_open  = !g_set.sync_on_open;  break;
    case 4: g_set.auto_discover = !g_set.auto_discover; break;
    case 5:
        audio_set_enabled(!audio_enabled());
        g_set.sounds = audio_enabled();
        break;
    case 6:
        g_set.bg_enabled = !g_set.bg_enabled;
        toast(g_set.bg_enabled
              ? "Segundo plano activado (necesita el sysmodule instalado)"
              : "Segundo plano desactivado");
        break;
    case 7:
        // Sin nadie delante, "preguntar" no es una opcion: se salta el juego.
        g_set.bg_policy = g_set.bg_policy == POLICY_SKIP   ? POLICY_SWITCH
                        : g_set.bg_policy == POLICY_SWITCH ? POLICY_PC
                                                           : POLICY_SKIP;
        break;
    case 8: g_set.bg_nudge = !g_set.bg_nudge; break;
    case 9: {
        int min = g_set.bg_interval / 60;
        min = (in->down & HidNpadButton_Left) ? min - 5 : min + 5;
        if (min < 1)   min = 60;
        if (min > 60)  min = 1;
        g_set.bg_interval = (u16)(min * 60);
        break;
    }
    case 10: audio_play(SND_SELECT); g_modal = MODAL_PCCFG; pccfg_fetch(); return;
    case 11:
        audio_play(SND_SELECT);
        if (g_games.n) run_sync(0, g_games.n);
        return;
    }

    audio_play(SND_TOGGLE);
    settings_save();
}

// --------------------------------------------------------------------------
// modal: opciones de un juego
// --------------------------------------------------------------------------

static void modal_game(const ui_input_t *in)
{
    game_t *g = &g_games.v[g_game_sel];
    bool ex = settings_excluded(g->application_id);

    game_rule_t *r = NULL;
    for (size_t i = 0; i < g_set.game_count; i++)
        if (g_set.games[i].title_id == g->application_id) r = &g_set.games[i];

    int x, y, w = 860, h = 420;
    draw_sheet(&x, &y, w, h);

    SDL_Texture *t = ui_image(g->icon, g->icon_size);
    if (t) ui_image_draw_round(t, x + 32, y + 28, 96, 96, 12, COL_PANEL);

    ui_text_clip(x + 148, y + 34, 24, w - 190, COL_TEXT, g->name);
    if (g->author[0]) ui_text_clip(x + 148, y + 68, 16, w - 190, COL_DIM, g->author);
    ui_text(x + 148, y + 94, 15, ui_alpha(COL_TEXT, 160), "%016lX", g->application_id);

    int ry = y + 152;
    const char *v0 = policy_label(r ? r->policy : POLICY_ASK);
    int rw = w - 64;

    ui_rect_round(x + 32, ry, rw, 74, 13, g_row_sel == 0 ? COL_PANEL_HI : COL_BG);
    if (g_row_sel == 0) ui_rect_round(x + 32, ry, 4, 74, 2, g_accent);
    ui_text(x + 56, ry + 14, 20, COL_TEXT, "Ante un conflicto en este juego");
    ui_text(x + 56, ry + 42, 15, COL_DIM, "En Preguntar usa el ajuste general");
    ui_text(x + 32 + rw - 24 - ui_text_w(19, v0), ry + 26, 19, g_accent, "%s", v0);

    ry += 84;
    const char *v1 = ex ? "Excluido" : "Se sincroniza";
    ui_rect_round(x + 32, ry, rw, 74, 13, g_row_sel == 1 ? COL_PANEL_HI : COL_BG);
    if (g_row_sel == 1) ui_rect_round(x + 32, ry, 4, 74, 2, g_accent);
    ui_text(x + 56, ry + 14, 20, COL_TEXT, "Excluir de la sincronizacion");
    ui_text(x + 56, ry + 42, 15, COL_DIM, "Se salta siempre, tambien en automatico");
    ui_text(x + 32 + rw - 24 - ui_text_w(19, v1), ry + 26, 19, ex ? COL_WARN : COL_ACCENT, "%s", v1);

    if (in->down & (HidNpadButton_Down | HidNpadButton_Up)) {
        g_row_sel = g_row_sel ? 0 : 1;
        audio_play(SND_MOVE);
    }

    if (in->down & (HidNpadButton_A | HidNpadButton_Right | HidNpadButton_Left)) {
        u8 pol = r ? r->policy : POLICY_ASK;
        u8 exc = r ? r->excluded : 0;

        if (g_row_sel == 0)
            pol = pol == POLICY_ASK    ? POLICY_SWITCH
                : pol == POLICY_SWITCH ? POLICY_PC
                : pol == POLICY_PC     ? POLICY_SKIP
                                       : POLICY_ASK;
        else
            exc = !exc;

        settings_set_game(g->application_id, pol, exc);
        settings_save();
        audio_play(SND_TOGGLE);
    }

    if (in->down & (HidNpadButton_B | HidNpadButton_X)) {
        g_modal = MODAL_NONE;
        g_row_sel = 0;
        audio_play(SND_BACK);
    }
}

// --------------------------------------------------------------------------
// modal: ajustes del PC
// --------------------------------------------------------------------------

static cfg_item_t g_cfg[CFG_MAX_ITEMS];
static size_t     g_cfg_n;
static int        g_cfg_sel;
static bool       g_cfg_ok;

static void pccfg_fetch(void)
{
    g_cfg_n = 0;
    g_cfg_ok = false;

    for (int i = 0; i < 3; i++) draw_busy("Pidiendo los ajustes al PC...");

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

static void modal_pccfg(const ui_input_t *in)
{
    int x, y, w = 980, h = 560;
    draw_sheet(&x, &y, w, h);

    ui_text(x + 36, y + 26, 26, COL_TEXT, "Ajustes del PC");
    ui_text(x + 36, y + 60, 15, COL_DIM, "%s",
            g_server_name[0] ? g_server_name : "daemon");

    if (!g_cfg_ok || g_cfg_n == 0) {
        ui_text_center(x + w / 2, y + h / 2 - 10, 20, COL_DIM,
                       "No se pudieron leer los ajustes del PC.");
        ui_text_center(x + w / 2, y + h / 2 + 20, 16, ui_alpha(COL_DIM, 160),
                       "Y para reintentar, B para volver");
        if (in->down & HidNpadButton_Y) pccfg_fetch();
        if (in->down & HidNpadButton_B) { g_modal = MODAL_NONE; audio_play(SND_BACK); }
        return;
    }

    int rows = 5;
    int top = g_cfg_sel - rows + 1;
    if (top < 0) top = 0;

    int ry = y + 96;
    for (int i = top; i < (int)g_cfg_n && i < top + rows; i++) {
        cfg_item_t *it = &g_cfg[i];
        bool sel = i == g_cfg_sel;
        int rw = w - 72;

        ui_rect_round(x + 36, ry, rw, 72, 13, sel ? COL_PANEL_HI : COL_BG);
        if (sel) ui_rect_round(x + 36, ry, 4, 72, 2, g_accent);

        ui_text_clip(x + 60, ry + 12, 19, rw - 300, COL_TEXT, it->label);
        if (it->help[0]) ui_text_clip(x + 60, ry + 40, 14, rw - 300, COL_DIM, it->help);

        const char *v = cfg_display(it);
        color_t vc = it->type == CFG_INFO ? COL_DIM : g_accent;
        int vmax = rw / 2 - 24;
        int vw = ui_text_w(18, v);
        if (vw > vmax) vw = vmax;
        ui_text_clip(x + 36 + rw - 24 - vw, ry + 25, 18, vmax, vc, v);

        if (ui_hit(in, x + 36, ry, rw, 72)) { g_cfg_sel = i; audio_play(SND_MOVE); }
        ry += 80;
    }

    if (g_cfg_n > (size_t)rows)
        ui_text(x + 36, y + h - 54, 15, COL_DIM, "%d de %zu   ·   B volver",
                g_cfg_sel + 1, g_cfg_n);
    else
        ui_text(x + 36, y + h - 54, 15, COL_DIM, "B volver");

    // --- entrada ---
    int before = g_cfg_sel;
    if (in->down & HidNpadButton_Down) g_cfg_sel = (g_cfg_sel + 1) % (int)g_cfg_n;
    if (in->down & HidNpadButton_Up)   g_cfg_sel = g_cfg_sel ? g_cfg_sel - 1 : (int)g_cfg_n - 1;
    if (g_cfg_sel != before) audio_play(SND_MOVE);

    if (in->down & HidNpadButton_B) { g_modal = MODAL_NONE; audio_play(SND_BACK); return; }
    if (in->down & HidNpadButton_Y) { pccfg_fetch(); return; }

    bool fwd  = (in->down & (HidNpadButton_A | HidNpadButton_Right)) != 0;
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
    case NAV_GAMES:    return "A sincronizar   Y todos   X opciones   ZL/ZR perfil   "
                              "L/R menu   + salir";
    case NAV_USERS:    return "A usar este perfil   X compartir o no   L/R menu   + salir";
    case NAV_HOSTS:    return "A usar este PC   Y buscar en la red   X anadir por IP   "
                              "L/R menu   + salir";
    default:           return "A cambiar   L/R menu   + salir";
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

    if (!users_list(&g_users)) {
        for (int i = 0; i < 200; i++) draw_busy("No hay perfiles en la consola.");
        goto done;
    }

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

        ui_clear(COL_BG);
        draw_rail(&in, g_modal == MODAL_NONE);
        draw_topbar();

        switch (g_nav) {
        case NAV_GAMES:    view_games(&in);    break;
        case NAV_USERS:    view_users(&in);    break;
        case NAV_HOSTS:    view_hosts(&in);    break;
        default:           view_settings(&in); break;
        }

        draw_hints(hints_for());
        draw_toast();

        if (g_modal == MODAL_GAME && g_games.n > 0) modal_game(&in);
        else if (g_modal == MODAL_PCCFG)            modal_pccfg(&in);

        ui_frame_end();

        if (g_modal != MODAL_NONE) continue;

        switch (g_nav) {
        case NAV_GAMES:    input_games(&in);    break;
        case NAV_USERS:    input_users(&in);    break;
        case NAV_HOSTS:    input_hosts(&in);    break;
        default:           input_settings(&in); break;
        }

        if (in.down & HidNpadButton_Plus) break;

        // L/R cambian de seccion; ZL/ZR de perfil, que es lo que mas se usa.
        if (in.down & (HidNpadButton_L | HidNpadButton_R)) {
            g_nav = (in.down & HidNpadButton_R)
                  ? (nav_t)((g_nav + 1) % NAV_COUNT)
                  : (nav_t)((g_nav + NAV_COUNT - 1) % NAV_COUNT);
            g_row_sel = 0;
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
