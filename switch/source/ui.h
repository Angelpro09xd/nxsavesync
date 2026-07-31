#pragma once

#include <switch.h>
#include <SDL2/SDL.h>
#include <stdbool.h>

#define UI_W 1280
#define UI_H 720

typedef struct { u8 r, g, b, a; } color_t;

// --------------------------------------------------------------------------
// paleta
// --------------------------------------------------------------------------
//
// El fondo es casi negro a proposito: el cristal solo se lee como cristal si
// hay oscuridad debajo y luz de color detras. Los tonos vivos los pone el
// acento, que sale del icono del juego elegido.

#define COL_BG        ((color_t){ 0x07, 0x09, 0x0E, 0xFF })
#define COL_BG_DEEP   ((color_t){ 0x03, 0x04, 0x07, 0xFF })
#define COL_GLASS     ((color_t){ 0xAF, 0xBE, 0xD6, 0xFF })   // tinte del vidrio
#define COL_TEXT      ((color_t){ 0xF4, 0xF7, 0xFB, 0xFF })
#define COL_DIM       ((color_t){ 0x9A, 0xA6, 0xB8, 0xFF })
#define COL_ACCENT    ((color_t){ 0x4A, 0xD8, 0xE6, 0xFF })
#define COL_OK        ((color_t){ 0x5C, 0xE0, 0x9B, 0xFF })
#define COL_WARN      ((color_t){ 0xFF, 0xC1, 0x4D, 0xFF })
#define COL_ERR       ((color_t){ 0xFF, 0x6B, 0x7A, 0xFF })

// Compatibilidad con el codigo que aun pide paneles solidos.
#define COL_PANEL     ((color_t){ 0x14, 0x18, 0x21, 0xFF })
#define COL_PANEL_HI  ((color_t){ 0x21, 0x27, 0x33, 0xFF })

