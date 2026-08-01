#include "notify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>


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
// estado para el aviso en pantalla
// --------------------------------------------------------------------------
//
// Formato de texto plano, una linea por cosa. Se eligio asi para que se pueda
// mirar con cualquier editor desde el PC cuando algo no cuadre, que con un
// binario compacto habria que descifrarlo.

void estado_write(const estado_t *e)
{
    mkdir(CFG_DIR, 0777);

    FILE *f = fopen(ESTADO_PATH, "w");
    if (!f) return;

    fprintf(f, "# NX Save Sync - estado para el aviso en pantalla\n");
    fprintf(f, "# lo escriben la app y el sysmodule; lo lee el overlay\n");
    fprintf(f, "pendientes=%d\n", e->pendientes);
    fprintf(f, "cuando=%llu\n", (unsigned long long)time(NULL));

    for (int i = 0; i < e->n && i < ESTADO_MAX_JUEGOS; i++) {
        // El nombre puede llevar cualquier cosa menos un salto de linea, asi
        // que el separador es una barra y el nombre va al final.
        fprintf(f, "juego=%u|%s\n", e->estado[i], e->nombre[i]);
    }

    fclose(f);
}

bool estado_read(estado_t *out)
{
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(ESTADO_PATH, "r");
    if (!f) return false;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        if (!strncmp(line, "pendientes=", 11)) {
            out->pendientes = atoi(line + 11);
        } else if (!strncmp(line, "cuando=", 7)) {
            out->when = strtoull(line + 7, NULL, 10);
        } else if (!strncmp(line, "juego=", 6) && out->n < ESTADO_MAX_JUEGOS) {
            char *barra = strchr(line + 6, '|');
            if (!barra) continue;
            *barra = '\0';
            out->estado[out->n] = (u8)atoi(line + 6);
            snprintf(out->nombre[out->n], sizeof(out->nombre[0]), "%s", barra + 1);
            out->n++;
        }
    }

    fclose(f);
    out->valid = true;
    return true;
}
