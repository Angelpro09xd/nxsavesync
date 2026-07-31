#!/usr/bin/env bash
# Construye NXSaveSync-Instalador.exe.
#
#   ./windows/build.sh
#
# Se puede hacer desde Linux: NSIS compila instaladores de Windows en cualquier
# sistema, y el Python que va dentro es la distribucion embebida oficial, que se
# descarga tal cual y no hay que compilar.
#
# Antes hay que tener construido el homebrew, el sysmodule y el overlay:
#   make -C switch && make -C sysmodule && make -C overlay
set -euo pipefail

AQUI="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$AQUI")"

PY_VER="3.11.9"
PY_ZIP="python-${PY_VER}-embed-amd64.zip"
PY_URL="https://www.python.org/ftp/python/${PY_VER}/${PY_ZIP}"

CACHE="$AQUI/cache"
BUILD="$AQUI/build"

say()  { printf '\033[36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33m!!\033[0m %s\n' "$*"; }
die()  { printf '\033[31m!!\033[0m %s\n' "$*"; exit 1; }

command -v makensis >/dev/null || die "falta makensis. Instalalo con: sudo apt install nsis"

# --- lo que tiene que estar construido --------------------------------------

NRO="$REPO/switch/nxsavesync.nro"
NSP="$REPO/sysmodule/nxsavesync-sys.nsp"
OVL="$REPO/overlay/nxsavesync.ovl"

[ -f "$NRO" ] || die "falta $NRO (make -C switch)"
[ -f "$NSP" ] || die "falta $NSP (make -C sysmodule)"
[ -f "$OVL" ] || warn "falta $OVL: el instalador ira sin overlay"

# --- Python embebido --------------------------------------------------------

mkdir -p "$CACHE"
if [ ! -f "$CACHE/$PY_ZIP" ]; then
    say "Descargando Python $PY_VER embebido..."
    curl -fL --retry 3 -o "$CACHE/$PY_ZIP.tmp" "$PY_URL"
    mv "$CACHE/$PY_ZIP.tmp" "$CACHE/$PY_ZIP"
fi

rm -rf "$BUILD"
mkdir -p "$BUILD/python" "$BUILD/app" "$BUILD/consola" "$AQUI/assets"

say "Preparando el Python que ira dentro..."
unzip -q "$CACHE/$PY_ZIP" -d "$BUILD/python"

# El Python embebido no mira las carpetas de al lado por defecto. El archivo
# ._pth es su lista de rutas, y hay que anadirle la del programa; si no, no
# encuentra ni nxsavesyncd.py.
PTH="$(ls "$BUILD"/python/python*._pth)"
{
    echo ""
    echo "# NX Save Sync: el programa vive en la carpeta de al lado."
    echo "../app"
    echo "import site"
} >> "$PTH"

# Sin idle, sin tests: pesan y no se usan.
rm -f "$BUILD"/python/*.chm

# --- el programa ------------------------------------------------------------

say "Copiando el programa..."
for f in nxsavesyncd.py emulators.py menu_web.py menu.html \
         nxsavesync_tray.pyw abrir_menu.pyw nxsavesync.ico; do
    [ -f "$REPO/pc/$f" ] || die "falta pc/$f"
    cp "$REPO/pc/$f" "$BUILD/app/"
done

cp "$REPO/pc/nxsavesync.ico" "$AQUI/assets/nxsavesync.ico"

# --- lo que va a la consola -------------------------------------------------

say "Copiando lo que va a la SD..."
cp "$NRO" "$BUILD/consola/nxsavesync.nro"
cp "$NSP" "$BUILD/consola/exefs.nsp"
cp "$REPO/sysmodule/toolbox.json" "$BUILD/consola/toolbox.json"
[ -f "$OVL" ] && cp "$OVL" "$BUILD/consola/nxsavesync.ovl"
cp "$AQUI/consola.ps1" "$BUILD/consola/consola.ps1"

# Atmosphere solo mira que el flag exista, no lo que hay dentro. Lleva una linea
# de texto porque algunos stacks MTP se atragantan con los archivos vacios.
printf 'NX Save Sync: este archivo hace que el sysmodule arranque con la consola.\r\n' \
    > "$BUILD/consola/boot2.flag"

# --- el instalador ----------------------------------------------------------

say "Compilando el instalador..."
# Sin -NOCD: makensis se situa en la carpeta del script y las rutas relativas
# de dentro (assets, build) resuelven donde deben.
makensis -V2 "$AQUI/instalador.nsi"

EXE="$REPO/NXSaveSync-Instalador.exe"
[ -f "$EXE" ] || die "makensis no dejo el .exe donde se esperaba"

say "Listo: $EXE  ($(du -h "$EXE" | cut -f1))"
echo
echo "  Copialo al PC con Windows y ejecutalo. No hace falta Python ni permisos"
echo "  de administrador."
