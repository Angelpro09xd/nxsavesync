#pragma once

#include <stdbool.h>

// Efectos de la interfaz. Se generan por sintesis al arrancar, asi que la app
// sigue siendo un solo .nro sin carpeta de recursos al lado.
typedef enum {
    SND_MOVE = 0,   // mover el cursor
    SND_SELECT,     // aceptar
    SND_BACK,       // volver
    SND_TOGGLE,     // cambiar un ajuste
    SND_ERROR,      // algo no se pudo hacer
    SND_DONE,       // sincronizacion terminada
    SND_WARN,       // el PC pide una decision
    SND_START,      // empieza una transferencia
    SND_COUNT
} sound_t;

bool audio_init(void);
void audio_exit(void);
void audio_play(sound_t s);
void audio_set_enabled(bool on);
bool audio_enabled(void);
