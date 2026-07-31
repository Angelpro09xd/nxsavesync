#!/usr/bin/env python3
"""Genera el logo de NX Save Sync.

    ./make_icon.py            -> switch/nxsavesync.jpg (256x256, lo que pide el NRO)
                                 y pc/nxsavesync.ico (multi-tamano, para Windows)
    ./make_icon.py --png X    -> ademas un PNG grande, para la web y el instalador

El logo es una lente de cristal con el ciclo de sincronizacion dentro: la flecha
de arriba baja hacia la consola, la de abajo sube hacia el PC. La lente no es
adorno: es la misma idea que la interfaz, algo transparente con luz detras, un
brillo especular arriba a la izquierda y un destello cruzado.

Se dibuja a 4x y se reduce al final. Es la forma barata de tener bordes suaves
sin escribir antialiasing a mano, y a 16 px la silueta sigue leyendose.
"""

from pathlib import Path
import argparse
import math

from PIL import Image, ImageDraw, ImageFilter

SIZE = 256
SS = 4                      # supermuestreo
W = SIZE * SS

# La misma paleta que la interfaz.
BG_TOP   = (16, 20, 28)
BG_BOT   = (4, 6, 10)
ACCENT   = (74, 216, 230)
ACCENT_D = (28, 122, 138)
WHITE    = (244, 247, 251)
GLASS    = (175, 190, 214)


def lienzo():
    return Image.new("RGBA", (W, W), (0, 0, 0, 0))


def fondo():
    """Cuadrado redondeado con degradado. La base sobre la que flota la lente."""
    img = Image.new("RGB", (W, W), BG_BOT)
    d = ImageDraw.Draw(img)
    for y in range(W):
        t = y / (W - 1)
        c = tuple(int(BG_TOP[i] + (BG_BOT[i] - BG_TOP[i]) * t) for i in range(3))
        d.line([(0, y), (W, y)], fill=c)

    mask = Image.new("L", (W, W), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, W - 1, W - 1], int(W * 0.20), fill=255)

    out = Image.new("RGB", (W, W), (0, 0, 0))
    out.paste(img, (0, 0), mask)
    return out.convert("RGBA")


def luz(cx, cy, r, color, alpha):
    """Mancha de luz difusa. Es lo que hace que la lente tenga algo detras."""
    capa = lienzo()
    ImageDraw.Draw(capa).ellipse([cx - r, cy - r, cx + r, cy + r],
                                 fill=color + (alpha,))
    return capa.filter(ImageFilter.GaussianBlur(r * 0.45))


def arco_flecha(d, cx, cy, r, thick, desde, hasta, color):
    """Arco grueso que termina en una punta de flecha limpia.

    La punta se construye con un eje tangente (hacia donde apunta) y otro radial
    (el ancho). Hacerla con tres angulos sueltos daba triangulos torcidos que
    parecian alas en vez de flechas.
    """
    d.arc([cx - r, cy - r, cx + r, cy + r], desde, hasta, fill=color, width=thick)

    ang = math.radians(hasta)
    px, py = cx + r * math.cos(ang), cy + r * math.sin(ang)

    tx, ty = -math.sin(ang), math.cos(ang)      # tangente: hacia donde avanza
    rx, ry = math.cos(ang), math.sin(ang)       # radial: el ancho

    largo = thick * 1.30
    medio = thick * 1.02

    d.polygon([(px + tx * largo, py + ty * largo),
               (px + rx * medio, py + ry * medio),
               (px - rx * medio, py - ry * medio)], fill=color)


