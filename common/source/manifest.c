#include "manifest.h"
#include "crc32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

bool mf_join(char *out, size_t out_size, const char *root, const char *rel)
{
    int k = snprintf(out, out_size, "%s/%s", root, rel);
    return k > 0 && (size_t)k < out_size;
}

static bool mf_push(manifest_t *m, const char *rel, u64 size, u32 crc)
{
    if (m->n == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 64;
        mf_entry_t *nv = realloc(m->v, cap * sizeof(mf_entry_t));
        if (!nv) return false;
        m->v   = nv;
        m->cap = cap;
    }

    mf_entry_t *e = &m->v[m->n];
    if (strlen(rel) >= sizeof(e->path)) return false;
    strcpy(e->path, rel);
    e->size = size;
    e->crc  = crc;
    m->n++;
    return true;
}

// Recorrido recursivo. `rel` es el prefijo relativo acumulado ("" en la raiz).
static bool walk(manifest_t *m, const char *root, const char *rel,
                 void (*progress)(const char *, size_t, void *), void *ud)
{
    char dirpath[PROTO_MAX_PATH + 64];
    if (rel[0] == '\0')
        snprintf(dirpath, sizeof(dirpath), "%s/", root);
    else if (!mf_join(dirpath, sizeof(dirpath), root, rel))
        return false;

    DIR *d = opendir(dirpath);
    if (!d) return false;

    bool ok = true;
    struct dirent *ent;

    while (ok && (ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;

        char childrel[PROTO_MAX_PATH];
        int k = rel[0] == '\0'
              ? snprintf(childrel, sizeof(childrel), "%s", ent->d_name)
              : snprintf(childrel, sizeof(childrel), "%s/%s", rel, ent->d_name);
        if (k <= 0 || (size_t)k >= sizeof(childrel)) { ok = false; break; }

        char full[PROTO_MAX_PATH + 64];
        if (!mf_join(full, sizeof(full), root, childrel)) { ok = false; break; }

        // d_type no siempre viene relleno en fsdev, asi que confirmamos con stat.
        bool is_dir;
        if (ent->d_type == DT_DIR)      is_dir = true;
        else if (ent->d_type == DT_REG) is_dir = false;
        else {
            struct stat st;
            if (stat(full, &st) != 0) { ok = false; break; }
            is_dir = S_ISDIR(st.st_mode);
        }

        if (is_dir) {
            ok = walk(m, root, childrel, progress, ud);
        } else {
            u32 crc; u64 size;
            if (!crc32_file(full, &crc, &size)) { ok = false; break; }
            ok = mf_push(m, childrel, size, crc);
            if (ok && progress) progress(childrel, m->n, ud);
        }
    }

    closedir(d);
    return ok;
}

bool manifest_build(manifest_t *m, const char *root,
                    void (*progress)(const char *, size_t, void *), void *ud)
{
    memset(m, 0, sizeof(*m));
    return walk(m, root, "", progress, ud);
}

void manifest_free(manifest_t *m)
{
    free(m->v);
    m->v = NULL;
    m->n = m->cap = 0;
}

bool mf_make_parents(const char *root, const char *rel)
{
    char acc[PROTO_MAX_PATH];
    if (strlen(rel) >= sizeof(acc)) return false;
    strcpy(acc, rel);

    // Vamos creando cada nivel intermedio; el ultimo componente es el archivo.
    for (char *p = acc; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';

        char full[PROTO_MAX_PATH + 64];
        if (!mf_join(full, sizeof(full), root, acc)) return false;
        if (mkdir(full, 0777) != 0 && errno != EEXIST) return false;

        *p = '/';
    }
    return true;
}

bool mf_delete(const char *root, const char *rel)
{
    char full[PROTO_MAX_PATH + 64];
    if (!mf_join(full, sizeof(full), root, rel)) return false;

    if (unlink(full) != 0 && errno != ENOENT) return false;

    // Podamos los directorios que hayan quedado vacios. rmdir falla si no lo
    // estan, que es justo lo que queremos, asi que paramos en el primer fallo.
    char acc[PROTO_MAX_PATH];
    if (strlen(rel) >= sizeof(acc)) return true;
    strcpy(acc, rel);

    for (;;) {
        char *slash = strrchr(acc, '/');
        if (!slash) break;
        *slash = '\0';

        if (!mf_join(full, sizeof(full), root, acc)) break;
        if (rmdir(full) != 0) break;
    }

    return true;
}
