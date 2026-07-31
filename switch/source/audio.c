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

// Definidos en la seccion de musica, al final.
static int16_t   *g_music_buf;
static Mix_Chunk *g_music;
static SDL_Thread *g_music_thread;
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

    Mix_AllocateChannels(12);
    // El canal 0 queda reservado para la musica: asi Mix_PlayChannel(-1) de un
    // efecto nunca se lo puede quitar de debajo.
    Mix_ReserveChannels(1);

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

    if (g_music_thread) { SDL_WaitThread(g_music_thread, NULL); g_music_thread = NULL; }
    if (g_music) { Mix_FreeChunk(g_music); g_music = NULL; }
    free(g_music_buf);
    g_music_buf = NULL;

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

// --------------------------------------------------------------------------
// musica de fondo
// --------------------------------------------------------------------------
//
// Ambiente alegre, a juego con el cristal. Tres capas:
//
//   1. El acorde, cuatro voces rotando por Do - Sol - Lam - Fa (I-V-vi-IV), que
//      es la progresion mas luminosa que existe. Cada nota lleva dos
//      osciladores desafinados un pelin entre si para que "flote".
//   2. Un arpegio corto que sube y baja por las notas del acorde. Es lo que le
//      da alegria: sin el, cuatro voces largas suenan a sala de espera.
//   3. Repiques con parciales de campana, y debajo una capa de aire.
//
// El bucle dura 32 segundos y se cose consigo mismo, asi que no hay costura
// audible al repetir.

#define MUS_SECONDS  32
#define MUS_FADE_S   2                       // cola que se pliega sobre el inicio
#define MUS_FRAMES   (MUS_SECONDS * RATE)
#define MUS_FADE     (MUS_FADE_S * RATE)

#define MUS_CHORDS_N 8                       // dos vueltas de cuatro acordes
#define MUS_CHORD_S  (MUS_SECONDS / (float)MUS_CHORDS_N)   // 4 s cada uno

// Corcheas a 93,75 pulsos por minuto. El numero no es redondo a proposito:
// tiene que caber un numero *entero* de corcheas en el bucle. Con 0,3125 s
// entraban 102,4 y al dar la vuelta el arpegio saltaba de nota a media
// envolvente: un chasquido cada 32 segundos.
#define MUS_STEP_S   0.32f
#define MUS_STEPS    ((int)(MUS_SECONDS / MUS_STEP_S))     // 100 exactas

#define SINE_BITS 12
#define SINE_SIZE (1 << SINE_BITS)

static float g_sine[SINE_SIZE];

// Tabla en vez de llamar a sin() por muestra: son millon y medio de fotogramas
// por unas cuantas docenas de osciladores, y en la consola eso se nota.
static void sine_table_init(void)
{
    for (int i = 0; i < SINE_SIZE; i++)
        g_sine[i] = (float)sin(2.0 * M_PI * i / SINE_SIZE);
}

static inline float sine_at(double phase)
{
    double f = phase * SINE_SIZE;
    int    i = (int)f;
    float  k = (float)(f - i);
    return g_sine[i & (SINE_SIZE - 1)] * (1.0f - k)
         + g_sine[(i + 1) & (SINE_SIZE - 1)] * k;
}

static inline float osc(double *phase, double inc)
{
    *phase += inc;
    if (*phase >= 1.0) *phase -= 1.0;
    return sine_at(*phase);
}

// Do - Sol - Lam - Fa, dos vueltas. Cuatro voces: bajo, relleno y dos arriba.
// La voz de arriba dibuja mi - re - mi - do, que es una melodia por si sola.
static const float MUS_CHORDS[4][4] = {
    { 130.81f, 196.00f, 261.63f, 329.63f },   // Do   (Do3 Sol3 Do4 Mi4)
    {  98.00f, 146.83f, 246.94f, 293.66f },   // Sol  (Sol2 Re3 Si3 Re4)
    { 110.00f, 164.81f, 261.63f, 329.63f },   // Lam  (La2 Mi3 Do4 Mi4)
    {  87.31f, 130.81f, 220.00f, 261.63f },   // Fa   (Fa2 Do3 La3 Do4)
};

// Notas por las que corre el arpegio en cada acorde, ya en octava alta.
static const float MUS_ARP[4][4] = {
    { 523.25f, 659.25f, 783.99f, 659.25f },   // Do5 Mi5 Sol5 Mi5
    { 493.88f, 587.33f, 783.99f, 587.33f },   // Si4 Re5 Sol5 Re5
    { 523.25f, 659.25f, 880.00f, 659.25f },   // Do5 Mi5 La5 Mi5
    { 523.25f, 698.46f, 880.00f, 698.46f },   // Do5 Fa5 La5 Fa5
};

// Distancia en el tiempo teniendo en cuenta que el bucle da la vuelta.
static float wrap_dist(float t, float center)
{
    float d = fabsf(t - center);
    return d > MUS_SECONDS / 2.0f ? MUS_SECONDS - d : d;
}

