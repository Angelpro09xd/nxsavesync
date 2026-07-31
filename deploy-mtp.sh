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

say()  { printf '\033[36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33m!!\033[0m %s\n' "$*"; }
die()  { printf '\033[31m!!\033[0m %s\n' "$*"; exit 1; }

[ -f "$NRO" ] || die "falta $NRO (compila con: make -C switch)"
[ -f "$NSP" ] || die "falta $NSP (compila con: make -C sysmodule)"

# --- localizar la SD --------------------------------------------------------

ROOT=""

# 1) SD montada como disco normal
for sd in /media/"$USER"/*/switch /run/media/"$USER"/*/switch; do
    [ -d "$sd" ] && { ROOT="$(dirname "$sd")"; break; }
done

# 2) MTP por gvfs (DBI). El nombre del almacen varia entre versiones, asi que
#    se busca cualquier carpeta que tenga dentro un /switch.
if [ -z "$ROOT" ]; then
    for mtp in /run/user/"$(id -u)"/gvfs/mtp:host=*; do
        [ -d "$mtp" ] || continue
        for store in "$mtp"/*; do
            [ -d "$store/switch" ] && { ROOT="$store"; break 2; }
        done
    done
fi

[ -n "$ROOT" ] || die "no encuentro la SD. Conecta la consola con DBI en modo MTP, o mete la SD en el lector."

say "SD encontrada en: $ROOT"

# --- copiar -----------------------------------------------------------------

DEST="$ROOT/atmosphere/contents/$SYS_TID"
mkdir -p "$DEST/flags"

say "Copiando la app..."
cp "$NRO" "$ROOT/switch/nxsavesync.nro"

say "Copiando el sysmodule..."
cp "$NSP" "$DEST/exefs.nsp"
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
