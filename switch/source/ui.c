#include "ui.h"

#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL2_gfxPrimitives.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

static u32 g_last_ticks;
static float g_dt;

static SDL_Window   *g_win;
static SDL_Renderer *g_ren;
static PadState      g_pad;

// --------------------------------------------------------------------------
// fuentes
// --------------------------------------------------------------------------
//
// La consola trae varias fuentes del sistema y ninguna las cubre todas: la
// estandar no tiene kanji y la china no tiene los simbolos de Nintendo. Para
// que un nombre como "妖怪ウォッチ4++" no salga en cuadraditos, cargamos varias
// y al escribir se parte el texto en tramos segun que fuente tenga cada glifo.

#define FONT_KINDS 5
#define SIZE_SLOTS 6

static const PlSharedFontType FONT_ORDER[FONT_KINDS] = {
    PlSharedFontType_Standard,
    PlSharedFontType_NintendoExt,
    PlSharedFontType_ChineseSimplified,
    PlSharedFontType_ChineseTraditional,
    PlSharedFontType_KO,
};

static PlFontData g_fontdata[FONT_KINDS];
static int        g_fontdata_n;

static struct {
    int       size;
    TTF_Font *f[FONT_KINDS];
} g_fonts[SIZE_SLOTS];
static int g_fonts_n;

static TTF_Font **fonts_for(int size)
{
    for (int i = 0; i < g_fonts_n; i++)
        if (g_fonts[i].size == size) return g_fonts[i].f;

    if (g_fonts_n >= SIZE_SLOTS) return g_fonts[0].f;   // se acabaron los huecos

    int slot = g_fonts_n;
    g_fonts[slot].size = size;

    for (int k = 0; k < g_fontdata_n; k++) {
        SDL_RWops *rw = SDL_RWFromConstMem(g_fontdata[k].address, g_fontdata[k].size);
        g_fonts[slot].f[k] = rw ? TTF_OpenFontRW(rw, 1, size) : NULL;
    }
    for (int k = g_fontdata_n; k < FONT_KINDS; k++) g_fonts[slot].f[k] = NULL;

    g_fonts_n++;
    return g_fonts[slot].f;
}

// Decodifica un codepoint UTF-8. Avanza *i.
static u32 utf8_next(const char *s, size_t *i)
{
    const u8 *p = (const u8 *)s + *i;
    u32 c = p[0];

    if (c < 0x80)             { *i += 1; return c; }
    if ((c & 0xE0) == 0xC0)   { *i += 2; return ((c & 0x1F) << 6) | (p[1] & 0x3F); }
    if ((c & 0xF0) == 0xE0)   { *i += 3; return ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); }
    if ((c & 0xF8) == 0xF0)   { *i += 4; return ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); }

    *i += 1;
    return 0xFFFD;
}

// Que fuente tiene este glifo. Si ninguna, la primera (dibujara el hueco).
static int font_for_cp(TTF_Font **set, u32 cp)
{
    for (int k = 0; k < FONT_KINDS; k++)
        if (set[k] && TTF_GlyphIsProvided32(set[k], cp)) return k;
    return 0;
}

// Recorre el texto llamando a `emit` por cada tramo homogeneo de fuente.
// Se usa dos veces por cadena: una para medir y otra para pintar.
static void for_each_run(const char *s, TTF_Font **set,
                         void (*emit)(TTF_Font *, const char *, void *), void *ud)
{
    size_t i = 0;
    while (s[i]) {
        size_t start = i;
        u32 cp = utf8_next(s, &i);
        int kind = font_for_cp(set, cp);

        // Extendemos el tramo mientras la fuente no cambie.
        size_t end = i;
        while (s[end]) {
            size_t probe = end;
            u32 next = utf8_next(s, &probe);
            if (font_for_cp(set, next) != kind) break;
            end = probe;
        }

        char buf[512];
        size_t len = end - start;
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        memcpy(buf, s + start, len);
        buf[len] = '\0';

        if (set[kind]) emit(set[kind], buf, ud);
        i = end;
    }
}

