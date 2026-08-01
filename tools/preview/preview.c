// Previsualizador de la interfaz.
//
// Compila ui.c y screens.c —los mismos archivos que van a la consola— contra
// datos inventados, y vuelca cada pantalla a PNG. Sirve para una cosa: poder
// mirar la interfaz mientras se escribe, en vez de compilar, copiar a la SD,
// arrancar y hacer una foto.
//
//   ./preview salida/          escribe un PNG por pantalla
//   ./preview salida/ --debug  ademas dibuja el contorno de cada caja
//   ./preview --live           ventana navegable con teclado y raton

#include "screens.h"

#include <SDL2/SDL_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// --------------------------------------------------------------------------
// iconos de mentira
// --------------------------------------------------------------------------
//
// Se generan como PNG en memoria porque ui_image espera bytes de imagen, igual
// que los que devuelve la consola.

typedef struct { void *data; size_t len; } blob_t;

static blob_t make_icon(int seed)
{
    const int S = 256;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, S, S, 32, SDL_PIXELFORMAT_RGBA32);
    if (!s) return (blob_t){ NULL, 0 };

    // Tres tonos separados y unas formas: lo unico que importa es que cada
    // icono tenga un color medio distinto, que es de donde sale el acento.
    float h = (float)(seed * 47 % 360) / 360.0f;
    float r0 = 0.5f + 0.5f * sinf(h * 6.283f);
    float g0 = 0.5f + 0.5f * sinf(h * 6.283f + 2.09f);
    float b0 = 0.5f + 0.5f * sinf(h * 6.283f + 4.19f);

    SDL_LockSurface(s);
    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            float fx = (float)x / S, fy = (float)y / S;
            float k = 0.35f + 0.65f * (1.0f - fy);
            float wave = 0.5f + 0.5f * sinf(fx * 12.0f + seed + fy * 5.0f);
            float d = sqrtf((fx - 0.5f) * (fx - 0.5f) + (fy - 0.45f) * (fy - 0.45f));
            float blob = d < 0.26f ? 1.0f : 0.25f;

            u8 *p = (u8 *)s->pixels + y * s->pitch + x * 4;
            p[0] = (u8)(255 * r0 * k * (0.55f + 0.45f * wave) * blob);
            p[1] = (u8)(255 * g0 * k * (0.55f + 0.45f * wave) * blob);
            p[2] = (u8)(255 * b0 * k * (0.70f + 0.30f * wave) * blob);
            p[3] = 255;
        }
    }
    SDL_UnlockSurface(s);

    void *buf = malloc(512 * 1024);
    SDL_RWops *rw = SDL_RWFromMem(buf, 512 * 1024);
    IMG_SavePNG_RW(s, rw, 0);
    size_t len = (size_t)SDL_RWtell(rw);
    SDL_RWclose(rw);
    SDL_FreeSurface(s);

    return (blob_t){ buf, len };
}

// --------------------------------------------------------------------------
// datos de mentira
// --------------------------------------------------------------------------

#define NGAMES 11
#define NUSERS 3
#define NHOSTS 3

static scr_game_t g_games[NGAMES];
static scr_user_t g_users[NUSERS];
static scr_host_t g_hosts[NHOSTS];

#define NEMUS 3
static scr_emu_t g_emus[NEMUS];
static blob_t     g_icons[NGAMES + NUSERS];

static const char *GAME_NAMES[NGAMES] = {
    "The Legend of Zelda: Tears of the Kingdom",
    "Tomodachi Life",
    "Mario Kart 8 Deluxe",
    "Animal Crossing: New Horizons",
    "Super Smash Bros. Ultimate",
    "Hollow Knight",
    "Xenoblade Chronicles 3",
    "Metroid Dread",
    "Splatoon 3",
    "妖怪ウォッチ4++",
    "Fire Emblem: Three Houses",
};

static const u8 STATES[NGAMES] = { 0, 1, 0, 2, 0, 3, 0, 1, 0, 2, 0 };

