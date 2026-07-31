// Motor de dibujo: cristal liquido.
//
// La idea de la que sale todo lo demas: el fondo se dibuja primero en una
// textura aparte, se reduce a una cadena de miniaturas y de ahi sale el
// desenfoque. Cada panel copia el trozo de fondo que le toca, lo tine, le doble
// la luz en el borde y se recorta con una mascara alfa suavizada.
//
// Es decir, el cristal ensena lo que tiene detras de verdad. No es un color
// pintado que lo imita, y por eso cambia solo cuando cambia el fondo.

#include "ui.h"

#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL2_gfxPrimitives.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

// --------------------------------------------------------------------------
// compatibilidad con el PC
// --------------------------------------------------------------------------
//
// El mismo ui.c compila para la consola y para el previsualizador del PC. Lo
// unico que cambia es de donde salen las fuentes y los botones. Poder ver la
// interfaz sin pasar por la SD es lo que hace que los fallos de colocacion se
// arreglen en minutos en vez de a ciegas.

#ifdef __SWITCH__
#  define HAVE_PL 1
#else
#  define HAVE_PL 0
#endif

static SDL_Window   *g_win;
static SDL_Renderer *g_ren;

#if HAVE_PL
static PadState g_pad;
#endif

// --------------------------------------------------------------------------
// estado
// --------------------------------------------------------------------------

static u32   g_last_ticks;
static float g_dt;
static u32   g_t0;

// Capas del fotograma.
static SDL_Texture *g_scene;        // el fondo tal cual
static SDL_Texture *g_mip[4];       // 640x360, 320x180, 160x90, 80x45
static SDL_Texture *g_blur_soft;    // 320x180, desenfoque corto
static SDL_Texture *g_blur_deep;    // 320x180, desenfoque largo
static SDL_Texture *g_scratch_a;    // contenido del cristal, sin recortar
static SDL_Texture *g_scratch_b;    // el mismo, ya recortado
static SDL_Texture *g_layer;        // capa que se vuelca con opacidad y escala

// Destino logico del dibujo: la pantalla, o una capa si hay una abierta. Todo
// lo que antes volvia a NULL vuelve aqui, o el cristal se saldria de la capa.
static SDL_Texture *g_target;

#define BLUR_W 320
#define BLUR_H 180
#define BLUR_K (UI_W / BLUR_W)      // 4

// Piezas reutilizables.
static SDL_Texture *g_glow;         // mancha radial blanca
static SDL_Texture *g_vig;          // vineta
static SDL_Texture *g_noise;        // grano

static bool g_in_backdrop;

const glass_t GLASS_PANEL   = { { 0xAF,0xBE,0xD6,0xFF }, 30, 26, 12, 40, 62, 90, 1 };
const glass_t GLASS_CARD    = { { 0xAF,0xBE,0xD6,0xFF }, 34, 24, 10,  0, 46, 70, 0 };
const glass_t GLASS_CARD_HI = { { 0xD6,0xE6,0xFF,0xFF }, 46, 40, 10, 26, 132, 120, 0 };
const glass_t GLASS_BAR     = { { 0xAF,0xBE,0xD6,0xFF }, 38, 30, 11, 30, 70, 78, 1 };
const glass_t GLASS_SHEET   = { { 0xB6,0xC6,0xE0,0xFF }, 40, 34, 13, 46, 86, 160, 1 };

// --------------------------------------------------------------------------
// fuentes
// --------------------------------------------------------------------------
//
// La consola trae varias fuentes y ninguna las cubre todas: la estandar no
// tiene kanji y la china no tiene los simbolos de Nintendo. Para que un nombre
// como "妖怪ウォッチ4++" no salga en cuadraditos, se cargan varias y al escribir
// se parte el texto en tramos segun que fuente tenga cada glifo.

#define FONT_KINDS 5
// Un hueco por cada tamano de letra que se use. Se quedo corto en 10 y el
// sintoma no se parecia a la causa: a partir del decimo tamano, fonts_for
// devolvia el primero, asi que los titulos salian con el cuerpo del texto.
#define SIZE_SLOTS 16

#if HAVE_PL
static const PlSharedFontType FONT_ORDER[FONT_KINDS] = {
    PlSharedFontType_Standard,
    PlSharedFontType_NintendoExt,
    PlSharedFontType_ChineseSimplified,
    PlSharedFontType_ChineseTraditional,
    PlSharedFontType_KO,
};
static PlFontData g_fontdata[FONT_KINDS];
#else
// En el PC basta una fuente: el previsualizador es para ver la colocacion.
static struct { void *address; size_t size; } g_fontdata[FONT_KINDS];
#endif

static int g_fontdata_n;

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

static int font_for_cp(TTF_Font **set, u32 cp)
{
    for (int k = 0; k < FONT_KINDS; k++)
        if (set[k] && TTF_GlyphIsProvided32(set[k], cp)) return k;
    return 0;
}

// Recorre el texto llamando a `emit` por cada tramo homogeneo de fuente.
static void for_each_run(const char *s, TTF_Font **set,
                         void (*emit)(TTF_Font *, const char *, void *), void *ud)
{
    size_t i = 0;
    while (s[i]) {
        size_t start = i;
        u32 cp = utf8_next(s, &i);
        int kind = font_for_cp(set, cp);

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

#define TCACHE 320

typedef struct {
    char         key[176];
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

    SDL_Texture *prev = SDL_GetRenderTarget(g_ren);

    SDL_Texture *target = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32,
                                            SDL_TEXTUREACCESS_TARGET, m.w, m.h);
    if (!target) return NULL;

    SDL_SetTextureBlendMode(target, SDL_BLENDMODE_BLEND);
    SDL_SetRenderTarget(g_ren, target);
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 0);
    SDL_RenderClear(g_ren);

    draw_ctx_t d = { 0, { c.r, c.g, c.b, c.a } };
    for_each_run(s, set, emit_draw, &d);

    // Volver al destino anterior, no a la pantalla: el texto tambien se dibuja
    // dentro de las capas del cristal.
    SDL_SetRenderTarget(g_ren, prev);

    *out_w = m.w;
    *out_h = m.h;
    return target;
}

