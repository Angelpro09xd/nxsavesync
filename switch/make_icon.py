#!/usr/bin/env python3
"""Genera el icono del homebrew (256x256 JPEG, lo que pide el formato NRO).

El logo son dos flechas en circulo: la de arriba baja hacia la consola y la de
abajo sube hacia el PC. Se dibuja a 4x y se reduce al final, que es la forma
barata de tener bordes suaves sin antialiasing manual.
"""

from pathlib import Path
import math

from PIL import Image, ImageDraw

SIZE = 256
SS = 4                      # supermuestreo
W = SIZE * SS

BG_TOP = (26, 31, 40)
BG_BOT = (12, 15, 20)
CYAN = (46, 196, 211)
CYAN_DIM = (32, 140, 152)
WHITE = (236, 239, 242)


def rounded_gradient(w, radius):
    """Fondo con degradado vertical y esquinas redondeadas."""
    img = Image.new("RGB", (w, w), BG_BOT)
    d = ImageDraw.Draw(img)
    for y in range(w):
        t = y / (w - 1)
        c = tuple(int(BG_TOP[i] + (BG_BOT[i] - BG_TOP[i]) * t) for i in range(3))
        d.line([(0, y), (w, y)], fill=c)

    mask = Image.new("L", (w, w), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, w - 1, w - 1], radius, fill=255)

    out = Image.new("RGB", (w, w), (0, 0, 0))
    out.paste(img, (0, 0), mask)
    return out


def arc_arrow(d, cx, cy, r, thick, start_deg, end_deg, color):
    """Arco grueso que termina en una punta de flecha limpia.

    La punta se construye con un eje tangente (hacia donde apunta) y otro radial
    (el ancho). Hacerla con tres angulos sueltos daba triangulos torcidos que
    parecian alas en vez de flechas.
    """
    d.arc([cx - r, cy - r, cx + r, cy + r], start_deg, end_deg,
          fill=color, width=thick)

    ang = math.radians(end_deg)
    px, py = cx + r * math.cos(ang), cy + r * math.sin(ang)

    # Tangente en sentido de avance del arco, y radial perpendicular.
    tx, ty = -math.sin(ang), math.cos(ang)
    rx, ry = math.cos(ang), math.sin(ang)

    length = thick * 1.35
    half   = thick * 1.05

    tip   = (px + tx * length, py + ty * length)
    left  = (px + rx * half,   py + ry * half)
    right = (px - rx * half,   py - ry * half)
    d.polygon([tip, left, right], fill=color)


def build():
    img = rounded_gradient(W, int(W * 0.18))
    d = ImageDraw.Draw(img)

    cx = cy = W // 2
    r = int(W * 0.27)
    thick = int(W * 0.085)

    # Dos arcos opuestos: el ciclo de ida y vuelta.
    arc_arrow(d, cx, cy, r, thick, 195, 350, CYAN)
    arc_arrow(d, cx, cy, r, thick, 15, 170, CYAN_DIM)

    # Punto central: el save que viaja.
    rr = int(W * 0.05)
    d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=WHITE)

    return img.resize((SIZE, SIZE), Image.LANCZOS)


if __name__ == "__main__":
    out = Path(__file__).with_name("nxsavesync.jpg")
    build().save(out, "JPEG", quality=95)
    print(f"icono escrito en {out}")