static void data_init(void)
{
    for (int i = 0; i < NGAMES; i++) {
        g_icons[i] = make_icon(i * 3 + 1);
        g_games[i].name       = GAME_NAMES[i];
        g_games[i].author     = "Nintendo";
        g_games[i].title_id   = 0x0100000000010000ull + (u64)i * 0x2000;
        g_games[i].state      = STATES[i];
        g_games[i].excluded   = (i == 5);
        g_games[i].icon       = g_icons[i].data;
        g_games[i].icon_size  = g_icons[i].len;
        // Alternando, para ver que la fila del detalle se rellena bien.
        g_games[i].emu        = (i % 3 == 0) ? "eden"
                              : (i % 3 == 1) ? "Ryujinx" : NULL;
    }

    const char *unames[NUSERS] = { "Angelpro09", "Invitado", "Hermano" };
    for (int i = 0; i < NUSERS; i++) {
        g_icons[NGAMES + i] = make_icon(100 + i * 11);
        g_users[i].name      = unames[i];
        g_users[i].icon      = g_icons[NGAMES + i].data;
        g_users[i].icon_size = g_icons[NGAMES + i].len;
        g_users[i].shared    = (i != 1);
    }

    g_hosts[0] = (scr_host_t){ "MI-PC", "192.168.1.50", "eden", 7878 };
    g_hosts[1] = (scr_host_t){ "ANGEL-WIN",        "192.168.1.248", "ryujinx", 7878 };
    g_hosts[2] = (scr_host_t){ "",                 "192.168.1.90",  "", 7878 };

    g_emus[0] = (scr_emu_t){ "eden", "C:\\Users\\Angel\\AppData\\Roaming\\eden", true };
    g_emus[1] = (scr_emu_t){ "Ryujinx", "C:\\Users\\Angel\\AppData\\Roaming\\Ryujinx", true };
    g_emus[2] = (scr_emu_t){ "citron", "C:\\Users\\Angel\\AppData\\Roaming\\citron", false };
}

// --------------------------------------------------------------------------
// pantallas
// --------------------------------------------------------------------------

static int g_sel = 2, g_top = 0, g_row = 1;
// Cuanto ha entrado la hoja: 1 = del todo. Con --anim se retrata a medias.
static float g_anim = 1.0f;

static scr_ctx_t ctx(nav_t nav)
{
    scr_ctx_t c = { 0 };
    c.accent    = ui_image_color(g_games[g_sel].icon, g_games[g_sel].icon_size);
    c.nav       = nav;
    c.user      = &g_users[0];
    c.pc_name   = "MI-PC";
    c.pc_emu    = "eden";
    c.mode      = "Automatico";
    c.mode_auto = true;
    c.pc_live   = true;
    c.version   = "v4.1";
    return c;
}

static const char *SET_LABELS[] = {
    "Modo de sincronizacion", "Ante un conflicto",
    "Preguntar si el PC trae cambios", "Sincronizar todo al abrir",
    "Buscar PCs al arrancar", "Sonidos", "Segundo plano (sysmodule)",
};
static const char *SET_HELPS[] = {
    "En automatico no pregunta: aplica la regla de abajo",
    "Que hacer cuando los dos lados han cambiado",
    "Confirmar antes de bajar cambios del emulador",
    "Se pone al dia sin que toques nada",
    "Descubrimiento por la red local",
    "Efectos de la interfaz",
    "Sincroniza sin abrir la app, al cerrar cada juego",
};
static const char *SET_VALUES[] = {
    "Automatico", "Gana el ultimo jugado", "Si", "No", "Si", "Si", "Activado",
};