static tcache_t *text_entry(const char *s, int size, color_t c)
{
    char key[176];
    snprintf(key, sizeof(key), "%d|%02X%02X%02X|%s", size, c.r, c.g, c.b, s);

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
// esquinas suavizadas
// --------------------------------------------------------------------------
//
// Un cuarto de disco con el borde difuminado, una textura por radio. Todos los
// redondeos de la interfaz salen de aqui: pintar el arco a mano dejaba escalones
// que en una pantalla de 6 pulgadas se ven perfectamente.

// Una entrada por radio distinto. Se quedo en 20 y la interfaz usa mas: al
// llenarse, corner_for devolvia la primera y los radios tardios se dibujaban
// con la curva de otro. El sintoma era una barra con los extremos vacios.
#define CORNERS 64

static struct { int r; SDL_Texture *t; } g_corner[CORNERS];
static int g_corner_n;

// Y las mismas esquinas huecas, para los contornos.
#define RINGS 40
static struct { int r, t; SDL_Texture *tex; } g_ring[RINGS];
static int g_ring_n;

static SDL_Texture *corner_for(int r)
{
    if (r < 1) return NULL;

    for (int i = 0; i < g_corner_n; i++)
        if (g_corner[i].r == r) return g_corner[i].t;

    // Si aun asi se llenara, el radio mas parecido desentona mucho menos que
    // el primero que entro.
    if (g_corner_n >= CORNERS) {
        int best = 0;
        for (int i = 1; i < g_corner_n; i++)
            if (abs(g_corner[i].r - r) < abs(g_corner[best].r - r)) best = i;
        return g_corner[best].t;
    }

    u8 *px = malloc((size_t)r * r * 4);
    if (!px) return NULL;

    for (int y = 0; y < r; y++) {
        for (int x = 0; x < r; x++) {
            // Distancia al centro del arco, que esta en la esquina interior.
            float dx = (float)r - (x + 0.5f);
            float dy = (float)r - (y + 0.5f);
            float d  = sqrtf(dx * dx + dy * dy);

            // Cobertura del pixel: 1 dentro, 0 fuera, y el paso de uno a otro
            // repartido en un pixel. Eso es todo el suavizado que hace falta.
            float cov = (float)r - d + 0.5f;
            if (cov < 0.0f) cov = 0.0f;
            if (cov > 1.0f) cov = 1.0f;

            u8 *p = px + ((size_t)y * r + x) * 4;
            p[0] = p[1] = p[2] = 255;
            p[3] = (u8)(cov * 255.0f + 0.5f);
        }
    }

    SDL_Texture *t = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STATIC, r, r);
    if (t) {
        SDL_UpdateTexture(t, NULL, px, r * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    }
    free(px);

    g_corner[g_corner_n].r = r;
    g_corner[g_corner_n].t = t;
    g_corner_n++;
    return t;
}

static void setcol(color_t c)
{
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
}

void ui_rect(int x, int y, int w, int h, color_t c)
{
    if (w <= 0 || h <= 0) return;
    setcol(c);
    SDL_Rect r = { x, y, w, h };
    SDL_RenderFillRect(g_ren, &r);
}

// Dibuja las cuatro esquinas de un redondeado. `mode` decide si se mezclan o se
// escriben tal cual (que es lo que necesita la mascara del cristal).
static void corners_draw(int x, int y, int w, int h, int r, color_t c,
                         SDL_BlendMode mode)
{
    SDL_Texture *t = corner_for(r);
    if (!t) return;

    SDL_SetTextureBlendMode(t, mode);
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t, c.a);

    SDL_Rect s = { 0, 0, r, r };
    SDL_Rect d;

    d = (SDL_Rect){ x, y, r, r };
    SDL_RenderCopyEx(g_ren, t, &s, &d, 0, NULL, SDL_FLIP_NONE);
    d = (SDL_Rect){ x + w - r, y, r, r };
    SDL_RenderCopyEx(g_ren, t, &s, &d, 0, NULL, SDL_FLIP_HORIZONTAL);
    d = (SDL_Rect){ x, y + h - r, r, r };
    SDL_RenderCopyEx(g_ren, t, &s, &d, 0, NULL, SDL_FLIP_VERTICAL);
    d = (SDL_Rect){ x + w - r, y + h - r, r, r };
    SDL_RenderCopyEx(g_ren, t, &s, &d, 0,
                     NULL, (SDL_RendererFlip)(SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL));

    SDL_SetTextureAlphaMod(t, 255);
    SDL_SetTextureColorMod(t, 255, 255, 255);
}

static void rect_round_mode(int x, int y, int w, int h, int r, color_t c,
                            SDL_BlendMode mode)
{
    if (w <= 0 || h <= 0) return;
    if (r <= 0) {
        SDL_BlendMode old;
        SDL_GetRenderDrawBlendMode(g_ren, &old);
        SDL_SetRenderDrawBlendMode(g_ren, mode);
        ui_rect(x, y, w, h, c);
        SDL_SetRenderDrawBlendMode(g_ren, old);
        return;
    }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 1) { ui_rect(x, y, w, h, c); return; }

    SDL_BlendMode old;
    SDL_GetRenderDrawBlendMode(g_ren, &old);
    SDL_SetRenderDrawBlendMode(g_ren, mode);

    ui_rect(x + r,         y,     w - 2 * r, h,         c);
    ui_rect(x,             y + r, r,         h - 2 * r, c);
    ui_rect(x + w - r,     y + r, r,         h - 2 * r, c);

    SDL_SetRenderDrawBlendMode(g_ren, old);

    corners_draw(x, y, w, h, r, c, mode);
}

void ui_rect_round(int x, int y, int w, int h, int r, color_t c)
{
    rect_round_mode(x, y, w, h, r, c, SDL_BLENDMODE_BLEND);
}

// --------------------------------------------------------------------------
// arranque
// --------------------------------------------------------------------------

static SDL_Texture *make_target(int w, int h)
{
    SDL_Texture *t = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_TARGET, w, h);
    if (t) {
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    }
    return t;
}

// Mancha radial blanca: la pieza con la que se pintan las luces del fondo, los
// halos y los reflejos.
static SDL_Texture *make_glow(int size)
{
    u8 *px = malloc((size_t)size * size * 4);
    if (!px) return NULL;

    float c = size * 0.5f;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float dx = (x + 0.5f - c) / c, dy = (y + 0.5f - c) / c;
            float d = sqrtf(dx * dx + dy * dy);
            float a = 1.0f - d;
            if (a < 0.0f) a = 0.0f;
            // Curva suave en vez de al cubo: al cubo la luz se concentraba en
            // un punto y el fondo quedaba negro, que es justo lo que el cristal
            // no puede ensenar.
            a = a * a * (3.0f - 2.0f * a);
            u8 *p = px + ((size_t)y * size + x) * 4;
            p[0] = p[1] = p[2] = 255;
            p[3] = (u8)(a * 255.0f);
        }
    }

    SDL_Texture *t = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STATIC, size, size);
    if (t) {
        SDL_UpdateTexture(t, NULL, px, size * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_ADD);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    }
    free(px);
    return t;
}

// Vineta: lo contrario de la mancha, negro que se cierra por los bordes.
static SDL_Texture *make_vignette(int size)
{
    u8 *px = malloc((size_t)size * size * 4);
    if (!px) return NULL;

    float c = size * 0.5f;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float dx = (x + 0.5f - c) / c, dy = (y + 0.5f - c) / c;
            float d = sqrtf(dx * dx + dy * dy) / 1.41421f;
            float a = d * d * 1.15f;
            if (a < 0.0f) a = 0.0f;
            if (a > 1.0f) a = 1.0f;
            u8 *p = px + ((size_t)y * size + x) * 4;
            p[0] = p[1] = p[2] = 0;
            p[3] = (u8)(a * 255.0f);
        }
    }

    SDL_Texture *t = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STATIC, size, size);
    if (t) {
        SDL_UpdateTexture(t, NULL, px, size * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    }
    free(px);
    return t;
}

// Grano. Sin el, el desenfoque parece un degradado pintado; con el, parece
// vidrio esmerilado. Es el detalle que mas vende el efecto y el mas barato.
static SDL_Texture *make_noise(int size)
{
    u8 *px = malloc((size_t)size * size * 4);
    if (!px) return NULL;

    u32 s = 0x9E3779B9u;
    for (int i = 0; i < size * size; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        u8 v = (u8)(s >> 24);
        u8 *p = px + (size_t)i * 4;
        p[0] = p[1] = p[2] = 255;
        p[3] = v;
    }

    SDL_Texture *t = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STATIC, size, size);
    if (t) {
        SDL_UpdateTexture(t, NULL, px, size * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeNearest);
    }
    free(px);
    return t;
}

