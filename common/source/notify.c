#include "notify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "font8x8.h"

// --------------------------------------------------------------------------
// LED del boton HOME
// --------------------------------------------------------------------------

// Un ciclo del patron: intensidad, pasos de transicion y duracion final.
static void cycle(HidsysNotificationLedPatternCycle *c, u8 intensity, u8 steps, u8 hold)
{
    c->ledIntensity     = intensity;
    c->transitionSteps  = steps;
    c->finalStepDuration = hold;
    c->pad = 0;
}

void notify_led(notify_kind_t kind)
{
    if (R_FAILED(hidsysInitialize())) return;

    HidsysNotificationLedPattern p;
    memset(&p, 0, sizeof(p));

    // baseMiniCycleDuration en pasos de 12.5 ms.
    switch (kind) {
    case NOTIFY_CHANGES:
        // Dos pulsos suaves: "he movido cosas".
        p.baseMiniCycleDuration = 0x8;   // 100 ms
        p.totalMiniCycles       = 3;     // 4 ciclos
        p.totalFullCycles       = 2;
        p.startIntensity        = 0x0;
        cycle(&p.miniCycles[0], 0xF, 0x8, 0x1);
        cycle(&p.miniCycles[1], 0x0, 0x8, 0x1);
        cycle(&p.miniCycles[2], 0xF, 0x8, 0x1);
        cycle(&p.miniCycles[3], 0x0, 0x8, 0x2);
        break;

    case NOTIFY_ATTENTION:
        // Tres pulsos rapidos: hace falta que entres a decidir.
        p.baseMiniCycleDuration = 0x4;   // 50 ms
        p.totalMiniCycles       = 5;
        p.totalFullCycles       = 3;
        p.startIntensity        = 0x0;
        for (int i = 0; i < 6; i++)
            cycle(&p.miniCycles[i], (i % 2) ? 0x0 : 0xF, 0x2, 0x1);
        break;

    case NOTIFY_FAIL:
        // Un pulso largo y tenue: algo fallo, pero sin alarmar.
        p.baseMiniCycleDuration = 0xF;   // 187.5 ms
        p.totalMiniCycles       = 1;
        p.totalFullCycles       = 1;
        p.startIntensity        = 0x0;
        cycle(&p.miniCycles[0], 0x6, 0xF, 0x4);
        cycle(&p.miniCycles[1], 0x0, 0xF, 0x1);
        break;

    default:
        hidsysExit();
        return;   // sin novedad no se molesta a nadie
    }

    // Hay que aplicarlo a cada mando por separado.
    HidsysUniquePadId pads[16];
    s32 total = 0;
    if (R_SUCCEEDED(hidsysGetUniquePadIds(pads, 16, &total))) {
        for (s32 i = 0; i < total; i++)
            hidsysSetNotificationLedPattern(&p, pads[i]);
    }

    hidsysExit();
}

// --------------------------------------------------------------------------
// registro para la app
// --------------------------------------------------------------------------

void notify_record(notify_kind_t kind, int pulled, int pushed, int deleted,
                   int pending, const char *detail)
{
    mkdir("sdmc:/switch", 0777);
    mkdir(CFG_DIR, 0777);

    FILE *f = fopen(NOTIFY_PATH, "w");
    if (!f) return;

    u64 now = 0;
    timeGetCurrentTime(TimeType_LocalSystemClock, &now);

    fprintf(f, "tipo=%u\n", (unsigned)kind);
    fprintf(f, "bajados=%d\n", pulled);
    fprintf(f, "subidos=%d\n", pushed);
    fprintf(f, "borrados=%d\n", deleted);
    fprintf(f, "pendientes=%d\n", pending);
    fprintf(f, "cuando=%llu\n", (unsigned long long)now);
    fprintf(f, "detalle=%s\n", detail ? detail : "");
    fclose(f);
}