// --------------------------------------------------------------------------
// cache de texto
// --------------------------------------------------------------------------
//
// Rasterizar cada cadena en cada fotograma se nota enseguida con una lista de
// juegos llena, asi que guardamos las texturas ya montadas.

#define TCACHE 192

typedef struct {
    char         key[160];
    SDL_Texture *tex;
    int          w, h;
    u32          used;
} tcache_t;

static tcache_t g_tc[TCACHE];
static u32      g_frame;

typedef struct { int w, h; } measure_t;

static void emit_measure(TTF_Font *f, const char *run, void *ud)
{
    measure_t *m = ud;
    int w = 0, h = 0;
    TTF_SizeUTF8(f, run, &w, &h);
    m->w += w;
    if (h > m->h) m->h = h;
}

typedef struct { int x; SDL_Color col; } draw_ctx_t;

static void emit_draw(TTF_Font *f, const char *run, void *ud)
{
    draw_ctx_t *d = ud;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(f, run, d->col);
    if (!surf) return;

    SDL_Texture *t = SDL_CreateTextureFromSurface(g_ren, surf);
    if (t) {
        SDL_Rect dst = { d->x, 0, surf->w, surf->h };
        SDL_RenderCopy(g_ren, t, NULL, &dst);
        SDL_DestroyTexture(t);
    }
    d->x += surf->w;
    SDL_FreeSurface(surf);
}

static SDL_Texture *render_string(const char *s, int size, color_t c, int *out_w, int *out_h)
{
    TTF_Font **set = fonts_for(size);

    measure_t m = { 0, 0 };
    for_each_run(s, set, emit_measure, &m);
    if (m.w <= 0 || m.h <= 0) return NULL;

    SDL_Texture *target = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA8888,
                                            SDL_TEXTUREACCESS_TARGET, m.w, m.h);
    if (!target) return NULL;

    SDL_SetTextureBlendMode(target, SDL_BLENDMODE_BLEND);
    SDL_SetRenderTarget(g_ren, target);
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 0);
    SDL_RenderClear(g_ren);

    draw_ctx_t d = { 0, { c.r, c.g, c.b, c.a } };
    for_each_run(s, set, emit_draw, &d);

    SDL_SetRenderTarget(g_ren, NULL);

    *out_w = m.w;
    *out_h = m.h;
    return target;
}

static tcache_t *text_entry(const char *s, int size, color_t c)
{
    char key[160];
    snprintf(key, sizeof(key), "%d|%02X%02X%02X|%s", size, c.r, c.g, c.b, s);

    // Hash sencillo con sondeo lineal.
    u32 h = 2166136261u;
    for (const char *p = key; *p; p++) h = (h ^ (u8)*p) * 16777619u;

    int victim = -1;
    u32 oldest = 0xFFFFFFFFu;

    for (int probe = 0; probe < 8; probe++) {
        int idx = (int)((h + probe) % TCACHE);
        tcache_t *e = &g_tc[idx];

        if (e->tex && !strcmp(e->key, key)) { e->used = g_frame; return e; }
        if (!e->tex) { victim = idx; break; }
        if (e->used < oldest) { oldest = e->used; victim = idx; }
    }

    tcache_t *e = &g_tc[victim];
    if (e->tex) SDL_DestroyTexture(e->tex);

    e->tex = render_string(s, size, c, &e->w, &e->h);
    snprintf(e->key, sizeof(e->key), "%s", key);
    e->used = g_frame;
    return e;
}

// --------------------------------------------------------------------------
// arranque
// --------------------------------------------------------------------------

bool ui_init(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) return false;
    if (TTF_Init() != 0) return false;
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);

    g_win = SDL_CreateWindow("NX Save Sync", 0, 0, UI_W, UI_H, 0);
    if (!g_win) return false;

    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) return false;

    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);

    if (R_SUCCEEDED(plInitialize(PlServiceType_User))) {
        for (int i = 0; i < FONT_KINDS; i++) {
            if (R_SUCCEEDED(plGetSharedFontByType(&g_fontdata[g_fontdata_n], FONT_ORDER[i])))
                g_fontdata_n++;
        }
    }
    if (g_fontdata_n == 0) return false;

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);

    return true;
}