bool ui_init(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) return false;
    if (TTF_Init() != 0) return false;
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

    g_win = SDL_CreateWindow("NX Save Sync", SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED, UI_W, UI_H, 0);
    if (!g_win) return false;

    g_ren = SDL_CreateRenderer(g_win, -1,
                               SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    if (!g_ren) return false;

    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);

    g_scene     = make_target(UI_W, UI_H);
    g_mip[0]    = make_target(UI_W / 2,  UI_H / 2);
    g_mip[1]    = make_target(UI_W / 4,  UI_H / 4);
    g_mip[2]    = make_target(UI_W / 8,  UI_H / 8);
    g_mip[3]    = make_target(UI_W / 16, UI_H / 16);
    g_blur_soft = make_target(BLUR_W, BLUR_H);
    g_blur_deep = make_target(BLUR_W, BLUR_H);
    g_scratch_a = make_target(UI_W, UI_H);
    g_scratch_b = make_target(UI_W, UI_H);
    g_layer     = make_target(UI_W, UI_H);

    if (!g_scene || !g_blur_soft || !g_blur_deep || !g_scratch_a || !g_scratch_b)
        return false;
    for (int i = 0; i < 4; i++) if (!g_mip[i]) return false;

    g_glow  = make_glow(192);
    g_vig   = make_vignette(256);
    g_noise = make_noise(128);

#if HAVE_PL
    if (R_SUCCEEDED(plInitialize(PlServiceType_User))) {
        for (int i = 0; i < FONT_KINDS; i++) {
            if (R_SUCCEEDED(plGetSharedFontByType(&g_fontdata[g_fontdata_n], FONT_ORDER[i])))
                g_fontdata_n++;
        }
    }
#else
    {
        // En el PC: una fuente del sistema, solo para ver la colocacion.
        const char *paths[] = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            NULL
        };
        for (int i = 0; paths[i]; i++) {
            FILE *f = fopen(paths[i], "rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            void *buf = malloc((size_t)n);
            if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
                g_fontdata[0].address = buf;
                g_fontdata[0].size    = (size_t)n;
                g_fontdata_n = 1;
            }
            fclose(f);
            break;
        }
    }
#endif
    if (g_fontdata_n == 0) return false;

#if HAVE_PL
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
#endif

    g_t0 = SDL_GetTicks();
    return true;
}

void ui_exit(void)
{
    ui_images_clear();

    for (int i = 0; i < TCACHE; i++)
        if (g_tc[i].tex) { SDL_DestroyTexture(g_tc[i].tex); g_tc[i].tex = NULL; }

    for (int i = 0; i < g_corner_n; i++)
        if (g_corner[i].t) SDL_DestroyTexture(g_corner[i].t);
    g_corner_n = 0;

    for (int i = 0; i < g_ring_n; i++)
        if (g_ring[i].tex) SDL_DestroyTexture(g_ring[i].tex);
    g_ring_n = 0;

    SDL_Texture *all[] = { g_scene, g_mip[0], g_mip[1], g_mip[2], g_mip[3],
                           g_blur_soft, g_blur_deep, g_scratch_a, g_scratch_b,
                           g_layer, g_glow, g_vig, g_noise };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        if (all[i]) SDL_DestroyTexture(all[i]);

    for (int i = 0; i < g_fonts_n; i++)
        for (int k = 0; k < FONT_KINDS; k++)
            if (g_fonts[i].f[k]) TTF_CloseFont(g_fonts[i].f[k]);

#if HAVE_PL
    plExit();
#endif
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    IMG_Quit();
    TTF_Quit();
    SDL_Quit();
}

// --------------------------------------------------------------------------
// fotograma
// --------------------------------------------------------------------------

#if !HAVE_PL
// Teclado del PC con la misma forma que el mando, para poder recorrer la
// interfaz en el previsualizador.
static u64 keys_now(void)
{
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    u64 b = 0;
    if (k[SDL_SCANCODE_UP])     b |= HidNpadButton_Up;
    if (k[SDL_SCANCODE_DOWN])   b |= HidNpadButton_Down;
    if (k[SDL_SCANCODE_LEFT])   b |= HidNpadButton_Left;
    if (k[SDL_SCANCODE_RIGHT])  b |= HidNpadButton_Right;
    if (k[SDL_SCANCODE_RETURN]) b |= HidNpadButton_A;
    if (k[SDL_SCANCODE_BACKSPACE]) b |= HidNpadButton_B;
    if (k[SDL_SCANCODE_X])      b |= HidNpadButton_X;
    if (k[SDL_SCANCODE_Y])      b |= HidNpadButton_Y;
    if (k[SDL_SCANCODE_Q])      b |= HidNpadButton_L;
    if (k[SDL_SCANCODE_E])      b |= HidNpadButton_R;
    if (k[SDL_SCANCODE_1])      b |= HidNpadButton_ZL;
    if (k[SDL_SCANCODE_3])      b |= HidNpadButton_ZR;
    if (k[SDL_SCANCODE_ESCAPE]) b |= HidNpadButton_Plus;
    return b;
}
static u64 g_prev_keys;
#endif

bool ui_frame_begin(ui_input_t *in)
{
#if HAVE_PL
    if (!appletMainLoop()) return false;
#endif

    u32 now = SDL_GetTicks();
    if (g_last_ticks) {
        g_dt = (float)(now - g_last_ticks) / 1000.0f;
        if (g_dt > 0.1f)  g_dt = 0.1f;    // tras una pausa larga, sin saltos raros
        if (g_dt <= 0.0f) g_dt = 1.0f / 60.0f;
    } else {
        g_dt = 1.0f / 60.0f;
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
#if !HAVE_PL
        case SDL_MOUSEBUTTONUP:
            in->tap = true;
            in->tap_x = ev.button.x;
            in->tap_y = ev.button.y;
            break;
        case SDL_MOUSEMOTION:
            in->touched = (ev.motion.state & SDL_BUTTON_LMASK) != 0;
            in->touch_x = ev.motion.x;
            in->touch_y = ev.motion.y;
            break;
#endif
        }
    }

#if HAVE_PL
    padUpdate(&g_pad);
    in->down = padGetButtonsDown(&g_pad);
    in->held = padGetButtons(&g_pad);
#else
    u64 k = keys_now();
    in->down = k & ~g_prev_keys;
    in->held = k;
    g_prev_keys = k;
#endif

    if (in->tap) ui_ripple(in->tap_x, in->tap_y, COL_TEXT);
    return true;
}

void ui_frame_end(void)
{
    SDL_RenderPresent(g_ren);
}

// --------------------------------------------------------------------------
// capas
// --------------------------------------------------------------------------

void ui_backdrop_begin(void)
{
    SDL_SetRenderTarget(g_ren, g_scene);
    g_in_backdrop = true;
}

static void blit_full(SDL_Texture *src, SDL_Texture *dst)
{
    SDL_SetRenderTarget(g_ren, dst);
    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_NONE);
    SDL_RenderCopy(g_ren, src, NULL, NULL);
    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
}

void ui_backdrop_end(void)
{
    g_in_backdrop = false;

    // Reducir de dos en dos. Cada paso promedia 2x2 de verdad; bajar de golpe a
    // un dieciseisavo con filtro bilineal se salta pixeles y el desenfoque sale
    // con moire en vez de suave.
    blit_full(g_scene,  g_mip[0]);
    blit_full(g_mip[0], g_mip[1]);
    blit_full(g_mip[1], g_mip[2]);
    blit_full(g_mip[2], g_mip[3]);

    // Y subir otra vez. El paso intermedio quita los escalones que deja estirar
    // una miniatura de 80x45 directamente al tamano de un panel.
    blit_full(g_mip[1], g_blur_soft);
    blit_full(g_mip[3], g_blur_deep);

    SDL_SetRenderTarget(g_ren, g_target);
    SDL_SetTextureBlendMode(g_scene, SDL_BLENDMODE_NONE);
    SDL_RenderCopy(g_ren, g_scene, NULL, NULL);
    SDL_SetTextureBlendMode(g_scene, SDL_BLENDMODE_BLEND);
}

// --------------------------------------------------------------------------
// capa de composicion
// --------------------------------------------------------------------------