// Dibuja una pantalla completa. `screen` elige cual.
static void draw_screen(int screen, const ui_input_t *in)
{
    static const char (*log)[160] = NULL;
    static char logbuf[6][160];
    if (!log) {
        snprintf(logbuf[0], 160, "Conectado a MI-PC / eden");
        snprintf(logbuf[1], 160, "The Legend of Zelda: Tears of the Kingdom");
        snprintf(logbuf[2], 160, "  3 bajados, 0 subidos, 0 borrados");
        snprintf(logbuf[3], 160, "Tomodachi Life");
        snprintf(logbuf[4], 160, "  0 bajados, 7 subidos, 1 borrado");
        snprintf(logbuf[5], 160, "Mario Kart 8 Deluxe");
        log = (const char (*)[160])logbuf;
    }

    nav_t nav = screen == 1 ? NAV_USERS
              : screen == 2 ? NAV_HOSTS
              : screen == 3 ? NAV_SETTINGS : NAV_GAMES;

    scr_ctx_t c = ctx(nav);

    scr_backdrop(&c, g_games[g_sel].icon, g_games[g_sel].icon_size);
    scr_dock(&c, in, true);

    switch (screen) {
    case 0:
        scr_topbar(&c, "Juegos", "NX Save Sync  ·  Angelpro09_Dev");
        scr_games(&c, in, g_games, NGAMES, g_sel, g_top,
                  "Gana el ultimo jugado", NULL, NULL, NULL);
        scr_hints("A sincronizar   Y todos   X opciones   ZL/ZR perfil   L/R menu   + salir");
        break;

    case 1:
        scr_topbar(&c, "Perfiles", "Cada perfil se sincroniza por separado. Puedes dejar alguno fuera.");
        scr_users(&c, in, g_users, NUSERS, g_row, 0);
        scr_hints("A usar este perfil   X compartir o no   L/R menu   + salir");
        break;

    case 2:
        scr_topbar(&c, "PCs", "Se buscan solos por la red. Tambien puedes anadir uno por IP.");
        scr_hosts(&c, in, g_hosts, NHOSTS, g_row, 0, g_emus, NEMUS);
        scr_hints("A usar este PC   Y buscar en la red   X anadir por IP   L/R menu");
        break;

    case 3: {
        scr_topbar(&c, "Ajustes", "Todo lo de la consola, y tambien lo del PC.");
        scr_row_t rows[7];
        for (int i = 0; i < 7; i++) {
            rows[i].label  = SET_LABELS[i];
            rows[i].help   = SET_HELPS[i];
            rows[i].value  = SET_VALUES[i];
            rows[i].vcolor = i == 6 ? COL_OK : c.accent;
        }
        scr_rows(&c, in, rows, 7, g_row, 0, 6);
        scr_hints("A cambiar   L/R menu   + salir");
        break;
    }

    case 4: {
        scr_topbar(&c, "Juegos", "NX Save Sync  ·  Angelpro09_Dev");
        scr_games(&c, in, g_games, NGAMES, g_sel, g_top,
                  "Gana el ultimo jugado", NULL, NULL, NULL);
        scr_hints("Elige una opcion");
        scr_choice_t ch[3] = {
            { "A", "Los del PC",        "baja lo del emulador" },
            { "X", "Los de la Switch",  "sube lo de la consola" },
            { "B", "No tocar nada",     "decidir mas tarde" },
        };
        scr_dialog(&c, in, "El PC tiene cambios", "Tomodachi Life",
                   "El PC tiene 7 archivo(s) mas nuevos y 1 borrado desde la "
                   "ultima vez. Elige con que version se queda la consola.",
                   ch, c.accent, g_anim);
        break;
    }

    case 5:
        scr_topbar(&c, "Juegos", "NX Save Sync  ·  Angelpro09_Dev");
        scr_hints("Sincronizando...");
        scr_sync(&c, "Sincronizando", "Mario Kart 8 Deluxe", log, 6, 0.62f, false, g_anim);
        break;

    case 6:
        scr_topbar(&c, "Juegos", "NX Save Sync  ·  Angelpro09_Dev");
        scr_games(&c, in, g_games, NGAMES, g_sel, g_top,
                  "Gana el ultimo jugado", NULL, NULL, NULL);
        scr_hints("A sincronizar   Y todos   X opciones   ZL/ZR perfil   L/R menu   + salir");
        scr_toast("Bajados 3, subidos 7", 1.0f, c.accent);
        break;

    case 7:
        scr_topbar(&c, "Juegos", "NX Save Sync  ·  Angelpro09_Dev");
        scr_games(&c, in, g_games, NGAMES, g_sel, g_top,
                  "Gana el ultimo jugado", NULL, NULL, NULL);
        scr_hints("A cambiar   arriba/abajo elegir   B volver");
        scr_game_opts(&c, in, &g_games[g_sel], "Gana el ultimo jugado", false, 0, g_anim);
        break;

    case 8: {
        scr_topbar(&c, "Ajustes", "Los de la consola, y tambien los del PC.");
        scr_row_t rows[7];
        for (int i = 0; i < 7; i++) {
            rows[i].label  = SET_LABELS[i];
            rows[i].help   = SET_HELPS[i];
            rows[i].value  = SET_VALUES[i];
            rows[i].vcolor = c.accent;
        }
        scr_rows(&c, in, rows, 7, 1, 0, 6);
        scr_hints("A cambiar   Y refrescar   B volver");
        scr_pc_cfg(&c, in, "MI-PC", rows, 7, 2, true, g_anim);
        break;
    }
    }

    ui_ripples_draw();
    ui_debug_draw();
}