void ui_exit(void)
{
    ui_images_clear();

    for (int i = 0; i < TCACHE; i++)
        if (g_tc[i].tex) { SDL_DestroyTexture(g_tc[i].tex); g_tc[i].tex = NULL; }

    for (int i = 0; i < g_fonts_n; i++)
        for (int k = 0; k < FONT_KINDS; k++)
            if (g_fonts[i].f[k]) TTF_CloseFont(g_fonts[i].f[k]);

    plExit();
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    IMG_Quit();
    TTF_Quit();
    SDL_Quit();
}

// --------------------------------------------------------------------------
// fotograma
// --------------------------------------------------------------------------

bool ui_frame_begin(ui_input_t *in)
{
    if (!appletMainLoop()) return false;

    u32 now = SDL_GetTicks();
    if (g_last_ticks) {
        g_dt = (float)(now - g_last_ticks) / 1000.0f;
        if (g_dt > 0.1f) g_dt = 0.1f;   // tras una pausa larga, sin saltos raros
        if (g_dt <= 0.0f) g_dt = 1.0f / 60.0f;
    }
    g_last_ticks = now;

    g_frame++;
    memset(in, 0, sizeof(*in));

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            return false;
        case SDL_FINGERDOWN:
            in->touched = true;
            in->touch_x = (int)(ev.tfinger.x * UI_W);
            in->touch_y = (int)(ev.tfinger.y * UI_H);
            break;
        case SDL_FINGERMOTION:
            in->touched = true;
            in->touch_x = (int)(ev.tfinger.x * UI_W);
            in->touch_y = (int)(ev.tfinger.y * UI_H);
            break;
        case SDL_FINGERUP:
            in->tap   = true;
            in->tap_x = (int)(ev.tfinger.x * UI_W);
            in->tap_y = (int)(ev.tfinger.y * UI_H);
            break;
        }
    }

    padUpdate(&g_pad);
    in->down = padGetButtonsDown(&g_pad);
    in->held = padGetButtons(&g_pad);

    return true;
}

void ui_frame_end(void)
{
    SDL_RenderPresent(g_ren);
}

// --------------------------------------------------------------------------
// primitivas
// --------------------------------------------------------------------------

static void setcol(color_t c)
{
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
}

void ui_clear(color_t c)
{
    setcol(c);
    SDL_RenderClear(g_ren);
}

void ui_rect(int x, int y, int w, int h, color_t c)
{
    setcol(c);
    SDL_Rect r = { x, y, w, h };
    SDL_RenderFillRect(g_ren, &r);
}

void ui_rect_outline(int x, int y, int w, int h, int thick, color_t c)
{
    for (int i = 0; i < thick; i++) {
        setcol(c);
        SDL_Rect r = { x + i, y + i, w - 2 * i, h - 2 * i };
        SDL_RenderDrawRect(g_ren, &r);
    }
}

// Esquinas redondeadas a base de rectangulos y un cuarto de circulo por esquina.
void ui_rect_round(int x, int y, int w, int h, int r, color_t c)
{
    if (r <= 0) { ui_rect(x, y, w, h, c); return; }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;

    ui_rect(x + r, y, w - 2 * r, h, c);
    ui_rect(x, y + r, r, h - 2 * r, c);
    ui_rect(x + w - r, y + r, r, h - 2 * r, c);

    setcol(c);
    for (int dy = 0; dy < r; dy++) {
        int dx = (int)(sqrt((double)(r * r - (r - dy) * (r - dy))) + 0.5);
        SDL_RenderDrawLine(g_ren, x + r - dx, y + dy, x + w - r + dx - 1, y + dy);
        SDL_RenderDrawLine(g_ren, x + r - dx, y + h - 1 - dy, x + w - r + dx - 1, y + h - 1 - dy);
    }
}

// --------------------------------------------------------------------------
// texto
// --------------------------------------------------------------------------