void ui_layer_begin(void)
{
    if (!g_layer) return;

    g_target = g_layer;
    SDL_SetRenderTarget(g_ren, g_layer);

    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 0);
    SDL_RenderClear(g_ren);
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
}

void ui_layer_end(float alpha, float scale, int cx, int cy, int dx, int dy)
{
    if (!g_layer) return;

    g_target = NULL;
    SDL_SetRenderTarget(g_ren, NULL);

    if (alpha <= 0.0f) return;
    if (alpha > 1.0f) alpha = 1.0f;

    // Un punto p va a parar a c + (p - c) * escala, asi que el origen de la
    // capa cae en c - c * escala.
    SDL_Rect dst = {
        (int)(cx - cx * scale) + dx,
        (int)(cy - cy * scale) + dy,
        (int)(UI_W * scale),
        (int)(UI_H * scale),
    };

    SDL_SetTextureAlphaMod(g_layer, (u8)(alpha * 255.0f));
    SDL_RenderCopy(g_ren, g_layer, NULL, &dst);
    SDL_SetTextureAlphaMod(g_layer, 255);
}

// --------------------------------------------------------------------------
// cristal
// --------------------------------------------------------------------------

#define GLASS_MAX 24

typedef struct {
    int x, y, w, h, r;
    glass_t st;
} gpanel_t;

static gpanel_t g_gp[GLASS_MAX];
static int      g_gp_n;

void ui_glass_begin(void) { g_gp_n = 0; }

void ui_glass_add(int x, int y, int w, int h, int r, const glass_t *g)
{
    if (g_gp_n >= GLASS_MAX || w <= 0 || h <= 0) return;

    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;

    g_gp[g_gp_n++] = (gpanel_t){ x, y, w, h, r, *g };
}

// El trozo de fondo desenfocado que le toca a este panel.
static void glass_backdrop(const gpanel_t *p)
{
    SDL_Texture *bl = p->st.deep ? g_blur_deep : g_blur_soft;

    SDL_Rect src = { p->x / BLUR_K, p->y / BLUR_K,
                     (p->w + BLUR_K - 1) / BLUR_K, (p->h + BLUR_K - 1) / BLUR_K };
    SDL_Rect dst = { p->x, p->y, p->w, p->h };

    SDL_SetTextureBlendMode(bl, SDL_BLENDMODE_NONE);
    SDL_RenderCopy(g_ren, bl, &src, &dst);
    SDL_SetTextureBlendMode(bl, SDL_BLENDMODE_BLEND);
}

// Refraccion del borde: cerca del canto, el cristal arrastra hacia dentro lo
// que hay justo fuera del panel. Es lo que le da grosor; sin esto el panel
// parece una calcomania pegada sobre el fondo.
static void glass_lens(const gpanel_t *p)
{
    if (!p->st.lens) return;

    SDL_Texture *bl = g_blur_soft;
    SDL_SetTextureBlendMode(bl, SDL_BLENDMODE_BLEND);

    const int band = 14;
    const int steps = 4;

    for (int s = 0; s < steps; s++) {
        int t0 = band * s / steps, t1 = band * (s + 1) / steps;
        int th = t1 - t0;
        if (th <= 0) continue;

        // Cuanto mas cerca del canto, mas se nota.
        u8 a = (u8)(p->st.lens * (steps - s) / steps);
        SDL_SetTextureAlphaMod(bl, a);

        // Arriba: se estira hacia dentro lo que queda por encima del panel.
        SDL_Rect d = { p->x, p->y + t0, p->w, th };
        SDL_Rect s0 = { p->x / BLUR_K, (p->y - (band - t0) * 2) / BLUR_K,
                        p->w / BLUR_K, (th * 2) / BLUR_K + 1 };
        SDL_RenderCopy(g_ren, bl, &s0, &d);

        d  = (SDL_Rect){ p->x, p->y + p->h - t1, p->w, th };
        s0 = (SDL_Rect){ p->x / BLUR_K, (p->y + p->h + (band - t1)) / BLUR_K,
                         p->w / BLUR_K, (th * 2) / BLUR_K + 1 };
        SDL_RenderCopy(g_ren, bl, &s0, &d);

        d  = (SDL_Rect){ p->x + t0, p->y, th, p->h };
        s0 = (SDL_Rect){ (p->x - (band - t0) * 2) / BLUR_K, p->y / BLUR_K,
                         (th * 2) / BLUR_K + 1, p->h / BLUR_K };
        SDL_RenderCopy(g_ren, bl, &s0, &d);

        d  = (SDL_Rect){ p->x + p->w - t1, p->y, th, p->h };
        s0 = (SDL_Rect){ (p->x + p->w + (band - t1)) / BLUR_K, p->y / BLUR_K,
                         (th * 2) / BLUR_K + 1, p->h / BLUR_K };
        SDL_RenderCopy(g_ren, bl, &s0, &d);
    }

    SDL_SetTextureAlphaMod(bl, 255);
}

static void glass_content(const gpanel_t *p)
{
    SDL_Rect clip = { p->x, p->y, p->w, p->h };
    SDL_RenderSetClipRect(g_ren, &clip);

    glass_backdrop(p);
    glass_lens(p);

    // Tinte: sube el vidrio por encima del fondo para que el texto se lea.
    if (p->st.tint_a) {
        SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
        ui_rect(p->x, p->y, p->w, p->h, ui_alpha(p->st.tint, p->st.tint_a));
    }

    // Reflejo: el vidrio recoge mas luz por arriba que por abajo.
    if (p->st.sheen) {
        int gh = p->h / 2;
        if (gh > 120) gh = 120;
        ui_gradient_v(p->x, p->y, p->w, gh,
                      (color_t){ 255, 255, 255, p->st.sheen },
                      (color_t){ 255, 255, 255, 0 });
    }

    // Grano.
    if (p->st.grain && g_noise) {
        SDL_SetTextureAlphaMod(g_noise, p->st.grain);
        for (int yy = p->y; yy < p->y + p->h; yy += 128)
            for (int xx = p->x; xx < p->x + p->w; xx += 128) {
                SDL_Rect d = { xx, yy, 128, 128 };
                SDL_RenderCopy(g_ren, g_noise, NULL, &d);
            }
        SDL_SetTextureAlphaMod(g_noise, 255);
    }

    SDL_RenderSetClipRect(g_ren, NULL);
}

// Sombra proyectada: tres capas cada vez mas grandes y mas tenues. No es un
// desenfoque de verdad, pero a estas opacidades no se distingue.
static void glass_shadow(const gpanel_t *p)
{
    if (!p->st.shadow) return;
    for (int i = 3; i >= 1; i--) {
        int g = i * 5;
        u8 a = (u8)(p->st.shadow / (i * 2 + 1));
        ui_rect_round(p->x - g, p->y - g + 7, p->w + 2 * g, p->h + 2 * g,
                      p->r + g, (color_t){ 0, 0, 0, a });
    }
}