def build():
    img = fondo()

    cx, cy = W * 0.5, W * 0.485
    R = W * 0.335                                # radio de la lente

    # --- luz detras de la lente -------------------------------------------
    img.alpha_composite(luz(cx, cy - R * 0.35, R * 1.25, ACCENT, 120))
    img.alpha_composite(luz(cx + R * 0.6, cy + R * 0.7, R * 0.9, ACCENT_D, 90))

    # --- sombra proyectada -------------------------------------------------
    sombra = lienzo()
    ImageDraw.Draw(sombra).ellipse(
        [cx - R, cy - R + R * 0.16, cx + R, cy + R + R * 0.16], fill=(0, 0, 0, 130))
    img.alpha_composite(sombra.filter(ImageFilter.GaussianBlur(R * 0.14)))

    # --- el cuerpo de la lente --------------------------------------------
    cuerpo = lienzo()
    d = ImageDraw.Draw(cuerpo)
    d.ellipse([cx - R, cy - R, cx + R, cy + R], fill=GLASS + (46,))

    # Degradado interno: el vidrio recoge mas luz por arriba que por abajo.
    filas = int(R * 1.1)
    for i in range(filas):
        t = i / filas
        a = int(52 * (1.0 - t) ** 2)
        if a <= 0:
            continue
        y = cy - R + i
        media = math.sqrt(max(R * R - (y - cy) ** 2, 0))
        d.line([(cx - media, y), (cx + media, y)], fill=(255, 255, 255, a))

    img.alpha_composite(cuerpo)

    # --- el ciclo, dentro de la lente -------------------------------------
    arte = lienzo()
    da = ImageDraw.Draw(arte)
    ra = R * 0.60
    grosor = int(R * 0.20)

    arco_flecha(da, cx, cy, ra, grosor, 195, 350, ACCENT + (255,))
    arco_flecha(da, cx, cy, ra, grosor, 15, 170, WHITE + (255,))

    punto = R * 0.11
    da.ellipse([cx - punto, cy - punto, cx + punto, cy + punto], fill=WHITE + (255,))
    img.alpha_composite(arte)

    # --- cantos y destello -------------------------------------------------
    #
    # El brillo no rodea la lente por igual: entra por arriba a la izquierda.
    # Esa asimetria es lo que hace que se lea como vidrio y no como un circulo.
    cantos = lienzo()
    dc = ImageDraw.Draw(cantos)
    grueso = max(int(R * 0.030), 2)

    dc.arc([cx - R, cy - R, cx + R, cy + R], 0, 360,
           fill=(255, 255, 255, 46), width=grueso)
    dc.arc([cx - R, cy - R, cx + R, cy + R], 178, 348,
           fill=(255, 255, 255, 190), width=grueso)
    img.alpha_composite(cantos)

    # Destello cruzado: una elipse girada y difusa, arriba a la izquierda.
    brillo = lienzo()
    ImageDraw.Draw(brillo).ellipse(
        [cx - R * 0.78, cy - R * 0.80, cx + R * 0.18, cy - R * 0.30],
        fill=(255, 255, 255, 70))
    brillo = brillo.rotate(-18, center=(cx, cy)).filter(
        ImageFilter.GaussianBlur(R * 0.10))
    img.alpha_composite(brillo)

    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--png", help="ademas, un PNG de 512 px (web e instalador)")
    args = ap.parse_args()

    grande = build()

    jpg = Path(__file__).with_name("nxsavesync.jpg")
    grande.resize((SIZE, SIZE), Image.LANCZOS).convert("RGB").save(
        jpg, "JPEG", quality=95)
    print(f"  icono del homebrew: {jpg}")

    # Windows elige un tamano u otro segun donde lo dibuje: bandeja, escritorio,
    # barra de tareas. Se reduce desde el original en cada uno, que sale mucho
    # mas limpio que dejar que lo escale el sistema desde uno solo.
    ico = Path(__file__).resolve().parent.parent / "pc" / "nxsavesync.ico"
    ico.parent.mkdir(parents=True, exist_ok=True)
    tam = [256, 128, 64, 48, 40, 32, 24, 20, 16]
    grande.resize((256, 256), Image.LANCZOS).save(
        ico, "ICO", sizes=[(s, s) for s in tam])
    print(f"  icono de Windows:   {ico}  ({', '.join(str(s) for s in tam)})")

    if args.png:
        p = Path(args.png)
        p.parent.mkdir(parents=True, exist_ok=True)
        grande.resize((512, 512), Image.LANCZOS).save(p)
        print(f"  PNG grande:         {p}")


if __name__ == "__main__":
    main()