static void draw_entry(tcache_t *e, int x, int y)
{
    if (!e || !e->tex) return;
    SDL_Rect dst = { x, y, e->w, e->h };
    SDL_RenderCopy(g_ren, e->tex, NULL, &dst);
}

void ui_text(int x, int y, int size, color_t c, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    draw_entry(text_entry(buf, size, c), x, y);
}

void ui_text_center(int cx, int y, int size, color_t c, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    tcache_t *e = text_entry(buf, size, c);
    if (e) draw_entry(e, cx - e->w / 2, y);
}

int ui_text_w(int size, const char *s)
{
    tcache_t *e = text_entry(s, size, COL_TEXT);
    return e ? e->w : 0;
}

void ui_text_clip(int x, int y, int size, int max_w, color_t c, const char *s)
{
    if (ui_text_w(size, s) <= max_w) {
        ui_text(x, y, size, c, "%s", s);
        return;
    }

    // Recortamos por caracteres completos, no por bytes, para no partir un
    // UTF-8 por la mitad y dejar basura en pantalla.
    char buf[512];
    size_t i = 0, last_ok = 0;
    while (s[i] && i < sizeof(buf) - 8) {
        size_t next = i;
        utf8_next(s, &next);

        memcpy(buf, s, next);
        strcpy(buf + next, "...");
        if (ui_text_w(size, buf) > max_w) break;

        last_ok = next;
        i = next;
    }

    memcpy(buf, s, last_ok);
    strcpy(buf + last_ok, "...");
    ui_text(x, y, size, c, "%s", buf);
}

// --------------------------------------------------------------------------
// imagenes
// --------------------------------------------------------------------------

#define ICACHE 96

static struct {
    const void  *src;
    SDL_Texture *tex;
    color_t      avg;
} g_ic[ICACHE];
static int g_ic_n;

// Color medio del icono, saturado un poco para que sirva de acento. Se muestrea
// en rejilla en vez de pixel a pixel: con iconos de 256x256 la diferencia no se
// ve y evita recorrer 65k pixeles al cargar cada juego.
static color_t average_color(SDL_Surface *surf)
{
    color_t out = COL_ACCENT;
    if (!surf || !surf->format) return out;

    SDL_Surface *conv = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_RGBA32, 0);
    if (!conv) return out;

    u64 r = 0, g = 0, b = 0, n = 0;
    const int step = 8;

    SDL_LockSurface(conv);
    for (int y = 0; y < conv->h; y += step) {
        u8 *row = (u8 *)conv->pixels + y * conv->pitch;
        for (int x = 0; x < conv->w; x += step) {
            u8 *px = row + x * 4;
            // Los pixeles muy oscuros no aportan tono y ensucian la media.
            if (px[0] + px[1] + px[2] < 90) continue;
            r += px[0]; g += px[1]; b += px[2];
            n++;
        }
    }
    SDL_UnlockSurface(conv);
    SDL_FreeSurface(conv);

    if (n == 0) return out;

    float fr = (float)r / n, fg = (float)g / n, fb = (float)b / n;

    // Subimos saturacion y fijamos el brillo para que siempre contraste con el
    // fondo oscuro, salga de un icono claro o de uno apagado.
    float mx = fr > fg ? (fr > fb ? fr : fb) : (fg > fb ? fg : fb);
    float mn = fr < fg ? (fr < fb ? fr : fb) : (fg < fb ? fg : fb);
    float mid = (mx + mn) * 0.5f;

    fr = mid + (fr - mid) * 1.7f;
    fg = mid + (fg - mid) * 1.7f;
    fb = mid + (fb - mid) * 1.7f;

    float lum = (fr * 0.30f + fg * 0.59f + fb * 0.11f);
    float want = 175.0f;
    float k = lum > 1.0f ? want / lum : 1.0f;
    fr *= k; fg *= k; fb *= k;

    out.r = (u8)(fr < 0 ? 0 : fr > 255 ? 255 : fr);
    out.g = (u8)(fg < 0 ? 0 : fg > 255 ? 255 : fg);
    out.b = (u8)(fb < 0 ? 0 : fb > 255 ? 255 : fb);
    out.a = 0xFF;
    return out;
}

