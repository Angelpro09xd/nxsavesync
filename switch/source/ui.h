#pragma once

#include <switch.h>
#include <SDL2/SDL.h>
#include <stdbool.h>

#define UI_W 1280
#define UI_H 720

typedef struct { u8 r, g, b, a; } color_t;

// Paleta. Oscura, con acento cian, para que no cante en la tele ni en portatil.
#define COL_BG        ((color_t){ 0x14, 0x17, 0x1C, 0xFF })
#define COL_PANEL     ((color_t){ 0x1E, 0x23, 0x2B, 0xFF })
#define COL_PANEL_HI  ((color_t){ 0x2A, 0x31, 0x3C, 0xFF })
#define COL_ACCENT    ((color_t){ 0x2E, 0xC4, 0xD3, 0xFF })
#define COL_TEXT      ((color_t){ 0xEC, 0xEF, 0xF2, 0xFF })
#define COL_DIM       ((color_t){ 0x8A, 0x93, 0xA0, 0xFF })
#define COL_OK        ((color_t){ 0x5F, 0xD0, 0x7A, 0xFF })
#define COL_WARN      ((color_t){ 0xF0, 0xB4, 0x3C, 0xFF })
#define COL_ERR       ((color_t){ 0xE8, 0x5D, 0x5D, 0xFF })
#define COL_SHADOW    ((color_t){ 0x00, 0x00, 0x00, 0x60 })

typedef struct {
    // botones pulsados en este fotograma
    u64  down;
    u64  held;
    // tactil
    bool touched;        // hay un dedo tocando
    bool tap;            // se ha levantado el dedo (toque completo)
    int  touch_x, touch_y;
    int  tap_x, tap_y;
} ui_input_t;

bool ui_init(void);
void ui_exit(void);

// Devuelve false si el sistema pide cerrar la app.
bool ui_frame_begin(ui_input_t *in);
void ui_frame_end(void);

// --- dibujo ---
void ui_clear(color_t c);
void ui_rect(int x, int y, int w, int h, color_t c);
void ui_rect_round(int x, int y, int w, int h, int r, color_t c);
void ui_rect_outline(int x, int y, int w, int h, int thick, color_t c);

void ui_text(int x, int y, int size, color_t c, const char *fmt, ...);
void ui_text_center(int cx, int y, int size, color_t c, const char *fmt, ...);
int  ui_text_w(int size, const char *s);
// Recorta con puntos suspensivos si no cabe en max_w.
void ui_text_clip(int x, int y, int size, int max_w, color_t c, const char *s);

// --- imagenes ---
// Cachea por puntero: pasa siempre el mismo buffer para el mismo icono.
SDL_Texture *ui_image(const void *jpeg, size_t len);
void ui_image_draw(SDL_Texture *t, int x, int y, int w, int h);
// `bg` es el color del panel sobre el que se dibuja: las esquinas se tapan
// con el, no con el fondo global, o quedan muescas oscuras alrededor.
void ui_image_draw_round(SDL_Texture *t, int x, int y, int w, int h, int r,
                         color_t bg);
void ui_images_clear(void);

// Color medio del icono. Sirve para tenir la interfaz con el juego elegido.
color_t ui_image_color(const void *jpeg, size_t len);

// --- utilidades ---
bool ui_hit(const ui_input_t *in, int x, int y, int w, int h);   // toque dentro
void ui_spinner(int cx, int cy, int r, color_t c);
u32  ui_ticks(void);

// Segundos desde el fotograma anterior, para que las animaciones no dependan
// de si va a 60 o a 30.
float ui_dt(void);

// Acerca `cur` a `target` un poco cada fotograma. `rate` mas alto = mas rapido.
float ui_approach(float cur, float target, float rate);

void ui_gradient_v(int x, int y, int w, int h, color_t top, color_t bottom);
// Igual pero respetando las esquinas superiores redondeadas del panel.
void ui_gradient_round_top(int x, int y, int w, int h, int r,
                           color_t top, color_t bottom);
void ui_rect_round_outline(int x, int y, int w, int h, int r, int thick, color_t c);

// Arco grueso entre dos angulos en grados (0 = derecha, crece en horario).
void ui_arc(int cx, int cy, int radius, int thick, float from_deg, float to_deg, color_t c);
void ui_tri(int x1, int y1, int x2, int y2, int x3, int y3, color_t c);
void ui_circle(int cx, int cy, int r, color_t c);

// El logo: dos flechas en circulo, el mismo dibujo que el icono del homebrew.
void ui_logo(int cx, int cy, int size, color_t c1, color_t c2, color_t dot);

// Anillo de progreso (0..1). Con progress < 0 gira solo, como espera.
void ui_ring(int cx, int cy, int radius, int thick, float progress,
             color_t fg, color_t bg);

color_t ui_mix(color_t a, color_t b, float t);
color_t ui_alpha(color_t c, u8 a);
