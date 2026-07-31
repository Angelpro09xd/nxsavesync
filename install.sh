#!/usr/bin/env bash
# Instala NXSaveSync: compila el homebrew y deja el daemon del PC listo.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

say() { printf '\033[36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33m!!\033[0m %s\n' "$*"; }

# --- homebrew ---------------------------------------------------------------

if [ -f /etc/profile.d/devkit-env.sh ]; then
    # shellcheck disable=SC1091
    source /etc/profile.d/devkit-env.sh
fi

export PATH="$PATH:${DEVKITPRO:-/opt/devkitpro}/portlibs/switch/bin"

if [ -z "${DEVKITPRO:-}" ]; then
    warn "devkitPro no esta instalado. Para compilar el .nro:"
    warn "  sudo apt install devkitpro-pacman"
    warn "  sudo dkp-pacman -S switch-dev switch-sdl2 switch-sdl2_ttf switch-sdl2_image switch-sdl2_gfx"
elif ! command -v sdl2-config >/dev/null; then
    warn "Falta SDL2 para Switch (la interfaz grafica lo necesita):"
    warn "  sudo dkp-pacman -S switch-sdl2 switch-sdl2_ttf switch-sdl2_image switch-sdl2_gfx"
else
    say "Compilando el homebrew..."
    make -C "$REPO/switch"
    say "Listo: $REPO/switch/nxsavesync.nro"

    say "Compilando el sysmodule..."
    make -C "$REPO/sysmodule"
    say "Listo: $REPO/sysmodule/nxsavesync-sys.nsp"

    say "Compilando el overlay..."
    make -C "$REPO/overlay"
    say "Listo: $REPO/overlay/nxsavesync.ovl  (necesita ovlloader)"
fi

# --- SD de la Switch --------------------------------------------------------

NRO="$REPO/switch/nxsavesync.nro"
NSP="$REPO/sysmodule/nxsavesync-sys.nsp"
OVL="$REPO/overlay/nxsavesync.ovl"
SYS_TID="420000000000534E"

if [ -f "$NRO" ]; then
    for sd in /media/"$USER"/*/switch /run/media/"$USER"/*/switch; do
        [ -d "$sd" ] || continue
        say "Tarjeta SD detectada en $sd, copiando el .nro"
        cp "$NRO" "$sd/"
        SD_FOUND=1

        # El sysmodule se copia pero NO se activa: hay que crear el flag a mano
        # o activarlo desde la app. Un proceso que corre siempre no se enciende
        # a espaldas de nadie.
        if [ -f "$NSP" ]; then
            root="$(dirname "$sd")"
            dest="$root/atmosphere/contents/$SYS_TID"
            mkdir -p "$dest/flags"
            cp "$NSP" "$dest/exefs.nsp"
            say "Sysmodule copiado en $dest (aun sin activar)"
            warn "Para que arranque solo con la consola:"
            warn "  touch \"$dest/flags/boot2.flag\""
            warn "Y activalo en la app: Ajustes -> Segundo plano"
        fi
    done
    if [ -z "${SD_FOUND:-}" ]; then
        say "Copia $NRO a /switch de la SD."
        say "Y $NSP a /atmosphere/contents/$SYS_TID/exefs.nsp si quieres el segundo plano."
    fi
fi

# --- daemon -----------------------------------------------------------------

chmod +x "$REPO/pc/nxsavesyncd.py"

say "Emuladores detectados:"
"$REPO/pc/nxsavesyncd.py" --list || warn "ninguno; el daemon usara ~/sync/saves"

UNIT_DIR="$HOME/.config/systemd/user"
mkdir -p "$UNIT_DIR"
sed "s|%h/sync/pc/nxsavesyncd.py|$REPO/pc/nxsavesyncd.py|" \
    "$REPO/pc/nxsavesync.service" > "$UNIT_DIR/nxsavesync.service"

if command -v systemctl >/dev/null; then
    systemctl --user daemon-reload
    say "Servicio instalado. Para arrancarlo en cada inicio de sesion:"
    say "  systemctl --user enable --now nxsavesync"
    say "Para ver el log:"
    say "  journalctl --user -u nxsavesync -f"
fi

# El descubrimiento necesita que el cortafuegos deje pasar el broadcast UDP.
if command -v ufw >/dev/null && ufw status 2>/dev/null | grep -q "Status: active"; then
    warn "Tienes ufw activo. Para que la consola encuentre el PC sola:"
    warn "  sudo ufw allow 7878/tcp && sudo ufw allow 7879/udp"
fi
