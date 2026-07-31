#include "screens.h"
#include "proto.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

const char *SCR_NAV_NAMES[NAV_COUNT] = { "Juegos", "Perfiles", "PCs", "Ajustes" };

// --------------------------------------------------------------------------
// color
// --------------------------------------------------------------------------

// Gira el tono rotando los canales. No es una conversion a HSV de verdad, pero
// da un color hermano del acento por tres asignaciones, y para pintar luces de
// fondo eso es exactamente lo que hace falta.
static color_t hue_shift(color_t c)
{
    return (color_t){ c.b, c.r, c.g, c.a };
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
// fondo vivo
// --------------------------------------------------------------------------
//
// De aqui sale todo. El cristal no tiene nada que ensenar si detras hay un
// color plano, asi que el fondo lleva el arte del juego elegido ampliado hasta
// perder la forma y tres luces que se mueven despacio. Al desenfocarlo queda
// una mancha de color que cambia con el juego, y eso es lo que se ve dentro de
// cada panel.

void scr_backdrop(const scr_ctx_t *c, const void *hero, size_t hero_len)
{
    float t = ui_time();

    ui_backdrop_begin();

    ui_gradient_v(0, 0, UI_W, UI_H, COL_BG_DEEP, COL_BG);

    color_t a1 = c->accent;
    color_t a2 = hue_shift(c->accent);
    color_t a3 = hue_shift(a2);

    // Las luces van debajo del arte, no encima: asi el icono se recorta contra
    // ellas y el conjunto tiene profundidad en vez de ser una sola mancha.
    ui_glow((int)(300 + sinf(t * 0.11f) * 170),
            (int)(150 + cosf(t * 0.09f) * 80),  520, 430, a1, 120);
    ui_glow((int)(1000 + sinf(t * 0.07f + 2.0f) * 190),
            (int)(540 + cosf(t * 0.13f + 1.0f) * 110), 480, 400, a2, 88);
    ui_glow((int)(660 + sinf(t * 0.05f + 4.0f) * 250),
            (int)(360 + cosf(t * 0.08f + 3.0f) * 140), 400, 360, a3, 62);

    SDL_Texture *tex = hero ? ui_image(hero, hero_len) : NULL;
    if (tex) {
        // Dos copias enormes girando en sentidos contrarios. A este tamano el
        // icono ya no se reconoce: es materia prima de color.
        ui_image_draw_rot(tex, 930, 200, 840, 840, sinf(t * 0.05f) * 7.0f, 52);
        ui_image_draw_rot(tex, 210, 630, 600, 600, -12.0f - sinf(t * 0.04f) * 6.0f, 26);
    }

    // Un ultimo halo por encima del arte, que lo funde con el resto.
    ui_glow((int)(760 + sinf(t * 0.06f + 1.0f) * 200), 300, 400, 360, a1, 44);

    // Y un velo que baja el nivel general. El color hay que verlo, pero el
    // fondo no puede competir con lo que se lee encima: sin esto la pantalla
    // entera se tenia del color del juego y el texto gris desaparecia.
    ui_rect(0, 0, UI_W, UI_H, (color_t){ 0, 0, 0, 92 });
    ui_vignette(190);

    ui_backdrop_end();
}

// --------------------------------------------------------------------------
// iconos del menu
// --------------------------------------------------------------------------
//
// Dibujados con rectangulos redondeados. Escalan sin verse borrosos y ahorran
// meter un atlas de imagenes en el binario solo para cuatro formas.

static void glyph_games(int x, int y, int s, color_t c)
{
    int u = (s - 5) / 2;
    ui_rect_round(x,         y,         u, u, 4, c);
    ui_rect_round(x + u + 5, y,         u, u, 4, c);
    ui_rect_round(x,         y + u + 5, u, u, 4, c);
    ui_rect_round(x + u + 5, y + u + 5, u, u, 4, c);
}

static void glyph_user(int x, int y, int s, color_t c)
{
    int head = s * 2 / 5;
    ui_circle(x + s / 2, y + head / 2 + 1, head / 2, c);
    // Los hombros: un redondeado alto recortado por abajo se lee como torso.
    ui_rect_round(x + 2, y + head + 5, s - 4, s - head - 5, (s - head) / 2, c);
}

static void glyph_pc(int x, int y, int s, color_t c)
{
    ui_rect_round(x, y + 1, s, s - 9, 4, c);
    ui_rect(x + s / 2 - 4, y + s - 8, 8, 4, c);
    ui_rect_round(x + s / 2 - 11, y + s - 4, 22, 3, 2, c);
}

static void glyph_gear(int x, int y, int s, color_t c)
{
    int h = 4, gap = (s - 3 * h) / 4;
    for (int i = 0; i < 3; i++) {
        int yy = y + gap + i * (h + gap);
        ui_rect_round(x, yy, s, h, h / 2, ui_alpha(c, 130));
        int knob = (i == 1) ? s - 14 : (i == 0 ? 3 : s / 2);
        ui_rect_round(x + knob, yy - 3, 11, h + 6, 5, c);
    }
}

// --------------------------------------------------------------------------
// dock
// --------------------------------------------------------------------------

int scr_dock(const scr_ctx_t *c, const ui_input_t *in, bool interactive)
{
    // La gota: persigue al elemento elegido con un muelle y se estira segun lo
    // rapido que vaya. Es un detalle tonto que cambia por completo la sensacion
    // de la navegacion, porque el cursor deja de teletransportarse.
    static float drop_y = 0.0f, drop_v = 0.0f;

    float target = (float)(DOCK_FIRST + (int)c->nav * DOCK_STEP);
    if (drop_y <= 0.0f) drop_y = target;          // sin volar desde arriba al arrancar
    drop_y = ui_spring(drop_y, target, &drop_v, 260.0f);

    // El logo va sobre el dock, en el hueco de la esquina. Es el unico sitio de
    // la interfaz donde no compite con nada.
    ui_logo(DOCK_X + DOCK_W / 2, 88, 68, c->accent,
            ui_mix(c->accent, COL_TEXT, 0.75f), COL_TEXT);

    ui_glass(DOCK_X, DOCK_Y, DOCK_W, DOCK_H, DOCK_R, &GLASS_BAR);
    ui_debug_box(DOCK_X, DOCK_Y, DOCK_W, DOCK_H, "dock");

    // Estirado por velocidad, con tope: sin el, un salto de Ajustes a Juegos
    // dejaba la gota mas larga que el propio dock.
    float stretch = fabsf(drop_v) * 0.055f;
    if (stretch > 26.0f) stretch = 26.0f;
    int dh = (int)(DOCK_ITEM + stretch);
    int dy = (int)drop_y - dh / 2;

    ui_glow(DOCK_X + DOCK_W / 2, (int)drop_y, 58, 44, c->accent, 46);
    ui_rect_round(DOCK_X + 7, dy, DOCK_W - 14, dh, (DOCK_W - 14) / 2,
                  ui_alpha(c->accent, 40));
    ui_rect_round_outline(DOCK_X + 7, dy, DOCK_W - 14, dh, (DOCK_W - 14) / 2, 1,
                          ui_alpha(c->accent, 150));

    int tapped = -1;

    for (int i = 0; i < NAV_COUNT; i++) {
        int cy = DOCK_FIRST + i * DOCK_STEP;
        bool on = i == (int)c->nav;
        color_t col = on ? COL_TEXT : ui_alpha(COL_DIM, 190);

        int gs = 26, gx = DOCK_X + DOCK_W / 2 - gs / 2, gy = cy - 20;
        switch (i) {
        case NAV_GAMES:    glyph_games(gx, gy, gs, col); break;
        case NAV_USERS:    glyph_user(gx, gy, gs, col);  break;
        case NAV_HOSTS:    glyph_pc(gx, gy, gs, col);    break;
        default:           glyph_gear(gx, gy, gs, col);  break;
        }

        ui_text_sh(DOCK_X + DOCK_W / 2 - ui_text_w(11, SCR_NAV_NAMES[i]) / 2,
                   cy + 13, 11, on ? COL_TEXT : ui_alpha(COL_DIM, 205),
                   "%s", SCR_NAV_NAMES[i]);

        // La zona tactil cubre el hueco entero, no solo la pastilla: si no,
        // quedan franjas muertas entre elementos que parecen fallo del tactil.
        if (interactive && !on &&
            ui_hit(in, DOCK_X, cy - DOCK_STEP / 2, DOCK_W, DOCK_STEP))
            tapped = i;
    }

    // Estado del PC, abajo.
    {
        bool have = c->pc_name != NULL;
        color_t col = !have ? COL_ERR : (c->pc_live ? COL_OK : COL_WARN);
        int cy = DOCK_Y + DOCK_H - 58;

        ui_glow(DOCK_X + DOCK_W / 2, cy, 22, 22, col, 120);
        ui_circle(DOCK_X + DOCK_W / 2, cy, 5, col);
        {
            const char *lbl = !have ? "sin PC" : (c->pc_live ? "listo" : "?");
            ui_text_sh(DOCK_X + DOCK_W / 2 - ui_text_w(11, lbl) / 2, cy + 11, 11,
                       ui_alpha(COL_DIM, 200), "%s", lbl);
        }
    }

    ui_text_center(DOCK_X + DOCK_W / 2, DOCK_Y + DOCK_H - 24, 11,
                   ui_alpha(COL_DIM, 120), "%s", c->version);

    return tapped;
}

// --------------------------------------------------------------------------
// barra superior
// --------------------------------------------------------------------------
//
// El titulo va suelto sobre el fondo, sin panel: asi la vista entra por el
// nombre de la seccion y no por una caja. Lo unico con cristal es la capsula
// de la derecha, que es donde esta el estado.

#define CHIP_MAX 3

typedef struct { const char *label, *value; color_t vc; int w; bool avatar; } chip_t;

void scr_topbar(const scr_ctx_t *c, const char *title, const char *sub)
{
    const int cap_y = 26, cap_h = 56, cap_r = 28;
    const int pad = 16, sep = 1;

    // Al cambiar de seccion el titulo entra desde la izquierda en vez de
    // aparecer de golpe. Se detecta solo comparando con el anterior.
    static char last[64];
    static float tin = 1.0f;
    if (strcmp(last, title) != 0) {
        snprintf(last, sizeof(last), "%s", title);
        tin = 0.0f;
    }
    tin = ui_approach(tin, 1.0f, 13.0f);
    int slide = (int)((1.0f - tin) * 22.0f);

    ui_text_sh(CONTENT_X - slide, HEAD_Y + 4, 34,
               ui_alpha(COL_TEXT, (u8)(255 * (0.25f + 0.75f * tin))), "%s", title);
    if (sub && sub[0])
        ui_text_sh(CONTENT_X + 2, HEAD_Y + 48, 14, ui_alpha(COL_DIM, 200), "%s", sub);

    int title_end = CONTENT_X + ui_text_w(34, title) + 28;

    chip_t ch[CHIP_MAX];
    int n = 0;

    // De menos a mas importante: si no cabe todo, cae primero el modo.
    if (c->mode) {
        int w = ui_text_w(14, "Modo");
        int vw = ui_text_w(16, c->mode);
        ch[n++] = (chip_t){ "Modo", c->mode, c->mode_auto ? COL_OK : COL_TEXT,
                            (w > vw ? w : vw) + pad * 2, false };
    }
    {
        const char *v = c->pc_name ? c->pc_name : "sin PC";
        int w = ui_text_w(14, "PC");
        int vw = ui_text_w(16, v);
        if (vw > 190) vw = 190;
        ch[n++] = (chip_t){ "PC", v, c->pc_name ? COL_TEXT : COL_ERR,
                            (w > vw ? w : vw) + pad * 2, false };
    }
    if (c->user) {
        int vw = ui_text_w(16, c->user->name);
        if (vw > 140) vw = 140;
        ch[n++] = (chip_t){ NULL, c->user->name, COL_TEXT, 34 + 10 + vw + pad * 2, true };
    }

    // Recorta chips por la izquierda hasta que la capsula no pise el titulo.
    int first = 0, total;
    for (;;) {
        total = 0;
        for (int i = first; i < n; i++) total += ch[i].w + (i > first ? sep : 0);
        if (CONTENT_R - total >= title_end || first >= n - 1) break;
        first++;
    }
    if (total <= 0) return;

    int x = CONTENT_R - total;

    ui_glass(x, cap_y, total, cap_h, cap_r, &GLASS_BAR);
    ui_debug_box(x, cap_y, total, cap_h, "chips");

    int cx = x;
    for (int i = first; i < n; i++) {
        if (i > first) {
            ui_rect(cx, cap_y + 14, sep, cap_h - 28, ui_alpha(COL_DIM, 60));
            cx += sep;
        }

        if (ch[i].avatar) {
            SDL_Texture *av = c->user->icon
                            ? ui_image(c->user->icon, c->user->icon_size) : NULL;
            if (av) ui_image_round(av, cx + pad, cap_y + 11, 34, 34, 11);
            else    ui_rect_round(cx + pad, cap_y + 11, 34, 34, 11,
                                  ui_alpha(COL_TEXT, 40));
            ui_text_clip(cx + pad + 44, cap_y + 19, 16,
                         ch[i].w - pad * 2 - 44, COL_TEXT, ch[i].value);
        } else {
            ui_text(cx + pad, cap_y + 12, 14, ui_alpha(COL_DIM, 190), "%s", ch[i].label);
            ui_text_clip(cx + pad, cap_y + 30, 16, ch[i].w - pad * 2,
                         ch[i].vc, ch[i].value);
        }
        cx += ch[i].w;
    }
}

// --------------------------------------------------------------------------
// pistas y avisos
// --------------------------------------------------------------------------

void scr_hints(const char *hints)
{
    if (!hints || !hints[0]) return;

    // El ancho persigue al del texto en vez de saltar: al cambiar de seccion la
    // barra se estira o se encoge, que es mucho menos brusco que reaparecer con
    // otro tamano.
    static float aw = 0.0f;

    int want = ui_text_w(16, hints) + 56;
    if (want > CONTENT_W) want = CONTENT_W;
    if (aw <= 0.0f) aw = (float)want;
    aw = ui_approach(aw, (float)want, 15.0f);

    int w = (int)aw;
    int x = CONTENT_X + (CONTENT_W - w) / 2;

    ui_glass(x, HINT_Y, w, HINT_H, HINT_H / 2, &GLASS_BAR);
    ui_debug_box(x, HINT_Y, w, HINT_H, "hints");
    ui_text_clip_center(x + w / 2, HINT_Y + 14, 16, w - 40,
                        ui_alpha(COL_TEXT, 225), hints);
}

void scr_toast(const char *msg, float in, color_t accent)
{
    if (in < 0.01f || !msg || !msg[0]) return;

    int w = ui_text_w(18, msg) + 72;
    if (w > CONTENT_W) w = CONTENT_W;
    int x = CONTENT_X + (CONTENT_W - w) / 2;
    int h = 56;
    int y = (int)(HINT_Y - 18 - h * in);

    ui_glass(x, y, w, h, h / 2, &GLASS_SHEET);
    ui_glow(x + 26, y + h / 2, 30, 24, accent, (u8)(150 * in));
    ui_circle(x + 26, y + h / 2, 6, accent);
    ui_text_clip(x + 48, y + 17, 18, w - 72, COL_TEXT, msg);
}

// --------------------------------------------------------------------------
// tarjetas
// --------------------------------------------------------------------------

static void card_pos(int index, int top, int *x, int *y)
{
    int k = index - top;
    *x = GRID_X + (k % GRID_COLS) * (CARD_W + CARD_GAP);
    *y = GRID_Y + (k / GRID_COLS) * (CARD_H + CARD_GAP);
}

#define CARD_PAD  18
#define CARD_ICON (CARD_W - 2 * CARD_PAD)     // 116
#define CARD_ICON_Y 13
#define CARD_NAME_Y (CARD_ICON_Y + CARD_ICON + 4)

static void card_content(const scr_game_t *g, int x, int y, bool sel)
{
    SDL_Texture *t = g->icon ? ui_image(g->icon, g->icon_size) : NULL;
    int ix = x + CARD_PAD, iy = y + CARD_ICON_Y;

    if (t) ui_image_round(t, ix, iy, CARD_ICON, CARD_ICON, 14);
    else {
        ui_rect_round(ix, iy, CARD_ICON, CARD_ICON, 14, ui_alpha(COL_TEXT, 26));
        ui_text_center(x + CARD_W / 2, iy + CARD_ICON / 2 - 14, 22,
                       ui_alpha(COL_TEXT, 90), "?");
    }

    if (g->excluded) {
        ui_rect_round(ix, iy, CARD_ICON, CARD_ICON, 14, (color_t){ 0, 0, 0, 175 });
        ui_text_center(x + CARD_W / 2, iy + CARD_ICON / 2 - 9, 14,
                       ui_alpha(COL_TEXT, 190), "excluido");
    }

    // Insignia de estado. El circulo oscuro de debajo la hace visible tambien
    // sobre iconos claros, que era donde antes desaparecia.
    color_t sc = state_color(g->state, g->excluded);
    int bx = ix + CARD_ICON - 9, by = iy + 9;
    ui_circle(bx, by, 9, (color_t){ 0, 0, 0, 170 });
    ui_glow(bx, by, 16, 16, sc, 110);
    ui_circle(bx, by, 5, sc);

    ui_text_clip_sh(x + 11, y + CARD_NAME_Y, 14, CARD_W - 22,
                    sel ? COL_TEXT : ui_alpha(COL_TEXT, 205), g->name);
}

// --------------------------------------------------------------------------
// panel de detalle
// --------------------------------------------------------------------------

// Los botones se anclan al fondo y el contenido se corta antes de llegar. En
// la version anterior todo se colocaba sumando hacia abajo y con un nombre
// largo el Title ID acababa debajo del boton de sincronizar.

#define DET_PAD  26
#define DET_BTN_H 52
#define DET_BTN_Y (DET_Y + DET_H - DET_PAD - DET_BTN_H)
#define DET_LIMIT (DET_BTN_Y - 18)
#define DET_IN_W  (DET_W - 2 * DET_PAD)

static void detail_row(int x, int *ty, const char *label, const char *value,
                       color_t vc)
{
    const int h = 38;
    if (*ty + h > DET_LIMIT) return;

    ui_text_sh(x, *ty + 11, 14, ui_alpha(COL_DIM, 210), "%s", label);

    int lw = ui_text_w(14, label);
    int max = DET_IN_W - lw - 16;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", value);
    int vw = ui_text_w(16, buf);
    if (vw > max) vw = max;
    ui_text_clip(x + DET_IN_W - vw, *ty + 9, 16, max, vc, buf);

    *ty += h;
    if (*ty + 2 < DET_LIMIT)
        ui_rect(x, *ty - 1, DET_IN_W, 1, ui_alpha(COL_DIM, 40));
}

static void detail_content(const scr_ctx_t *c, const scr_game_t *g,
                           const char *policy_label,
                           const ui_input_t *in, bool *hit_sync, bool *hit_opts)
{
    const int x = DET_X + DET_PAD;
    int ty = DET_Y + 22;

    // Icono grande, con su propio halo del color del juego.
    const int icon = 128;
    int ix = DET_X + (DET_W - icon) / 2;
    ui_glow(ix + icon / 2, ty + icon / 2, 108, 100, c->accent, 90);

    SDL_Texture *t = g->icon ? ui_image(g->icon, g->icon_size) : NULL;
    if (t) ui_image_round(t, ix, ty, icon, icon, 22);
    else   ui_rect_round(ix, ty, icon, icon, 22, ui_alpha(COL_TEXT, 26));
    ty += icon + 16;

    // El nombre puede necesitar dos lineas; se mide antes de escribirlo para
    // que lo de debajo sepa donde empieza.
    int name_h = ui_text_wrap_h(22, DET_IN_W, 26, g->name);
    if (name_h > 52) name_h = 52;              // dos lineas como mucho
    ui_text_wrap(x, ty, 22, DET_IN_W, 26, COL_TEXT, g->name);
    ty += name_h + 4;

    if (g->author && g->author[0] && ty + 22 < DET_LIMIT) {
        ui_text_clip_sh(x, ty, 14, DET_IN_W, ui_alpha(COL_DIM, 215), g->author);
        ty += 24;
    }

    // Ficha de estado.
    if (ty + 36 < DET_LIMIT) {
        const char *st = state_text(g->state, g->excluded);
        color_t sc = state_color(g->state, g->excluded);
        const int lead = 28;                 // hueco del punto de color
        int w = lead + ui_text_w(14, st) + 18;
        if (w > DET_IN_W) w = DET_IN_W;

        ui_rect_round(x, ty, w, 34, 17, ui_alpha(sc, 40));
        ui_rect_round_outline(x, ty, w, 34, 17, 1, ui_alpha(sc, 110));
        ui_circle(x + 15, ty + 17, 4, sc);
        ui_text_clip_sh(x + lead, ty + 8, 14, w - lead - 14, sc, st);
        ty += 46;
    }

    detail_row(x, &ty, "Ante un conflicto", policy_label, c->accent);

    if (c->user)
        detail_row(x, &ty, "Perfil", c->user->name, ui_alpha(COL_TEXT, 215));

    char tid[32];
    snprintf(tid, sizeof(tid), "%016llX", (unsigned long long)g->title_id);
    detail_row(x, &ty, "Title ID", tid, ui_alpha(COL_TEXT, 190));

    // Botones, en una sola fila para que quepa todo lo de arriba.
    const int gap = 10;
    const int w2 = 150, w1 = DET_IN_W - gap - w2;

    // Relleno con brillo arriba en vez de color plano: el acento sale del icono
    // del juego y a saturacion completa hay juegos que dan un boton fosforito.
    ui_glow(x + w1 / 2, DET_BTN_Y + DET_BTN_H / 2, w1 / 2 + 10, 34, c->accent, 80);
    ui_rect_round(x, DET_BTN_Y, w1, DET_BTN_H, 16, ui_shade(c->accent, 0.46f));
    ui_gradient_round_top(x, DET_BTN_Y, w1, DET_BTN_H / 2, 16,
                          ui_alpha(c->accent, 90), ui_alpha(c->accent, 0));
    ui_rect_round_outline(x, DET_BTN_Y, w1, DET_BTN_H, 16, 1,
                          ui_alpha(c->accent, 220));
    ui_text_clip_center(x + w1 / 2, DET_BTN_Y + 15, 18, w1 - 24,
                        COL_TEXT, "A   Sincronizar");

    int bx = x + w1 + gap;
    ui_rect_round(bx, DET_BTN_Y, w2, DET_BTN_H, 16, ui_alpha(COL_TEXT, 30));
    ui_rect_round_outline(bx, DET_BTN_Y, w2, DET_BTN_H, 16, 1, ui_alpha(COL_TEXT, 55));
    ui_text_clip_center(bx + w2 / 2, DET_BTN_Y + 16, 16, w2 - 20,
                        COL_TEXT, "X   Opciones");

    ui_debug_box(x, DET_BTN_Y, w1, DET_BTN_H, "sync");
    ui_debug_box(bx, DET_BTN_Y, w2, DET_BTN_H, "opts");

    if (in) {
        if (hit_sync && ui_hit(in, x, DET_BTN_Y, w1, DET_BTN_H))  *hit_sync = true;
        if (hit_opts && ui_hit(in, bx, DET_BTN_Y, w2, DET_BTN_H)) *hit_opts = true;
    }
}

// --------------------------------------------------------------------------
// vista de juegos
// --------------------------------------------------------------------------

int scr_games(const scr_ctx_t *c, const ui_input_t *in,
              const scr_game_t *g, int n, int sel, int top,
              const char *policy_label, bool *retap,
              bool *hit_sync, bool *hit_opts)
{
    // La tarjeta elegida crece un poco. Solo crece el cristal, no el contenido:
    // asi el icono y el nombre no bailan al moverse por la rejilla.
    static float lift = 0.0f;
    lift = ui_approach(lift, 1.0f, 14.0f);

    int tapped = -1;
    if (retap) *retap = false;

    // 1. Todo el cristal de la vista en un solo lote. Cuesta lo mismo una
    //    tarjeta que trece, porque el coste esta en los cambios de destino.
    ui_glass_begin();

    for (int i = top; i < top + GRID_PAGE && i < n; i++) {
        int x, y;
        card_pos(i, top, &x, &y);
        bool s = (i == sel);
        int grow = s ? (int)(5 * lift) : 0;
        ui_glass_add(x - grow, y - grow, CARD_W + grow * 2, CARD_H + grow * 2,
                     20 + grow / 2, s ? &GLASS_CARD_HI : &GLASS_CARD);
    }

    ui_glass_add(DET_X, DET_Y, DET_W, DET_H, 28, &GLASS_PANEL);
    ui_glass_end();

    ui_debug_box(DET_X, DET_Y, DET_W, DET_H, "detalle");

    // 2. El contenido encima.
    for (int i = top; i < top + GRID_PAGE && i < n; i++) {
        int x, y;
        card_pos(i, top, &x, &y);
        card_content(&g[i], x, y, i == sel);
        ui_debug_box(x, y, CARD_W, CARD_H, "");

        if (ui_hit(in, x, y, CARD_W, CARD_H)) {
            tapped = i;
            if (retap) *retap = (i == sel);
        }
    }

    // 3. El anillo de la elegida, por encima de las tarjetas vecinas: dibujado
    //    antes, la de al lado lo tapaba a medias y parecia un parpadeo.
    {
        int x, y;
        card_pos(sel, top, &x, &y);
        int grow = (int)(5 * lift);
        ui_rect_round_outline(x - grow, y - grow, CARD_W + grow * 2,
                              CARD_H + grow * 2, 20 + grow / 2, 2,
                              ui_alpha(c->accent, 210));
    }

    if (sel >= 0 && sel < n)
        detail_content(c, &g[sel], policy_label, in, hit_sync, hit_opts);

    // 4. Paginas.
    if (n > GRID_PAGE) {
        int pages = (n + GRID_PAGE - 1) / GRID_PAGE;
        int page  = top / GRID_PAGE;
        int total = pages * 8 + (pages - 1) * 8;
        int px = GRID_X + (GRID_R - GRID_X - total) / 2;
        for (int i = 0; i < pages; i++) {
            bool on = i == page;
            ui_rect_round(px, BODY_B + 10, on ? 20 : 8, 6, 3,
                          on ? c->accent : ui_alpha(COL_DIM, 90));
            px += (on ? 20 : 8) + 8;
        }
    }

    return tapped;
}

void scr_games_empty(const scr_ctx_t *c)
{
    (void)c;
    int cx = CONTENT_X + CONTENT_W / 2;

    ui_glass(cx - 260, BODY_Y + 130, 520, 190, 28, &GLASS_PANEL);
    ui_text_center(cx, BODY_Y + 178, 22, COL_TEXT,
                   "Este perfil no tiene partidas guardadas.");
    ui_text_center(cx, BODY_Y + 214, 16, ui_alpha(COL_DIM, 190),
                   "Juega un rato y vuelve, o cambia de perfil.");
}

// --------------------------------------------------------------------------
// listas
// --------------------------------------------------------------------------
//
// Las tres listas comparten forma: primero se acumula el cristal de todas las
// filas visibles y luego se pinta el contenido. Si se hiciera fila a fila,
// cada una seria un lote y se irian los cambios de destino en tonterias.

typedef struct { int x, y, w, h, first, count; } list_t;

static list_t list_layout(int row_h, int gap, int n, int sel)
{
    list_t L;
    L.x = CONTENT_X;
    L.w = CONTENT_W;
    L.y = BODY_Y;
    L.h = row_h;

    int visible = (BODY_B - BODY_Y + gap) / (row_h + gap);
    if (visible < 1) visible = 1;

    L.first = 0;
    if (sel >= visible) L.first = sel - visible + 1;
    if (L.first > n - visible) L.first = n - visible;
    if (L.first < 0) L.first = 0;

    L.count = n - L.first < visible ? n - L.first : visible;
    return L;
}

static void list_glass(const list_t *L, int gap, int sel)
{
    ui_glass_begin();
    for (int i = 0; i < L->count; i++) {
        int idx = L->first + i;
        ui_glass_add(L->x, L->y + i * (L->h + gap), L->w, L->h, 20,
                     idx == sel ? &GLASS_CARD_HI : &GLASS_CARD);
    }
    ui_glass_end();
}

// Barra de acento en el canto izquierdo de la fila elegida. Sobre cristal la
// diferencia de tinte sola no basta para saber donde estas.
static void row_marker(int x, int y, int h, color_t accent)
{
    ui_rect_round(x + 8, y + 14, 4, h - 28, 2, accent);
    ui_glow(x + 10, y + h / 2, 24, h / 2, accent, 90);
}

// Marca de "en uso": una pastilla con el acento.
static void tag_in_use(const scr_ctx_t *c, int rx, int cy, const char *text)
{
    int w = ui_text_w(14, text) + 28;
    ui_rect_round(rx - w, cy - 16, w, 32, 16, ui_alpha(c->accent, 55));
    ui_rect_round_outline(rx - w, cy - 16, w, 32, 16, 1, ui_alpha(c->accent, 140));
    ui_text_center(rx - w / 2, cy - 9, 14, c->accent, "%s", text);
}

static void list_footer(int shown, int total, int sel)
{
    if (total <= shown) return;
    ui_text_right(CONTENT_R, BODY_B + 6, 14, ui_alpha(COL_DIM, 160),
                  "%d de %d", sel + 1, total);
}

int scr_users(const scr_ctx_t *c, const ui_input_t *in,
              const scr_user_t *u, int n, int sel, int in_use)
{
    const int gap = 12;
    list_t L = list_layout(86, gap, n, sel);
    list_glass(&L, gap, sel);

    int tapped = -1;
    for (int i = 0; i < L.count; i++) {
        int idx = L.first + i;
        int y = L.y + i * (L.h + gap);
        const scr_user_t *v = &u[idx];

        SDL_Texture *av = v->icon ? ui_image(v->icon, v->icon_size) : NULL;
        if (av) ui_image_round(av, L.x + 22, y + 15, 56, 56, 18);
        else    ui_rect_round(L.x + 22, y + 15, 56, 56, 18, ui_alpha(COL_TEXT, 30));

        if (idx == sel) row_marker(L.x, y, L.h, c->accent);

        ui_text_clip_sh(L.x + 96, y + 20, 20, L.w - 340, COL_TEXT, v->name);
        ui_text_sh(L.x + 96, y + 48, 14, v->shared ? COL_OK : ui_alpha(COL_DIM, 200),
                   "%s", v->shared ? "Se sincroniza" : "No se comparte");

        if (idx == in_use) tag_in_use(c, L.x + L.w - 24, y + L.h / 2, "en uso");

        ui_debug_box(L.x, y, L.w, L.h, "");
        if (ui_hit(in, L.x, y, L.w, L.h)) tapped = idx;
    }

    list_footer(L.count, n, sel);
    return tapped;
}

int scr_hosts(const scr_ctx_t *c, const ui_input_t *in,
              const scr_host_t *h, int n, int sel, int in_use)
{
    const int gap = 12;
    if (n == 0) {
        ui_glass(CONTENT_X, BODY_Y, CONTENT_W, 96, 20, &GLASS_CARD);
        ui_text(CONTENT_X + 32, BODY_Y + 36, 18, ui_alpha(COL_DIM, 200),
                "Ninguno todavia. Pulsa Y para buscar en la red.");
        return -1;
    }

    list_t L = list_layout(80, gap, n, sel);
    list_glass(&L, gap, sel);

    int tapped = -1;
    for (int i = 0; i < L.count; i++) {
        int idx = L.first + i;
        int y = L.y + i * (L.h + gap);
        const scr_host_t *v = &h[idx];

        char sub[192];
        snprintf(sub, sizeof(sub), "%s:%u%s%s", v->ip, v->port,
                 v->emu && v->emu[0] ? "   ·   " : "", v->emu ? v->emu : "");

        if (idx == sel) row_marker(L.x, y, L.h, c->accent);

        ui_rect_round(L.x + 24, y + 22, 36, 36, 12, ui_alpha(COL_TEXT, 24));
        glyph_pc(L.x + 30, y + 28, 24, ui_alpha(COL_TEXT, 190));

        ui_text_clip_sh(L.x + 76, y + 17, 20, L.w - 320, COL_TEXT,
                        v->name && v->name[0] ? v->name : v->ip);
        ui_text_clip_sh(L.x + 76, y + 44, 14, L.w - 320, ui_alpha(COL_DIM, 205), sub);

        if (idx == in_use) tag_in_use(c, L.x + L.w - 24, y + L.h / 2, "en uso");

        ui_debug_box(L.x, y, L.w, L.h, "");
        if (ui_hit(in, L.x, y, L.w, L.h)) tapped = idx;
    }

    list_footer(L.count, n, sel);
    return tapped;
}

int scr_rows(const scr_ctx_t *c, const ui_input_t *in,
             const scr_row_t *r, int n, int sel, int top, int visible)
{
    const int gap = 10, h = 70;

    if (top > n - visible) top = n - visible;
    if (top < 0) top = 0;

    list_t L = { CONTENT_X, BODY_Y, CONTENT_W, h, top,
                 n - top < visible ? n - top : visible };
    list_glass(&L, gap, sel);

    int tapped = -1;
    for (int i = 0; i < L.count; i++) {
        int idx = L.first + i;
        int y = L.y + i * (h + gap);
        const scr_row_t *v = &r[idx];

        // El valor manda sobre el ancho del titulo: se mide primero y lo que
        // sobra es para el texto de la izquierda. Asi nunca se pisan.
        int vmax = L.w / 2 - 48;
        int vw = v->value ? ui_text_w(18, v->value) : 0;
        if (vw > vmax) vw = vmax;

        int tmax = L.w - 56 - vw - 24;

        if (idx == sel) row_marker(L.x, y, h, c->accent);

        ui_text_clip_sh(L.x + 30, y + (v->help && v->help[0] ? 13 : (h - 24) / 2),
                        20, tmax, COL_TEXT, v->label);
        if (v->help && v->help[0])
            ui_text_clip_sh(L.x + 30, y + 41, 14, tmax, ui_alpha(COL_DIM, 200), v->help);

        if (v->value && v->value[0])
            ui_text_clip(L.x + L.w - 26 - vw, y + (h - 24) / 2, 18, vmax,
                         v->vcolor, v->value);

        ui_debug_box(L.x, y, L.w, L.h, "");
        if (ui_hit(in, L.x, y, L.w, h)) tapped = idx;
    }

    list_footer(L.count, n, sel);
    return tapped;
}

// --------------------------------------------------------------------------
// hojas
// --------------------------------------------------------------------------

// El velo va antes del cristal y fuera de la capa que se escala: si se
// escalara con la hoja dejaria una franja sin oscurecer en los bordes.
void scr_scrim(float in)
{
    if (in <= 0.0f) return;
    if (in > 1.0f) in = 1.0f;
    ui_rect(0, 0, UI_W, UI_H, (color_t){ 0, 0, 0, (u8)(150 * in) });
    ui_debug_reset();
}

void scr_sheet(const scr_ctx_t *c, int w, int h, int *out_x, int *out_y)
{
    (void)c;
    int x = CONTENT_X + (CONTENT_W - w) / 2;
    int y = (UI_H - h) / 2;

    // Como el desenfoque sale del fondo y no de la pantalla, la hoja queda mas
    // clara que lo que la rodea, que es justo lo que hace que se lea como algo
    // que esta delante del velo.
    ui_glass(x, y, w, h, 30, &GLASS_SHEET);
    ui_debug_box(x, y, w, h, "hoja");

    *out_x = x;
    *out_y = y;
}

int scr_dialog(const scr_ctx_t *c, const ui_input_t *in,
               const char *heading, const char *name, const char *body,
               const scr_choice_t *ch, color_t tint, float anim)
{
    const int w = 920, h = 396;
    int x, y;

    scr_scrim(anim);
    ui_layer_begin();
    scr_sheet(c, w, h, &x, &y);

    // Un punto de color y un halo bastan para decir de que tipo es el aviso.
    // La franja de lado a lado gritaba demasiado para un dialogo.
    ui_glow(x + w / 2, y + 40, 190, 40, tint, 110);
    ui_circle(x + w / 2, y + 26, 5, tint);

    ui_text_clip_center(x + w / 2, y + 42, 27, w - 100, COL_TEXT, heading);
    if (name && name[0])
        ui_text_clip_center(x + w / 2, y + 82, 20, w - 140, tint, name);

    const int by = y + h - 110, bh = 82;
    const int bw = (w - 64 - 2 * 14) / 3;

    // El cuerpo se centra en el hueco que queda entre el titulo y los botones,
    // que ya estan colocados. No puede empujarlos ni salirse de la hoja.
    {
        const int top = y + 118, bottom = by - 18;
        const int tw = w - 260;
        int th = ui_text_wrap_h(18, tw, 25, body ? body : "");
        int ty = top + (bottom - top - th) / 2;
        if (ty < top) ty = top;
        ui_text_wrap_center(x + w / 2, ty, 18, tw, 25,
                            ui_alpha(COL_TEXT, 205), body ? body : "");
    }

    int hit = -1;
    for (int i = 0; i < 3; i++) {
        int bx = x + 32 + i * (bw + 14);
        bool hot = (i == 0);

        if (hot) ui_glow(bx + bw / 2, by + bh / 2, bw / 2, 40, tint, 70);
        ui_rect_round(bx, by, bw, bh, 18,
                      hot ? ui_alpha(tint, 52) : ui_alpha(COL_TEXT, 22));
        ui_rect_round_outline(bx, by, bw, bh, 18, hot ? 2 : 1,
                              hot ? tint : ui_alpha(COL_TEXT, 55));

        ui_text_clip_center(bx + bw / 2, by + 18, 18, bw - 24, COL_TEXT,
                            ch[i].title);
        (void)0;
        if (ch[i].sub && ch[i].sub[0]) {
            // Sobre el cristal claro de la hoja, el gris se perdia del todo.
            char sub[96];
            snprintf(sub, sizeof(sub), "%s", ch[i].sub);
            ui_text_clip_center(bx + bw / 2, by + 46, 14, bw - 24,
                                ui_alpha(COL_TEXT, 190), sub);
        }

        // La letra del boton, en su esquina.
        ui_rect_round(bx + 10, by + 10, 22, 22, 11, ui_alpha(COL_TEXT, 40));
        ui_text_center(bx + 21, by + 13, 14, COL_TEXT, "%s", ch[i].key);

        ui_debug_box(bx, by, bw, bh, "");
        if (ui_hit(in, bx, by, bw, bh)) hit = i;
    }

    ui_layer_end(anim, SHEET_SCALE(anim), x + w / 2, y + h / 2, 0, SHEET_RISE(anim));
    return hit;
}

// --------------------------------------------------------------------------
// hoja de sincronizacion
// --------------------------------------------------------------------------

void scr_sync(const scr_ctx_t *c, const char *title, const char *now,
              const char (*log)[160], int log_n,
              float progress, bool finished, float anim)
{
    const int w = 950, h = 512;
    int x, y;

    scr_scrim(anim);
    ui_layer_begin();
    scr_sheet(c, w, h, &x, &y);

    const int pad = 40;
    const int ring_r = 46;
    const int rcx = x + w - pad - ring_r, rcy = y + 44 + ring_r;

    ui_text_sh(x + pad, y + 28, 27, COL_TEXT, "%s", title);

    ui_glow(rcx, rcy, ring_r + 22, ring_r + 22, c->accent, finished ? 130 : 80);
    ui_ring(rcx, rcy, ring_r, 6, progress, c->accent, ui_alpha(COL_TEXT, 45));
    if (progress >= 0.0f)
        ui_text_center(rcx, rcy - 12, 20, COL_TEXT, "%d%%", (int)(progress * 100));
    else
        ui_spinner(rcx, rcy, 14, ui_alpha(c->accent, 210));

    // El nombre del juego en curso no puede llegar al anillo.
    int text_w = w - 2 * pad - (ring_r * 2 + 28);
    if (now && now[0])
        ui_text_clip(x + pad, y + 74, 20, text_w, c->accent, now);

    ui_rect(x + pad, y + 112, w - 2 * pad, 1, ui_alpha(COL_DIM, 55));

    int ly = y + 130;
    const int line_h = 27;
    const int log_bottom = y + h - 96;

    for (int i = 0; i < log_n && ly + line_h <= log_bottom; i++) {
        // Las lineas viejas se apagan, para que la vista caiga en lo ultimo.
        // Las lineas viejas se apagan, pero sin llegar a ser ilegibles.
        u8 a = (u8)(150 + 105 * (i + 1) / (log_n > 0 ? log_n : 1));
        ui_text_clip_sh(x + pad, ly, 18, w - 2 * pad, ui_alpha(COL_TEXT, a), log[i]);
        ly += line_h;
    }

    if (finished) {
        const char *msg = "A   Volver";
        int bw = ui_text_w(18, msg) + 72, bh = 52;
        int bx = x + (w - bw) / 2;
        ui_glow(bx + bw / 2, y + h - 62 + bh / 2, bw / 2, 32, c->accent, 90);
        ui_rect_round(bx, y + h - 62 - 6, bw, bh, 16, c->accent);
        ui_text_center(bx + bw / 2, y + h - 62 + 8, 18, COL_BG, "%s", msg);
    } else {
        ui_spinner(x + pad + 9, y + h - 46, 9, ui_alpha(c->accent, 200));
        ui_text(x + pad + 30, y + h - 56, 16, ui_alpha(COL_DIM, 200),
                "No apagues la consola");
    }

    ui_layer_end(anim, SHEET_SCALE(anim), x + w / 2, y + h / 2, 0, SHEET_RISE(anim));
}

// --------------------------------------------------------------------------
// hoja: opciones de un juego
// --------------------------------------------------------------------------

// Devuelve la fila tocada, o -1.
int scr_game_opts(const scr_ctx_t *c, const ui_input_t *in, const scr_game_t *g,
                  const char *policy, bool excluded, int sel, float anim)
{
    const int w = 820, h = 352;
    int x, y;

    scr_scrim(anim);
    ui_layer_begin();
    scr_sheet(c, w, h, &x, &y);

    const int pad = 34;
    const int icon = 92;

    SDL_Texture *t = g->icon ? ui_image(g->icon, g->icon_size) : NULL;
    if (t) ui_image_round(t, x + pad, y + 30, icon, icon, 18);
    else   ui_rect_round(x + pad, y + 30, icon, icon, 18, ui_alpha(COL_TEXT, 26));

    const int tx = x + pad + icon + 20;
    const int tw = w - (tx - x) - pad;

    ui_text_clip_sh(tx, y + 34, 22, tw, COL_TEXT, g->name);
    if (g->author && g->author[0])
        ui_text_clip_sh(tx, y + 66, 14, tw, ui_alpha(COL_DIM, 215), g->author);
    ui_text_sh(tx, y + 90, 14, ui_alpha(COL_TEXT, 165), "%016llX",
               (unsigned long long)g->title_id);

    const scr_row_t rows[2] = {
        { "Ante un conflicto en este juego", "En Preguntar usa el ajuste general",
          policy, c->accent },
        { "Excluir de la sincronizacion", "Se salta siempre, tambien en automatico",
          excluded ? "Excluido" : "Se sincroniza", excluded ? COL_WARN : COL_OK },
    };

    int hit = -1;
    int ry = y + 152;
    const int rw = w - 2 * pad, rh = 78;

    for (int i = 0; i < 2; i++) {
        bool on = i == sel;

        ui_rect_round(x + pad, ry, rw, rh, 18,
                      ui_alpha(COL_TEXT, on ? 34 : 16));
        if (on) {
            ui_rect_round_outline(x + pad, ry, rw, rh, 18, 1, ui_alpha(c->accent, 170));
            ui_rect_round(x + pad + 10, ry + 16, 4, rh - 32, 2, c->accent);
        }

        // El valor manda sobre el ancho del titulo, como en las listas.
        int vmax = rw / 2 - 40;
        int vw = ui_text_w(18, rows[i].value);
        if (vw > vmax) vw = vmax;

        ui_text_clip_sh(x + pad + 26, ry + 16, 19, rw - 52 - vw - 20,
                        COL_TEXT, rows[i].label);
        ui_text_clip_sh(x + pad + 26, ry + 44, 14, rw - 52 - vw - 20,
                        ui_alpha(COL_DIM, 205), rows[i].help);
        ui_text_clip(x + pad + rw - 26 - vw, ry + 27, 18, vmax,
                     rows[i].vcolor, rows[i].value);

        ui_debug_box(x + pad, ry, rw, rh, "");
        if (ui_hit(in, x + pad, ry, rw, rh)) hit = i;
        ry += rh + 12;
    }

    ui_layer_end(anim, SHEET_SCALE(anim), x + w / 2, y + h / 2, 0, SHEET_RISE(anim));
    return hit;
}

// --------------------------------------------------------------------------
// hoja: ajustes del PC
// --------------------------------------------------------------------------

int scr_pc_cfg(const scr_ctx_t *c, const ui_input_t *in, const char *server,
               const scr_row_t *rows, int n, int sel, bool ok, float anim)
{
    const int w = 980, h = 560;
    int x, y;

    scr_scrim(anim);
    ui_layer_begin();
    scr_sheet(c, w, h, &x, &y);

    const int pad = 36;

    ui_text_sh(x + pad, y + 26, 27, COL_TEXT, "Ajustes del PC");
    ui_text_sh(x + pad, y + 62, 14, ui_alpha(COL_DIM, 205), "%s",
               server && server[0] ? server : "daemon");

    if (!ok || n == 0) {
        ui_text_clip_center(x + w / 2, y + h / 2 - 12, 20, w - 120,
                            ui_alpha(COL_TEXT, 215),
                            "No se pudieron leer los ajustes del PC.");
        ui_text_clip_center(x + w / 2, y + h / 2 + 18, 16, w - 120,
                            ui_alpha(COL_DIM, 200), "Y para reintentar, B para volver");
        ui_layer_end(anim, SHEET_SCALE(anim), x + w / 2, y + h / 2, 0, SHEET_RISE(anim));
        return -1;
    }

    // Cuantas filas caben de verdad entre la cabecera y el pie de la hoja.
    const int top = y + 100, bottom = y + h - 62;
    // 70 y no 74: con 74 solo entraban cuatro filas y sobraba un hueco de
    // setenta pixeles al fondo de la hoja.
    const int rh = 70, gap = 10;
    int visible = (bottom - top + gap) / (rh + gap);
    if (visible < 1) visible = 1;

    int first = 0;
    if (sel >= visible) first = sel - visible + 1;
    if (first > n - visible) first = n - visible;
    if (first < 0) first = 0;

    int hit = -1;
    int ry = top;
    const int rw = w - 2 * pad;

    for (int i = first; i < n && i < first + visible; i++) {
        bool on = i == sel;

        ui_rect_round(x + pad, ry, rw, rh, 17, ui_alpha(COL_TEXT, on ? 34 : 15));
        if (on) {
            ui_rect_round_outline(x + pad, ry, rw, rh, 17, 1, ui_alpha(c->accent, 170));
            ui_rect_round(x + pad + 10, ry + 15, 4, rh - 30, 2, c->accent);
        }

        int vmax = rw / 2 - 40;
        int vw = rows[i].value ? ui_text_w(18, rows[i].value) : 0;
        if (vw > vmax) vw = vmax;
        int tmax = rw - 52 - vw - 20;

        ui_text_clip_sh(x + pad + 26, ry + (rows[i].help && rows[i].help[0] ? 14 : 26),
                        19, tmax, COL_TEXT, rows[i].label);
        if (rows[i].help && rows[i].help[0])
            ui_text_clip_sh(x + pad + 26, ry + 42, 14, tmax,
                            ui_alpha(COL_DIM, 200), rows[i].help);
        if (rows[i].value)
            ui_text_clip(x + pad + rw - 26 - vw, ry + 25, 18, vmax,
                         rows[i].vcolor, rows[i].value);

        ui_debug_box(x + pad, ry, rw, rh, "");
        if (ui_hit(in, x + pad, ry, rw, rh)) hit = i;
        ry += rh + gap;
    }

    ui_text_sh(x + pad, y + h - 46, 14, ui_alpha(COL_DIM, 200),
               n > visible ? "%d de %d   ·   B volver" : "B volver", sel + 1, n);

    ui_layer_end(anim, SHEET_SCALE(anim), x + w / 2, y + h / 2, 0, SHEET_RISE(anim));
    return hit;
}
