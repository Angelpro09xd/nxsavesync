#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

settings_t g_set;

static void defaults(void)
{
    memset(&g_set, 0, sizeof(g_set));
    g_set.mode          = MODE_MANUAL;
    g_set.policy        = POLICY_SWITCH;
    g_set.sync_on_open  = 0;
    g_set.auto_discover = 1;
    g_set.sounds        = 1;
    g_set.music         = 1;
    g_set.aviso         = 1;
    // Justo despues del icono de la consola, bajo la linea de la barra.
    g_set.aviso_x       = 140;
    g_set.aviso_y       = 660;
    g_set.ask_incoming  = 1;
    g_set.bg_enabled    = 0;
    g_set.bg_policy     = POLICY_SKIP;
    g_set.bg_interval   = 300;
    g_set.bg_on_exit    = 1;
    g_set.bg_nudge      = 1;
}

// --------------------------------------------------------------------------
// guardado
// --------------------------------------------------------------------------

static void ensure_dir(void)
{
    mkdir("sdmc:/switch", 0777);
    mkdir(CFG_DIR, 0777);
}

void settings_save(void)
{
    ensure_dir();

    FILE *f = fopen(CFG_PATH, "w");
    if (f) {
        fprintf(f, "# NX Save Sync - ajustes generales\n");
        fprintf(f, "# modo: 0 manual (pregunta), 1 automatico\n");
        fprintf(f, "modo=%u\n", g_set.mode);
        fprintf(f, "# politica en automatico: 1 gana switch, 2 gana pc, 3 no tocar\n");
        fprintf(f, "politica=%u\n", g_set.policy);
        fprintf(f, "sincronizar_al_abrir=%u\n", g_set.sync_on_open);
        fprintf(f, "buscar_pcs=%u\n", g_set.auto_discover);
        fprintf(f, "sonidos=%u\n", g_set.sounds);
        fprintf(f, "musica=%u\n", g_set.music);
        fprintf(f, "\n# Aviso sobre el menu HOME (lo dibuja el overlay)\n");
        fprintf(f, "aviso=%u\n", g_set.aviso);
        fprintf(f, "aviso_x=%u\n", g_set.aviso_x);
        fprintf(f, "aviso_y=%u\n", g_set.aviso_y);
        fprintf(f, "preguntar_si_cambia_pc=%u\n", g_set.ask_incoming);
        fprintf(f, "\n# Sysmodule (sincronizacion en segundo plano)\n");
        fprintf(f, "fondo=%u\n", g_set.bg_enabled);
        fprintf(f, "fondo_politica=%u\n", g_set.bg_policy);
        fprintf(f, "fondo_intervalo=%u\n", g_set.bg_interval);
        fprintf(f, "fondo_al_salir=%u\n", g_set.bg_on_exit);
        fprintf(f, "fondo_avisos_pc=%u\n", g_set.bg_nudge);
        fprintf(f, "host_activo=%u\n", (unsigned)g_set.host_sel);
        for (size_t i = 0; i < g_set.host_count; i++)
            fprintf(f, "host=%s|%u|%s|%s\n", g_set.hosts[i].ip, g_set.hosts[i].port,
                    g_set.hosts[i].name, g_set.hosts[i].emu);
        fclose(f);
    }

    f = fopen(CFG_PERGAME, "w");
    if (f) {
        fprintf(f, "# title_id|politica|excluido\n");
        for (size_t i = 0; i < g_set.game_count; i++)
            fprintf(f, "%016lX|%u|%u\n", g_set.games[i].title_id,
                    g_set.games[i].policy, g_set.games[i].excluded);
        fclose(f);
    }

    f = fopen(CFG_PROFILES, "w");
    if (f) {
        fprintf(f, "# Perfiles de la consola que participan en la sincronizacion.\n");
        fprintf(f, "# uid_hi:uid_lo|compartido\n");
        for (size_t i = 0; i < g_set.profile_count; i++)
            fprintf(f, "%016lX:%016lX|%u\n",
                    g_set.profiles[i].uid.uid[0], g_set.profiles[i].uid.uid[1],
                    g_set.profiles[i].shared);
        fclose(f);
    }
}

// --------------------------------------------------------------------------
// carga
// --------------------------------------------------------------------------

static void parse_host_line(char *val)
{
    if (g_set.host_count >= SET_MAX_HOSTS) return;

    host_t h;
    memset(&h, 0, sizeof(h));

    char *ip   = strtok(val, "|");
    char *port = strtok(NULL, "|");
    char *name = strtok(NULL, "|");
    char *emu  = strtok(NULL, "|");

    if (!ip) return;
    snprintf(h.ip, sizeof(h.ip), "%s", ip);
    h.port = port ? (u16)atoi(port) : PROTO_DEFAULT_PORT;
    if (!h.port) h.port = PROTO_DEFAULT_PORT;
    snprintf(h.name, sizeof(h.name), "%s", name ? name : ip);
    snprintf(h.emu, sizeof(h.emu), "%s", emu ? emu : "");

    g_set.hosts[g_set.host_count++] = h;
}

