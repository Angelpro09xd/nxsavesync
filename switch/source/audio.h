#pragma once

#include <stdbool.h>
#include <stdint.h>

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

// --------------------------------------------------------------------------
// musica de fondo
// --------------------------------------------------------------------------
//
// Un bucle ambiental sintetizado, a juego con la interfaz: un acorde lento que
// respira y algun repique de cristal encima. Se genera en un hilo aparte y
// empieza a sonar cuando esta listo, para no retrasar el arranque de la app.

// Para el previsualizador del PC: genera el bucle en un buffer propio, sin
// tocar SDL_mixer. Asi la musica se puede escuchar antes de copiar nada a la SD.
int  audio_music_frames(void);      // fotogramas del buffer (incluye la cola)
void audio_music_render(int16_t *out);

void audio_music_init(void);     // lanza la generacion
void audio_music_poll(void);     // llamar cada fotograma: arranca al estar lista
void audio_set_music(bool on);
bool audio_music_enabled(void);
