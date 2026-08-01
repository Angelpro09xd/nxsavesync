#!/usr/bin/env bash
# Descarga libtesla y stb_truetype, y aplica el parche del aviso.
#
#   ./overlay/preparar.sh
#
# Ninguna de las dos se guarda en este repositorio: son de terceros y tienen su
# propia licencia (libtesla es GPLv2 de WerWolv, stb_truetype es de dominio
# publico de Sean Barrett). Se bajan aqui y se parchean en local.
#
# El parche es de tres lineas y hace una sola cosa: que el bucle de espera del
# combo no se quede bloqueado para siempre, sino que se despierte cada 100 ms
# para dibujar el aviso del menu HOME. Sin eso, libtesla duerme en
# `eventWait(..., UINT64_MAX)` y no hay forma de pintar nada mientras el panel
# esta cerrado.
set -euo pipefail

AQUI="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INC="$AQUI/libs/libtesla/include"

say() { printf '\033[36m==>\033[0m %s\n' "$*"; }

mkdir -p "$INC"

if [ ! -f "$INC/tesla.hpp.original" ]; then
    say "Descargando libtesla..."
    curl -fsSL -o "$INC/tesla.hpp.original" \
        https://raw.githubusercontent.com/WerWolv/libtesla/master/include/tesla.hpp
fi

if [ ! -f "$INC/stb_truetype.h" ]; then
    say "Descargando stb_truetype..."
    curl -fsSL -o "$INC/stb_truetype.h" \
        https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h
fi

say "Aplicando el parche del aviso..."
python3 - "$INC" <<'PY'
import sys, pathlib

inc = pathlib.Path(sys.argv[1])
src = (inc / "tesla.hpp.original").read_text()

ANCLA = "            eventWait(&shData.comboEvent, UINT64_MAX);"
if ANCLA not in src:
    sys.exit("no encuentro la espera del combo en tesla.hpp; "
             "habra cambiado y hay que revisar el parche")

NUEVO = """            // --- parche de NX Save Sync ---------------------------------
            //
            // En vez de dormir hasta el combo, se despierta cada 100 ms para
            // dibujar el aviso del menu HOME. El aviso va en su propia capa y
            // NO pide el primer plano, asi que se ve encima del menu sin
            // quitarle el mando a nadie.
            while (shData.running) {
                if (R_SUCCEEDED(eventWait(&shData.comboEvent, 100000000ULL)))
                    break;
                nxss_aviso_tick();
            }
            if (!shData.running) break;
            // --- fin del parche -----------------------------------------"""

src = src.replace(ANCLA, NUEVO, 1)

# La declaracion va a nivel de espacio de nombres: `extern "C"` no se puede
# poner dentro del cuerpo de una funcion.
GUARDA = "#pragma once"
src = src.replace(GUARDA,
                  GUARDA + '\n\n// NX Save Sync: lo dibuja overlay/source/aviso.cpp\n'
                           'extern "C" void nxss_aviso_tick(void);\n', 1)

(inc / "tesla.hpp").write_text(src)
print("  tesla.hpp parcheado")
PY

say "Listo. Ahora: make -C overlay"
