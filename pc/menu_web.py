"""El menú de NX Save Sync para el PC.

Se abre desde el icono de la bandeja. Es una página servida en local, solo para
127.0.0.1, que se abre en el navegador que ya tengas.

Es una página y no una ventana de escritorio por dos motivos, y el primero pesa
más de lo que parece:

1. **Tiene el mismo aspecto que el homebrew.** El cristal de la consola necesita
   desenfocar el fondo, y ningún kit de ventanas de escritorio hace eso. Un
   navegador sí, con `backdrop-filter`, así que el menú del PC y la interfaz de
   la Switch se ven como la misma cosa.
2. **Cabe en el Python reducido que lleva el instalador.** La distribución
   embebida de Python no trae tkinter, y arrastrar una copia completa de Python
   solo por una ventana de ajustes no compensa.

Las opciones no se escriben aquí: salen de `Runtime.schema()`, que es la misma
lista que se le manda a la consola. Así el menú del PC y el de la Switch no
pueden acabar diciendo cosas distintas.
"""

from __future__ import annotations

import json
import os
import secrets
import socket
import subprocess
import sys
import threading
import webbrowser
from collections import deque
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

AQUI = Path(__file__).resolve().parent

PUERTO_PREFERIDO = 7877
MAX_REGISTRO = 200


def abrir_en_explorador(ruta: Path) -> None:
    try:
        ruta.mkdir(parents=True, exist_ok=True)
        if sys.platform == "win32":
            os.startfile(str(ruta))          # noqa: S606
        elif sys.platform == "darwin":
            subprocess.Popen(["open", str(ruta)])
        else:
            subprocess.Popen(["xdg-open", str(ruta)])
    except Exception:
        pass


