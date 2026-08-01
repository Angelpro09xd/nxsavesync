#include "aviso.hpp"
#include "fuente.h"

#include <tesla.hpp>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>

extern "C" u64 __nx_vi_layer_id;

namespace {

// --------------------------------------------------------------------------
// medidas
// --------------------------------------------------------------------------
//
// La capa es de tamano fijo y el aviso se dibuja dentro, pegado a la izquierda
// y del ancho que pida su contenido. El resto queda transparente.
//
// Es a proposito: la barra de abajo del menu HOME cambia de contenido segun lo
// que tengas seleccionado, asi que un aviso que ocupara todo el hueco acabaria
// tapando algo. Anclado a la izquierda y corto, no llega.

constexpr u32 CAPA_W = 620;
constexpr u32 CAPA_H = 46;

constexpr int PILL_H   = 34;
constexpr int PILL_Y   = (CAPA_H - PILL_H) / 2;
constexpr int PAD_X    = 14;      // margen interior a los lados
constexpr int PUNTO_R  = 5;
constexpr int TRAS_PUNTO = 12;    // hueco entre el punto y el texto

const char *CFG_FILE    = "/switch/nxsavesync/config.txt";
const char *ESTADO_FILE = "/switch/nxsavesync/estado.txt";
const char *LOG_FILE    = "/switch/nxsavesync/aviso.log";

// Registro de diagnostico. Solo se escribe cuando algo cambia, no en cada
// fotograma: son diez pasadas por segundo y llenaria la tarjeta.
void anota(const char *fmt, ...)
{
    char linea[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(linea, sizeof(linea), fmt, ap);
    va_end(ap);

    tsl::hlp::doWithSDCardHandle([&] {
        FILE *f = fopen(LOG_FILE, "a");
        if (!f) return;
        fprintf(f, "%s\n", linea);
        fclose(f);
    });
}

constexpr u64 REFRESCO_NS = 2000000000ULL;   // se relee la SD cada 2 s

// --------------------------------------------------------------------------
// estado
// --------------------------------------------------------------------------

struct Color { u8 r, g, b, a; };

constexpr Color FONDO   = { 0x10, 0x14, 0x1C, 0xE6 };
constexpr Color BORDE   = { 0xE2, 0xEC, 0xFA, 0x30 };
constexpr Color TEXTO   = { 0xF4, 0xF7, 0xFB, 0xFF };
constexpr Color TENUE   = { 0x98, 0xA5, 0xB8, 0xFF };
constexpr Color VERDE   = { 0x5C, 0xE0, 0x9B, 0xFF };
constexpr Color NARANJA = { 0xFF, 0xC1, 0x4D, 0xFF };

ViDisplay   g_display;
ViLayer     g_layer;
NWindow     g_win;
Framebuffer g_fb;

u64  g_layer_id   = 0;
bool g_arrancado  = false;
bool g_roto       = false;      // algo fallo: no se vuelve a intentar

u64  g_ultima_lectura = 0;
bool g_encendido      = true;
int  g_pos_x = 140, g_pos_y = 660;

int  g_pendientes = 0;
char g_texto[192] = "";
bool g_hay_datos  = false;

// --------------------------------------------------------------------------
// lectura de la SD
// --------------------------------------------------------------------------

// Un valor entero de config.txt. Devuelve `def` si no esta.
int cfg_int(const char *clave, int def)
{
    FILE *f = fopen(CFG_FILE, "r");
    if (!f) return def;

    char linea[192];
    size_t n = strlen(clave);
    int valor = def;

    while (fgets(linea, sizeof(linea), f)) {
        if (linea[0] == '#') continue;
        if (strncmp(linea, clave, n) != 0 || linea[n] != '=') continue;
        valor = atoi(linea + n + 1);
        break;
    }

    fclose(f);
    return valor;
}

// Arma la linea del aviso a partir de estado.txt.
void lee_estado()
{
    g_pendientes = 0;
    g_hay_datos  = false;
    g_texto[0]   = '\0';

    FILE *f = fopen(ESTADO_FILE, "r");
    if (!f) return;

    char nombres[192] = "";
    int  puestos = 0;
    char linea[256];

    while (fgets(linea, sizeof(linea), f)) {
        char *nl = strchr(linea, '\n');
        if (nl) *nl = '\0';
        if (linea[0] == '#' || linea[0] == '\0') continue;

        if (!strncmp(linea, "pendientes=", 11)) {
            g_pendientes = atoi(linea + 11);
            g_hay_datos = true;
        } else if (!strncmp(linea, "juego=", 6)) {
            // El nombre va detras de la primera barra, para que pueda llevar
            // barras el mismo sin romper nada.
            char *barra = strchr(linea + 6, '|');
            if (!barra) continue;

            const char *nombre = barra + 1;
            if (puestos >= 3) continue;

            if (puestos) strncat(nombres, ", ", sizeof(nombres) - strlen(nombres) - 1);
            strncat(nombres, nombre, sizeof(nombres) - strlen(nombres) - 1);
            puestos++;
        }
    }
    fclose(f);

    if (g_pendientes <= 0) {
        snprintf(g_texto, sizeof(g_texto), "Todo al dia");
    } else if (nombres[0]) {
        snprintf(g_texto, sizeof(g_texto), "%d %s: %s",
                 g_pendientes, g_pendientes == 1 ? "espera" : "esperan", nombres);
    } else {
        snprintf(g_texto, sizeof(g_texto), "%d %s",
                 g_pendientes, g_pendientes == 1 ? "juego espera" : "juegos esperan");
    }
}

void relee_si_toca()
{
    u64 ahora = armGetSystemTick();
    if (g_ultima_lectura && armTicksToNs(ahora - g_ultima_lectura) < REFRESCO_NS)
        return;
    g_ultima_lectura = ahora;

    // Un solo montaje para las cuatro lecturas. En un overlay la SD no esta
    // montada: hay que pedirla y devolverla en cada acceso, que es justo lo que
    // hace este envoltorio de libtesla.
    tsl::hlp::doWithSDCardHandle([&] {
        g_encendido = cfg_int("aviso", 1) != 0;
        g_pos_x     = cfg_int("aviso_x", 140);
        g_pos_y     = cfg_int("aviso_y", 660);
        lee_estado();
    });
}

// --------------------------------------------------------------------------
// dibujo
// --------------------------------------------------------------------------

u8 *g_px = nullptr;
u32 g_stride = 0;

inline void punto(int x, int y, Color c)
{
    if (x < 0 || y < 0 || x >= (int)CAPA_W || y >= (int)CAPA_H || c.a == 0) return;

    u8 *p = g_px + y * g_stride + x * 4;
    if (c.a == 255) {
        p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = 255;
        return;
    }

    // Mezcla sobre lo que ya haya en la capa. El fondo de la capa es
    // transparente, asi que el alfa resultante tambien se acumula.
    u32 a = c.a, ia = 255 - a;
    p[0] = (u8)((c.r * a + p[0] * ia) / 255);
    p[1] = (u8)((c.g * a + p[1] * ia) / 255);
    p[2] = (u8)((c.b * a + p[2] * ia) / 255);
    p[3] = (u8)(a + p[3] * ia / 255);
}

// Rectangulo con las esquinas redondeadas. El borde se suaviza por cobertura,
// que a este tamano es la diferencia entre parecer un adorno y parecer un fallo.
void pastilla(int x, int y, int w, int h, int r, Color c)
{
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            float dx = 0.0f, dy = 0.0f;
            if (i < r)          dx = (float)(r - i) - 0.5f;
            else if (i >= w - r) dx = (float)(i - (w - r)) + 0.5f;
            if (j < r)          dy = (float)(r - j) - 0.5f;
            else if (j >= h - r) dy = (float)(j - (h - r)) + 0.5f;

            float cov = 1.0f;
            if (dx > 0.0f && dy > 0.0f) {
                float d = __builtin_sqrtf(dx * dx + dy * dy);
                cov = (float)r - d + 0.5f;
                if (cov <= 0.0f) continue;
                if (cov > 1.0f) cov = 1.0f;
            }

            Color cc = c;
            cc.a = (u8)(c.a * cov);
            punto(x + i, y + j, cc);
        }
    }
}