// Envolvente de cada acorde: una campana suave centrada en su turno. Se solapan,
// asi que uno entra mientras el anterior aun se apaga y nunca hay un corte.
static float chord_env(float t, int k)
{
    const float half = MUS_CHORD_S * 1.55f;
    float d = wrap_dist(t, (k + 0.5f) * MUS_CHORD_S);
    if (d >= half) return 0.0f;
    float x = cosf(d / half * (float)M_PI / 2.0f);
    return x * x;
}

// Parciales de campana. Las relaciones no son enteras a proposito: eso es lo
// que distingue un cristal de una flauta.
static const struct { float ratio, amp, decay; } BELL[] = {
    { 1.00f, 1.00f, 1.5f },
    { 2.00f, 0.44f, 1.0f },
    { 2.76f, 0.30f, 0.7f },
    { 5.40f, 0.14f, 0.45f },
};
#define BELL_N ((int)(sizeof(BELL) / sizeof(BELL[0])))

// Repiques: cuando, a que altura y hacia que lado. Todos son notas del acorde
// que suena en ese momento, asi que nunca choca nada.
static const struct { float at, freq, pan; } MUS_PINGS[] = {
    {  3.1f, 1046.50f, -0.55f },   // Do6
    {  7.4f,  987.77f,  0.45f },   // Si5
    { 11.3f, 1318.51f, -0.30f },   // Mi6
    { 15.7f, 1174.66f,  0.60f },   // Re6
    { 19.2f, 1046.50f, -0.50f },
    { 23.6f, 1567.98f,  0.35f },   // Sol6
    { 27.4f, 1318.51f, -0.20f },
    { 30.8f, 1760.00f,  0.50f },   // La6
};
#define MUS_PING_N ((int)(sizeof(MUS_PINGS) / sizeof(MUS_PINGS[0])))

static volatile bool g_music_ready;
static bool g_music_on = true;
static bool g_music_playing;

#define MUS_CHANNEL 0

int audio_music_frames(void) { return MUS_FRAMES + MUS_FADE; }