// Contorno luminoso. Arriba y a la izquierda entra la luz, asi que ese canto
// brilla; el de abajo apenas se insinua. Esa asimetria es la que hace que el
// panel parezca tener volumen.
static void glass_rim(const gpanel_t *p)
{
    if (!p->st.rim) return;

    const int r = p->r;
    const u8  a = p->st.rim;

    // Contorno completo y tenue: define el canto por los cuatro lados.
    ui_rect_round_outline(p->x, p->y, p->w, p->h, r, 1,
                          (color_t){ 255, 255, 255, (u8)(a / 3) });

    // Y encima el brillo especular, arriba, que se apaga hacia los extremos.
    // Cortado en seco se veia el final de la linea y delataba el truco; asi
    // parece luz resbalando por el borde.
    const int x0 = p->x + r / 2, x1 = p->x + p->w - r / 2;
    const int n = 28;

    for (int i = 0; i < n; i++) {
        float t = (float)i / n;
        float k = sinf(t * (float)M_PI);
        int sx = x0 + (int)((x1 - x0) * t);
        int sw = (x1 - x0) / n + 1;

        ui_rect(sx, p->y,     sw, 1, (color_t){ 255, 255, 255, (u8)(a * k) });
        ui_rect(sx, p->y + 1, sw, 1, (color_t){ 255, 255, 255, (u8)(a * k * 0.28f) });
    }

    // El canto izquierdo recoge algo menos de luz, y solo en su mitad alta.
    const int y0 = p->y + r / 2, y1 = p->y + p->h / 2;
    for (int i = 0; i < 14; i++) {
        float t = (float)i / 14;
        float k = (1.0f - t) * 0.55f;
        int sy = y0 + (int)((y1 - y0) * t);
        ui_rect(p->x, sy, 1, (y1 - y0) / 14 + 1,
                (color_t){ 255, 255, 255, (u8)(a * k) });
    }
}

void ui_glass_end(void)
{
    if (g_gp_n == 0) return;

    // 1. Sombras, sobre lo que ya haya en el destino (pantalla o capa).
    SDL_SetRenderTarget(g_ren, g_target);
    for (int i = 0; i < g_gp_n; i++) glass_shadow(&g_gp[i]);

    // 2. El contenido del cristal, todavia rectangular.
    SDL_SetRenderTarget(g_ren, g_scratch_a);
    for (int i = 0; i < g_gp_n; i++) glass_content(&g_gp[i]);

    // 3. La mascara: el mismo panel en blanco, con las esquinas suavizadas. Y
    //    encima el contenido en modo multiplicar, que respeta el alfa que ya
    //    hay. Asi el recorte redondo es de verdad y funciona sobre cualquier
    //    cosa, tambien encima de otro panel.
    SDL_SetRenderTarget(g_ren, g_scratch_b);
    SDL_SetTextureBlendMode(g_scratch_a, SDL_BLENDMODE_MOD);

    for (int i = 0; i < g_gp_n; i++) {
        gpanel_t *p = &g_gp[i];
        SDL_Rect rc = { p->x, p->y, p->w, p->h };

        SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 0);
        SDL_RenderFillRect(g_ren, &rc);

        rect_round_mode(p->x, p->y, p->w, p->h, p->r,
                        (color_t){ 255, 255, 255, 255 }, SDL_BLENDMODE_NONE);

        SDL_RenderSetClipRect(g_ren, &rc);
        SDL_RenderCopy(g_ren, g_scratch_a, &rc, &rc);
        SDL_RenderSetClipRect(g_ren, NULL);
    }

    SDL_SetTextureBlendMode(g_scratch_a, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);

    // 4. Al destino.
    SDL_SetRenderTarget(g_ren, g_target);
    for (int i = 0; i < g_gp_n; i++) {
        gpanel_t *p = &g_gp[i];
        SDL_Rect rc = { p->x, p->y, p->w, p->h };
        SDL_RenderCopy(g_ren, g_scratch_b, &rc, &rc);
    }

    // 5. Los cantos.
    for (int i = 0; i < g_gp_n; i++) glass_rim(&g_gp[i]);

    g_gp_n = 0;
}

void ui_glass(int x, int y, int w, int h, int r, const glass_t *g)
{
    ui_glass_begin();
    ui_glass_add(x, y, w, h, r, g);
    ui_glass_end();
}

// --------------------------------------------------------------------------
// primitivas
// --------------------------------------------------------------------------

void ui_clear(color_t c)
{
    setcol(c);
    SDL_RenderClear(g_ren);
}

void ui_rect_outline(int x, int y, int w, int h, int thick, color_t c)
{
    for (int i = 0; i < thick; i++) {
        setcol(c);
        SDL_Rect r = { x + i, y + i, w - 2 * i, h - 2 * i };
        SDL_RenderDrawRect(g_ren, &r);
    }
}

// Anillo de esquina: la misma idea que corner_for, pero hueco por dentro. Se
// cachea por (radio, grosor) porque los contornos usan siempre los mismos.

static SDL_Texture *ring_corner_for(int r, int thick)
{
    if (r < 1 || thick < 1) return NULL;

    for (int i = 0; i < g_ring_n; i++)
        if (g_ring[i].r == r && g_ring[i].t == thick) return g_ring[i].tex;

    if (g_ring_n >= RINGS) {
        int best = 0;
        for (int i = 1; i < g_ring_n; i++)
            if (abs(g_ring[i].r - r) < abs(g_ring[best].r - r)) best = i;
        return g_ring[best].tex;
    }

    u8 *px = malloc((size_t)r * r * 4);
    if (!px) return NULL;

    for (int y = 0; y < r; y++) {
        for (int x = 0; x < r; x++) {
            float dx = (float)r - (x + 0.5f), dy = (float)r - (y + 0.5f);
            float d  = sqrtf(dx * dx + dy * dy);

            float out = (float)r - d + 0.5f;             // dentro del borde exterior
            float in  = (float)(r - thick) - d + 0.5f;   // dentro del interior
            if (out < 0.0f) out = 0.0f;
            if (out > 1.0f) out = 1.0f;
            if (in  < 0.0f) in  = 0.0f;
            if (in  > 1.0f) in  = 1.0f;

            float cov = out - in;                        // solo el anillo
            if (cov < 0.0f) cov = 0.0f;

            u8 *p = px + ((size_t)y * r + x) * 4;
            p[0] = p[1] = p[2] = 255;
            p[3] = (u8)(cov * 255.0f + 0.5f);
        }
    }

    SDL_Texture *t = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STATIC, r, r);
    if (t) {
        SDL_UpdateTexture(t, NULL, px, r * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    }
    free(px);

    g_ring[g_ring_n].r = r; g_ring[g_ring_n].t = thick; g_ring[g_ring_n].tex = t;
    g_ring_n++;
    return t;
}

void ui_rect_round_outline(int x, int y, int w, int h, int r, int thick, color_t c)
{
    if (w <= 0 || h <= 0 || thick <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;

    for (int i = 0; i < thick; i++) {
        ui_rect(x + r,         y + i,         w - 2 * r, 1, c);
        ui_rect(x + r,         y + h - 1 - i, w - 2 * r, 1, c);
        ui_rect(x + i,         y + r,         1, h - 2 * r, c);
        ui_rect(x + w - 1 - i, y + r,         1, h - 2 * r, c);
    }

    if (r <= 0) return;

    SDL_Texture *t = ring_corner_for(r, thick);
    if (!t) return;

    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t, c.a);

    SDL_Rect s = { 0, 0, r, r }, d;
    d = (SDL_Rect){ x, y, r, r };
    SDL_RenderCopyEx(g_ren, t, &s, &d, 0, NULL, SDL_FLIP_NONE);
    d = (SDL_Rect){ x + w - r, y, r, r };
    SDL_RenderCopyEx(g_ren, t, &s, &d, 0, NULL, SDL_FLIP_HORIZONTAL);
    d = (SDL_Rect){ x, y + h - r, r, r };
    SDL_RenderCopyEx(g_ren, t, &s, &d, 0, NULL, SDL_FLIP_VERTICAL);
    d = (SDL_Rect){ x + w - r, y + h - r, r, r };
    SDL_RenderCopyEx(g_ren, t, &s, &d, 0, NULL,
                     (SDL_RendererFlip)(SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL));

    SDL_SetTextureColorMod(t, 255, 255, 255);
    SDL_SetTextureAlphaMod(t, 255);
}

