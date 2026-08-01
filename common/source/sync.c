#include "sync.h"
#include "proto.h"
#include "manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#define MOUNT_NAME "svsave"
#define MOUNT_ROOT MOUNT_NAME ":"

#ifndef XFER_CHUNK
#define XFER_CHUNK (256 * 1024)
#endif

typedef struct {
    u8   action;
    char path[PROTO_MAX_PATH];
} plan_item_t;

typedef struct {
    plan_item_t *v;
    size_t       n, cap;
} plan_t;

static void ui_log(sync_ui_t *ui, const char *fmt, ...)
{
    if (!ui || !ui->log) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    ui->log(ui->ud, "%s", line);
}

// Si el PC respondio con OP_ERROR, lo registra. Devuelve true si el opcode
// recibido es el esperado.
static bool expect_op(net_t *n, u8 got, u8 want, sync_ui_t *ui)
{
    if (got == want) return true;

    if (got == OP_ERROR) {
        u32 code = 0;
        char msg[256] = "";
        net_r_u32(n, &code);
        net_r_str(n, msg, sizeof(msg));
        ui_log(ui, "  ! el PC rechazo la peticion (%u): %s", code, msg);
    } else {
        ui_log(ui, "  ! respuesta inesperada del PC: 0x%02X", got);
    }
    return false;
}

static void w_uid(net_t *n, AccountUid uid)
{
    net_w_u64(n, uid.uid[0]);
    net_w_u64(n, uid.uid[1]);
}

bool sync_hello(net_t *n, sync_ui_t *ui, char *server_out, size_t server_size,
                char *emu_out, size_t emu_size)
{
    net_begin(n, OP_HELLO);
    net_w_u32(n, PROTO_VERSION);
    net_w_str(n, "switch");
    net_w_str(n, "nxsavesync");
    if (!net_send(n)) { ui_log(ui, "! %s", net_error(n)); return false; }

    u8 op;
    if (!net_recv(n, &op)) { ui_log(ui, "! %s", net_error(n)); return false; }
    if (!expect_op(n, op, OP_HELLO_OK, ui)) return false;

    u32 version = 0, flags = 0;
    char server[128] = "", emu[64] = "";
    net_r_u32(n, &version);
    net_r_str(n, server, sizeof(server));
    net_r_str(n, emu, sizeof(emu));
    net_r_u32(n, &flags);

    if (version != PROTO_VERSION) {
        ui_log(ui, "! el PC habla la version %u del protocolo y la app la %u",
             version, PROTO_VERSION);
        return false;
    }

    if (server_out) snprintf(server_out, server_size, "%s", server);
    if (emu_out)    snprintf(emu_out, emu_size, "%s", emu);

    ui_log(ui, "Conectado a %s%s%s", server[0] ? server : "PC",
           emu[0] ? " / " : "", emu);
    return true;
}

bool sync_profile(net_t *n, AccountUid uid, const char *name,
                  const void *avatar, size_t avatar_len, u8 emu,
                  char *msg, size_t msg_size)
{
    if (msg) msg[0] = '\0';

    net_begin(n, OP_PROFILE);
    w_uid(n, uid);
    net_w_str(n, name ? name : "");
    net_w_u8(n, emu);

    // La foto va con su longitud delante. Puede no haberla: hay perfiles sin
    // imagen, y el emulador pondra la suya por defecto.
    net_w_u32(n, (u32)avatar_len);
    if (avatar_len) net_w_bytes(n, avatar, avatar_len);

    if (!net_send(n)) return false;

    u8 op;
    if (!net_recv(n, &op)) return false;
    if (op != OP_PROFILE_RES) return false;

    u8 ok = 0;
    char texto[256] = "";
    if (!net_r_u8(n, &ok)) return false;
    net_r_str(n, texto, sizeof(texto));

    if (msg) snprintf(msg, msg_size, "%s", texto);
    return ok != 0;
}