static const char *SCREEN_FILE[] = {
    "01-juegos", "02-perfiles", "03-pcs", "04-ajustes",
    "05-dialogo", "06-sincronizando", "07-aviso",
    "08-opciones-juego", "09-ajustes-pc",
};
#define NSCREENS 9

int main(int argc, char **argv)
{
    bool live = false, debug = false;
    int  only = -1;
    const char *outdir = "salida";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--live"))       live = true;
        else if (!strcmp(argv[i], "--debug")) debug = true;
        else if (!strcmp(argv[i], "--sel") && i + 1 < argc) g_sel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--anim") && i + 1 < argc) g_anim = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = atoi(argv[++i]);
        else outdir = argv[i];
    }

    if (!ui_init()) {
        fprintf(stderr, "no se pudo arrancar SDL: %s\n", SDL_GetError());
        return 1;
    }
    ui_debug_set(debug);
    data_init();

    if (live) {
        int screen = 0;
        for (;;) {
            ui_input_t in;
            if (!ui_frame_begin(&in)) break;
            if (in.down & HidNpadButton_Plus) break;

            if (in.down & HidNpadButton_R) screen = (screen + 1) % NSCREENS;
            if (in.down & HidNpadButton_L) screen = (screen + NSCREENS - 1) % NSCREENS;
            if (in.down & HidNpadButton_Right) g_sel = (g_sel + 1) % NGAMES;
            if (in.down & HidNpadButton_Left)  g_sel = g_sel ? g_sel - 1 : NGAMES - 1;
            if (in.down & HidNpadButton_Down)  g_row = (g_row + 1) % 6;
            if (in.down & HidNpadButton_Up)    g_row = g_row ? g_row - 1 : 5;

            draw_screen(screen, &in);
            ui_frame_end();
        }
        ui_exit();
        return 0;
    }

    char path[512];
    for (int s = 0; s < NSCREENS; s++) {
        if (only >= 0 && s != only) continue;
        // Unos cuantos fotogramas antes de la foto: las animaciones tienen que
        // haber llegado a su sitio o se retrata la interfaz a medio entrar.
        for (int f = 0; f < 45; f++) {
            ui_input_t in;
            if (!ui_frame_begin(&in)) break;
            draw_screen(s, &in);
            ui_frame_end();
        }

        snprintf(path, sizeof(path), "%s/%s.png", outdir, SCREEN_FILE[s]);
        if (ui_screenshot(path)) printf("  %s\n", path);
        else fprintf(stderr, "  fallo al guardar %s: %s\n", path, SDL_GetError());
    }

    ui_exit();
    return 0;
}