SDL_Texture *ui_image(const void *jpeg, size_t len)
{
    if (!jpeg || len == 0) return NULL;

    for (int i = 0; i < g_ic_n; i++)
        if (g_ic[i].src == jpeg) return g_ic[i].tex;

    if (g_ic_n >= ICACHE) return NULL;

    SDL_RWops *rw = SDL_RWFromConstMem(jpeg, (int)len);
    if (!rw) return NULL;

    SDL_Surface *surf = IMG_Load_RW(rw, 1);
    if (!surf) return NULL;

    SDL_Texture *t = SDL_CreateTextureFromSurface(g_ren, surf);

    g_ic[g_ic_n].src = jpeg;
    g_ic[g_ic_n].tex = t;
    g_ic[g_ic_n].avg = average_color(surf);
    g_ic_n++;

    SDL_FreeSurface(surf);
    return t;
}

color_t ui_image_color(const void *jpeg, size_t len)
{
    for (int i = 0; i < g_ic_n; i++)
        if (g_ic[i].src == jpeg) return g_ic[i].avg;

    // Aun no cargado: forzamos la carga, que ya calcula el color.
    if (ui_image(jpeg, len)) {
        for (int i = 0; i < g_ic_n; i++)
            if (g_ic[i].src == jpeg) return g_ic[i].avg;
    }
    return COL_ACCENT;
}

void ui_image_draw(SDL_Texture *t, int x, int y, int w, int h)
{
    if (!t) return;
    SDL_Rect dst = { x, y, w, h };
    SDL_RenderCopy(g_ren, t, NULL, &dst);
}

// Sin stencil no hay recorte redondeado de verdad, asi que pintamos la imagen y
// tapamos las esquinas. El color debe ser el del panel de detras: usar siempre
// el fondo global dejaba una muesca oscura alrededor de cada icono.
void ui_image_draw_round(SDL_Texture *t, int x, int y, int w, int h, int r, color_t bg)
{
    ui_image_draw(t, x, y, w, h);
    if (r <= 0) return;

    setcol(bg);
    for (int dy = 0; dy < r; dy++) {
        int dx = (int)(sqrt((double)(r * r - (r - dy) * (r - dy))) + 0.5);
        SDL_RenderDrawLine(g_ren, x, y + dy, x + r - dx - 1, y + dy);
        SDL_RenderDrawLine(g_ren, x + w - r + dx, y + dy, x + w - 1, y + dy);
        SDL_RenderDrawLine(g_ren, x, y + h - 1 - dy, x + r - dx - 1, y + h - 1 - dy);
        SDL_RenderDrawLine(g_ren, x + w - r + dx, y + h - 1 - dy, x + w - 1, y + h - 1 - dy);
    }
}

void ui_images_clear(void)
{
    for (int i = 0; i < g_ic_n; i++)
        if (g_ic[i].tex) SDL_DestroyTexture(g_ic[i].tex);
    memset(g_ic, 0, sizeof(g_ic));
    g_ic_n = 0;
}

// --------------------------------------------------------------------------
// utilidades
// --------------------------------------------------------------------------

bool ui_hit(const ui_input_t *in, int x, int y, int w, int h)
{
    return in->tap && in->tap_x >= x && in->tap_x < x + w
                   && in->tap_y >= y && in->tap_y < y + h;
}

void ui_spinner(int cx, int cy, int r, color_t c)
{
    double t = SDL_GetTicks() / 1000.0;
    setcol(c);
    for (int i = 0; i < 10; i++) {
        double a = t * 4.0 + i * 0.35;
        int alpha = 255 - i * 22;
        SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, (u8)(alpha < 0 ? 0 : alpha));
        int x = cx + (int)(cos(a) * r);
        int y = cy + (int)(sin(a) * r);
        SDL_Rect dot = { x - 3, y - 3, 6, 6 };
        SDL_RenderFillRect(g_ren, &dot);
    }
}