void ui_gradient_v(int x, int y, int w, int h, color_t top, color_t bottom)
{
    if (h <= 0) return;
    for (int i = 0; i < h; i++) {
        color_t c = ui_mix(top, bottom, (float)i / (h > 1 ? h - 1 : 1));
        SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
        SDL_RenderDrawLine(g_ren, x, y + i, x + w - 1, y + i);
    }
}

void ui_gradient_round_top(int x, int y, int w, int h, int r,
                           color_t top, color_t bottom)
{
    for (int i = 0; i < h; i++) {
        color_t c = ui_mix(top, bottom, (float)i / (h > 1 ? h - 1 : 1));
        SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);

        int inset = 0;
        if (i < r) {
            int dy = r - i;
            inset = r - (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);
        }
        SDL_RenderDrawLine(g_ren, x + inset, y + i, x + w - 1 - inset, y + i);
    }
}

void ui_circle(int cx, int cy, int r, color_t c)
{
    filledCircleRGBA(g_ren, (Sint16)cx, (Sint16)cy, (Sint16)r, c.r, c.g, c.b, c.a);
    aacircleRGBA(g_ren, (Sint16)cx, (Sint16)cy, (Sint16)r, c.r, c.g, c.b, c.a);
}

void ui_tri(int x1, int y1, int x2, int y2, int x3, int y3, color_t c)
{
    filledTrigonRGBA(g_ren, (Sint16)x1, (Sint16)y1, (Sint16)x2, (Sint16)y2,
                     (Sint16)x3, (Sint16)y3, c.r, c.g, c.b, c.a);
    aatrigonRGBA(g_ren, (Sint16)x1, (Sint16)y1, (Sint16)x2, (Sint16)y2,
                 (Sint16)x3, (Sint16)y3, c.r, c.g, c.b, c.a);
}

void ui_arc(int cx, int cy, int radius, int thick, float from_deg, float to_deg, color_t c)
{
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);

    float from = from_deg * (float)M_PI / 180.0f;
    float to   = to_deg   * (float)M_PI / 180.0f;

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

void ui_glow(int cx, int cy, int rx, int ry, color_t c, u8 a)
{
    if (!g_glow || a == 0) return;
    SDL_SetTextureColorMod(g_glow, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(g_glow, a);
    SDL_Rect d = { cx - rx, cy - ry, rx * 2, ry * 2 };
    SDL_RenderCopy(g_ren, g_glow, NULL, &d);
    SDL_SetTextureColorMod(g_glow, 255, 255, 255);
    SDL_SetTextureAlphaMod(g_glow, 255);
}

void ui_vignette(u8 a)
{
    if (!g_vig || a == 0) return;
    SDL_SetTextureAlphaMod(g_vig, a);
    SDL_RenderCopy(g_ren, g_vig, NULL, NULL);
    SDL_SetTextureAlphaMod(g_vig, 255);
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

// Sombra bajo el texto. Sobre cristal el fondo cambia bajo cada letra, asi que
// sin esto hay palabras que se pierden segun lo que pase por detras.
static void text_shadow(const char *s, int x, int y, int size)
{
    tcache_t *e = text_entry(s, size, (color_t){ 0, 0, 0, 255 });
    if (!e || !e->tex) return;
    SDL_SetTextureAlphaMod(e->tex, 90);
    draw_entry(e, x, y + 1);
    draw_entry(e, x + 1, y + 1);
    SDL_SetTextureAlphaMod(e->tex, 255);
}

void ui_text_sh(int x, int y, int size, color_t c, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    text_shadow(buf, x, y, size);
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

void ui_text_right(int rx, int y, int size, color_t c, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    tcache_t *e = text_entry(buf, size, c);
    if (e) draw_entry(e, rx - e->w, y);
}

int ui_text_w(int size, const char *s)
{
    tcache_t *e = text_entry(s, size, COL_TEXT);
    return e ? e->w : 0;
}

int ui_text_h(int size)
{
    TTF_Font **set = fonts_for(size);
    return set[0] ? TTF_FontHeight(set[0]) : size;
}

// Deja en `out` el texto recortado con puntos suspensivos si no cabe.
static void clip_to(char *out, size_t out_size, int size, int max_w, const char *s)
{
    if (ui_text_w(size, s) <= max_w) {
        snprintf(out, out_size, "%s", s);
        return;
    }

    // Por caracteres completos, no por bytes: cortar un UTF-8 por la mitad deja
    // basura en pantalla.
    size_t i = 0, last_ok = 0;
    char buf[512];

    while (s[i] && i < sizeof(buf) - 8) {
        size_t next = i;
        utf8_next(s, &next);
        if (next >= sizeof(buf) - 8) break;

        memcpy(buf, s, next);
        strcpy(buf + next, "...");
        if (ui_text_w(size, buf) > max_w) break;

        last_ok = next;
        i = next;
    }

    memcpy(buf, s, last_ok);
    strcpy(buf + last_ok, "...");
    snprintf(out, out_size, "%s", buf);
}

int ui_text_clip(int x, int y, int size, int max_w, color_t c, const char *s)
{
    char buf[512];
    clip_to(buf, sizeof(buf), size, max_w, s);
    ui_text(x, y, size, c, "%s", buf);
    return ui_text_w(size, buf);
}

int ui_text_clip_sh(int x, int y, int size, int max_w, color_t c, const char *s)
{
    char buf[512];
    clip_to(buf, sizeof(buf), size, max_w, s);
    text_shadow(buf, x, y, size);
    ui_text(x, y, size, c, "%s", buf);
    return ui_text_w(size, buf);
}

int ui_text_clip_center(int cx, int y, int size, int max_w, color_t c, const char *s)
{
    char buf[512];
    clip_to(buf, sizeof(buf), size, max_w, s);
    int w = ui_text_w(size, buf);
    ui_text(cx - w / 2, y, size, c, "%s", buf);
    return w;
}

// Reparte el texto en lineas. `draw` a false solo mide, que es como se reserva
// el hueco antes de colocar nada debajo.
static int wrap_impl(int x, int y, int size, int w, int line_h, color_t c,
                     const char *s, bool draw, bool center)
{
    char buf[640];
    snprintf(buf, sizeof(buf), "%s", s ? s : "");

    char line[420] = "";
    int  ly = y;

    for (char *word = strtok(buf, " "); word; word = strtok(NULL, " ")) {
        char probe[400];
        snprintf(probe, sizeof(probe), "%s%s%s", line, line[0] ? " " : "", word);

        if (ui_text_w(size, probe) > w && line[0]) {
            if (draw) {
                if (center) ui_text_center(x, ly, size, c, "%s", line);
                else        ui_text(x, ly, size, c, "%s", line);
            }
            ly += line_h;
            snprintf(line, sizeof(line), "%s", word);
        } else {
            snprintf(line, sizeof(line), "%s", probe);
        }
    }

    if (line[0]) {
        if (draw) {
            if (center) ui_text_center(x, ly, size, c, "%s", line);
            else        ui_text(x, ly, size, c, "%s", line);
        }
        ly += line_h;
    }

    return ly - y;
}

int ui_text_wrap(int x, int y, int size, int w, int line_h, color_t c, const char *s)
{
    return wrap_impl(x, y, size, w, line_h, c, s, true, false);
}

int ui_text_wrap_center(int cx, int y, int size, int w, int line_h, color_t c, const char *s)
{
    return wrap_impl(cx, y, size, w, line_h, c, s, true, true);
}

int ui_text_wrap_h(int size, int w, int line_h, const char *s)
{
    return wrap_impl(0, 0, size, w, line_h, COL_TEXT, s, false, false);
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
// en rejilla: con iconos de 256x256 la diferencia no se ve y evita recorrer
// 65k pixeles cada vez que se carga un juego.
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
            if (px[0] + px[1] + px[2] < 90) continue;   // lo muy oscuro no da tono
            r += px[0]; g += px[1]; b += px[2];
            n++;
        }
    }
    SDL_UnlockSurface(conv);
    SDL_FreeSurface(conv);

    if (n == 0) return out;

    float fr = (float)r / n, fg = (float)g / n, fb = (float)b / n;

    // Mas saturacion y brillo fijo, para que siempre contraste con el fondo
    // oscuro venga de un icono claro o de uno apagado.
    float mx = fr > fg ? (fr > fb ? fr : fb) : (fg > fb ? fg : fb);
    float mn = fr < fg ? (fr < fb ? fr : fb) : (fg < fb ? fg : fb);
    float mid = (mx + mn) * 0.5f;

    // Sin pasarse: con 1.8 un icono verde daba un acento de rotulador, y ese
    // color acaba rellenando el boton principal.
    fr = mid + (fr - mid) * 1.45f;
    fg = mid + (fg - mid) * 1.45f;
    fb = mid + (fb - mid) * 1.45f;

    // Tope de saturacion. Un icono de un solo color puro daba un acento que
    // luego rellena el boton principal, y salia un boton de rotulador.
    mx = fr > fg ? (fr > fb ? fr : fb) : (fg > fb ? fg : fb);
    mn = fr < fg ? (fr < fb ? fr : fb) : (fg < fb ? fg : fb);
    if (mx > 1.0f) {
        float sat = (mx - mn) / mx;
        if (sat > 0.62f) {
            float k2 = 0.62f / sat;
            float m2 = (mx + mn) * 0.5f;
            fr = m2 + (fr - m2) * k2;
            fg = m2 + (fg - m2) * k2;
            fb = m2 + (fb - m2) * k2;
        }
    }

    float lum  = (fr * 0.30f + fg * 0.59f + fb * 0.11f);
    float want = 168.0f;
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
    if (t) SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);

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

void ui_image_draw_a(SDL_Texture *t, int x, int y, int w, int h, u8 alpha)
{
    if (!t) return;
    SDL_SetTextureAlphaMod(t, alpha);
    ui_image_draw(t, x, y, w, h);
    SDL_SetTextureAlphaMod(t, 255);
}

void ui_image_draw_rot(SDL_Texture *t, int cx, int cy, int w, int h,
                       float deg, u8 alpha)
{
    if (!t) return;
    SDL_SetTextureAlphaMod(t, alpha);
    SDL_Rect dst = { cx - w / 2, cy - h / 2, w, h };
    SDL_RenderCopyEx(g_ren, t, NULL, &dst, deg, NULL, SDL_FLIP_NONE);
    SDL_SetTextureAlphaMod(t, 255);
}

// Iconos recortados en redondo. Se cachea el resultado ya recortado: son
// imagenes fijas, asi que el recorte se hace una vez y no en cada fotograma.
#define ROUNDCACHE 24

static struct {
    SDL_Texture *src, *out;
    int w, h, r;
    u32 used;
} g_rc[ROUNDCACHE];

void ui_image_round(SDL_Texture *t, int x, int y, int w, int h, int r)
{
    if (!t) return;
    if (r <= 0) { ui_image_draw(t, x, y, w, h); return; }

    int slot = -1;
    for (int i = 0; i < ROUNDCACHE; i++) {
        if (g_rc[i].out && g_rc[i].src == t && g_rc[i].w == w && g_rc[i].h == h
            && g_rc[i].r == r) {
            g_rc[i].used = g_frame;
            SDL_Rect d = { x, y, w, h };
            SDL_RenderCopy(g_ren, g_rc[i].out, NULL, &d);
            return;
        }
        if (!g_rc[i].out) { slot = i; break; }
        if (slot < 0 || g_rc[i].used < g_rc[slot].used) slot = i;
    }
    if (slot < 0) slot = 0;
    if (g_rc[slot].out) SDL_DestroyTexture(g_rc[slot].out);

    SDL_Texture *out = make_target(w, h);
    if (!out) return;

    SDL_Texture *prev = SDL_GetRenderTarget(g_ren);
    SDL_SetRenderTarget(g_ren, out);

    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 0);
    SDL_RenderClear(g_ren);

    // Mismo truco que el cristal: mascara blanca y encima la imagen en modo
    // multiplicar, que deja el alfa de la mascara intacto.
    rect_round_mode(0, 0, w, h, r, (color_t){ 255, 255, 255, 255 }, SDL_BLENDMODE_NONE);

    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_MOD);
    SDL_Rect full = { 0, 0, w, h };
    SDL_RenderCopy(g_ren, t, NULL, &full);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);

    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderTarget(g_ren, prev);

    g_rc[slot].src = t;   g_rc[slot].out = out;
    g_rc[slot].w   = w;   g_rc[slot].h   = h;
    g_rc[slot].r   = r;   g_rc[slot].used = g_frame;

    SDL_Rect d = { x, y, w, h };
    SDL_RenderCopy(g_ren, out, NULL, &d);
}

