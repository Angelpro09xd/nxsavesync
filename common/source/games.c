#include "games.h"
#include "proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// El sysmodule puede no tener 'ns' disponible, y llamar a un servicio sin
// iniciar revienta el proceso. Por eso se puede apagar la consulta de metadatos.
static bool g_metadata = true;

void games_set_metadata_enabled(bool on) { g_metadata = on; }

bool user_nickname(AccountUid uid, char *out, size_t out_size)
{
    AccountProfile     profile;
    AccountProfileBase base;

    if (R_FAILED(accountGetProfile(&profile, uid))) return false;

    Result rc = accountProfileGet(&profile, NULL, &base);
    accountProfileClose(&profile);
    if (R_FAILED(rc)) return false;

    snprintf(out, out_size, "%s", base.nickname);
    return true;
}

// --------------------------------------------------------------------------
// perfiles
// --------------------------------------------------------------------------

bool users_list(userlist_t *u)
{
    memset(u, 0, sizeof(*u));

    AccountUid uids[8];
    s32 count = 0;
    if (R_FAILED(accountListAllUsers(uids, 8, &count)) || count <= 0)
        return false;

    for (s32 i = 0; i < count && u->n < 8; i++) {
        user_t *e = &u->v[u->n];
        memset(e, 0, sizeof(*e));
        e->uid = uids[i];

        if (!user_nickname(uids[i], e->name, sizeof(e->name)))
            snprintf(e->name, sizeof(e->name), "Usuario %d", i + 1);

        AccountProfile profile;
        if (R_SUCCEEDED(accountGetProfile(&profile, uids[i]))) {
            u32 size = 0;
            if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &size)) && size > 0) {
                u8 *buf = malloc(size);
                if (buf) {
                    u32 got = 0;
                    if (R_SUCCEEDED(accountProfileLoadImage(&profile, buf, size, &got)) && got > 0) {
                        e->icon      = buf;
                        e->icon_size = got;
                    } else {
                        free(buf);
                    }
                }
            }
            accountProfileClose(&profile);
        }

        u->n++;
    }

    return u->n > 0;
}

void users_free(userlist_t *u)
{
    for (size_t i = 0; i < u->n; i++) free(u->v[i].icon);
    memset(u, 0, sizeof(*u));
}

// --------------------------------------------------------------------------
// juegos
// --------------------------------------------------------------------------

// Rellena nombre, autor e icono desde el NACP. El control data ronda los 144 KB
// (casi todo icono), asi que reutilizamos un unico buffer para toda la lista y
// solo copiamos el JPEG de los que hagan falta.
static void resolve_meta(u64 app_id, NsApplicationControlData *buf, bool with_icon, game_t *out)
{
    snprintf(out->name, sizeof(out->name), "%016lX", app_id);
    out->author[0] = '\0';
    out->icon      = NULL;
    out->icon_size = 0;

    if (!g_metadata) return;

    u64 actual = 0;
    Result rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, app_id,
                                            buf, sizeof(NsApplicationControlData), &actual);
    if (R_FAILED(rc) || actual < sizeof(buf->nacp)) return;

    NacpLanguageEntry *entry = NULL;
    if (R_SUCCEEDED(nacpGetLanguageEntry(&buf->nacp, &entry)) && entry) {
        if (entry->name[0])   snprintf(out->name, sizeof(out->name), "%s", entry->name);
        if (entry->author[0]) snprintf(out->author, sizeof(out->author), "%s", entry->author);
    }

    if (!with_icon) return;

    size_t icon_size = actual - sizeof(buf->nacp);
    if (icon_size == 0 || icon_size > sizeof(buf->icon)) return;

    u8 *copy = malloc(icon_size);
    if (!copy) return;
    memcpy(copy, buf->icon, icon_size);
    out->icon      = copy;
    out->icon_size = icon_size;
}

static bool push(gamelist_t *g, const game_t *entry)
{
    if (g->n == g->cap) {
        size_t cap = g->cap ? g->cap * 2 : 32;
        game_t *nv = realloc(g->v, cap * sizeof(game_t));
        if (!nv) return false;
        g->v   = nv;
        g->cap = cap;
    }
    g->v[g->n++] = *entry;
    return true;
}

static int cmp_by_name(const void *a, const void *b)
{
    return strcasecmp(((const game_t *)a)->name, ((const game_t *)b)->name);
}

bool games_list(gamelist_t *g, AccountUid uid, bool with_icons)
{
    memset(g, 0, sizeof(*g));

    FsSaveDataInfoReader reader;
    if (R_FAILED(fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_User)))
        return false;

    NsApplicationControlData *ctrl = malloc(sizeof(NsApplicationControlData));
    if (!ctrl) { fsSaveDataInfoReaderClose(&reader); return false; }

    bool ok = true;
    FsSaveDataInfo info[16];
    s64 got = 0;

    while (R_SUCCEEDED(fsSaveDataInfoReaderRead(&reader, info, 16, &got)) && got > 0) {
        for (s64 i = 0; i < got; i++) {
            if (info[i].save_data_type != FsSaveDataType_Account) continue;
            if (info[i].application_id == 0) continue;
            if (!accountUidIsValid(&info[i].uid)) continue;
            if (memcmp(&info[i].uid, &uid, sizeof(AccountUid)) != 0) continue;

            game_t entry;
            memset(&entry, 0, sizeof(entry));
            entry.application_id = info[i].application_id;
            entry.state          = SUM_UNKNOWN;
            resolve_meta(info[i].application_id, ctrl, with_icons, &entry);

            if (!push(g, &entry)) { free(entry.icon); ok = false; break; }
        }
        if (!ok) break;
    }

    free(ctrl);
    fsSaveDataInfoReaderClose(&reader);

    if (ok && g->n > 1)
        qsort(g->v, g->n, sizeof(game_t), cmp_by_name);

    return ok;
}

void games_free(gamelist_t *g)
{
    for (size_t i = 0; i < g->n; i++) free(g->v[i].icon);
    free(g->v);
    g->v = NULL;
    g->n = g->cap = 0;
}