typedef struct {
    u64  down;           // botones pulsados en este fotograma
    u64  held;
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

// --------------------------------------------------------------------------
// capas
// --------------------------------------------------------------------------
//
// Un fotograma se dibuja en dos capas:
//
//   ui_backdrop_begin();   ...el fondo vivo...   ui_backdrop_end();
//   ...el cristal y el texto encima...
//
// Entre las dos, ui_backdrop_end() reduce el fondo a una cadena de miniaturas.
// De ahi sale el desenfoque que ve cada panel: es el fondo de verdad, no una
// textura pintada, asi que el cristal reacciona a lo que tiene detras.

void ui_backdrop_begin(void);
void ui_backdrop_end(void);

// --------------------------------------------------------------------------
// capa de composicion
// --------------------------------------------------------------------------
//
// Todo lo que se dibuje entre begin y end va a una capa aparte, y al cerrarla
// se vuelca entera con una opacidad y una escala. Es lo que permite que un menu
// entre creciendo y fundiendose en vez de aparecer de golpe: sin esto habria que
// dar opacidad a cada texto, cada icono y cada panel por separado.
//
// `scale` va alrededor de (cx, cy), y (dx, dy) desplaza el resultado.

void ui_layer_begin(void);
void ui_layer_end(float alpha, float scale, int cx, int cy, int dx, int dy);

// --------------------------------------------------------------------------
// cristal
// --------------------------------------------------------------------------

typedef struct {
    color_t tint;     // color del vidrio
    u8 tint_a;        // cuanto tine (0 = solo desenfoque)
    u8 sheen;         // brillo degradado de arriba
    u8 grain;         // grano de esmerilado
    u8 lens;          // refraccion del borde: dobla lo que hay detras
    u8 rim;           // contorno luminoso
    u8 shadow;        // sombra proyectada
    u8 deep;          // 1 = desenfoque fuerte (paneles grandes), 0 = suave
} glass_t;

// Estilos ya montados, para no repetir numeros por todo el codigo.
extern const glass_t GLASS_PANEL;    // paneles grandes
extern const glass_t GLASS_CARD;     // tarjetas
extern const glass_t GLASS_CARD_HI;  // tarjeta elegida
extern const glass_t GLASS_BAR;      // barras flotantes
extern const glass_t GLASS_SHEET;    // dialogos

// Varios paneles en una sola pasada: se acumulan y se componen juntos.
// Cuesta lo mismo uno que doce, asi que la rejilla entera va en un lote.
void ui_glass_begin(void);
void ui_glass_add(int x, int y, int w, int h, int r, const glass_t *g);
void ui_glass_end(void);

// Atajo para un panel suelto.
void ui_glass(int x, int y, int w, int h, int r, const glass_t *g);

// --------------------------------------------------------------------------
// dibujo
// --------------------------------------------------------------------------

void ui_clear(color_t c);
void ui_rect(int x, int y, int w, int h, color_t c);
// Redondeado con bordes suavizados (esquinas cacheadas por radio).
void ui_rect_round(int x, int y, int w, int h, int r, color_t c);
void ui_rect_round_outline(int x, int y, int w, int h, int r, int thick, color_t c);
void ui_rect_outline(int x, int y, int w, int h, int thick, color_t c);

void ui_gradient_v(int x, int y, int w, int h, color_t top, color_t bottom);
void ui_gradient_round_top(int x, int y, int w, int h, int r,
                           color_t top, color_t bottom);

void ui_circle(int cx, int cy, int r, color_t c);
void ui_tri(int x1, int y1, int x2, int y2, int x3, int y3, color_t c);
void ui_arc(int cx, int cy, int radius, int thick, float from_deg, float to_deg, color_t c);

// Mancha de luz suave. Es la pieza con la que se pinta el fondo vivo, los
// halos y los reflejos: una textura radial que se estira y se tine.
void ui_glow(int cx, int cy, int rx, int ry, color_t c, u8 a);
// Vineta: cierra los bordes en negro para que la vista caiga al centro.
void ui_vignette(u8 a);

// --------------------------------------------------------------------------
// texto
// --------------------------------------------------------------------------

void ui_text(int x, int y, int size, color_t c, const char *fmt, ...);
void ui_text_center(int cx, int y, int size, color_t c, const char *fmt, ...);
void ui_text_right(int rx, int y, int size, color_t c, const char *fmt, ...);
int  ui_text_w(int size, const char *s);
int  ui_text_h(int size);
// Recorta con puntos suspensivos si no cabe en max_w. Devuelve el ancho usado.
int  ui_text_clip(int x, int y, int size, int max_w, color_t c, const char *s);
int  ui_text_clip_center(int cx, int y, int size, int max_w, color_t c, const char *s);

// Sobre cristal el texto pierde contraste, asi que lleva sombra propia.
void ui_text_sh(int x, int y, int size, color_t c, const char *fmt, ...);
int  ui_text_clip_sh(int x, int y, int size, int max_w, color_t c, const char *s);

// Parte un texto en lineas que quepan en `w`. Devuelve el alto total.
int  ui_text_wrap(int x, int y, int size, int w, int line_h, color_t c, const char *s);
int  ui_text_wrap_center(int cx, int y, int size, int w, int line_h, color_t c, const char *s);
// Alto que ocuparia, sin dibujar. Para reservar sitio antes de colocar nada.
int  ui_text_wrap_h(int size, int w, int line_h, const char *s);

// --------------------------------------------------------------------------
// imagenes
// --------------------------------------------------------------------------

// Cachea por puntero: pasa siempre el mismo buffer para el mismo icono.
SDL_Texture *ui_image(const void *jpeg, size_t len);
void ui_image_draw(SDL_Texture *t, int x, int y, int w, int h);
void ui_image_draw_a(SDL_Texture *t, int x, int y, int w, int h, u8 alpha);
void ui_image_draw_rot(SDL_Texture *t, int cx, int cy, int w, int h,
                       float deg, u8 alpha);
// Recortada en redondo de verdad, con alfa: sirve encima de cualquier cosa.
void ui_image_round(SDL_Texture *t, int x, int y, int w, int h, int r);
void ui_images_clear(void);

// Color medio del icono, subido de saturacion. Tine la interfaz entera.
color_t ui_image_color(const void *jpeg, size_t len);

// --------------------------------------------------------------------------
// utilidades
// --------------------------------------------------------------------------

bool ui_hit(const ui_input_t *in, int x, int y, int w, int h);
bool ui_over(const ui_input_t *in, int x, int y, int w, int h);   // dedo encima
u32  ui_ticks(void);
float ui_time(void);       // segundos desde que arranco, en coma flotante

// Segundos desde el fotograma anterior, para que las animaciones no dependan
// de si va a 60 o a 30.
float ui_dt(void);
// Acerca `cur` a `target` un poco cada fotograma. `rate` mas alto = mas rapido.
float ui_approach(float cur, float target, float rate);
// Muelle critico: llega sin rebotar pero con inercia. `vel` es el estado.
float ui_spring(float cur, float target, float *vel, float stiff);

color_t ui_mix(color_t a, color_t b, float t);
color_t ui_alpha(color_t c, u8 a);
// Sube o baja el brillo manteniendo el tono.
color_t ui_shade(color_t c, float k);

// Ondas al tocar: se lanzan y se dibujan solas al final del fotograma.
void ui_ripple(int x, int y, color_t c);
void ui_ripples_draw(void);

void ui_spinner(int cx, int cy, int r, color_t c);
// Anillo de progreso (0..1). Con progress < 0 gira solo, como espera.
void ui_ring(int cx, int cy, int radius, int thick, float progress,
             color_t fg, color_t bg);

// El logo: dos flechas en circulo, el mismo dibujo que el icono del homebrew.
void ui_logo(int cx, int cy, int size, color_t c1, color_t c2, color_t dot);

// --------------------------------------------------------------------------
// diagnostico
// --------------------------------------------------------------------------
//
// Dibuja el contorno de cada caja que se registre. Con esto los solapamientos
// se ven de un vistazo en vez de buscarlos a ojo.

// Vuelca el fotograma actual a un PNG. Es lo que usa el previsualizador del PC
// para poder mirar la interfaz; en la consola no se llama.
bool ui_screenshot(const char *path);

void ui_debug_box(int x, int y, int w, int h, const char *name);
// Olvida las cajas registradas. Lo llama una hoja modal al taparlo todo: lo que
// queda debajo del velo ya no compite por el sitio.
void ui_debug_reset(void);
void ui_debug_set(bool on);
bool ui_debug_on(void);
void ui_debug_draw(void);