void ui_images_clear(void)
{
    for (int i = 0; i < g_ic_n; i++)
        if (g_ic[i].tex) SDL_DestroyTexture(g_ic[i].tex);
    memset(g_ic, 0, sizeof(g_ic));
    g_ic_n = 0;

    // Las recortadas apuntan a las que se acaban de liberar.
    for (int i = 0; i < ROUNDCACHE; i++)
        if (g_rc[i].out) { SDL_DestroyTexture(g_rc[i].out); g_rc[i].out = NULL; }
    memset(g_rc, 0, sizeof(g_rc));
}

// --------------------------------------------------------------------------
// utilidades
// --------------------------------------------------------------------------

bool ui_hit(const ui_input_t *in, int x, int y, int w, int h)
{
    return in->tap && in->tap_x >= x && in->tap_x < x + w
                   && in->tap_y >= y && in->tap_y < y + h;
}

bool ui_over(const ui_input_t *in, int x, int y, int w, int h)
{
    return in->touched && in->touch_x >= x && in->touch_x < x + w
                       && in->touch_y >= y && in->touch_y < y + h;
}

u32 ui_ticks(void) { return SDL_GetTicks(); }

float ui_time(void) { return (float)(SDL_GetTicks() - g_t0) / 1000.0f; }

float ui_dt(void) { return g_dt; }

float ui_approach(float cur, float target, float rate)
{
    // Exponencial: independiente de los fotogramas por segundo, asi que se ve
    // igual acoplada a la tele que en portatil.
    float k = 1.0f - expf(-rate * g_dt);
    return cur + (target - cur) * k;
}