bool sync_emus(net_t *n, emu_info_t *out, size_t max, size_t *out_n)
{
    *out_n = 0;

    net_begin(n, OP_EMUS_REQ);
    if (!net_send(n)) return false;

    u8 op;
    if (!net_recv(n, &op)) return false;
    if (op != OP_EMUS_RES) return false;

    u32 count = 0;
    if (!net_r_u32(n, &count)) return false;

    for (u32 i = 0; i < count; i++) {
        char name[64] = "", path[256] = "";
        u8 activo = 1;

        if (!net_r_str(n, name, sizeof(name))) return false;
        if (!net_r_str(n, path, sizeof(path))) return false;
        if (!net_r_u8(n, &activo)) return false;

        // Se leen todos aunque no quepan, o el resto de la trama se
        // desalinearia y la siguiente respuesta saldria basura.
        if (i >= max) continue;

        snprintf(out[i].name, sizeof(out[i].name), "%s", name);
        snprintf(out[i].path, sizeof(out[i].path), "%s", path);
        out[i].active = activo != 0;
        (*out_n)++;
    }
    return true;
}

bool sync_summary(net_t *n, AccountUid uid, const u64 *title_ids, size_t count,
                  u8 *out_states, u8 *out_emu)
{
    for (size_t i = 0; i < count; i++) out_states[i] = SUM_UNKNOWN;
    if (out_emu) for (size_t i = 0; i < count; i++) out_emu[i] = 0xFF;

    net_begin(n, OP_SUMMARY_REQ);
    w_uid(n, uid);
    net_w_u32(n, (u32)count);
    for (size_t i = 0; i < count; i++) net_w_u64(n, title_ids[i]);
    if (!net_send(n)) return false;

    u8 op;
    if (!net_recv(n, &op)) return false;
    if (op != OP_SUMMARY_RES) return false;

    u32 got = 0;
    if (!net_r_u32(n, &got)) return false;

    for (u32 i = 0; i < got; i++) {
        u64 tid; u8 state, emu;
        if (!net_r_u64(n, &tid) || !net_r_u8(n, &state) || !net_r_u8(n, &emu))
            return false;

        for (size_t k = 0; k < count; k++)
            if (title_ids[k] == tid) {
                out_states[k] = state;
                if (out_emu) out_emu[k] = emu;
                break;
            }
    }
    return true;
}

// --------------------------------------------------------------------------
// ajustes del daemon
// --------------------------------------------------------------------------

bool sync_cfg_get(net_t *n, cfg_item_t *out, size_t max, size_t *count)
{
    *count = 0;

    net_begin(n, OP_CFG_GET);
    if (!net_send(n)) return false;

    u8 op;
    if (!net_recv(n, &op) || op != OP_CFG_RES) return false;

    u32 total = 0;
    if (!net_r_u32(n, &total)) return false;

    for (u32 i = 0; i < total; i++) {
        cfg_item_t item;
        memset(&item, 0, sizeof(item));

        if (!net_r_str(n, item.key, sizeof(item.key))) return false;
        if (!net_r_u8(n, &item.type)) return false;
        if (!net_r_str(n, item.label, sizeof(item.label))) return false;
        if (!net_r_str(n, item.help, sizeof(item.help))) return false;
        if (!net_r_str(n, item.value, sizeof(item.value))) return false;

        u32 n_opts = 0;
        if (!net_r_u32(n, &n_opts)) return false;

        for (u32 k = 0; k < n_opts; k++) {
            char opt[64];
            if (!net_r_str(n, opt, sizeof(opt))) return false;
            if (k < CFG_MAX_OPTIONS) {
                snprintf(item.options[k], sizeof(item.options[k]), "%s", opt);
                item.n_options = k + 1;
            }
        }

        if (*count < max) out[(*count)++] = item;
    }

    return true;
}

bool sync_cfg_set(net_t *n, const char *key, const char *value,
                  char *msg_out, size_t msg_size)
{
    net_begin(n, OP_CFG_SET);
    net_w_str(n, key);
    net_w_str(n, value);
    if (!net_send(n)) return false;

    u8 op;
    if (!net_recv(n, &op)) return false;

    if (op == OP_ERROR) {
        u32 code = 0;
        net_r_u32(n, &code);
        if (msg_out) net_r_str(n, msg_out, msg_size);
        return false;
    }
    if (op != OP_CFG_OK) return false;

    if (msg_out) net_r_str(n, msg_out, msg_size);
    return true;
}

