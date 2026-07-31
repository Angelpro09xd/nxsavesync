#include "audio.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RATE     48000
#define CHANNELS 2

static Mix_Chunk *g_snd[SND_COUNT];
static bool       g_ready;
static bool       g_on = true;

// --------------------------------------------------------------------------
// sintesis
// --------------------------------------------------------------------------
//
// Todo se genera aqui en vez de traer .wav sueltos: son cuatro ondas simples y
// asi la app no arrastra una carpeta de recursos que se pueda perder al copiar.

typedef struct {
    float freq_from, freq_to;   // barrido de frecuencia
    int   ms;
    float vol;
    float attack, release;      // fracciones del total (0..1)
    int   harmonics;            // 1 = seno puro; mas = timbre con cuerpo
    bool  square;
} tone_t;

// Envolvente: sin esto los tonos chasquean al empezar y al cortar.
static float envelope(float t, float attack, float release)
{
    if (t < attack)       return t / attack;
    if (t > 1.0f - release) return (1.0f - t) / release;
    return 1.0f;
}

static void render_tone(int16_t *out, int frames, const tone_t *tone, float mix)
{
    double phase = 0.0;

    for (int i = 0; i < frames; i++) {
        float t = (float)i / (float)frames;
        float freq = tone->freq_from + (tone->freq_to - tone->freq_from) * t;

        phase += 2.0 * M_PI * freq / RATE;

        double v;
        if (tone->square) {
            v = sin(phase) >= 0.0 ? 0.6 : -0.6;
        } else {
            v = sin(phase);
            // Un par de armonicos suaves para que no suene a pitido de reloj.
            for (int h = 2; h <= tone->harmonics; h++)
                v += sin(phase * h) / (h * 2.0);
        }

        float amp = tone->vol * envelope(t, tone->attack, tone->release);
        int sample = (int)(v * amp * 32767.0 * mix);

        for (int c = 0; c < CHANNELS; c++) {
            int idx = i * CHANNELS + c;
            int acc = out[idx] + sample;
            if (acc >  32767) acc =  32767;
            if (acc < -32768) acc = -32768;
            out[idx] = (int16_t)acc;
        }
    }
}

// Encadena varios tonos, uno detras de otro.
static Mix_Chunk *make_sequence(const tone_t *tones, int count)
{
    int total_frames = 0;
    for (int i = 0; i < count; i++)
        total_frames += tones[i].ms * RATE / 1000;

    size_t bytes = (size_t)total_frames * CHANNELS * sizeof(int16_t);
    int16_t *buf = calloc(1, bytes);
    if (!buf) return NULL;

    int offset = 0;
    for (int i = 0; i < count; i++) {
        int frames = tones[i].ms * RATE / 1000;
        render_tone(buf + (size_t)offset * CHANNELS, frames, &tones[i], 1.0f);
        offset += frames;
    }

    Mix_Chunk *chunk = Mix_QuickLoad_RAW((Uint8 *)buf, (Uint32)bytes);
    if (!chunk) { free(buf); return NULL; }

    // Mix_QuickLoad_RAW no copia el buffer; lo liberamos nosotros al salir.
    chunk->allocated = 1;
    return chunk;
}

// Acorde: varios tonos a la vez, para el sonido de "terminado".
static Mix_Chunk *make_chord(const tone_t *tones, int count)
{
    int frames = 0;
    for (int i = 0; i < count; i++) {
        int f = tones[i].ms * RATE / 1000;
        if (f > frames) frames = f;
    }

    size_t bytes = (size_t)frames * CHANNELS * sizeof(int16_t);
    int16_t *buf = calloc(1, bytes);
    if (!buf) return NULL;

    for (int i = 0; i < count; i++) {
        int f = tones[i].ms * RATE / 1000;
        render_tone(buf, f, &tones[i], 1.0f / count);
    }

    Mix_Chunk *chunk = Mix_QuickLoad_RAW((Uint8 *)buf, (Uint32)bytes);
    if (!chunk) { free(buf); return NULL; }
    chunk->allocated = 1;
    return chunk;
}