u32 ui_ticks(void)
{
    return SDL_GetTicks();
}

// --------------------------------------------------------------------------
// animacion y adornos
// --------------------------------------------------------------------------

float ui_dt(void) { return g_dt; }

float ui_approach(float cur, float target, float rate)
{
    // Interpolacion exponencial: independiente de los fotogramas por segundo,
    // asi que se ve igual acoplada a la tele que en portatil.
    float k = 1.0f - expf(-rate * g_dt);
    return cur + (target - cur) * k;
}

color_t ui_mix(color_t a, color_t b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    color_t o;
    o.r = (u8)(a.r + (b.r - a.r) * t);
    o.g = (u8)(a.g + (b.g - a.g) * t);
    o.b = (u8)(a.b + (b.b - a.b) * t);
    o.a = (u8)(a.a + (b.a - a.a) * t);
    return o;
}

color_t ui_alpha(color_t c, u8 a)
{
    c.a = a;
    return c;
}

void ui_gradient_v(int x, int y, int w, int h, color_t top, color_t bottom)
{
    for (int i = 0; i < h; i++) {
        color_t c = ui_mix(top, bottom, (float)i / (h > 1 ? h - 1 : 1));
        SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
        SDL_RenderDrawLine(g_ren, x, y + i, x + w - 1, y + i);
    }
}

void ui_rect_round_outline(int x, int y, int w, int h, int r, int thick, color_t c)
{
    for (int i = 0; i < thick; i++) {
        // Un anillo por capa; suficiente para el grosor de 2-4 px que usamos.
        ui_rect(x + r, y + i, w - 2 * r, 1, c);
        ui_rect(x + r, y + h - 1 - i, w - 2 * r, 1, c);
        ui_rect(x + i, y + r, 1, h - 2 * r, c);
        ui_rect(x + w - 1 - i, y + r, 1, h - 2 * r, c);
    }

    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
    for (int a = 0; a <= 90; a += 3) {
        float rad = a * (float)M_PI / 180.0f;
        for (int i = 0; i < thick; i++) {
            int rr = r - i;
            int dx = (int)(cosf(rad) * rr), dy = (int)(sinf(rad) * rr);
            SDL_RenderDrawPoint(g_ren, x + r - dx, y + r - dy);
            SDL_RenderDrawPoint(g_ren, x + w - r + dx - 1, y + r - dy);
            SDL_RenderDrawPoint(g_ren, x + r - dx, y + h - r + dy - 1);
            SDL_RenderDrawPoint(g_ren, x + w - r + dx - 1, y + h - r + dy - 1);
        }
    }
}

void ui_ring(int cx, int cy, int radius, int thick, float progress,
             color_t fg, color_t bg)
{
    const int STEPS = 180;

    // Fondo completo
    SDL_SetRenderDrawColor(g_ren, bg.r, bg.g, bg.b, bg.a);
    for (int i = 0; i < STEPS; i++) {
        float a = (float)i / STEPS * 2.0f * (float)M_PI;
        for (int t = 0; t < thick; t++) {
            int rr = radius - t;
            SDL_RenderDrawPoint(g_ren, cx + (int)(cosf(a) * rr), cy + (int)(sinf(a) * rr));
        }
    }

    float from, to;
    if (progress < 0.0f) {
        // Indeterminado: un arco que da vueltas.
        float t = SDL_GetTicks() / 1000.0f * 1.6f;
        from = t;
        to   = t + 1.6f;
    } else {
        if (progress > 1.0f) progress = 1.0f;
        from = -(float)M_PI / 2.0f;
        to   = from + progress * 2.0f * (float)M_PI;
    }

    SDL_SetRenderDrawColor(g_ren, fg.r, fg.g, fg.b, fg.a);
    int steps = (int)((to - from) / (2.0f * (float)M_PI) * STEPS);
    for (int i = 0; i <= steps; i++) {
        float a = from + (to - from) * ((float)i / (steps > 0 ? steps : 1));
        for (int t = 0; t < thick; t++) {
            int rr = radius - t;
            SDL_RenderDrawPoint(g_ren, cx + (int)(cosf(a) * rr), cy + (int)(sinf(a) * rr));
        }
    }
}