void circulo(int cx, int cy, int r, Color c)
{
    for (int j = -r; j <= r; j++) {
        for (int i = -r; i <= r; i++) {
            float d = __builtin_sqrtf((float)(i * i + j * j));
            float cov = (float)r - d + 0.5f;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            Color cc = c;
            cc.a = (u8)(c.a * cov);
            punto(cx + i, cy + j, cc);
        }
    }
}

int ancho_texto(const char *s)
{
    return (int)strlen(s) * (FUENTE_W + 1);
}

// La fuente es de mapa de bits y solo cubre ASCII. Un nombre con kanji sale con
// interrogantes, que es feo pero legible; el nombre completo esta a un combo de
// distancia, en el panel.
void texto(int x, int y, const char *s, Color c)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned ch = *p;
        if (ch < FUENTE_MIN || ch > FUENTE_MAX) ch = '?';

        const unsigned short *col = FUENTE[ch - FUENTE_MIN];
        for (int i = 0; i < FUENTE_W; i++)
            for (int j = 0; j < FUENTE_H; j++)
                if (col[i] & (1 << j)) punto(x + i, y + j, c);

        x += FUENTE_W + 1;
    }
}

// --------------------------------------------------------------------------
// la capa
// --------------------------------------------------------------------------

bool arranca()
{
    Result rc;

    anota("--- arrancando el aviso ---");

    if (R_FAILED(rc = viInitialize(ViServiceType_Manager))) {
        anota("viInitialize fallo: 0x%X", rc); return false;
    }
    if (R_FAILED(rc = viOpenDefaultDisplay(&g_display))) {
        anota("viOpenDefaultDisplay fallo: 0x%X", rc); return false;
    }

    // viCreateLayer consume esta variable global, que ahora mismo tiene la capa
    // de Tesla. Hay que devolversela o al salir destruiria la nuestra en vez de
    // la suya.
    u64 de_tesla = __nx_vi_layer_id;
    __nx_vi_layer_id = 0;

    bool ok = R_SUCCEEDED(viCreateManagedLayer(&g_display, (ViLayerFlags)0, 0,
                                               &__nx_vi_layer_id))
           && R_SUCCEEDED(viCreateLayer(&g_display, &g_layer));

    g_layer_id = __nx_vi_layer_id;
    __nx_vi_layer_id = de_tesla;

    if (!ok) { anota("no se pudo crear la capa"); return false; }
    anota("capa creada, id=%llu", (unsigned long long)g_layer_id);

    viSetLayerScalingMode(&g_layer, ViScalingMode_FitToLayer);

    if (s32 z = 0; R_SUCCEEDED(viGetZOrderCountMax(&g_display, &z)) && z > 0)
        viSetLayerZ(&g_layer, z);

    // Sin apuntarse a la pila de capas por defecto no se ve nada.
    tsl::hlp::viAddToLayerStack(&g_layer, ViLayerStack_Default);
    tsl::hlp::viAddToLayerStack(&g_layer, ViLayerStack_Lcd);
    tsl::hlp::viAddToLayerStack(&g_layer, ViLayerStack_Screenshot);

    if (R_FAILED(rc = viSetLayerSize(&g_layer, CAPA_W, CAPA_H))) {
        anota("viSetLayerSize fallo: 0x%X", rc); return false;
    }
    if (R_FAILED(rc = viSetLayerPosition(&g_layer, g_pos_x, g_pos_y))) {
        anota("viSetLayerPosition fallo: 0x%X", rc); return false;
    }
    if (R_FAILED(rc = nwindowCreateFromLayer(&g_win, &g_layer))) {
        anota("nwindowCreateFromLayer fallo: 0x%X", rc); return false;
    }
    if (R_FAILED(rc = framebufferCreate(&g_fb, &g_win, CAPA_W, CAPA_H,
                                        PIXEL_FORMAT_RGBA_8888, 2))) {
        anota("framebufferCreate fallo: 0x%X", rc); return false;
    }
    if (R_FAILED(rc = framebufferMakeLinear(&g_fb))) {
        anota("framebufferMakeLinear fallo: 0x%X", rc); return false;
    }

    anota("capa lista en %d,%d de %ux%u", g_pos_x, g_pos_y, CAPA_W, CAPA_H);
    return true;
}