float ui_spring(float cur, float target, float *vel, float stiff)
{
    // Muelle criticamente amortiguado: llega deprisa, no rebota, y conserva
    // inercia. Es lo que hace que la seleccion se sienta con peso.
    //
    // Integrado a pasos fijos y no de un salto por fotograma. Con un solo paso
    // el muelle es estable a 60 fps pero se dispara si el fotograma se alarga:
    // en el previsualizador, con fotogramas de 100 ms, la gota del menu salia
    // despedida fuera del dock. En la consola pasaria igual en cualquier bache.
    const float STEP = 1.0f / 120.0f;
    float damp = 2.0f * sqrtf(stiff);
    float left = g_dt;

    while (left > 0.0f) {
        float h = left > STEP ? STEP : left;
        float a = (target - cur) * stiff - *vel * damp;
        *vel += a * h;
        cur  += *vel * h;
        left -= h;
    }
    return cur;
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

color_t ui_alpha(color_t c, u8 a) { c.a = a; return c; }

color_t ui_shade(color_t c, float k)
{
    float r = c.r * k, g = c.g * k, b = c.b * k;
    c.r = (u8)(r > 255 ? 255 : r < 0 ? 0 : r);
    c.g = (u8)(g > 255 ? 255 : g < 0 ? 0 : g);
    c.b = (u8)(b > 255 ? 255 : b < 0 ? 0 : b);
    return c;
}

// --------------------------------------------------------------------------
// ondas al tocar
// --------------------------------------------------------------------------

#define RIPPLES 6

static struct { float x, y, t; color_t c; bool on; } g_rip[RIPPLES];

void ui_ripple(int x, int y, color_t c)
{
    for (int i = 0; i < RIPPLES; i++) {
        if (g_rip[i].on) continue;
        g_rip[i] = (typeof(g_rip[0])){ (float)x, (float)y, 0.0f, c, true };
        return;
    }
    g_rip[0] = (typeof(g_rip[0])){ (float)x, (float)y, 0.0f, c, true };
}

void ui_ripples_draw(void)
{
    for (int i = 0; i < RIPPLES; i++) {
        if (!g_rip[i].on) continue;

        g_rip[i].t += g_dt * 1.9f;
        if (g_rip[i].t >= 1.0f) { g_rip[i].on = false; continue; }

        float t = g_rip[i].t;
        int   r = (int)(24 + t * 96);
        u8    a = (u8)((1.0f - t) * (1.0f - t) * 110);

        ui_glow((int)g_rip[i].x, (int)g_rip[i].y, r, r, g_rip[i].c, a);
        ui_rect_round_outline((int)g_rip[i].x - r, (int)g_rip[i].y - r,
                              r * 2, r * 2, r, 1,
                              ui_alpha(g_rip[i].c, (u8)(a * 3 / 4)));
    }
}

void ui_spinner(int cx, int cy, int r, color_t c)
{
    float t = ui_time();
    for (int i = 0; i < 10; i++) {
        float a = t * 4.0f + i * 0.35f;
        int alpha = 255 - i * 22;
        if (alpha < 0) alpha = 0;
        int x = cx + (int)(cosf(a) * r);
        int y = cy + (int)(sinf(a) * r);
        ui_circle(x, y, 3, ui_alpha(c, (u8)alpha));
    }
}

void ui_ring(int cx, int cy, int radius, int thick, float progress,
             color_t fg, color_t bg)
{
    const int STEPS = 220;

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
        float t = ui_time() * 1.6f;
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

// --------------------------------------------------------------------------
// logo
// --------------------------------------------------------------------------

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
    // Una lente de cristal con el ciclo dentro. Es el mismo dibujo que el icono
    // del homebrew (ver switch/make_icon.py), con las piezas que se pueden
    // hacer en vivo: luz detras, cuerpo translucido, canto asimetrico y arcos.
    int R = size * 34 / 100;
    if (R < 6) R = 6;

    ui_glow(cx, cy - R / 3, R * 13 / 10, R * 13 / 10, c1, 90);

    ui_circle(cx, cy, R, ui_alpha(COL_GLASS, 46));

    // El canto no brilla igual por todas partes: la luz entra por arriba a la
    // izquierda. Esa asimetria es lo que lo hace parecer vidrio.
    int grueso = R / 24 + 1;
    ui_arc(cx, cy, R, grueso, 0.0f, 360.0f, (color_t){ 255, 255, 255, 46 });
    ui_arc(cx, cy, R, grueso, 178.0f, 348.0f, (color_t){ 255, 255, 255, 185 });

    int r     = R * 60 / 100;
    int thick = R * 20 / 100;
    if (thick < 2) thick = 2;

    ui_arc(cx, cy, r, thick, 195.0f, 350.0f, c1);
    arc_head(cx, cy, r, thick, 350.0f, c1);

    ui_arc(cx, cy, r, thick, 15.0f, 170.0f, c2);
    arc_head(cx, cy, r, thick, 170.0f, c2);

    int rr = R * 11 / 100;
    if (rr < 2) rr = 2;
    ui_circle(cx, cy, rr, dot);
}

// --------------------------------------------------------------------------
// diagnostico
// --------------------------------------------------------------------------
//
// Registrar cada caja y pintarlas todas al final convierte "algo se solapa" en
// algo que se ve. Los seis solapamientos de la version anterior se buscaron a
// ojo uno a uno; esto los ensena de golpe.

bool ui_screenshot(const char *path)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, UI_W, UI_H, 32,
                                                    SDL_PIXELFORMAT_RGBA32);
    if (!s) return false;

    bool ok = SDL_RenderReadPixels(g_ren, NULL, SDL_PIXELFORMAT_RGBA32,
                                   s->pixels, s->pitch) == 0
              && IMG_SavePNG(s, path) == 0;

    SDL_FreeSurface(s);
    return ok;
}

#define DBOX_MAX 64

static struct { int x, y, w, h; char name[24]; } g_dbox[DBOX_MAX];
static int  g_dbox_n;
static bool g_debug;

void ui_debug_set(bool on) { g_debug = on; }
bool ui_debug_on(void)     { return g_debug; }

void ui_debug_box(int x, int y, int w, int h, const char *name)
{
    if (!g_debug || g_dbox_n >= DBOX_MAX) return;
    g_dbox[g_dbox_n].x = x; g_dbox[g_dbox_n].y = y;
    g_dbox[g_dbox_n].w = w; g_dbox[g_dbox_n].h = h;
    snprintf(g_dbox[g_dbox_n].name, sizeof(g_dbox[0].name), "%s", name ? name : "");
    g_dbox_n++;
}

void ui_debug_reset(void) { g_dbox_n = 0; }

void ui_debug_draw(void)
{
    if (!g_debug) { g_dbox_n = 0; return; }

    for (int i = 0; i < g_dbox_n; i++) {
        // Rojo si se cruza con otra caja, verde si esta sola.
        bool clash = false;
        for (int j = 0; j < g_dbox_n && !clash; j++) {
            if (i == j) continue;

            bool cruza = g_dbox[i].x < g_dbox[j].x + g_dbox[j].w &&
                         g_dbox[j].x < g_dbox[i].x + g_dbox[i].w &&
                         g_dbox[i].y < g_dbox[j].y + g_dbox[j].h &&
                         g_dbox[j].y < g_dbox[i].y + g_dbox[i].h;
            if (!cruza) continue;

            // Estar dentro de otra caja no es solaparse: un boton vive dentro
            // de su panel. Lo que delata un fallo es el cruce parcial.
            bool dentro = g_dbox[i].x >= g_dbox[j].x && g_dbox[i].y >= g_dbox[j].y &&
                          g_dbox[i].x + g_dbox[i].w <= g_dbox[j].x + g_dbox[j].w &&
                          g_dbox[i].y + g_dbox[i].h <= g_dbox[j].y + g_dbox[j].h;
            bool fuera  = g_dbox[j].x >= g_dbox[i].x && g_dbox[j].y >= g_dbox[i].y &&
                          g_dbox[j].x + g_dbox[j].w <= g_dbox[i].x + g_dbox[i].w &&
                          g_dbox[j].y + g_dbox[j].h <= g_dbox[i].y + g_dbox[i].h;
            clash = !dentro && !fuera;
        }
        color_t c = clash ? (color_t){ 255, 0, 0, 220 } : (color_t){ 0, 255, 90, 130 };
        ui_rect_outline(g_dbox[i].x, g_dbox[i].y, g_dbox[i].w, g_dbox[i].h, 1, c);
        if (g_dbox[i].name[0])
            ui_text(g_dbox[i].x + 2, g_dbox[i].y + 1, 11, c, "%s", g_dbox[i].name);
    }
    g_dbox_n = 0;
}