// --------------------------------------------------------------------------
// plan
// --------------------------------------------------------------------------

static bool plan_push(plan_t *p, u8 action, const char *path)
{
    if (p->n == p->cap) {
        size_t cap = p->cap ? p->cap * 2 : 32;
        plan_item_t *nv = realloc(p->v, cap * sizeof(plan_item_t));
        if (!nv) return false;
        p->v   = nv;
        p->cap = cap;
    }
    if (strlen(path) >= PROTO_MAX_PATH) return false;
    p->v[p->n].action = action;
    strcpy(p->v[p->n].path, path);
    p->n++;
    return true;
}

// Lee un cuerpo ya cargado en memoria con formato [u32 n][n x (u8, str)].
static bool plan_read(net_t *n, plan_t *p, sync_ui_t *ui)
{
    u32 count = 0;
    if (!net_r_u32(n, &count)) { ui_log(ui, "! %s", net_error(n)); return false; }

    for (u32 i = 0; i < count; i++) {
        u8 action;
        char path[PROTO_MAX_PATH];
        if (!net_r_u8(n, &action) || !net_r_str(n, path, sizeof(path))) {
            ui_log(ui, "! %s", net_error(n));
            return false;
        }
        if (!plan_push(p, action, path)) { ui_log(ui, "! sin memoria"); return false; }
    }
    return true;
}

static void plan_free(plan_t *p)
{
    free(p->v);
    p->v = NULL;
    p->n = p->cap = 0;
}

// --------------------------------------------------------------------------
// transferencias
// --------------------------------------------------------------------------

static bool do_pull(net_t *n, AccountUid uid, u64 app_id, const char *rel, sync_ui_t *ui)
{
    net_begin(n, OP_PULL_REQ);
    w_uid(n, uid);
    net_w_u64(n, app_id);
    net_w_str(n, rel);
    if (!net_send(n)) { ui_log(ui, "! %s", net_error(n)); return false; }

    u8 op; u32 plen;
    if (!net_recv_header(n, &op, &plen)) { ui_log(ui, "! %s", net_error(n)); return false; }

    if (op != OP_PULL_RES) {
        if (!net_recv_body(n, plen)) { ui_log(ui, "! %s", net_error(n)); return false; }
        return expect_op(n, op, OP_PULL_RES, ui);
    }

    u8 sizebuf[8];
    if (!net_recv_raw(n, sizebuf, 8)) { ui_log(ui, "! %s", net_error(n)); return false; }
    u64 size = 0;
    for (int i = 0; i < 8; i++) size |= (u64)sizebuf[i] << (8 * i);

    // A partir de aqui el cuerpo del mensaje ya viene en camino. Si algo falla
    // por causas locales hay que consumirlo igualmente, o el siguiente mensaje
    // se leeria a partir de una posicion corrida.
    char full[PROTO_MAX_PATH + 64];

    if (!mf_make_parents(MOUNT_ROOT, rel)) {
        ui_log(ui, "  ! no se pudieron crear las carpetas de %s", rel);
        net_drain_raw(n, size);
        return false;
    }

    if (!mf_join(full, sizeof(full), MOUNT_ROOT, rel)) {
        net_drain_raw(n, size);
        return false;
    }

    FILE *f = fopen(full, "wb");
    if (!f) {
        ui_log(ui, "  ! no se pudo escribir %s", rel);
        net_drain_raw(n, size);
        return false;
    }

    static u8 chunk[XFER_CHUNK];
    u64 left = size;
    bool ok  = true;

    while (left > 0) {
        size_t want = left < sizeof(chunk) ? (size_t)left : sizeof(chunk);
        if (!net_recv_raw(n, chunk, want)) { ui_log(ui, "! %s", net_error(n)); ok = false; break; }
        left -= want;

        if (fwrite(chunk, 1, want, f) != want) {
            ui_log(ui, "  ! fallo al escribir %s (savedata lleno?)", rel);
            net_drain_raw(n, left);
            ok = false;
            break;
        }
    }

    if (fclose(f) != 0) ok = false;
    return ok;
}