void dibuja()
{
    u32 stride = 0;
    void *buf = framebufferBegin(&g_fb, &stride);
    if (!buf) return;

    g_px = (u8 *)buf;
    g_stride = stride;

    memset(buf, 0, (size_t)stride * CAPA_H);      // todo transparente

    if (g_encendido && g_hay_datos) {
        Color acento = g_pendientes > 0 ? NARANJA : VERDE;

        // El ancho lo pide el contenido, con tope: asi nunca alcanza a lo que
        // haya en la parte derecha de la barra del menu.
        int tw = ancho_texto(g_texto);
        int w  = PAD_X + PUNTO_R * 2 + TRAS_PUNTO + tw + PAD_X;

        if (w > (int)CAPA_W) {
            // No cabe: se recorta el texto y se cierra con puntos suspensivos.
            int cabe = ((int)CAPA_W - PAD_X * 2 - PUNTO_R * 2 - TRAS_PUNTO)
                       / (FUENTE_W + 1) - 3;
            if (cabe < 4) cabe = 4;
            if (cabe < (int)strlen(g_texto)) {
                g_texto[cabe] = '\0';
                strncat(g_texto, "...", sizeof(g_texto) - strlen(g_texto) - 1);
            }
            tw = ancho_texto(g_texto);
            w  = PAD_X + PUNTO_R * 2 + TRAS_PUNTO + tw + PAD_X;
            if (w > (int)CAPA_W) w = CAPA_W;
        }

        pastilla(0, PILL_Y, w, PILL_H, PILL_H / 2, FONDO);
        pastilla(0, PILL_Y, w, 1, 0, BORDE);

        circulo(PAD_X + PUNTO_R, PILL_Y + PILL_H / 2, PUNTO_R, acento);

        texto(PAD_X + PUNTO_R * 2 + TRAS_PUNTO,
              PILL_Y + (PILL_H - FUENTE_H) / 2,
              g_texto, g_pendientes > 0 ? TEXTO : TENUE);
    }

    framebufferEnd(&g_fb);
}

}   // namespace

