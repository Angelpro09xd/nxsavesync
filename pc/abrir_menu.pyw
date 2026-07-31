"""Abre el menu de NX Save Sync.

Es lo que hay detras del icono del escritorio. Si el programa ya esta en marcha
—normalmente lo esta, vive junto al reloj— solo abre su menu en el navegador. Si
no lo esta, lo arranca, y el menu se abre solo en cuanto este listo.

Hacerlo asi evita el fallo tipico: pulsar el icono dos veces y acabar con dos
copias del daemon peleandose por el mismo puerto.
"""

from __future__ import annotations

import subprocess
import sys
import urllib.error
import urllib.request
import webbrowser
from pathlib import Path

AQUI = Path(__file__).resolve().parent
sys.path.insert(0, str(AQUI))


def direccion_guardada() -> str | None:
    try:
        import nxsavesyncd as daemon
        f = daemon.STATE_DIR / "menu.url"
    except Exception:
        f = AQUI / "menu.url"

    try:
        url = f.read_text(encoding="utf-8").strip()
    except OSError:
        return None
    return url or None


def responde(url: str) -> bool:
    """Que el archivo exista no basta: el programa pudo cerrarse sin borrarlo."""
    try:
        with urllib.request.urlopen(url, timeout=1.5) as r:
            return r.status == 200
    except (urllib.error.URLError, OSError):
        return False


def main() -> int:
    url = direccion_guardada()
    if url and responde(url):
        webbrowser.open(url)
        return 0

    # No esta en marcha: se arranca. El programa abre el menu al terminar de
    # levantarse, asi que desde aqui no hay nada mas que hacer.
    pythonw = Path(sys.executable).with_name("pythonw.exe")
    exe = str(pythonw if pythonw.exists() else sys.executable)

    try:
        subprocess.Popen([exe, str(AQUI / "nxsavesync_tray.pyw"), "--abrir-menu"],
                         cwd=str(AQUI))
    except Exception:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