static bool do_push(net_t *n, AccountUid uid, u64 app_id, const char *rel, sync_ui_t *ui)
{
    char full[PROTO_MAX_PATH + 64];
    if (!mf_join(full, sizeof(full), MOUNT_ROOT, rel)) return false;

    FILE *f = fopen(full, "rb");
    if (!f) { ui_log(ui, "  ! no se pudo leer %s", rel); return false; }

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return false; }
    rewind(f);

    net_begin(n, OP_PUSH);
    w_uid(n, uid);
    net_w_u64(n, app_id);
    net_w_str(n, rel);
    net_w_u64(n, (u64)size);
    if (!net_send_streaming(n, (u64)size)) {
        ui_log(ui, "! %s", net_error(n));
        fclose(f);
        return false;
    }

    static u8 chunk[XFER_CHUNK];
    u64 left = (u64)size;
    bool ok  = true;

    while (left > 0) {
        size_t want = left < sizeof(chunk) ? (size_t)left : sizeof(chunk);
        if (fread(chunk, 1, want, f) != want) {
            // Ya mandamos la cabecera con el tamano: no podemos parar a medias
            // sin desincronizar el flujo, asi que rellenamos con ceros y damos
            // el archivo por fallido.
            ui_log(ui, "  ! fallo al leer %s", rel);
            memset(chunk, 0, want);
            ok = false;
        }
        if (!net_send_raw(n, chunk, want)) { ui_log(ui, "! %s", net_error(n)); fclose(f); return false; }
        left -= want;
    }
    fclose(f);

    u8 op;
    if (!net_recv(n, &op)) { ui_log(ui, "! %s", net_error(n)); return false; }
    if (!expect_op(n, op, OP_PUSH_OK, ui)) return false;

    return ok;
}

static bool do_del_remote(net_t *n, AccountUid uid, u64 app_id, const char *rel, sync_ui_t *ui)
{
    net_begin(n, OP_DEL_REMOTE);
    w_uid(n, uid);
    net_w_u64(n, app_id);
    net_w_str(n, rel);
    if (!net_send(n)) { ui_log(ui, "! %s", net_error(n)); return false; }

    u8 op;
    if (!net_recv(n, &op)) { ui_log(ui, "! %s", net_error(n)); return false; }
    return expect_op(n, op, OP_DEL_OK, ui);
}

// --------------------------------------------------------------------------
// sincronizacion de un juego
// --------------------------------------------------------------------------

// Hora actual de la consola, en segundos. Sirve para que el PC calcule el
// desfase entre los dos relojes; si no se puede leer se manda 0 y la politica
// "gana el ultimo jugado" se abstiene.
static u64 console_clock(void)
{
    u64 ts = 0;
    if (R_FAILED(timeGetCurrentTime(TimeType_LocalSystemClock, &ts))) return 0;
    return ts;
}

static void send_manifest(net_t *n, manifest_t *m)
{
    net_w_u32(n, (u32)m->n);
    for (size_t i = 0; i < m->n; i++) {
        net_w_str(n, m->v[i].path);
        net_w_u64(n, m->v[i].size);
        net_w_u32(n, m->v[i].crc);
    }
}

static bool execute_plan(net_t *n, AccountUid uid, u64 app_id, const char *title_name,
                         plan_t *plan, sync_ui_t *ui, sync_stats_t *st)
{
    for (size_t i = 0; i < plan->n; i++) {
        const char *rel = plan->v[i].path;

        if (ui && ui->progress) ui->progress(ui->ud, title_name, i, plan->n);

        switch (plan->v[i].action) {
        case ACT_PULL:
            ui_log(ui, "  <- %s", rel);
            if (!do_pull(n, uid, app_id, rel, ui)) return false;
            st->pulled++;
            break;

        case ACT_PUSH:
            ui_log(ui, "  -> %s", rel);
            if (!do_push(n, uid, app_id, rel, ui)) return false;
            st->pushed++;
            break;

        case ACT_DEL_LOCAL:
            ui_log(ui, "  x  %s (borrado en el PC)", rel);
            if (!mf_delete(MOUNT_ROOT, rel)) {
                ui_log(ui, "  ! no se pudo borrar %s", rel);
                return false;
            }
            st->del_local++;
            break;

        case ACT_DEL_REMOTE:
            ui_log(ui, "  x  %s (borrado en la Switch)", rel);
            if (!do_del_remote(n, uid, app_id, rel, ui)) return false;
            st->del_remote++;
            break;

        case ACT_CONFLICT:
            // Se resuelven antes de llegar aqui; si queda alguno, se ignora.
            break;

        default:
            ui_log(ui, "  ! accion desconocida %u", plan->v[i].action);
            return false;
        }
    }

    if (ui && ui->progress) ui->progress(ui->ud, title_name, plan->n, plan->n);
    return true;
}