// --------------------------------------------------------------------------

extern "C" void nxss_aviso_tick(void)
{
    if (g_roto) return;

    static bool primera = true;
    if (primera) {
        primera = false;
        anota("el bucle llama al aviso (el parche funciona)");
    }

    relee_si_toca();

    static int visto = -2;
    if (visto != g_pendientes || !g_hay_datos) {
        visto = g_pendientes;
        anota("estado: encendido=%d datos=%d pendientes=%d texto=\"%s\"",
              g_encendido, g_hay_datos, g_pendientes, g_texto);
    }

    if (!g_arrancado) {
        if (!arranca()) { anota("el aviso se queda apagado"); g_roto = true; return; }
        g_arrancado = true;
    }

    // La posicion se puede cambiar sin reiniciar nada.
    static int px = -1, py = -1;
    if (px != g_pos_x || py != g_pos_y) {
        viSetLayerPosition(&g_layer, g_pos_x, g_pos_y);
        px = g_pos_x; py = g_pos_y;
    }

    static int dibujados = 0;
    dibuja();
    if (++dibujados == 1) anota("primer fotograma dibujado");
}

extern "C" void nxss_aviso_exit(void)
{
    if (!g_arrancado) return;

    framebufferClose(&g_fb);
    nwindowClose(&g_win);
    viCloseLayer(&g_layer);
    viDestroyManagedLayer(&g_layer);
    viCloseDisplay(&g_display);

    g_arrancado = false;
}
