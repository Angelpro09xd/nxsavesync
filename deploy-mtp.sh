#!/usr/bin/env bash
# Copia la app y el sysmodule a la SD de la consola por MTP (DBI conectado por
# USB), o a la SD montada como disco si la tienes en el lector.
#
#   ./deploy-mtp.sh
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SYS_TID="420000000000534E"
NRO="$REPO/switch/nxsavesync.nro"
NSP="$REPO/sysmodule/nxsavesync-sys.nsp"
OVL="$REPO/overlay/nxsavesync.ovl"

say()  { printf '\033[36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33m!!\033[0m %s\n' "$*"; }
die()  { printf '\033[31m!!\033[0m %s\n' "$*"; exit 1; }

[ -f "$NRO" ] || die "falta $NRO (compila con: make -C switch)"
[ -f "$NSP" ] || die "falta $NSP (compila con: make -C sysmodule)"

# --- localizar la SD --------------------------------------------------------
#
# El orden importa y el criterio tambien. Antes se cogia cualquier disco montado
# que tuviera una carpeta /switch dentro, y eso escribio una vez en un NVMe de
# 954 GB que resulto ser la biblioteca de juegos del PC: tenia un /switch con
# dumps .nsp y colo. Ahora el MTP manda, y a un disco se le exige que parezca
# una SD de Switch de verdad.

ROOT=""
VIA=""

# 1) MTP por gvfs (DBI). Es el caso explicito: si la consola esta conectada,
#    es ahi donde se quiere escribir.
for mtp in /run/user/"$(id -u)"/gvfs/mtp:host=*; do
    [ -d "$mtp" ] || continue
    for store in "$mtp"/*; do
        [ -d "$store/switch" ] || continue
        [ -d "$store/Nintendo" ] || [ -d "$store/atmosphere" ] || continue
        ROOT="$store"; VIA="MTP"; break 2
    done
done

# 2) La SD en el lector. Una SD de Switch lleva Nintendo/ en la raiz y va en
#    FAT32 o exFAT; un disco del PC con una carpeta /switch no cumple ninguna
#    de las dos cosas.
if [ -z "$ROOT" ]; then
    for sd in /media/"$USER"/*/switch /run/media/"$USER"/*/switch; do
        [ -d "$sd" ] || continue
        cand="$(dirname "$sd")"

        [ -d "$cand/Nintendo" ] || {
            warn "$cand tiene /switch pero no /Nintendo: no es una SD de Switch, la salto"
            continue
        }

        fs="$(findmnt -no FSTYPE "$cand" 2>/dev/null || true)"
        case "$fs" in
            vfat|exfat|fuseblk|msdos|"") ;;
            *) warn "$cand esta en $fs; una SD de Switch va en FAT32 o exFAT, la salto"
               continue ;;
        esac

        ROOT="$cand"; VIA="lector"; break
    done
fi

[ -n "$ROOT" ] || die "no encuentro la SD. Conecta la consola con DBI en modo MTP, o mete la SD en el lector."

say "SD encontrada por $VIA: $ROOT"

# --- copiar -----------------------------------------------------------------

DEST="$ROOT/atmosphere/contents/$SYS_TID"
mkdir -p "$DEST/flags"

say "Copiando la app..."
cp "$NRO" "$ROOT/switch/nxsavesync.nro"

say "Copiando el sysmodule..."
cp "$NSP" "$DEST/exefs.nsp"

# El overlay solo sirve con ovlloader (Tesla o Ultrahand) instalado. Se copia
# igualmente: si no lo tienes, el archivo se queda ahi sin hacer nada.
if [ -f "$OVL" ]; then
    mkdir -p "$ROOT/switch/.overlays"
    cp "$OVL" "$ROOT/switch/.overlays/nxsavesync.ovl"
    if [ -d "$ROOT/switch/.overlays" ] && ls "$ROOT/switch/.overlays"/*.ovl >/dev/null 2>&1; then
        say "Overlay copiado en switch/.overlays"
    fi
fi
[ -f "$REPO/sysmodule/toolbox.json" ] && cp "$REPO/sysmodule/toolbox.json" "$DEST/toolbox.json"

if [ ! -f "$DEST/flags/boot2.flag" ]; then
    : > "$DEST/flags/boot2.flag"
    say "boot2.flag creado: arrancara con la consola"
fi

# --- verificar --------------------------------------------------------------
#
# Por MTP una copia puede quedarse a medias sin dar error, asi que se comprueba
# el contenido y no solo que el archivo exista.

ok=1
for pair in "$NRO|$ROOT/switch/nxsavesync.nro" "$NSP|$DEST/exefs.nsp"; do
    src="${pair%%|*}"; dst="${pair##*|}"
    if [ "$(md5sum "$src" | cut -d' ' -f1)" = "$(md5sum "$dst" | cut -d' ' -f1)" ]; then
        say "OK  $(basename "$src")  $(stat -c%s "$dst") bytes"
    else
        warn "DIFIERE $(basename "$src") -- vuelve a lanzarlo"
        ok=0
    fi
done

[ "$ok" = 1 ] || exit 1

say "Listo. Reinicia la consola para cargar el sysmodule nuevo."