void settings_load(void)
{
    defaults();

    FILE *f = fopen(CFG_PATH, "r");
    if (!f) { settings_save(); return; }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        line[strcspn(line, "\r\n")] = '\0';

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line, *val = eq + 1;

        if      (!strcmp(key, "modo"))                 g_set.mode = (u8)atoi(val);
        else if (!strcmp(key, "politica"))             g_set.policy = (u8)atoi(val);
        else if (!strcmp(key, "sincronizar_al_abrir")) g_set.sync_on_open = (u8)atoi(val);
        else if (!strcmp(key, "buscar_pcs"))           g_set.auto_discover = (u8)atoi(val);
        else if (!strcmp(key, "sonidos"))              g_set.sounds = (u8)atoi(val);
        else if (!strcmp(key, "musica"))               g_set.music = (u8)atoi(val);
        else if (!strcmp(key, "aviso"))                g_set.aviso = (u8)atoi(val);
        else if (!strcmp(key, "aviso_x"))              g_set.aviso_x = (u16)atoi(val);
        else if (!strcmp(key, "aviso_y"))              g_set.aviso_y = (u16)atoi(val);
        else if (!strcmp(key, "preguntar_si_cambia_pc")) g_set.ask_incoming = (u8)atoi(val);
        else if (!strcmp(key, "fondo"))                g_set.bg_enabled = (u8)atoi(val);
        else if (!strcmp(key, "fondo_politica"))       g_set.bg_policy = (u8)atoi(val);
        else if (!strcmp(key, "fondo_intervalo"))      g_set.bg_interval = (u16)atoi(val);
        else if (!strcmp(key, "fondo_al_salir"))       g_set.bg_on_exit = (u8)atoi(val);
        else if (!strcmp(key, "fondo_avisos_pc"))      g_set.bg_nudge = (u8)atoi(val);
        else if (!strcmp(key, "host_activo"))          g_set.host_sel = (size_t)atoi(val);
        else if (!strcmp(key, "host"))                 parse_host_line(val);
    }
    fclose(f);

    if (g_set.host_sel >= g_set.host_count) g_set.host_sel = 0;
    if (g_set.bg_interval < 30) g_set.bg_interval = 30;

    f = fopen(CFG_PERGAME, "r");
    if (f) {
        while (fgets(line, sizeof(line), f) && g_set.game_count < SET_MAX_PERGAME) {
            if (line[0] == '#') continue;
            u64 tid = 0; unsigned pol = 0, exc = 0;
            if (sscanf(line, "%lX|%u|%u", &tid, &pol, &exc) >= 2 && tid) {
                g_set.games[g_set.game_count].title_id = tid;
                g_set.games[g_set.game_count].policy   = (u8)pol;
                g_set.games[g_set.game_count].excluded = (u8)exc;
                g_set.game_count++;
            }
        }
        fclose(f);
    }

    f = fopen(CFG_PROFILES, "r");
    if (f) {
        while (fgets(line, sizeof(line), f) && g_set.profile_count < SET_MAX_USERS) {
            if (line[0] == '#') continue;
            u64 hi = 0, lo = 0; unsigned sh = 1;
            if (sscanf(line, "%lX:%lX|%u", &hi, &lo, &sh) >= 2) {
                g_set.profiles[g_set.profile_count].uid.uid[0] = hi;
                g_set.profiles[g_set.profile_count].uid.uid[1] = lo;
                g_set.profiles[g_set.profile_count].shared     = (u8)sh;
                g_set.profile_count++;
            }
        }
        fclose(f);
    }
}

// --------------------------------------------------------------------------
// consultas
// --------------------------------------------------------------------------

const host_t *settings_host(void)
{
    if (g_set.host_count == 0) return NULL;
    if (g_set.host_sel >= g_set.host_count) g_set.host_sel = 0;
    if (g_set.bg_interval < 30) g_set.bg_interval = 30;
    return &g_set.hosts[g_set.host_sel];
}

void settings_add_host(const host_t *h, bool select_it)
{
    for (size_t i = 0; i < g_set.host_count; i++) {
        if (!strcmp(g_set.hosts[i].ip, h->ip)) {
            // Ya lo conociamos: refrescamos nombre y emulador por si cambiaron.
            g_set.hosts[i] = *h;
            if (select_it) g_set.host_sel = i;
            return;
        }
    }

    if (g_set.host_count >= SET_MAX_HOSTS) return;
    g_set.hosts[g_set.host_count] = *h;
    if (select_it) g_set.host_sel = g_set.host_count;
    g_set.host_count++;
}

static game_rule_t *find_game(u64 title_id)
{
    for (size_t i = 0; i < g_set.game_count; i++)
        if (g_set.games[i].title_id == title_id) return &g_set.games[i];
    return NULL;
}

u8 settings_policy_for(u64 title_id)
{
    game_rule_t *r = find_game(title_id);
    if (r && r->policy != POLICY_ASK) return r->policy;
    return g_set.policy;
}

bool settings_excluded(u64 title_id)
{
    game_rule_t *r = find_game(title_id);
    return r && r->excluded;
}

void settings_set_game(u64 title_id, u8 policy, u8 excluded)
{
    game_rule_t *r = find_game(title_id);
    if (!r) {
        if (g_set.game_count >= SET_MAX_PERGAME) return;
        r = &g_set.games[g_set.game_count++];
        r->title_id = title_id;
    }
    r->policy   = policy;
    r->excluded = excluded;
}

static profile_rule_t *find_profile(AccountUid uid)
{
    for (size_t i = 0; i < g_set.profile_count; i++)
        if (memcmp(&g_set.profiles[i].uid, &uid, sizeof(AccountUid)) == 0)
            return &g_set.profiles[i];
    return NULL;
}

bool settings_profile_shared(AccountUid uid)
{
    profile_rule_t *p = find_profile(uid);
    // Sin regla, se comparte: es lo que espera quien tiene un solo perfil.
    return p ? p->shared != 0 : true;
}

void settings_set_profile(AccountUid uid, bool shared)
{
    profile_rule_t *p = find_profile(uid);
    if (!p) {
        if (g_set.profile_count >= SET_MAX_USERS) return;
        p = &g_set.profiles[g_set.profile_count++];
        p->uid = uid;
    }
    p->shared = shared ? 1 : 0;
}
