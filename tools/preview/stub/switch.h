// Lo justo de libnx para que ui.c compile en el PC.
//
// El previsualizador existe para una sola cosa: poder mirar la interfaz sin
// pasar por la SD. Aqui no se emula nada de la consola, solo los tipos y los
// nombres de los botones que ui.c menciona.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

typedef struct { u64 uid[2]; } AccountUid;

enum {
    HidNpadButton_A     = 1u << 0,
    HidNpadButton_B     = 1u << 1,
    HidNpadButton_X     = 1u << 2,
    HidNpadButton_Y     = 1u << 3,
    HidNpadButton_L     = 1u << 6,
    HidNpadButton_R     = 1u << 7,
    HidNpadButton_ZL    = 1u << 8,
    HidNpadButton_ZR    = 1u << 9,
    HidNpadButton_Plus  = 1u << 10,
    HidNpadButton_Minus = 1u << 11,
    HidNpadButton_Left  = 1u << 12,
    HidNpadButton_Up    = 1u << 13,
    HidNpadButton_Right = 1u << 14,
    HidNpadButton_Down  = 1u << 15,
};