void audio_music_render(int16_t *out)
{
    const int total = MUS_FRAMES + MUS_FADE;

    // El previsualizador llama aqui directamente, sin pasar por el hilo.
    if (g_sine[1] == 0.0f) sine_table_init();

    // Estado de los osciladores del pad: 4 acordes x 4 voces x 3 osciladores
    // (dos desafinados y una octava por debajo, muy floja).
    static double ph[4][4][3];
    memset(ph, 0, sizeof(ph));

    double air_lp = 0.0;
    uint32_t rnd = 0x1234567u;

    for (int i = 0; i < total; i++) {
        float t = (float)(i % MUS_FRAMES) / RATE;
        float l = 0.0f, r = 0.0f;

        // --- el acorde ---
        for (int k = 0; k < MUS_CHORDS_N; k++) {
            float env = chord_env(t, k);
            if (env < 0.002f) continue;          // acorde callado: ni se calcula

            const float *notas = MUS_CHORDS[k % 4];

            for (int v = 0; v < 4; v++) {
                float f = notas[v];

                // Cada voz respira a su ritmo, para que el acorde no suene fijo.
                // Multiplos de 1/32 Hz: asi cada voz completa un numero entero
                // de respiraciones dentro del bucle y el nivel coincide al dar
                // la vuelta.
                float breathe = 0.84f + 0.16f * sinf(t * ((6 + v) / 32.0f) * 6.2831f
                                                     + k * 1.7f + v);
                float a = env * breathe * (v == 0 ? 0.30f : 0.19f);

                double *p = ph[k % 4][v];
                float s = osc(&p[0], f           / RATE)
                        + osc(&p[1], f * 1.0016f / RATE) * 0.9f
                        + osc(&p[2], f * 0.5f    / RATE) * 0.26f;

                // Las voces graves al centro y las agudas abiertas: da anchura
                // sin descolocar el bajo.
                float pan = (v - 1.5f) * 0.22f;
                l += s * a * (1.0f - pan) * 0.5f;
                r += s * a * (1.0f + pan) * 0.5f;
            }
        }

        // --- el arpegio ---
        //
        // Es lo que separa "ambiente alegre" de "sala de espera": notas cortas y
        // brillantes que suben y bajan por el acorde. La fase se deduce del
        // tiempo, asi que no hace falta guardar estado y el bucle sigue exacto.
        {
            int   paso = (int)(t / MUS_STEP_S);
            float dt   = t - paso * MUS_STEP_S;

            for (int atras = 0; atras < 2; atras++) {   // la nota anterior aun suena
                int   pp = paso - atras;
                float pt = dt + atras * MUS_STEP_S;
                if (pp < 0) { pp += MUS_STEPS; pt = dt + atras * MUS_STEP_S; }

                float e = expf(-pt / 0.30f);
                if (e < 0.004f) continue;

                int acorde = (int)((pp * MUS_STEP_S) / MUS_CHORD_S) % 4;
                // Las cuatro notas de la fila ya suben y bajan (la ultima repite
                // la segunda), y cuatro divide a cien: el ciclo cierra justo al
                // dar la vuelta el bucle.
                float f = MUS_ARP[acorde][pp % 4];

                // Timbre de pua: el fundamental con dos armonicos flojos.
                float s = sine_at(fmod((double)pt * f, 1.0))
                        + sine_at(fmod((double)pt * f * 2.0, 1.0)) * 0.30f
                        + sine_at(fmod((double)pt * f * 3.0, 1.0)) * 0.12f;

                // Va alternando de lado, que da sensacion de movimiento.
                float pan = (pp % 2) ? 0.34f : -0.34f;
                float a = 0.062f * e;
                l += s * a * (1.0f - pan) * 0.5f;
                r += s * a * (1.0f + pan) * 0.5f;
            }
        }

        // --- repiques de cristal ---
        for (int p = 0; p < MUS_PING_N; p++) {
            float dt = t - MUS_PINGS[p].at;
            if (dt < 0.0f) dt += MUS_SECONDS;    // el que cae al final da la vuelta
            if (dt > 3.5f) continue;

            float s = 0.0f;
            for (int b = 0; b < BELL_N; b++) {
                float e = expf(-dt / BELL[b].decay);
                if (e < 0.001f) continue;
                double phase = fmod((double)dt * MUS_PINGS[p].freq * BELL[b].ratio, 1.0);
                s += sine_at(phase) * BELL[b].amp * e;
            }

            float a = 0.070f;
            l += s * a * (1.0f - MUS_PINGS[p].pan) * 0.5f;
            r += s * a * (1.0f + MUS_PINGS[p].pan) * 0.5f;
        }

        // --- aire ---
        rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5;
        float n = ((float)(int32_t)rnd / 2147483648.0f);
        air_lp += (n - air_lp) * 0.016;          // paso bajo de un polo
        float air = (float)air_lp * 0.045f * (0.6f + 0.4f * sinf(t * (7 / 32.0f) * 6.2831f));
        l += air;
        r += air;

        // Saturacion suave: recorta los picos sin el chasquido del recorte duro.
        l = tanhf(l * 1.15f) * 0.92f;
        r = tanhf(r * 1.15f) * 0.92f;

        out[i * 2 + 0] = (int16_t)(l * 32767.0f);
        out[i * 2 + 1] = (int16_t)(r * 32767.0f);
    }

    // Coser el bucle: la cola se pliega sobre el principio con un cruce, asi que
    // al repetir no hay salto. Sin esto se oye un golpe cada 32 segundos.
    for (int i = 0; i < MUS_FADE; i++) {
        float k = (float)i / MUS_FADE;
        for (int c = 0; c < 2; c++) {
            int idx = i * 2 + c;
            int tail = (MUS_FRAMES + i) * 2 + c;
            out[idx] = (int16_t)(out[idx] * k + out[tail] * (1.0f - k));
        }
    }
}

static int music_thread(void *ud)
{
    (void)ud;
    sine_table_init();
    audio_music_render(g_music_buf);
    g_music_ready = true;
    return 0;
}

void audio_music_init(void)
{
    if (!g_ready || g_music_buf) return;

    // Se genera en un hilo aparte: son 32 segundos de audio y hacerlo en el
    // arranque dejaba la app parada un rato antes de mostrar nada.
    size_t bytes = (size_t)(MUS_FRAMES + MUS_FADE) * 2 * sizeof(int16_t);
    g_music_buf = malloc(bytes);
    if (!g_music_buf) return;

    g_music_thread = SDL_CreateThread(music_thread, "nxss-music", NULL);
    if (!g_music_thread) { free(g_music_buf); g_music_buf = NULL; }
}

void audio_music_poll(void)
{
    if (!g_music_ready || g_music) return;

    if (g_music_thread) { SDL_WaitThread(g_music_thread, NULL); g_music_thread = NULL; }

    size_t bytes = (size_t)MUS_FRAMES * 2 * sizeof(int16_t);
    g_music = Mix_QuickLoad_RAW((Uint8 *)g_music_buf, (Uint32)bytes);
    if (!g_music) return;

    Mix_VolumeChunk(g_music, 30);            // de fondo es de fondo
    if (g_music_on) {
        Mix_PlayChannel(MUS_CHANNEL, g_music, -1);
        g_music_playing = true;
    }
}

void audio_set_music(bool on)
{
    g_music_on = on;
    if (!g_music) return;

    if (on && !g_music_playing) {
        Mix_PlayChannel(MUS_CHANNEL, g_music, -1);
        g_music_playing = true;
    } else if (!on && g_music_playing) {
        Mix_HaltChannel(MUS_CHANNEL);
        g_music_playing = false;
    }
}

bool audio_music_enabled(void) { return g_music_on; }