bool sync_title(net_t *n, AccountUid uid, const char *user_name,
                u64 app_id, const char *title_name,
                u8 mode, u8 policy, sync_ui_t *ui, sync_stats_t *st)
{
    memset(st, 0, sizeof(*st));

    Result rc = fsdevMountSaveData(MOUNT_NAME, app_id, uid);
    if (R_FAILED(rc)) {
        ui_log(ui, "  ! no se pudo montar el savedata (0x%08X)", rc);
        return false;
    }

    bool ok = false;
    manifest_t m = {0};
    plan_t plan = {0};

    if (!manifest_build(&m, MOUNT_ROOT, NULL, NULL)) {
        ui_log(ui, "  ! no se pudo leer el savedata");
        goto out;
    }

    net_begin(n, OP_PLAN_REQ);
    w_uid(n, uid);
    net_w_str(n, user_name ? user_name : "");
    net_w_u64(n, app_id);
    net_w_str(n, title_name);
    net_w_u8(n, mode);
    net_w_u8(n, policy);
    net_w_u64(n, console_clock());
    net_w_u64(n, manifest_newest(&m));
    send_manifest(n, &m);
    // Ojo: `m` no se libera aqui. Si el plan sale vacio lo reutilizamos para el
    // COMMIT y nos ahorramos volver a leer y recalcular el CRC del save entero.

    if (!net_send(n)) { ui_log(ui, "! %s", net_error(n)); goto out; }

    u8 op;
    if (!net_recv(n, &op)) { ui_log(ui, "! %s", net_error(n)); goto out; }
    if (!expect_op(n, op, OP_PLAN_RES, ui)) goto out;

    // El PC puede anteponer un aviso cuando la situacion es ambigua (una de las
    // dos carpetas vacia, la ruta cambiada...). En ese caso viene sin plan y
    // espera a que el usuario diga que hacer.
    u8 warning = WARN_NONE;
    char wmsg[512] = "";
    if (!net_r_u8(n, &warning) || !net_r_str(n, wmsg, sizeof(wmsg))) {
        ui_log(ui, "! %s", net_error(n));
        goto out;
    }
    if (!plan_read(n, &plan, ui)) goto out;

    if (warning != WARN_NONE) {
        st->warning = warning;

        int decision = ui && ui->ask_warning
                     ? ui->ask_warning(ui->ud, title_name, warning, wmsg)
                     : DEC_SKIP;

        if (decision != DEC_SWITCH && decision != DEC_PC) {
            ui_log(ui, "  se deja este juego como esta");
            st->skipped = true;
            ok = true;
            goto out;
        }

        net_begin(n, OP_DECIDE);
        w_uid(n, uid);
        net_w_u64(n, app_id);
        net_w_u8(n, (u8)decision);
        if (!net_send(n)) { ui_log(ui, "! %s", net_error(n)); goto out; }

        if (!net_recv(n, &op)) { ui_log(ui, "! %s", net_error(n)); goto out; }
        if (!expect_op(n, op, OP_DECIDE_RES, ui)) goto out;

        plan_free(&plan);
        if (!plan_read(n, &plan, ui)) goto out;
    }

    // Conflictos: se decide por juego entero, nunca archivo a archivo.
    size_t n_conflicts = 0;
    for (size_t i = 0; i < plan.n; i++)
        if (plan.v[i].action == ACT_CONFLICT) n_conflicts++;

    if (n_conflicts > 0) {
        st->conflicts = (int)n_conflicts;

        int winner = ui && ui->ask_conflict
                   ? ui->ask_conflict(ui->ud, title_name, n_conflicts)
                   : -1;

        if (winner != WINNER_SWITCH && winner != WINNER_PC) {
            ui_log(ui, "  conflicto sin resolver, se salta este juego");
            st->skipped = true;
            ok = true;   // no es un error: el usuario decidio no tocarlo
            goto out;
        }

        net_begin(n, OP_RESOLVE);
        w_uid(n, uid);
        net_w_u64(n, app_id);
        net_w_u8(n, (u8)winner);
        if (!net_send(n)) { ui_log(ui, "! %s", net_error(n)); goto out; }

        if (!net_recv(n, &op)) { ui_log(ui, "! %s", net_error(n)); goto out; }
        if (!expect_op(n, op, OP_RESOLVE_RES, ui)) goto out;

        // El plan resuelto sustituye por completo al anterior.
        plan_free(&plan);
        if (!plan_read(n, &plan, ui)) goto out;
    }

    // Cambios que vienen del PC hacia la consola. Si hay callback puesto, se
    // pregunta antes de aplicarlos: bajar del PC sobreescribe la partida de la
    // consola, y no siempre es lo que se quiere aunque el PC sea el que cambio.
    if (ui && ui->ask_incoming && plan.n > 0) {
        size_t entrantes = 0, borrados = 0;
        for (size_t i = 0; i < plan.n; i++) {
            if (plan.v[i].action == ACT_PULL)      entrantes++;
            else if (plan.v[i].action == ACT_DEL_LOCAL) borrados++;
        }

        if (entrantes + borrados > 0) {
            int decision = ui->ask_incoming(ui->ud, title_name, entrantes, borrados);

            if (decision == DEC_SKIP) {
                ui_log(ui, "  se deja este juego como esta");
                st->skipped = true;
                ok = true;
                goto out;
            }

            // DEC_PC = aplicar el plan tal cual (lo del PC manda, que es lo que
            // ya proponia). DEC_SWITCH = darle la vuelta y subir lo de aqui.
            if (decision == DEC_SWITCH) {
                net_begin(n, OP_DECIDE);
                w_uid(n, uid);
                net_w_u64(n, app_id);
                net_w_u8(n, DEC_SWITCH);
                if (!net_send(n)) { ui_log(ui, "! %s", net_error(n)); goto out; }

                if (!net_recv(n, &op)) { ui_log(ui, "! %s", net_error(n)); goto out; }
                if (!expect_op(n, op, OP_DECIDE_RES, ui)) goto out;

                plan_free(&plan);
                if (!plan_read(n, &plan, ui)) goto out;
            }
        }
    }

    if (plan.n == 0) {
        // Nada que transferir, pero se confirma igual: el PC necesita registrar
        // la base para esta carpeta. Si no lo hiciera, un cambio de ruta en el
        // PC dejaria la base vacia para siempre y no podria distinguir "archivo
        // nuevo" de "archivo borrado" en la siguiente sincronizacion.
        ui_log(ui, "  ya estaba al dia");
    } else {
        if (!execute_plan(n, uid, app_id, title_name, &plan, ui, st)) goto out;

        // Sin esto la Switch descarta todo lo escrito al desmontar.
        rc = fsdevCommitDevice(MOUNT_NAME);
        if (R_FAILED(rc)) {
            ui_log(ui, "  ! fallo el commit del savedata (0x%08X)", rc);
            goto out;
        }

        // El manifiesto final es la nueva base para la proxima sincronizacion.
        manifest_free(&m);
        if (!manifest_build(&m, MOUNT_ROOT, NULL, NULL)) {
            ui_log(ui, "  ! no se pudo releer el savedata tras el commit");
            goto out;
        }
    }

    net_begin(n, OP_COMMIT);
    w_uid(n, uid);
    net_w_u64(n, app_id);
    send_manifest(n, &m);

    if (!net_send(n)) { ui_log(ui, "! %s", net_error(n)); goto out; }
    if (!net_recv(n, &op)) { ui_log(ui, "! %s", net_error(n)); goto out; }
    if (!expect_op(n, op, OP_COMMIT_OK, ui)) goto out;

    ok = true;

out:
    manifest_free(&m);
    plan_free(&plan);
    fsdevUnmountDevice(MOUNT_NAME);
    return ok;
}
