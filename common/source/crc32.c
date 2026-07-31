#include "crc32.h"

#include <stdio.h>

static u32 s_table[256];
static bool s_table_ready = false;

static void build_table(void)
{
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        s_table[i] = c;
    }
    s_table_ready = true;
}

u32 crc32_update(u32 crc, const void *data, size_t len)
{
    if (!s_table_ready) build_table();

    const u8 *p = (const u8 *)data;
    crc = ~crc;
    while (len--)
        crc = s_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

bool crc32_file(const char *path, u32 *out_crc, u64 *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    // 64 KB: el fs de la Switch va bastante mas rapido con lecturas grandes
    // que con las de 4 KB por defecto de stdio.
    static u8 buf[64 * 1024];
    u32 crc = 0;
    u64 total = 0;
    size_t n;

    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        crc = crc32_update(crc, buf, n);
        total += n;
    }

    bool ok = !ferror(f);
    fclose(f);

    if (!ok) return false;

    if (out_crc)  *out_crc = crc;
    if (out_size) *out_size = total;
    return true;
}