class MenuWeb:
    """Servidor del menú. Se arranca una vez y se queda escuchando."""

    def __init__(self, tray, daemon):
        self.tray = tray
        self.daemon = daemon
        self.rt = getattr(tray, "rt", None)

        # Un testigo en la dirección. Sin él, cualquier página abierta en el
        # navegador podría llamar a este servidor y cambiar ajustes o rutas.
        self.token = secrets.token_urlsafe(18)

        self.httpd: ThreadingHTTPServer | None = None
        self.puerto = 0
        self.registro: deque[str] = deque(maxlen=MAX_REGISTRO)
        self.ultimo_error: str = ""
        self.ultima_traza: str = ""

        self._engancha_registro()

    # -- fallos ---------------------------------------------------------------

    def anota_fallo(self, contexto: str) -> None:
        """Deja el fallo en el registro y guardado para poder ensenarlo."""
        import traceback
        self.ultima_traza = traceback.format_exc()
        primera = self.ultima_traza.strip().splitlines()[-1] if self.ultima_traza else "?"
        self.ultimo_error = f"{contexto}: {primera}"
        self.registro.append(f"{datetime.now():%H:%M:%S}  FALLO {self.ultimo_error}")

        try:
            import nxsavesyncd as d
            f = d.STATE_DIR / "menu-fallos.log"
            f.parent.mkdir(parents=True, exist_ok=True)
            with open(f, "a", encoding="utf-8") as fh:
                fh.write(f"\n=== {datetime.now():%Y-%m-%d %H:%M:%S}  {contexto} ===\n")
                fh.write(self.ultima_traza)
        except Exception:
            pass

    # -- registro ------------------------------------------------------------

    def _engancha_registro(self) -> None:
        """Se cuelga del log del daemon sin desplazar a quien ya estuviera."""
        anterior = getattr(self.daemon, "_log_sink", None)

        def sink(msg: str) -> None:
            self.registro.append(f"{datetime.now():%H:%M:%S}  {msg}")
            if callable(anterior):
                try:
                    anterior(msg)
                except Exception:
                    pass

        try:
            self.daemon.set_log_sink(sink)
        except Exception:
            pass

    # -- arranque ------------------------------------------------------------

    def abrir(self) -> None:
        """Arranca el servidor si hace falta y abre el navegador."""
        if self.httpd is None:
            self._arranca()
        if self.httpd is None:
            return
        webbrowser.open(self.url())

    def _arranca(self) -> None:
        menu = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):       # sin ruido en la consola
                pass

            # Nada de esto sale de la máquina, pero el testigo evita que otra
            # página del navegador toque los ajustes a espaldas del usuario.
            def _autorizado(self) -> bool:
                from urllib.parse import urlparse, parse_qs
                q = parse_qs(urlparse(self.path).query)
                return q.get("t", [""])[0] == menu.token

            def _responde(self, code, tipo, cuerpo: bytes):
                self.send_response(code)
                self.send_header("Content-Type", tipo)
                self.send_header("Content-Length", str(len(cuerpo)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(cuerpo)

            def _json(self, obj, code=200):
                # `default=str` es una red, no una excusa: un objeto que se
                # cuele sale como texto en vez de tumbar la pagina entera. Un
                # dato mal formateado es un problema; un menu en blanco sin
                # explicacion es otro mucho peor.
                self._responde(code, "application/json; charset=utf-8",
                               json.dumps(obj, ensure_ascii=False,
                                          default=str).encode())

            # Cualquier fallo del servidor acaba en el registro Y en la
            # respuesta. Un menu que se queda en blanco sin decir por que es
            # imposible de arreglar a distancia.
            def handle_one_request(self):
                try:
                    super().handle_one_request()
                except Exception:
                    menu.anota_fallo("atendiendo " + str(self.path))

            def do_GET(self):
                ruta = self.path.split("?")[0]

                if not self._autorizado():
                    self._responde(403, "text/plain; charset=utf-8",
                                   "Falta el testigo. Abre el menú desde el "
                                   "icono de la bandeja.".encode())
                    return

                if ruta == "/":
                    try:
                        html = (AQUI / "menu.html").read_text(encoding="utf-8")
                    except OSError:
                        self._responde(500, "text/plain", b"falta menu.html")
                        return
                    html = html.replace("__TOKEN__", menu.token)
                    self._responde(200, "text/html; charset=utf-8", html.encode())

                elif ruta.startswith("/caratula/"):
                    # /caratula/<TITLEID>.jpg
                    tid = ruta.rsplit("/", 1)[-1].replace(".jpg", "")
                    try:
                        p = menu.daemon.fichas_dir() / f"{tid}.jpg"
                        self._responde(200, "image/jpeg", p.read_bytes())
                    except Exception:
                        self._responde(404, "text/plain", b"")

                elif ruta == "/api/juegos":
                    try:
                        self._json(menu.juegos())
                    except Exception as e:
                        menu.anota_fallo("montando los juegos")
                        self._json({"error": f"{type(e).__name__}: {e}"}, 500)

                elif ruta == "/logo.png":
                    ico = AQUI / "nxsavesync.ico"
                    if ico.exists():
                        self._responde(200, "image/x-icon", ico.read_bytes())
                    else:
                        self._responde(404, "text/plain", b"")

                elif ruta == "/api/estado":
                    try:
                        self._json(menu.estado())
                    except Exception as e:
                        menu.anota_fallo("montando el estado")
                        self._json({"error": f"{type(e).__name__}: {e}",
                                    "traza": menu.ultima_traza}, 500)

                else:
                    self._responde(404, "text/plain", b"")

            def do_POST(self):
                ruta = self.path.split("?")[0]

                if not self._autorizado():
                    self._json({"ok": False, "msg": "no autorizado"}, 403)
                    return

                n = int(self.headers.get("Content-Length", 0) or 0)
                try:
                    datos = json.loads(self.rfile.read(n) or b"{}")
                except Exception:
                    datos = {}

                if ruta == "/api/set":
                    self._json(menu.aplica(datos.get("clave", ""),
                                           str(datos.get("valor", ""))))
                elif ruta == "/api/accion":
                    self._json(menu.accion(datos.get("accion", "")))
                else:
                    self._json({"ok": False, "msg": "no existe"}, 404)

        # Si el puerto de siempre está ocupado, vale cualquiera: la dirección se
        # la damos nosotros al navegador, nadie tiene que teclearla.
        for puerto in (PUERTO_PREFERIDO, 0):
            try:
                self.httpd = ThreadingHTTPServer(("127.0.0.1", puerto), Handler)
                break
            except OSError:
                continue

        if self.httpd is None:
            return

        self.puerto = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True,
                         name="nxss-menu").start()

        # La direccion queda escrita para que el acceso directo del escritorio
        # pueda abrir el menu sin arrancar una segunda copia del programa.
        try:
            f = self.direccion_path()
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_text(self.url(), encoding="utf-8")
        except Exception:
            pass

    def url(self) -> str:
        return f"http://127.0.0.1:{self.puerto}/?t={self.token}"

    @staticmethod
    def direccion_path() -> Path:
        '''Donde queda escrita la direccion del menu en marcha.'''
        try:
            import nxsavesyncd as d
            return d.STATE_DIR / "menu.url"
        except Exception:
            return AQUI / "menu.url"

    def cerrar(self) -> None:
        if self.httpd:
            self.httpd.shutdown()
            self.httpd = None

    def juegos(self) -> list[dict]:
        """Una ficha por juego: caratula, estado y lo que se movio.

        Sale de lo que va apuntando el daemon en cada sincronizacion, no de
        preguntarle a la consola: el menu tiene que servir aunque la Switch
        este apagada.
        """
        out = []
        try:
            base = self.daemon.fichas_dir()
        except Exception:
            return out

        for f in base.glob("*.json"):
            try:
                d = json.loads(f.read_text(encoding="utf-8"))
            except Exception:
                continue

            tid = d.get("title_id") or f.stem
            d["caratula"] = (base / f"{tid}.jpg").is_file()

            if isinstance(d.get("cuando"), (int, float)):
                d["cuando_txt"] = datetime.fromtimestamp(
                    d["cuando"]).strftime("%d/%m/%Y %H:%M")
            out.append(d)

        # Lo ultimo tocado primero, que es lo que uno busca.
        out.sort(key=lambda x: x.get("cuando") or 0, reverse=True)
        return out

    @staticmethod
    def _cola_del_log(lineas: int = 25) -> str:
        try:
            import nxsavesyncd as d
            texto = (d.STATE_DIR / "nxsavesync.log").read_text(
                encoding="utf-8", errors="replace")
        except Exception:
            return ""
        return "\n".join(texto.splitlines()[-lineas:])

    # -- datos ---------------------------------------------------------------

    def estado(self) -> dict:
        d, rt = self.daemon, self.rt

        ajustes = []
        if rt is not None:
            try:
                ajustes = rt.schema()
            except Exception:
                ajustes = []

        emus = []
        if rt is not None:
            for e in getattr(rt, "emus", []) or []:
                emus.append({
                    "nombre": e.name,
                    "ruta": str(getattr(e, "base", "") or getattr(e, "root", "")),
                    "activo": bool(self._emu_activo(e)),
                })

        copias = []
        try:
            base = d.backups_dir()
            if base.exists():
                for p in sorted(base.iterdir(),
                                key=lambda q: q.stat().st_mtime, reverse=True)[:24]:
                    if not p.is_dir():
                        continue
                    n = sum(1 for _ in p.rglob("*") if _.is_file())
                    copias.append({
                        "nombre": p.name,
                        "cuando": datetime.fromtimestamp(
                            p.stat().st_mtime).strftime("%d/%m/%Y %H:%M"),
                        "archivos": n,
                    })
        except Exception:
            pass

        stats = dict(getattr(rt, "stats", {}) or {})

        # El daemon guarda ahi un datetime, no una marca de tiempo. Comprobar
        # solo int/float dejaba pasar el objeto entero al JSON y tumbaba la
        # pagina con un TypeError, pero solo si ya habias sincronizado alguna
        # vez: con "ultima" a None no se notaba.
        ultima = stats.get("ultima")
        if isinstance(ultima, datetime):
            stats["ultima"] = ultima.strftime("%d/%m/%Y %H:%M")
        elif isinstance(ultima, (int, float)):
            stats["ultima"] = datetime.fromtimestamp(ultima).strftime("%d/%m/%Y %H:%M")

        # Un resumen de salud que la pagina puede ensenar tal cual. Lo mas
        # habitual es que el daemon no haya llegado a arrancar, y entonces todo
        # lo demas sale vacio sin que se sepa por que.
        diag = {
            "rt": rt is not None,
            "emus": len(getattr(rt, "emus", []) or []) if rt else 0,
            "ajustes": len(ajustes),
            "error": self.ultimo_error,
        }

        # Si el daemon no arranco, lo que hace falta es *su* error, no el
        # nuestro. La bandeja lo deja en su registro; se pesca el final.
        if rt is None and not diag["error"]:
            diag["error"] = self._cola_del_log()

        return {
            "diag": diag,
            "nombre": socket.gethostname(),
            "emulador": getattr(rt, "emu_name", "") if rt else "",
            "ips": self._ips(),
            "puerto": getattr(getattr(rt, "args", None), "port", 7878),
            "stats": stats,
            "ajustes": ajustes,
            "emuladores": emus,
            "rutas": {
                "copias": str(self._safe(d.backups_dir)),
                "estado": str(self._safe(d.estado_dir)),
            },
            "copias": copias,
            "registro": list(self.registro)[-60:],
            "inicio": self._autoarranque(),
            "windows": sys.platform == "win32",
        }

    @staticmethod
    def _safe(fn):
        try:
            return fn()
        except Exception:
            return ""

    def _emu_activo(self, e) -> bool:
        try:
            return e in self.rt.active_emus()
        except Exception:
            return True

    def _ips(self) -> list[str]:
        try:
            return self.daemon.local_ips()
        except Exception:
            return []

    def _autoarranque(self) -> bool:
        try:
            return bool(self.tray.autoarranque_activo())
        except Exception:
            return False

    # -- acciones ------------------------------------------------------------

    def aplica(self, clave: str, valor: str) -> dict:
        if self.rt is None:
            return {"ok": False, "msg": "el daemon no está en marcha"}
        try:
            msg = self.rt.apply(clave, valor)
            self.daemon.save_config(self.rt.cfg)
            return {"ok": True, "msg": msg or "Guardado"}
        except Exception as e:
            return {"ok": False, "msg": f"No se pudo aplicar: {e}"}

    def accion(self, que: str) -> dict:
        try:
            if que == "rescan":
                return self.aplica("rescan", "1")

            if que == "copias":
                abrir_en_explorador(self.daemon.backups_dir())
                return {"ok": True, "msg": "Carpeta de copias abierta"}

            if que == "inicio":
                self.tray.alterna_autoarranque()
                return {"ok": True,
                        "msg": ("Se abrirá al iniciar sesión"
                                if self._autoarranque() else
                                "Ya no se abre al iniciar sesión")}

            return {"ok": False, "msg": "acción desconocida"}
        except Exception as e:
            return {"ok": False, "msg": f"No se pudo: {e}"}