void ui_gradient_round_top(int x, int y, int w, int h, int r,
                           color_t top, color_t bottom)
{
    for (int i = 0; i < h; i++) {
        color_t c = ui_mix(top, bottom, (float)i / (h > 1 ? h - 1 : 1));
        SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);

        // Las primeras `r` filas se estrechan siguiendo el arco de la esquina,
        // asi el degradado no pinta un rectangulo sobre el panel redondeado.
        int inset = 0;
        if (i < r) {
            int dy = r - i;
            inset = r - (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);
        }
        SDL_RenderDrawLine(g_ren, x + inset, y + i, x + w - 1 - inset, y + i);
    }
}

// --------------------------------------------------------------------------
// logo
// --------------------------------------------------------------------------

void ui_arc(int cx, int cy, int radius, int thick, float from_deg, float to_deg, color_t c)
{
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);

    float from = from_deg * (float)M_PI / 180.0f;
    float to   = to_deg   * (float)M_PI / 180.0f;

    // Un punto por cada ~0.6 px de arco exterior: suficiente para que no se vea
    // punteado y sin gastar de mas.
    int steps = (int)(fabsf(to - from) * radius / 0.6f) + 8;

    for (int i = 0; i <= steps; i++) {
        float a = from + (to - from) * ((float)i / steps);
        float ca = cosf(a), sa = sinf(a);
        for (int t = 0; t < thick; t++) {
            int rr = radius - t;
            SDL_RenderDrawPoint(g_ren, cx + (int)(ca * rr), cy + (int)(sa * rr));
        }
    }
}

void ui_tri(int x1, int y1, int x2, int y2, int x3, int y3, color_t c)
{
    filledTrigonRGBA(g_ren, (Sint16)x1, (Sint16)y1, (Sint16)x2, (Sint16)y2,
                     (Sint16)x3, (Sint16)y3, c.r, c.g, c.b, c.a);
    aatrigonRGBA(g_ren, (Sint16)x1, (Sint16)y1, (Sint16)x2, (Sint16)y2,
                 (Sint16)x3, (Sint16)y3, c.r, c.g, c.b, c.a);
}

void ui_circle(int cx, int cy, int r, color_t c)
{
    filledCircleRGBA(g_ren, (Sint16)cx, (Sint16)cy, (Sint16)r, c.r, c.g, c.b, c.a);
    aacircleRGBA(g_ren, (Sint16)cx, (Sint16)cy, (Sint16)r, c.r, c.g, c.b, c.a);
}

// Punta de flecha al final de un arco: eje tangente para apuntar, radial para
// el ancho. Es la misma construccion que usa el generador del icono.
static void arc_head(int cx, int cy, int radius, int thick, float at_deg, color_t c)
{
    float a = at_deg * (float)M_PI / 180.0f;
    float px = cx + radius * cosf(a), py = cy + radius * sinf(a);
    float tx = -sinf(a), ty = cosf(a);
    float rx = cosf(a),  ry = sinf(a);

    float len = thick * 1.35f, half = thick * 1.05f;

    ui_tri((int)(px + tx * len),  (int)(py + ty * len),
           (int)(px + rx * half), (int)(py + ry * half),
           (int)(px - rx * half), (int)(py - ry * half), c);
}

void ui_logo(int cx, int cy, int size, color_t c1, color_t c2, color_t dot)
{
    int r     = size * 27 / 100;
    int thick = size * 9 / 100;
    if (thick < 2) thick = 2;

    ui_arc(cx, cy, r, thick, 195.0f, 350.0f, c1);
    arc_head(cx, cy, r, thick, 350.0f, c1);

    ui_arc(cx, cy, r, thick, 15.0f, 170.0f, c2);
    arc_head(cx, cy, r, thick, 170.0f, c2);

    int rr = size * 5 / 100;
    if (rr < 2) rr = 2;
    ui_circle(cx, cy, rr, dot);
}
