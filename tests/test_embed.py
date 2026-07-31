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
              + struct.pack("<I", daemon.PROTO_VERSION) + struct.pack("<H", 6) + b"switch"
              + struct.pack("<H", 3) + b"dev")
    hdr = c.recv(5)
    check("responde al saludo", hdr and hdr[0] == 0x81)
    c.recv(1024)
    c.close()
    time.sleep(0.5)

    check("aviso de conexion", any(t == "conectado" for t, _ in eventos))
    check("aviso de desconexion", any(t == "desconectado" for t, _ in eventos))
    check("contador de conexiones", capturado["rt"].stats["conexiones"] >= 1)

    print("\n3b) El evento de sync trae lo que hace falta para el aviso")
    TITLE = 0x0100000000010000
    UID = struct.pack("<QQ", 0x1122, 0x3344)

    def w_str(t):
        b = t.encode(); return struct.pack("<H", len(b)) + b

    class Cli:
        def __init__(self):
            self.s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
            self.send(0x01, struct.pack("<I", daemon.PROTO_VERSION)
                      + w_str("test") + w_str("dev"))
            self.recv()

        def send(self, op, p=b""):
            self.s.sendall(bytes([op]) + struct.pack("<I", len(p)) + p)

        def recv(self):
            h = b""
            while len(h) < 5:
                h += self.s.recv(5 - len(h))
            n = struct.unpack_from("<I", h, 1)[0]
            b = b""
            while len(b) < n:
                b += self.s.recv(n - len(b))
            return h[0], b

        def sincroniza(self, archivos):
            import zlib
            man = struct.pack("<I", len(archivos))
            for nombre, datos in archivos.items():
                man += (w_str(nombre) + struct.pack("<Q", len(datos))
                        + struct.pack("<I", zlib.crc32(datos)))
            self.send(0x02, UID + w_str("Angel") + struct.pack("<Q", TITLE)
                      + w_str("Juego De Prueba") + bytes([0, 0])
                      + struct.pack("<QQ", 0, 0) + man)
            op, r = self.recv()
            (ln,) = struct.unpack_from("<H", r, 1)
            pos = 3 + ln
            (n,) = struct.unpack_from("<I", r, pos)
            pos += 4
            plan = []
            for _ in range(n):
                a = r[pos]
                (ln,) = struct.unpack_from("<H", r, pos + 1)
                plan.append((a, r[pos + 3:pos + 3 + ln].decode()))
                pos += 3 + ln

            for a, ruta in plan:
                if a == 1:      # PUSH
                    d = archivos[ruta]
                    self.s.sendall(bytes([0x04])
                                   + struct.pack("<I", 16 + 8 + 2 + len(ruta) + 8 + len(d))
                                   + UID + struct.pack("<Q", TITLE) + w_str(ruta)
                                   + struct.pack("<Q", len(d)) + d)
                    self.recv()

            self.send(0x07, UID + struct.pack("<Q", TITLE) + man)
            self.recv()
            self.s.close()
            return plan

    eventos.clear()
    cli = Cli()
    cli.sincroniza({"a.sav": b"hola", "b.sav": b"mundo"})
    time.sleep(0.4)

    syncs = [d for t, d in eventos if t == "sync"]
    check("hubo evento de sync", len(syncs) == 1)
    check("es un diccionario con detalle", isinstance(syncs[0], dict))
    check(f"cuenta 2 cambios ({syncs[0].get('cambios')})", syncs[0].get("cambios") == 2)
    check("trae el nombre del juego", syncs[0].get("nombre") == "Juego De Prueba")
    check("distingue subidos de bajados",
          syncs[0].get("subidos") == 2 and syncs[0].get("bajados") == 0)

    # Segunda pasada sin cambios: no debe generar aviso.
    eventos.clear()
    cli = Cli()
    cli.sincroniza({"a.sav": b"hola", "b.sav": b"mundo"})
    time.sleep(0.4)
    syncs = [d for t, d in eventos if t == "sync"]
    check("un juego ya al dia reporta 0 cambios",
          len(syncs) == 1 and syncs[0].get("cambios") == 0)

    print("\n3c) El texto del aviso")
    from importlib.machinery import SourceFileLoader
    import importlib.util
    loader = SourceFileLoader("tray", str(PC_DIR / "nxsavesync_tray.pyw"))
    spec = importlib.util.spec_from_loader("tray", loader)
    tray = importlib.util.module_from_spec(spec)
    loader.exec_module(tray)

    t, x = tray.resume_tanda(["Zelda BotW"], a_consola=3)
    check(f"singular con un juego ({t})", t.startswith("1 partida sincronizada"))

    # Lo importante: se distingue que el PC ha mandado archivos a la consola,
    # que es cuando de verdad te ha cambiado lo que tienes en la Switch.
    t, x = tray.resume_tanda(["Zelda", "Minecraft"], a_consola=8, al_pc=0)
    check(f"avisa de lo enviado a la consola ({t})", "8 a la consola" in t)
    check("y no menciona el PC si no fue nada", "al PC" not in t)

    t, x = tray.resume_tanda(["Tomodachi"], a_consola=0, al_pc=3)
    check(f"avisa de lo recibido de la consola ({t})",
          "3 al PC" in t and "a la consola" not in t)

    t, x = tray.resume_tanda(["Zelda", "Mario", "Kirby"], a_consola=5, al_pc=12)
    check(f"y de las dos direcciones a la vez ({t})",
          "5 a la consola" in t and "12 al PC" in t)
    check("lista los nombres", x == "Zelda, Mario, Kirby")

    largos = [f"Juego con un nombre muy largo numero {i}" for i in range(20)]
    t, x = tray.resume_tanda(largos, a_consola=99)
    check(f"recorta y resume el resto ({len(x)} car.)", len(x) < 255 and "mas" in x)

    print("\n3d) El PC avisa a la red cuando cambia el emulador")
    # Es la pieza que permite que, al terminar de jugar en el emulador, la
    # consola lo recoja al momento en vez de esperar a su repaso periodico.
    escucha = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    escucha.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    escucha.settimeout(6)
    escucha.bind(("", daemon.NUDGE_PORT))

    daemon.send_nudge("prueba")
    try:
        datos, _ = escucha.recvfrom(64)
        llego = True
    except socket.timeout:
        datos, llego = b"", False

    check("el aviso llega por la red", llego)
    check("lleva la marca correcta", datos.startswith(daemon.NUDGE_MSG))
    if llego:
        (ver,) = struct.unpack_from("<I", datos, len(daemon.NUDGE_MSG))
        check(f"y la version del protocolo ({ver})", ver == daemon.PROTO_VERSION)

    # El vigilante lo dispara solo al ver un cambio de verdad.
    vigilante = daemon.Watcher(lambda uid, tid: TMP / "saves" / "vigilado", interval=1)
    (TMP / "saves/vigilado").mkdir(parents=True, exist_ok=True)
    vigilante.watch("UID", 0x1234, "Juego vigilado")
    vigilante.start()
    time.sleep(2.5)                                    # primera foto
    (TMP / "saves/vigilado/partida.sav").write_bytes(b"acabo de jugar en el emulador")

    try:
        datos, _ = escucha.recvfrom(64)
        aviso_automatico = datos.startswith(daemon.NUDGE_MSG)
    except socket.timeout:
        aviso_automatico = False
    vigilante.stop_flag.set()
    escucha.close()

    check("y se dispara solo al detectar el cambio", aviso_automatico)

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
