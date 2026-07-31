// Vuelca la musica de fondo a un WAV para poder escucharla en el PC.
//
//   ./musica ambiente.wav
//
// Usa el mismo generador que la consola (audio.c), asi que lo que suene aqui es
// exactamente lo que sonara alli.

#include "audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define RATE 48000

static void put32(FILE *f, unsigned v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, unsigned short v) { fwrite(&v, 2, 1, f); }

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "ambiente.wav";

    int frames = audio_music_frames();
    int16_t *buf = malloc((size_t)frames * 2 * sizeof(int16_t));
    if (!buf) return 1;

    audio_music_render(buf);

    // El bucle util es lo que queda al quitar la cola, que ya se ha plegado
    // sobre el principio. Son 32 s exactos que encadenan consigo mismos.
    int loop = frames - 2 * RATE;
    unsigned bytes = (unsigned)loop * 2 * sizeof(int16_t);

    FILE *f = fopen(path, "wb");
    if (!f) { free(buf); return 1; }

    fwrite("RIFF", 1, 4, f);  put32(f, 36 + bytes);  fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);  put32(f, 16);
    put16(f, 1); put16(f, 2); put32(f, RATE);
    put32(f, RATE * 2 * 2); put16(f, 4); put16(f, 16);
    fwrite("data", 1, 4, f);  put32(f, bytes);
    fwrite(buf, 1, bytes, f);
    fclose(f);

    // Un resumen para saber si esta bien sin tener que escucharlo.
    long pico = 0; double suma = 0;
    for (int i = 0; i < loop * 2; i++) {
        long v = buf[i] < 0 ? -buf[i] : buf[i];
        if (v > pico) pico = v;
        suma += (double)buf[i] * buf[i];
    }
    double rms = sqrt(suma / (loop * 2));

    printf("%s  %d s  pico %ld/32767 (%.0f%%)  rms %.0f (%.1f%% de escala)\n",
           path, loop / RATE, pico, pico * 100.0 / 32767.0, rms, rms * 100.0 / 32767.0);

    // Costura del bucle: el ultimo fotograma contra el primero.
    int d = abs(buf[0] - buf[(loop - 1) * 2]);
    printf("salto en la costura: %d (por debajo de ~300 no se oye)\n", d);

    free(buf);
    return 0;
}
