#pragma once

#include <switch.h>
#include <stddef.h>

// CRC32 (polinomio IEEE 802.3, el mismo que usa zlib y el modulo zlib de Python).
u32 crc32_update(u32 crc, const void *data, size_t len);

// Calcula el CRC32 de un archivo entero. Devuelve false si no se pudo leer.
bool crc32_file(const char *path, u32 *out_crc, u64 *out_size);