bool notify_read(notify_info_t *out, bool consume)
{
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(NOTIFY_PATH, "r");
    if (!f) return false;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *k = line, *v = eq + 1;

        if      (!strcmp(k, "tipo"))       out->kind    = (u8)atoi(v);
        else if (!strcmp(k, "bajados"))    out->pulled  = atoi(v);
        else if (!strcmp(k, "subidos"))    out->pushed  = atoi(v);
        else if (!strcmp(k, "borrados"))   out->deleted = atoi(v);
        else if (!strcmp(k, "pendientes")) out->pending = atoi(v);
        else if (!strcmp(k, "cuando"))     out->when    = strtoull(v, NULL, 10);
        else if (!strcmp(k, "detalle"))    snprintf(out->detail, sizeof(out->detail), "%s", v);
    }
    fclose(f);

    if (consume) remove(NOTIFY_PATH);

    out->valid = true;
    return true;
}

// --------------------------------------------------------------------------
// aviso como imagen en el Album
// --------------------------------------------------------------------------

#define ALBUM_W 1280
#define ALBUM_H 720

typedef struct { u8 r, g, b; } rgb_t;

static void px(u8 *buf, int x, int y, rgb_t c)
{
    if (x < 0 || y < 0 || x >= ALBUM_W || y >= ALBUM_H) return;
    u8 *p = buf + ((size_t)y * ALBUM_W + x) * 4;
    p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = 0xFF;
}

static void fill(u8 *buf, int x, int y, int w, int h, rgb_t c)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            px(buf, x + i, y + j, c);
}

// Escribe una cadena escalando la fuente por un multiplo entero, que mantiene
// los bordes limpios sin necesidad de suavizado.
static void text(u8 *buf, int x, int y, int escala, rgb_t c, const char *s)
{
    for (; *s; s++) {
        u8 ch = (u8)*s;
        if (ch >= FONT_FIRST && ch <= FONT_LAST) {
            const u8 *g = g_font8x8[ch - FONT_FIRST];
            for (int fy = 0; fy < FONT_H; fy++)
                for (int fx = 0; fx < FONT_W; fx++)
                    if (g[fy] & (1 << fx))
                        fill(buf, x + fx * escala, y + fy * escala, escala, escala, c);
        }
        x += FONT_W * escala;
    }
}

bool notify_album(const char *titulo, const char *const *lineas, notify_kind_t kind)
{
    // 3,6 MB: se pide y se suelta aqui mismo, no se deja reservado.
    size_t bytes = (size_t)ALBUM_W * ALBUM_H * 4;
    u8 *buf = malloc(bytes);
    if (!buf) return false;

    const rgb_t fondo  = { 0x14, 0x17, 0x1C };
    const rgb_t panel  = { 0x1E, 0x23, 0x2B };
    const rgb_t texto  = { 0xEC, 0xEF, 0xF2 };
    const rgb_t tenue  = { 0x8A, 0x93, 0xA0 };
    const rgb_t acento = kind == NOTIFY_ATTENTION ? (rgb_t){ 0xF0, 0xB4, 0x3C }
                       : kind == NOTIFY_FAIL      ? (rgb_t){ 0xE8, 0x5D, 0x5D }
                                                  : (rgb_t){ 0x2E, 0xC4, 0xD3 };

    fill(buf, 0, 0, ALBUM_W, ALBUM_H, fondo);

    // Tarjeta centrada con una franja de color arriba.
    const int cx = 140, cy = 130, cw = ALBUM_W - 280, chh = ALBUM_H - 260;
    fill(buf, cx, cy, cw, chh, panel);
    fill(buf, cx, cy, cw, 8, acento);

    text(buf, cx + 48, cy + 52, 4, acento, "NX SAVE SYNC");
    text(buf, cx + 48, cy + 110, 3, texto, titulo ? titulo : "");

    fill(buf, cx + 48, cy + 158, cw - 96, 2, tenue);

    int y = cy + 190;
    for (int i = 0; lineas && lineas[i]; i++) {
        text(buf, cx + 48, y, 2, i == 0 ? texto : tenue, lineas[i]);
        y += 34;
        if (y > cy + chh - 90) break;
    }

    text(buf, cx + 48, cy + chh - 54, 2, tenue, "Abre NX Save Sync para resolverlo");

    bool ok = false;
    if (R_SUCCEEDED(capssuInitialize())) {
        CapsApplicationAlbumEntry entry;
        ok = R_SUCCEEDED(capssuSaveScreenShot(buf, bytes, AlbumReportOption_Disable,
                                              AlbumImageOrientation_Unknown0, &entry));
        capssuExit();
    }

    free(buf);
    return ok;
}