// --------------------------------------------------------------------------

bool audio_init(void)
{
    if (Mix_OpenAudio(RATE, AUDIO_S16SYS, CHANNELS, 1024) != 0)
        return false;

    Mix_AllocateChannels(8);

    // Moverse: clic corto y bajo, se repite mucho y no debe cansar.
    g_snd[SND_MOVE] = make_sequence((tone_t[]){
        { 880.0f, 940.0f, 28, 0.18f, 0.15f, 0.60f, 2, false },
    }, 1);

    // Aceptar: dos notas hacia arriba.
    g_snd[SND_SELECT] = make_sequence((tone_t[]){
        { 660.0f, 660.0f, 40, 0.26f, 0.10f, 0.35f, 3, false },
        { 990.0f, 990.0f, 70, 0.26f, 0.05f, 0.55f, 3, false },
    }, 2);

    // Volver: las mismas dos, del reves.
    g_snd[SND_BACK] = make_sequence((tone_t[]){
        { 620.0f, 620.0f, 36, 0.22f, 0.10f, 0.35f, 2, false },
        { 440.0f, 440.0f, 64, 0.22f, 0.05f, 0.60f, 2, false },
    }, 2);

    // Cambiar un ajuste: un toque seco con un poco de cuerpo.
    g_snd[SND_TOGGLE] = make_sequence((tone_t[]){
        { 520.0f, 700.0f, 45, 0.20f, 0.12f, 0.50f, 2, false },
    }, 1);

    // Error: grave y con onda cuadrada, para que se note que algo va mal.
    g_snd[SND_ERROR] = make_sequence((tone_t[]){
        { 240.0f, 200.0f, 90, 0.24f, 0.08f, 0.40f, 1, true },
        { 180.0f, 150.0f, 130, 0.22f, 0.05f, 0.55f, 1, true },
    }, 2);

    // Aviso: dos toques iguales, como quien llama a la puerta.
    g_snd[SND_WARN] = make_sequence((tone_t[]){
        { 700.0f, 700.0f, 60, 0.24f, 0.10f, 0.40f, 2, false },
        {   0.0f,   0.0f, 45, 0.00f, 0.50f, 0.50f, 1, false },
        { 700.0f, 700.0f, 60, 0.24f, 0.10f, 0.45f, 2, false },
    }, 3);

    // Empezar: barrido corto hacia arriba.
    g_snd[SND_START] = make_sequence((tone_t[]){
        { 380.0f, 780.0f, 110, 0.20f, 0.12f, 0.45f, 2, false },
    }, 1);

    // Terminado: acorde mayor, que suene a cosa bien hecha.
    g_snd[SND_DONE] = make_chord((tone_t[]){
        {  660.0f,  660.0f, 320, 0.30f, 0.03f, 0.70f, 3, false },
        {  830.0f,  830.0f, 300, 0.26f, 0.06f, 0.70f, 3, false },
        {  990.0f,  990.0f, 340, 0.24f, 0.10f, 0.70f, 3, false },
        { 1320.0f, 1320.0f, 280, 0.16f, 0.18f, 0.70f, 2, false },
    }, 4);

    g_ready = true;
    return true;
}

void audio_exit(void)
{
    if (!g_ready) return;

    for (int i = 0; i < SND_COUNT; i++) {
        if (g_snd[i]) {
            // Con allocated=1 el propio Mix_FreeChunk libera el buffer; hacerlo
            // tambien aqui seria liberarlo dos veces.
            Mix_FreeChunk(g_snd[i]);
            g_snd[i] = NULL;
        }
    }

    Mix_CloseAudio();
    g_ready = false;
}

void audio_play(sound_t s)
{
    if (!g_ready || !g_on || s < 0 || s >= SND_COUNT || !g_snd[s]) return;
    // -1 = primer canal libre; si estan todos ocupados simplemente se pierde,
    // que es preferible a cortar un sonido a medias.
    Mix_PlayChannel(-1, g_snd[s], 0);
}

void audio_set_enabled(bool on) { g_on = on; }
bool audio_enabled(void)        { return g_on; }
