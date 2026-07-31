#!/usr/bin/env python3
"""Genera `font8x8.h`: una fuente de mapa de bits para el sysmodule.

El sysmodule no tiene SDL ni freetype, y meterle un rasterizador de TrueType
solo para pintar un aviso no compensa. Con una fuente de 8x8 bits basta: son
760 bytes de datos, se escala por multiplos enteros y se lee perfectamente.

Se genera aqui con PIL en vez de copiar una tabla a mano, que es justo el tipo
de datos donde un error tipografico no se detecta hasta que algo se ve raro.

    python3 common/tools/make_font.py
"""

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

PRIMERO, ULTIMO = 32, 126          # ASCII imprimible
ANCHO = ALTO = 8

CANDIDATAS = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Bold.ttf",
]


def carga_fuente():
    for ruta in CANDIDATAS:
        if Path(ruta).is_file():
            # 7 px es el tamano que deja caber el glifo ENTERO en la celda de
            # 8x8, colas incluidas. Con 9 se veia mas grueso pero se comian las
            # colas de g, j, p, y: "juego" salia "iuego".
            return ImageFont.truetype(ruta, 7)
    raise SystemExit("no encuentro ninguna fuente monoespaciada")


def glifo(fuente, ch: str) -> list[int]:
    """Devuelve 8 bytes: un bit por pixel, el bit 0 es el pixel de la izquierda."""
    img = Image.new("L", (ANCHO, ALTO), 0)
    d = ImageDraw.Draw(img)
    d.text((0, 0), ch, fill=255, font=fuente)

    filas = []
    for y in range(ALTO):
        bits = 0
        for x in range(ANCHO):
            if img.getpixel((x, y)) > 90:
                bits |= 1 << x
        filas.append(bits)
    return filas


def main() -> None:
    fuente = carga_fuente()
    salida = Path(__file__).resolve().parent.parent / "source" / "font8x8.h"

    lineas = [
        "// Generado por common/tools/make_font.py -- no editar a mano.",
        "//",
        "// Fuente de 8x8 para los avisos que el sysmodule guarda en el Album.",
        "// Un byte por fila, un bit por pixel, el bit 0 es el pixel izquierdo.",
        "#pragma once",
        "",
        "#include <switch.h>",
        "",
        f"#define FONT_FIRST {PRIMERO}",
        f"#define FONT_LAST  {ULTIMO}",
        f"#define FONT_W {ANCHO}",
        f"#define FONT_H {ALTO}",
        "",
        f"static const u8 g_font8x8[{ULTIMO - PRIMERO + 1}][{ALTO}] = {{",
    ]

    for code in range(PRIMERO, ULTIMO + 1):
        filas = glifo(fuente, chr(code))
        hexs = ", ".join(f"0x{b:02X}" for b in filas)
        ch = chr(code)
        comentario = "espacio" if ch == " " else ch
        lineas.append(f"    {{ {hexs} }},   // {comentario}")

    lineas += ["};", ""]

    salida.write_text("\n".join(lineas))
    print(f"fuente escrita en {salida} ({ULTIMO - PRIMERO + 1} glifos)")


if __name__ == "__main__":
    main()
