#!/usr/bin/env python3
"""Comprueba que el daemon se puede embeber en otra aplicacion.

Es lo que hace la app de bandeja de Windows: lo arranca en un hilo, se engancha
a sus eventos y lo para al salir. Aqui se verifica esa parte, que es la misma en
todos los sistemas; lo unico que no se puede probar fuera de Windows es el
dibujo del icono.
"""

import shutil
import socket
import struct
import sys
import tempfile
import threading
import time
from pathlib import Path

PC_DIR = Path(__file__).resolve().parent.parent / "pc"
sys.path.insert(0, str(PC_DIR))

TMP = Path(tempfile.gettempdir()) / "nxsavesync-test-embed"
PORT = 7893


def check(label, cond):
    print(f"  {'OK  ' if cond else 'FALLO'} {label}")
    if not cond:
        raise SystemExit(1)


def main():
    shutil.rmtree(TMP, ignore_errors=True)
    (TMP / "saves").mkdir(parents=True)

    # El daemon lee estas rutas al importarse, asi que se fijan antes.
    import os
    os.environ["XDG_DATA_HOME"] = str(TMP / "data")
    os.environ["XDG_CONFIG_HOME"] = str(TMP / "config")

    import nxsavesyncd as daemon

    print("1) Sumidero del registro")
    lineas = []
    daemon.set_log_sink(lineas.append)

    print("\n2) Arranque en un hilo, como hace la bandeja")
    stop = threading.Event()
    capturado = {}
    eventos = []

    def engancha(rt):
        capturado["rt"] = rt
        rt.on_event = lambda tipo, dato: eventos.append((tipo, dato))

    hilo = threading.Thread(
        target=daemon.main,
        kwargs=dict(argv=["--port", str(PORT), "--no-discovery", "--no-watch",
                          "--dir", str(TMP / "saves")],
                    stop_event=stop, rt_hook=engancha),
        daemon=True)
    hilo.start()
    time.sleep(2)

    check("el hilo sigue vivo", hilo.is_alive())
    check("el gancho recibio el Runtime", "rt" in capturado)
    check("el registro llega al sumidero", len(lineas) > 0)
    check("dice en que puerto escucha",
          any(str(PORT) in l for l in lineas))

    print("\n3) Acepta conexiones y avisa de ellas")
    c = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    c.sendall(bytes([0x01]) + struct.pack("<I", 4 + 2 + 6 + 2 + 3)
              + struct.pack("<I", 3) + struct.pack("<H", 6) + b"switch"
              + struct.pack("<H", 3) + b"dev")
    hdr = c.recv(5)
    check("responde al saludo", hdr and hdr[0] == 0x81)
    c.recv(1024)
    c.close()
    time.sleep(0.5)

    check("aviso de conexion", any(t == "conectado" for t, _ in eventos))
    check("aviso de desconexion", any(t == "desconectado" for t, _ in eventos))
    check("contador de conexiones", capturado["rt"].stats["conexiones"] >= 1)

    print("\n4) Parada limpia")
    # Sin el tiempo de espera en accept(), esto se quedaria colgado para siempre
    # y la aplicacion no se podria cerrar.
    stop.set()
    hilo.join(timeout=6)
    check("el hilo termino", not hilo.is_alive())

    # Y el puerto queda libre para el siguiente arranque.
    time.sleep(0.3)
    s = socket.socket()
    try:
        s.bind(("127.0.0.1", PORT))
        libre = True
    except OSError:
        libre = False
    finally:
        s.close()
    check("el puerto queda libre", libre)

    print("\n5) La app de bandeja se puede importar fuera de Windows")
    import subprocess
    r = subprocess.run([sys.executable, str(PC_DIR / "nxsavesync_tray.pyw")],
                       capture_output=True, text=True, timeout=30)
    if sys.platform == "win32":
        check("(saltado: estamos en Windows)", True)
    else:
        check("avisa en vez de reventar", "solo para Windows" in r.stdout)
        check("sin traza de error", "Traceback" not in r.stderr)

    print("\nTODO OK")


main()
