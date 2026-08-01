#!/usr/bin/env python3
"""Servidor de NXSaveSync v3: sincroniza los savedata de una o varias Switch
con los emuladores de este PC.

- Habla el protocolo de PROTOCOL.md por TCP y responde al descubrimiento por UDP.
- Opera directamente sobre las carpetas de saves de los emuladores detectados.
- Con varios emuladores manda el que jugaste mas recientemente y replica al resto.
- Se configura desde la propia consola.
- Guarda el estado por perfil de la consola, asi que dos usuarios no se pisan.
- Vigila la carpeta del emulador para saber que juegos tienen cambios pendientes.

Uso:
    ./nxsavesyncd.py                 # autodetecta el emulador
    ./nxsavesyncd.py --list          # muestra lo detectado y sale
    ./nxsavesyncd.py --dir ./saves   # ignora los emuladores y usa una carpeta
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import socket
import struct
import sys
import threading
import time
import zlib
from datetime import datetime
from pathlib import Path

import emulators

PROTO_VERSION = 7
DEFAULT_PORT = 7878
DISCOVERY_PORT = 7879
MAX_FRAME = 64 * 1024 * 1024
CHUNK = 256 * 1024

DISC_PROBE = b"NXSS?"
DISC_REPLY = b"NXSS!"

# Toque a la consola. El PC no puede sincronizar por su cuenta -la consola es
# siempre quien abre la conexion-, pero si puede avisar de que hay novedades
# para que el sysmodule las recoja al momento.
NUDGE_PORT = 7880
NUDGE_MSG = b"NXSN"

OP_HELLO, OP_HELLO_OK = 0x01, 0x81
OP_PLAN_REQ, OP_PLAN_RES = 0x02, 0x82
OP_PULL_REQ, OP_PULL_RES = 0x03, 0x83
OP_PUSH, OP_PUSH_OK = 0x04, 0x84
OP_DEL_REMOTE, OP_DEL_OK = 0x05, 0x85
OP_RESOLVE, OP_RESOLVE_RES = 0x06, 0x86
OP_COMMIT, OP_COMMIT_OK = 0x07, 0x87
OP_SUMMARY_REQ, OP_SUMMARY_RES = 0x08, 0x88
OP_DECIDE, OP_DECIDE_RES = 0x09, 0x89
OP_CFG_GET, OP_CFG_RES = 0x0A, 0x8A
OP_CFG_SET, OP_CFG_OK = 0x0B, 0x8B
OP_EMUS_REQ, OP_EMUS_RES = 0x0C, 0x8C   # v5: que emuladores hay en este PC
OP_PROFILE, OP_PROFILE_RES = 0x0D, 0x8D # v6: clonar el perfil de la consola
OP_GAME_ICON, OP_GAME_ICON_OK = 0x0E, 0x8E  # v7: caratula para el menu del PC
OP_ERROR = 0xFF

ACT_PULL, ACT_PUSH, ACT_DEL_LOCAL, ACT_DEL_REMOTE, ACT_CONFLICT = 0, 1, 2, 3, 4
WINNER_SWITCH, WINNER_PC = 0, 1

MODE_MANUAL, MODE_AUTO = 0, 1
POLICY_ASK, POLICY_SWITCH, POLICY_PC, POLICY_SKIP, POLICY_NEWEST = 0, 1, 2, 3, 4

# Margen por debajo del cual los dos lados se consideran empatados: no merece la
# pena decidir una partida por unos segundos de diferencia.
TIE_SECONDS = 90

WARN_NONE, WARN_PC_EMPTY, WARN_SWITCH_EMPTY, WARN_ROOT_CHANGED = 0, 1, 2, 3
DEC_SWITCH, DEC_PC, DEC_SKIP = 0, 1, 2

CFG_BOOL, CFG_INT, CFG_CHOICE, CFG_INFO, CFG_ACTION = 0, 1, 2, 3, 4

SUM_SYNCED, SUM_PC_CHANGED, SUM_UNKNOWN, SUM_NO_DIR = 0, 1, 2, 3

def _data_dir() -> Path:
    """Donde guardar estado y copias, segun lo que espera cada sistema."""
    if sys.platform == "win32":
        base = os.environ.get("LOCALAPPDATA") or (Path.home() / "AppData/Local")
        return Path(base) / "NXSaveSync"
    if sys.platform == "darwin":
        return Path.home() / "Library/Application Support/nxsavesync"
    return Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "nxsavesync"


def _config_path() -> Path:
    if sys.platform == "win32":
        base = os.environ.get("APPDATA") or (Path.home() / "AppData/Roaming")
        return Path(base) / "NXSaveSync" / "config.json"
    if sys.platform == "darwin":
        return Path.home() / "Library/Application Support/nxsavesync/config.json"
    return Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "nxsavesync/config.json"


STATE_DIR = _data_dir()
CONFIG_PATH = _config_path()
BACKUP_KEEP = 10

# Rutas que el usuario puede cambiar desde la interfaz. Vacio = la de por
# defecto, que es la que devuelve _data_dir().
_rutas = {"backups": None, "estado": None}


def set_rutas(backups: str | None = None, estado: str | None = None) -> None:
    if backups is not None:
        _rutas["backups"] = str(backups) if backups else None
    if estado is not None:
        _rutas["estado"] = str(estado) if estado else None


def backups_dir() -> Path:
    return Path(_rutas["backups"]) if _rutas["backups"] else STATE_DIR / "backups"


def estado_dir() -> Path:
    return Path(_rutas["estado"]) if _rutas["estado"] else STATE_DIR / "state"

# Archivos que el emulador (o el escritorio) deja dentro de la carpeta del save
# y que no forman parte de la partida. Si se sincronizaran, acabarian metidos en
# el savedata de la consola, que solo debe contener lo que escribe el juego.
IGNORED_FILES = {
    ".yuzu_save_size",
    ".ryujinx_save_size",
    ".DS_Store",
    "Thumbs.db",
}


# Sumidero opcional del registro, para que una interfaz pueda mostrarlo.
_log_sink = None


def set_log_sink(fn) -> None:
    global _log_sink
    _log_sink = fn


def log(msg: str) -> None:
    line = f"[{datetime.now():%H:%M:%S}] {msg}"
    print(line, flush=True)
    if _log_sink:
        try:
            _log_sink(line)
        except Exception:
            pass


class ProtocolError(Exception):
    pass


class ClientError(Exception):
    """Error que se le devuelve a la Switch sin cortar la conexion."""

    def __init__(self, message: str, code: int = 1):
        super().__init__(message)
        self.code = code
        self.message = message


# --------------------------------------------------------------------------
# rutas y utilidades
# --------------------------------------------------------------------------


def safe_rel(path: str) -> str:
    """Valida una ruta relativa que viene por la red.

    Sin esto, un cliente podria pedir '../../..' y escribir fuera de la carpeta
    del savedata.
    """
    if not path or path.startswith("/") or "\\" in path or "\x00" in path:
        raise ClientError(f"ruta rechazada: {path!r}")
    parts = path.split("/")
    if any(p in ("", ".", "..") for p in parts):
        raise ClientError(f"ruta rechazada: {path!r}")
    return path


def uid_str(hi: int, lo: int) -> str:
    return f"{hi:016X}{lo:016X}"


# Cache de CRC por archivo, validada con (mtime, tamano). Evita releer saves de
# varios MB en cada SUMMARY, que si no haria la lista de juegos lentisima.
_crc_cache: dict[str, tuple[float, int, int]] = {}
_crc_lock = threading.Lock()


def crc32_file(path: Path) -> tuple[int, int]:
    key = str(path)
    try:
        st = path.stat()
    except OSError:
        raise

    with _crc_lock:
        hit = _crc_cache.get(key)
        if hit and hit[0] == st.st_mtime and hit[1] == st.st_size:
            return hit[2], st.st_size

    crc = 0
    with path.open("rb") as f:
        while True:
            chunk = f.read(CHUNK)
            if not chunk:
                break
            crc = zlib.crc32(chunk, crc)

    with _crc_lock:
        _crc_cache[key] = (st.st_mtime, st.st_size, crc)
    return crc, st.st_size


def invalidate_crc(path: Path) -> None:
    with _crc_lock:
        _crc_cache.pop(str(path), None)


def scan_dir(root: Path) -> dict[str, int]:
    """{ruta_relativa: crc32} de todos los archivos bajo root."""
    out: dict[str, int] = {}
    if not root.is_dir():
        return out

    for path in root.rglob("*"):
        if not path.is_file() or path.is_symlink():
            continue
        if path.name in IGNORED_FILES:
            continue
        try:
            crc, _ = crc32_file(path)
        except OSError:
            continue
        out[path.relative_to(root).as_posix()] = crc
    return out


# --------------------------------------------------------------------------
# configuracion
# --------------------------------------------------------------------------


def load_config() -> dict:
    if not CONFIG_PATH.is_file():
        return {}
    try:
        return json.loads(CONFIG_PATH.read_text())
    except (json.JSONDecodeError, OSError):
        log(f"aviso: {CONFIG_PATH} ilegible, se usan los valores por defecto")
        return {}


def save_config(cfg: dict) -> None:
    CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
    CONFIG_PATH.write_text(json.dumps(cfg, indent=2, sort_keys=True))


# --------------------------------------------------------------------------
# framing
# --------------------------------------------------------------------------


class Conn:
    def __init__(self, sock: socket.socket):
        self.sock = sock
        self.remaining = 0

    def _recv_exact(self, n: int) -> bytes:
        buf = bytearray()
        while len(buf) < n:
            chunk = self.sock.recv(min(n - len(buf), CHUNK))
            if not chunk:
                raise ProtocolError("la Switch cerro la conexion")
            buf += chunk
        return bytes(buf)

    def read_header(self) -> tuple[int, int]:
        hdr = self._recv_exact(5)
        op = hdr[0]
        (plen,) = struct.unpack_from("<I", hdr, 1)
        if plen > MAX_FRAME:
            raise ProtocolError(f"trama de {plen} bytes por encima del limite")
        self.remaining = plen
        return op, plen

    def take(self, n: int) -> bytes:
        if n > self.remaining:
            raise ProtocolError("mensaje truncado")
        data = self._recv_exact(n)
        self.remaining -= n
        return data

    def r_u8(self) -> int:
        return self.take(1)[0]

    def r_u32(self) -> int:
        return struct.unpack("<I", self.take(4))[0]

    def r_u64(self) -> int:
        return struct.unpack("<Q", self.take(8))[0]

    def r_uid(self) -> str:
        return uid_str(self.r_u64(), self.r_u64())

    def r_bytes(self) -> bytes:
        """Un bloque con su longitud delante. Para la foto del perfil."""
        n = self.r_u32()
        if n > 4 * 1024 * 1024:
            raise ClientError(f"bloque de {n} bytes, demasiado grande")
        return self.take(n) if n else b""

    def r_str(self) -> str:
        (n,) = struct.unpack("<H", self.take(2))
        return self.take(n).decode("utf-8", "replace")

    def drain(self) -> None:
        while self.remaining:
            self.take(min(self.remaining, CHUNK))

    def send(self, op: int, payload: bytes = b"") -> None:
        self.sock.sendall(bytes([op]) + struct.pack("<I", len(payload)) + payload)

    def send_streaming_header(self, op: int, payload: bytes, extra: int) -> None:
        self.sock.sendall(bytes([op]) + struct.pack("<I", len(payload) + extra) + payload)

    def send_raw(self, data: bytes) -> None:
        self.sock.sendall(data)

    def send_error(self, message: str, code: int = 1) -> None:
        body = message.encode("utf-8")[:2000]
        self.send(OP_ERROR, struct.pack("<I", code) + struct.pack("<H", len(body)) + body)


def w_str(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack("<H", len(b)) + b


# --------------------------------------------------------------------------
# estado por perfil
# --------------------------------------------------------------------------


def fichas_dir() -> Path:
    """Donde viven las fichas por juego: nombre, caratula y ultimo movimiento.

    Es para el menu del PC. Va aparte del estado de sincronizacion porque
    aquello es maquinaria y esto es solo para ensenar.
    """
    d = estado_dir() / "juegos"
    d.mkdir(parents=True, exist_ok=True)
    return d


def ficha_path(title_id: int) -> Path:
    return fichas_dir() / f"{title_id:016X}.json"


def icono_path(title_id: int) -> Path:
    return fichas_dir() / f"{title_id:016X}.jpg"


def lee_ficha(title_id: int) -> dict:
    try:
        return json.loads(ficha_path(title_id).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def apunta_ficha(title_id: int, nombre: str, uid: str, **campos) -> None:
    """Deja constancia de lo que le ha pasado a un juego.

    Se guarda el ultimo movimiento y un acumulado, que juntos responden a las
    dos preguntas que uno se hace: que paso la ultima vez, y cuanto se ha
    movido en total.
    """
    f = lee_ficha(title_id)
    f["title_id"] = f"{title_id:016X}"
    if nombre:
        f["nombre"] = nombre
    f["uid"] = uid
    f["cuando"] = time.time()
    f.update(campos)

    for k in ("bajados", "subidos", "borrados"):
        f["total_" + k] = int(f.get("total_" + k, 0)) + int(campos.get(k, 0))
    f["veces"] = int(f.get("veces", 0)) + 1

    try:
        ficha_path(title_id).write_text(json.dumps(f, ensure_ascii=False),
                                        encoding="utf-8")
    except OSError:
        pass


def apunta_estado(title_id: int, estado: int, nombre: str = "") -> None:
    """Refresca solo el estado de la ficha, y solo si cambio.

    El resumen pasa por todos los juegos en cada conexion; reescribir una
    docena de archivos cada vez para dejarlos igual no tiene sentido.
    """
    f = lee_ficha(title_id)
    if f.get("estado") == estado and (not nombre or f.get("nombre") == nombre):
        return

    f["title_id"] = f"{title_id:016X}"
    f["estado"] = estado
    if nombre:
        f["nombre"] = nombre
    try:
        ficha_path(title_id).write_text(json.dumps(f, ensure_ascii=False),
                                        encoding="utf-8")
    except OSError:
        pass


def state_path(uid: str, title_id: int) -> Path:
    return estado_dir() / uid / f"{title_id:016x}.json"


def load_base(uid: str, title_id: int, root: Path,
              hermanas: set[str] | None = None) -> tuple[dict[str, int], bool]:
    """Estado en que quedaron los dos lados tras la ultima sync correcta.

    La base describe *el acuerdo entre la consola y el PC*, no el contenido de
    una carpeta concreta. La distincion importa en cuanto hay mas de un
    emulador: `resolve` elige cada vez el que se escribio mas tarde, asi que
    jugar en Ryujinx despues de haber jugado en eden cambia la carpeta elegida
    sin que haya cambiado nada del acuerdo.

    Por eso `hermanas` -- las carpetas de este mismo juego en los demas
    emuladores -- no cuenta como cambio de sitio. Sin esto, alternar de
    emulador daba "la carpeta ha cambiado" en cada sincronizacion y el juego se
    quedaba sin sincronizar esperando una decision.

    Un cambio de verdad (el emulador movio su carpeta, o apuntas a otro sitio)
    si invalida la base: describiria archivos de otro lado, y aplicarla haria
    que una carpeta nueva pareciera "el PC ha cambiado" y machacaria la partida
    buena de la consola.
    """
    p = state_path(uid, title_id)
    if not p.is_file():
        return {}, False

    try:
        data = json.loads(p.read_text())
    except (json.JSONDecodeError, OSError):
        log(f"  aviso: estado ilegible para {title_id:016X}, se tratara como primera sync")
        return {}, False

    if not isinstance(data, dict) or "files" not in data:
        log(f"  aviso: estado en formato antiguo para {title_id:016X}, "
            f"se tratara como primera sync")
        return {}, False

    anterior = data.get("root")
    if anterior != str(root):
        # Las dos carpetas tienen que ser destinos actuales de este juego. Con
        # comprobar solo la vieja bastaba para el caso normal, pero daba por
        # buena una carpeta ajena si `hermanas` venia incompleta.
        if hermanas and anterior in hermanas and str(root) in hermanas:
            # Otro emulador del mismo juego: el acuerdo sigue siendo el mismo.
            log(f"  la comparacion cambia de emulador ({Path(anterior).name} -> "
                f"{Path(str(root)).name}); la base sigue valiendo")
        else:
            log(f"  aviso: la carpeta de saves ha cambiado")
            log(f"           antes: {anterior}")
            log(f"           ahora: {root}")
            return {}, True

    try:
        return {k: int(v) for k, v in data["files"].items()}, False
    except (ValueError, AttributeError):
        return {}, False


def save_base(uid: str, title_id: int, manifest: dict[str, int], root: Path) -> None:
    p = state_path(uid, title_id)
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_suffix(".tmp")
    tmp.write_text(json.dumps(
        {"root": str(root), "files": manifest, "at": datetime.now().isoformat(timespec="seconds")},
        indent=1, sort_keys=True))
    tmp.replace(p)


def nombre_carpeta(title_name: str, title_id: int) -> str:
    """Nombre de carpeta legible para un juego.

    Antes las copias se guardaban solo con el title id, que no dice nada al
    mirarlas meses despues. Ahora llevan el nombre delante y el id detras, que
    sigue haciendo falta para no confundir dos juegos que se llamen parecido.
    """
    limpio = "".join(c if (c.isalnum() or c in " -_.()[]") else "_"
                     for c in (title_name or "").strip())
    limpio = " ".join(limpio.split())[:60].rstrip(". ")
    return f"{limpio} [{title_id:016X}]" if limpio else f"{title_id:016X}"


def make_backup(uid: str, title_id: int, root: Path, title_name: str) -> None:
    """Copia de seguridad de la carpeta del PC antes de tocarla."""
    if not root.is_dir() or not any(root.rglob("*")):
        return

    dest_dir = backups_dir() / uid / nombre_carpeta(title_name, title_id)

    # Si ya habia copias con el nombre viejo (solo el id), se trasladan para no
    # dejar el historial partido en dos sitios.
    antiguo = backups_dir() / uid / f"{title_id:016x}"
    if antiguo.is_dir() and antiguo != dest_dir:
        dest_dir.mkdir(parents=True, exist_ok=True)
        for z in antiguo.glob("*.zip"):
            destino = dest_dir / z.name
            if not destino.exists():
                z.rename(destino)
        try:
            antiguo.rmdir()
        except OSError:
            pass
    dest_dir.mkdir(parents=True, exist_ok=True)
    # Con precision de segundo, dos syncs seguidas se pisarian la copia.
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")[:-3]
    base = dest_dir / stamp

    try:
        shutil.make_archive(str(base), "zip", root_dir=root)
    except OSError as e:
        log(f"  aviso: no se pudo hacer copia de seguridad de {title_name}: {e}")
        return

    log(f"  copia de seguridad: {base}.zip")

    for old in sorted(dest_dir.glob("*.zip"))[:-BACKUP_KEEP]:
        old.unlink(missing_ok=True)


# --------------------------------------------------------------------------
# resolucion a tres vias
# --------------------------------------------------------------------------


def compute_plan(
    switch: dict[str, int], pc: dict[str, int], base: dict[str, int]
) -> list[tuple[int, str]]:
    plan: list[tuple[int, str]] = []

    for path in sorted(set(switch) | set(pc) | set(base)):
        s = switch.get(path)
        p = pc.get(path)
        b = base.get(path)

        if s == p:
            continue

        if s == b:
            # La Switch no lo ha tocado desde la ultima sync: manda el PC.
            plan.append((ACT_PULL if p is not None else ACT_DEL_LOCAL, path))
        elif p == b:
            # El PC no lo ha tocado: manda la Switch.
            plan.append((ACT_PUSH if s is not None else ACT_DEL_REMOTE, path))
        else:
            plan.append((ACT_CONFLICT, path))

    return plan


def detect_warning(
    switch: dict[str, int], pc: dict[str, int], base: dict[str, int], root_changed: bool
) -> tuple[int, str]:
    """Detecta situaciones ambiguas que no conviene resolver por nuestra cuenta.

    El caso peligroso: ya hubo una sync (base no vacia) y de golpe un lado
    aparece vacio. Comparando a tres bandas eso se lee como "los han borrado" y
    el plan seria borrarlos tambien en el otro lado. Pero puede ser justo lo
    contrario: que la carpeta se haya movido, que el emulador la haya recreado
    en blanco, o que hayas limpiado el PC y quieras volver a subirlos.

    Con la informacion que hay aqui no se puede distinguir, asi que en vez de
    elegir (o de negarse en seco, que deja al usuario sin salida) se devuelve un
    aviso y decide quien esta delante de la consola.
    """
    if root_changed and switch and pc:
        return WARN_ROOT_CHANGED, (
            "La carpeta de saves del PC ha cambiado de sitio, asi que lo que se "
            "sabia de la ultima sincronizacion ya no vale. Las dos versiones "
            "estan completas: elige cual se queda."
        )

    if not base:
        return WARN_NONE, ""

    if switch and not pc:
        return WARN_PC_EMPTY, (
            f"El PC tenia {len(base)} archivo(s) registrados pero su carpeta esta "
            f"vacia. Puede que el emulador la haya recreado en blanco o que se "
            f"hayan borrado. La consola conserva {len(switch)}."
        )

    if pc and not switch:
        return WARN_SWITCH_EMPTY, (
            f"La consola aparece sin archivos pero habia {len(base)} registrados. "
            f"El PC conserva {len(pc)}."
        )

    return WARN_NONE, ""


def replan_after_decision(
    switch: dict[str, int], pc: dict[str, int], decision: int
) -> list[tuple[int, str]]:
    """Plan tras responder a un aviso, ignorando la base anterior.

    Sin base, un archivo que solo esta en un lado se copia al otro en vez de
    interpretarse como un borrado. Eso es justo lo que hace falta para
    regenerar: la consola vuelve a subir lo que el PC perdio, o al reves.
    """
    if decision == DEC_SKIP:
        return []

    plan = compute_plan(switch, pc, {})
    winner = WINNER_SWITCH if decision == DEC_SWITCH else WINNER_PC
    return resolve_plan(plan, switch, pc, winner)


def resolve_plan(
    plan: list[tuple[int, str]], switch: dict[str, int], pc: dict[str, int], winner: int
) -> list[tuple[int, str]]:
    """Sustituye cada CONFLICT por la accion que impone el lado ganador."""
    out: list[tuple[int, str]] = []

    for action, path in plan:
        if action != ACT_CONFLICT:
            out.append((action, path))
        elif winner == WINNER_SWITCH:
            out.append((ACT_PUSH if path in switch else ACT_DEL_REMOTE, path))
        else:
            out.append((ACT_PULL if path in pc else ACT_DEL_LOCAL, path))

    return out


def apply_policy(
    plan: list[tuple[int, str]], switch: dict[str, int], pc: dict[str, int],
    policy: int, ganador_reciente: int = -1
) -> tuple[list[tuple[int, str]], bool]:
    """Resuelve los conflictos en modo automatico. Devuelve (plan, hubo_conflicto)."""
    had = any(a == ACT_CONFLICT for a, _ in plan)
    if not had:
        return plan, False

    if policy == POLICY_NEWEST:
        # Si no se pudo averiguar quien jugo mas tarde, no se elige a ciegas.
        if ganador_reciente in (WINNER_SWITCH, WINNER_PC):
            return resolve_plan(plan, switch, pc, ganador_reciente), True
        return [], True

    if policy == POLICY_SWITCH:
        return resolve_plan(plan, switch, pc, WINNER_SWITCH), True
    if policy == POLICY_PC:
        return resolve_plan(plan, switch, pc, WINNER_PC), True

    # POLICY_SKIP (y POLICY_ASK, que en automatico no tiene sentido): no se
    # toca nada de ese juego. Mejor dejarlo pendiente que elegir a ciegas.
    return [], True


def encode_warning(code: int, message: str) -> bytes:
    return bytes([code]) + w_str(message)


def encode_plan(plan: list[tuple[int, str]]) -> bytes:
    body = struct.pack("<I", len(plan))
    for action, path in plan:
        body += bytes([action]) + w_str(path)
    return body


# --------------------------------------------------------------------------
# vigilancia de la carpeta del emulador
# --------------------------------------------------------------------------


class Watcher(threading.Thread):
    """Repasa cada pocos segundos las carpetas ya conocidas del emulador.

    No dispara ninguna sincronizacion por si sola: la consola no puede aceptar
    conexiones entrantes mientras no este la app abierta. Lo que hace es dejar
    los CRC calientes en cache y anotar en el log que hay cambios sin
    sincronizar, para que al abrir la app la lista salga al instante.
    """

    def __init__(self, resolver, interval: int = 5, nudge: bool = True):
        super().__init__(daemon=True)
        self.resolver = resolver
        self.interval = interval
        self.nudge = nudge
        self.stop_flag = threading.Event()
        self.known: dict[tuple[str, int], dict[str, int]] = {}
        self.names: dict[tuple[str, int], str] = {}
        self.pending: set[tuple[str, int]] = set()
        self.lock = threading.Lock()

    def watch(self, uid: str, title_id: int, name: str) -> None:
        """Empieza a vigilar un juego. La primera pasada solo toma la foto."""
        key = (uid, title_id)
        with self.lock:
            self.names[key] = name
            if key not in self.known:
                self.known[key] = None   # None = aun sin foto inicial

    def run(self) -> None:
        while not self.stop_flag.wait(self.interval):
            with self.lock:
                keys = list(self.known.keys())

            for key in keys:
                uid, title_id = key
                try:
                    root = self.resolver(uid, title_id)
                except Exception:
                    continue
                if root is None:
                    continue

                current = scan_dir(root)

                with self.lock:
                    previous = self.known.get(key)
                    self.known[key] = current

                if previous is not None and current != previous:
                    name = self.names.get(key, f"{title_id:016X}")
                    log(f"cambio en el emulador: {name} ({len(current)} archivo(s))")
                    with self.lock:
                        self.pending.add(key)
                    if self.nudge:
                        send_nudge(name)


# --------------------------------------------------------------------------
# modo sin consola
# --------------------------------------------------------------------------


class Espejo(threading.Thread):
    """Mantiene iguales los emuladores entre si, sin que la consola participe.

    Sirve para el caso de tener eden y Ryujinx y querer jugar en cualquiera de
    los dos con la misma partida, tenga la Switch encendida o no.

    Las tres reglas que lo hacen seguro, y ninguna sobra:

    1. **Solo se copia si el original lleva un rato quieto.** Un emulador que
       acaba de escribir puede estar a mitad de guardar. Copiar eso deja una
       partida corrupta en los demas.
    2. **Antes de sobrescribir, copia de seguridad.** Es la misma que se hace
       al sincronizar con la consola, con el nombre del juego.
    3. **Manda la fecha, y solo entre carpetas del mismo PC.** Comparar fechas
       entre la consola y el PC no vale porque sus relojes no coinciden; entre
       dos emuladores de la misma maquina si.
    """

    # Cuanto tiene que llevar quieto el original antes de fiarse de el.
    REPOSO = 20

    def __init__(self, rt, interval: int = 15):
        super().__init__(daemon=True)
        self.rt = rt
        self.interval = interval
        self.stop_flag = threading.Event()
        self.hechos = 0

    def run(self) -> None:
        while not self.stop_flag.wait(self.interval):
            if not self.rt.sin_consola:
                continue
            try:
                self.pasada()
            except Exception as e:
                log(f"espejo: {e}")

    # -- la base ------------------------------------------------------------
    #
    # Lo que todos tenian la ultima vez que quedaron iguales. Sin esto, un
    # borrado no se puede distinguir de "a ese le falta un archivo", porque
    # borrar no cambia la fecha de los que quedan: el lado que borro parece el
    # mas viejo, pierde, y el archivo resucita.

    @staticmethod
    def _base_path(title_id: int) -> Path:
        d = estado_dir() / "espejo"
        d.mkdir(parents=True, exist_ok=True)
        return d / f"{title_id:016X}.json"

    def _lee_base(self, title_id: int) -> dict[str, int] | None:
        try:
            return {k: int(v) for k, v in
                    json.loads(self._base_path(title_id).read_text()).items()}
        except (OSError, json.JSONDecodeError, ValueError, AttributeError):
            return None

    def _guarda_base(self, title_id: int, manifest: dict[str, int]) -> None:
        try:
            self._base_path(title_id).write_text(json.dumps(manifest))
        except OSError:
            pass

    def pasada(self) -> None:
        activos = self.rt.active_emus()
        if len(activos) < 2:
            return

        # De donde puede salir cada juego. Una carpeta vacia no cuenta como
        # version: es la ausencia de una.
        fuentes: dict[int, list] = {}
        for emu in activos:
            try:
                for tid, carpeta in emulators.titulos(emu).items():
                    m = scan_dir(carpeta)
                    if not m:
                        continue
                    fuentes.setdefault(tid, []).append(
                        (emu, carpeta, m, newest_mtime(carpeta)))
            except Exception:
                continue

        ahora = time.time()

        for tid, sitios in fuentes.items():
            base = self._lee_base(tid)

            # Quien ha cambiado desde la ultima vez que estaban iguales. Si solo
            # uno, ese manda aunque su fecha sea mas vieja: es justo el caso del
            # borrado. Si varios, decide la fecha, que entre carpetas del mismo
            # PC si es de fiar.
            cambiados = [x for x in sitios if base is None or x[2] != base]
            candidatos = cambiados or sitios
            candidatos.sort(key=lambda x: x[3], reverse=True)
            emu_o, origen, man_o, fecha_o = candidatos[0]

            # Recien escrito puede estar a medio guardar. Copiar eso deja una
            # partida corrupta en los demas.
            if ahora - fecha_o < self.REPOSO:
                continue

            nombre = lee_ficha(tid).get("nombre") or f"{tid:016X}"
            movido = False

            for emu_d in activos:
                if emu_d is emu_o:
                    continue

                # El destino puede no tener todavia el juego: es el caso mas
                # habitual y el que hace util todo esto. Ryujinx es la
                # excepcion, porque sin entrada en su indice no se puede saber
                # que carpeta le toca.
                try:
                    destino = emu_d.save_dir(tid)
                except Exception:
                    destino = None
                if destino is None:
                    continue

                try:
                    destino.mkdir(parents=True, exist_ok=True)
                    man_d = scan_dir(destino)
                    if man_d == man_o:
                        continue

                    if man_d:
                        make_backup("sin-consola", tid, destino, nombre)

                    n = self.rt._replica(origen, destino)
                    self.rt.sync_shadows_de(emu_d, tid)
                    self.hechos += 1
                    movido = True
                    log(f"espejo: {nombre}: {emu_o.name} -> {emu_d.name} "
                        f"({n} archivo(s))")
                except Exception as e:
                    log(f"espejo: no se pudo copiar {nombre} a {emu_d.name}: {e}")

            if movido or base != man_o:
                self._guarda_base(tid, man_o)


# --------------------------------------------------------------------------
# descubrimiento
# --------------------------------------------------------------------------


def send_nudge(motivo: str = "") -> None:
    """Avisa por broadcast de que hay cambios que recoger.

    Es un aviso, no una orden: la consola decide si sincroniza y cuando. Si no
    hay ninguna escuchando, el datagrama se pierde y no pasa nada.
    """
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        s.settimeout(1.0)
        s.sendto(NUDGE_MSG + struct.pack("<I", PROTO_VERSION),
                 ("255.255.255.255", NUDGE_PORT))
        s.close()
        log(f"  aviso enviado a la red{': ' + motivo if motivo else ''}")
    except OSError as e:
        log(f"  no se pudo avisar a la red: {e}")


class Discovery(threading.Thread):
    """Contesta a las sondas UDP para que la consola encuentre este PC sola."""

    def __init__(self, tcp_port: int, emu_name):
        super().__init__(daemon=True)
        self.tcp_port = tcp_port
        # Puede ser un texto fijo o algo que lo devuelva. Con un texto fijo, si
        # instalas un emulador despues de arrancar el daemon, la consola seguiria
        # viendo el nombre viejo hasta reiniciarlo.
        self._emu_name = emu_name
        self.stop_flag = threading.Event()

    @property
    def emu_name(self) -> str:
        try:
            return self._emu_name() if callable(self._emu_name) else self._emu_name
        except Exception:
            return "?"

    def run(self) -> None:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            if sys.platform != "win32":
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("", DISCOVERY_PORT))
            s.settimeout(1.0)
        except OSError as e:
            log(f"aviso: no se pudo abrir el descubrimiento en el {DISCOVERY_PORT}: {e}")
            return

        hostname = socket.gethostname()
        log(f"Descubrimiento escuchando en UDP {DISCOVERY_PORT}")

        while not self.stop_flag.is_set():
            try:
                data, addr = s.recvfrom(512)
            except socket.timeout:
                continue
            except OSError:
                break

            if not data.startswith(DISC_PROBE) or len(data) < len(DISC_PROBE) + 4:
                continue

            (version,) = struct.unpack_from("<I", data, len(DISC_PROBE))
            if version != PROTO_VERSION:
                continue

            reply = (DISC_REPLY + struct.pack("<I", PROTO_VERSION)
                     + struct.pack("<H", self.tcp_port)
                     + w_str(hostname) + w_str(self.emu_name)
                     + struct.pack("<I", 0))
            try:
                s.sendto(reply, addr)
                log(f"Descubrimiento: respondido a {addr[0]}")
            except OSError:
                pass

        s.close()


# --------------------------------------------------------------------------
# sesion
# --------------------------------------------------------------------------

ACTION_NAMES = {
    ACT_PULL: "-> Switch",
    ACT_PUSH: "-> PC",
    ACT_DEL_LOCAL: "borrar en Switch",
    ACT_DEL_REMOTE: "borrar en PC",
    ACT_CONFLICT: "CONFLICTO",
}


class Session:
    def __init__(self, conn: Conn, rt: "Runtime"):
        self.conn = conn
        self.rt = rt
        self.titles: dict[tuple[str, int], dict] = {}
        self.backed_up: set[tuple[str, int]] = set()

    @property
    def resolver(self):
        return self.rt.resolve

    @property
    def watcher(self):
        return self.rt.watcher

    @property
    def emu_name(self) -> str:
        return self.rt.emu_name

    def root_for(self, uid: str, title_id: int) -> Path:
        root = self.resolver(uid, title_id)
        if root is None:
            raise ClientError(
                f"no hay carpeta de saves en el PC para {title_id:016X}. "
                "Abre el juego una vez en el emulador y guarda partida."
            )
        return root

    def ensure_backup(self, uid: str, title_id: int, name: str) -> None:
        key = (uid, title_id)
        if key in self.backed_up:
            return
        self.backed_up.add(key)
        make_backup(uid, title_id, self.root_for(uid, title_id), name)

    # -- handlers -----------------------------------------------------------

    def on_hello(self) -> None:
        version = self.conn.r_u32()
        client = self.conn.r_str()
        device = self.conn.r_str()
        log(f"Conectada una Switch ({client}/{device}, protocolo v{version})")

        if version != PROTO_VERSION:
            self.conn.send_error(f"el PC habla la version {PROTO_VERSION}")
            raise ProtocolError(f"version incompatible: {version}")

        self.conn.send(OP_HELLO_OK,
                       struct.pack("<I", PROTO_VERSION)
                       + w_str(socket.gethostname())
                       + w_str(self.emu_name)
                       + struct.pack("<I", 0))

    def on_summary_req(self) -> None:
        uid = self.conn.r_uid()
        count = self.conn.r_u32()
        ids = [self.conn.r_u64() for _ in range(count)]

        body = struct.pack("<I", len(ids))
        for title_id in ids:
            try:
                root = self.resolver(uid, title_id)
            except Exception:
                root = None

            if root is None or not root.is_dir():
                state = SUM_NO_DIR
            else:
                base = load_base_quiet(uid, title_id, root,
                                       self.rt.hermanas(uid, title_id))
                if not base:
                    state = SUM_UNKNOWN
                else:
                    state = SUM_SYNCED if scan_dir(root) == base else SUM_PC_CHANGED

                if self.watcher is not None:
                    self.watcher.watch(uid, title_id, f"{title_id:016X}")

            # v5: y en cual de los emuladores se jugo por ultima vez.
            try:
                emu = self.rt.last_played(uid, title_id)
            except Exception:
                emu = -1

            apunta_estado(title_id, state)

            # 0xFE quiere decir "la consola". Se saca de la ficha, que es donde
            # quedo apuntado al terminar la ultima sincronizacion.
            if lee_ficha(title_id).get("lugar") == "Consola":
                emu = -2

            # v7: y si ya tenemos su caratula, para no pedirla otra vez.
            tiene = 1 if icono_path(title_id).is_file() else 0

            body += (struct.pack("<Q", title_id) + bytes([state])
                     + bytes([0xFE if emu == -2 else (0xFF if emu < 0 else emu & 0xFF)])
                     + bytes([tiene]))

        self.conn.send(OP_SUMMARY_RES, body)

    def on_game_icon(self) -> None:
        """La consola manda la caratula de un juego, una sola vez.

        El PC no tiene forma de sacarla por su cuenta: los emuladores guardan
        partidas, no la ficha del juego. Y sin caratula el menu es una lista de
        nombres, que para reconocer un juego de un vistazo no vale.
        """
        title_id = self.conn.r_u64()
        nombre = self.conn.r_str()
        datos = self.conn.r_bytes()

        if datos:
            try:
                icono_path(title_id).write_bytes(datos)
            except OSError:
                pass

        f = lee_ficha(title_id)
        f["title_id"] = f"{title_id:016X}"
        if nombre:
            f["nombre"] = nombre
        try:
            ficha_path(title_id).write_text(json.dumps(f, ensure_ascii=False),
                                            encoding="utf-8")
        except OSError:
            pass

        log(f"  caratula guardada: {nombre} ({len(datos)} bytes)")
        self.conn.send(OP_GAME_ICON_OK)

    def on_profile(self) -> None:
        """Deja el perfil de la consola dentro de uno o varios emuladores.

        Solo la identidad: nombre y foto. Las partidas van despues por la via
        de siempre, juego a juego, que ya sabe hacer copias de seguridad y
        resolver conflictos. Mezclarlo todo en una operacion habria significado
        reimplementar eso peor.
        """
        uid = self.conn.r_uid()
        nombre = self.conn.r_str()
        cual = self.conn.r_u8()              # indice de emulador, 0xFF = todos
        avatar = self.conn.r_bytes()

        log(f"clonar perfil: {nombre!r} ({uid[:8]}...) "
            f"{'en todos' if cual == 0xFF else f'en el emulador {cual}'}"
            f"{f', foto de {len(avatar)} bytes' if avatar else ', sin foto'}")

        destinos = self.rt.emus if cual == 0xFF else (
            [self.rt.emus[cual]] if cual < len(self.rt.emus) else [])

        if not destinos:
            self.conn.send(OP_PROFILE_RES, bytes([0]) + w_str("no hay ese emulador"))
            return

        hechos, fallos = [], []
        for emu in destinos:
            try:
                hechos.append(emulators.clona_perfil(emu, uid, nombre, avatar or None))
                log(f"  {hechos[-1]}")
            except Exception as e:
                fallos.append(f"{emu.name}: {e}")
                log(f"  no se pudo en {emu.name}: {e}")

        if hechos:
            msg = "; ".join(hechos)
            if fallos:
                msg += f" (sin hacer: {len(fallos)})"
            self.conn.send(OP_PROFILE_RES, bytes([1]) + w_str(msg))
        else:
            self.conn.send(OP_PROFILE_RES, bytes([0]) + w_str("; ".join(fallos)))

    def on_emus_req(self) -> None:
        """Los emuladores que hay en este PC, para poder verlos en la consola."""
        emus = self.rt.emus
        body = struct.pack("<I", len(emus))
        for e in emus:
            body += (w_str(e.name) + w_str(str(e.base))
                     + bytes([0 if e.name in self.rt.disabled else 1]))
        self.conn.send(OP_EMUS_RES, body)
        log(f"  lista de emuladores enviada a la consola ({len(emus)})")

    def on_plan_req(self) -> None:
        uid = self.conn.r_uid()
        user_name = self.conn.r_str()
        title_id = self.conn.r_u64()
        name = self.conn.r_str()
        mode = self.conn.r_u8()
        policy = self.conn.r_u8()
        switch_clock = self.conn.r_u64()     # hora actual de la consola
        switch_newest = self.conn.r_u64()    # fecha del save mas reciente
        count = self.conn.r_u32()

        switch: dict[str, int] = {}
        for _ in range(count):
            path = safe_rel(self.conn.r_str())
            self.conn.r_u64()  # tamano, solo informativo
            switch[path] = self.conn.r_u32()

        root = self.root_for(uid, title_id)
        pc = scan_dir(root)
        base, root_changed = load_base(uid, title_id, root,
                                       self.rt.hermanas(uid, title_id))

        log(f"{name} [{title_id:016X}]  perfil {user_name or uid[:8]}")
        log(f"  switch={len(switch)} archivos  pc={len(pc)}  base={len(base)}")

        # Solo se calcula si hace falta, que implica recorrer la carpeta.
        ganador_reciente, motivo_reciente = -1, ""
        if policy == POLICY_NEWEST:
            ganador_reciente, motivo_reciente = decide_newest(
                switch_clock, switch_newest, root)
            log(f"  ultimo jugado: {motivo_reciente}")

        self.titles[(uid, title_id)] = {
            "name": name, "switch": switch, "pc": pc, "plan": [], "root": root,
            "switch_clock": switch_clock, "switch_newest": switch_newest,
        }
        if self.watcher is not None:
            self.watcher.watch(uid, title_id, name)

        warning, message = detect_warning(switch, pc, base, root_changed)
        if warning != WARN_NONE:
            # Con "gana el ultimo jugado" el aviso tambien se resuelve solo,
            # siempre que se haya podido averiguar donde se jugo despues.
            if (mode == MODE_AUTO and policy == POLICY_NEWEST
                    and ganador_reciente in (WINNER_SWITCH, WINNER_PC)):
                decision = (DEC_SWITCH if ganador_reciente == WINNER_SWITCH
                            else DEC_PC)
                plan = replan_after_decision(switch, pc, decision)
                self.titles[(uid, title_id)]["plan"] = plan
                log(f"  aviso resuelto solo ({motivo_reciente}): "
                    f"{len(plan)} accion(es)")
                self.conn.send(OP_PLAN_RES, encode_warning(WARN_NONE, "") + encode_plan(plan))
                return

            if mode == MODE_AUTO and policy in (POLICY_SWITCH, POLICY_PC):
                # En automatico ya hay una regla dicha de antemano: se aplica sin
                # preguntar, que es justo lo que se pidio al elegir ese modo.
                decision = DEC_SWITCH if policy == POLICY_SWITCH else DEC_PC
                plan = replan_after_decision(switch, pc, decision)
                self.titles[(uid, title_id)]["plan"] = plan
                log(f"  aviso resuelto solo ({policy_name(policy)}): {len(plan)} accion(es)")
                self.conn.send(OP_PLAN_RES, encode_warning(WARN_NONE, "") + encode_plan(plan))
                return

            log(f"  AVISO ({warning}): {message}")
            self.conn.send(OP_PLAN_RES, encode_warning(warning, message) + encode_plan([]))
            return

        plan = compute_plan(switch, pc, base)
        log(f"  -> {len(plan)} accion(es)")

        if mode == MODE_AUTO:
            plan, auto_resolved = apply_policy(plan, switch, pc, policy,
                                               ganador_reciente)
            if auto_resolved:
                if plan:
                    detalle = motivo_reciente if policy == POLICY_NEWEST else policy_name(policy)
                    log(f"  conflicto resuelto solo: {detalle}")
                elif policy == POLICY_NEWEST:
                    log(f"  conflicto sin resolver: {motivo_reciente or 'no se sabe'}")
                else:
                    log(f"  conflicto: no se toca nada (politica '{policy_name(policy)}')")

        self.titles[(uid, title_id)]["plan"] = plan

        for action, path in plan[:20]:
            log(f"    {ACTION_NAMES[action]:>16}  {path}")
        if len(plan) > 20:
            log(f"    ... y {len(plan) - 20} mas")

        self.conn.send(OP_PLAN_RES, encode_warning(WARN_NONE, "") + encode_plan(plan))

    def on_decide(self) -> None:
        uid = self.conn.r_uid()
        title_id = self.conn.r_u64()
        decision = self.conn.r_u8()

        ctx = self.titles.get((uid, title_id))
        if ctx is None:
            raise ClientError("DECIDE sin un PLAN_REQ previo para ese juego")

        plan = replan_after_decision(ctx["switch"], ctx["pc"], decision)
        ctx["plan"] = plan

        etiqueta = {DEC_SWITCH: "manda la consola", DEC_PC: "manda el PC",
                    DEC_SKIP: "no tocar nada"}.get(decision, "?")
        log(f"  decision del usuario: {etiqueta}  ->  {len(plan)} accion(es)")
        for action, path in plan[:20]:
            log(f"    {ACTION_NAMES[action]:>16}  {path}")
        if len(plan) > 20:
            log(f"    ... y {len(plan) - 20} mas")

        self.conn.send(OP_DECIDE_RES, encode_plan(plan))

    def on_resolve(self) -> None:
        uid = self.conn.r_uid()
        title_id = self.conn.r_u64()
        winner = self.conn.r_u8()

        ctx = self.titles.get((uid, title_id))
        if ctx is None:
            raise ClientError("RESOLVE sin un PLAN_REQ previo para ese juego")

        plan = resolve_plan(ctx["plan"], ctx["switch"], ctx["pc"], winner)
        ctx["plan"] = plan

        log(f"  conflicto resuelto a favor de {'la Switch' if winner == WINNER_SWITCH else 'el PC'}")
        self.conn.send(OP_RESOLVE_RES, encode_plan(plan))

    def on_pull_req(self) -> None:
        uid = self.conn.r_uid()
        title_id = self.conn.r_u64()
        rel = safe_rel(self.conn.r_str())

        path = self.root_for(uid, title_id) / rel
        if not path.is_file():
            raise ClientError(f"el PC no tiene {rel}")

        size = path.stat().st_size
        self.conn.send_streaming_header(OP_PULL_RES, struct.pack("<Q", size), size)

        sent = 0
        with path.open("rb") as f:
            while sent < size:
                chunk = f.read(min(CHUNK, size - sent))
                if not chunk:
                    # El archivo encogio a mitad de envio; rellenamos para no
                    # desincronizar el flujo (la Switch lo detectara por CRC).
                    chunk = b"\0" * (size - sent)
                self.conn.send_raw(chunk)
                sent += len(chunk)

    def on_push(self) -> None:
        uid = self.conn.r_uid()
        title_id = self.conn.r_u64()
        rel = safe_rel(self.conn.r_str())
        size = self.conn.r_u64()

        name = self.titles.get((uid, title_id), {}).get("name", f"{title_id:016X}")
        self.ensure_backup(uid, title_id, name)

        path = self.root_for(uid, title_id) / rel
        path.parent.mkdir(parents=True, exist_ok=True)

        # Escribimos a un temporal y renombramos: si se corta la conexion a
        # medias, el save que ya estaba no queda a medio pisar.
        tmp = path.with_name(path.name + ".nxsync-tmp")
        received = 0
        try:
            with tmp.open("wb") as f:
                while received < size:
                    chunk = self.conn.take(min(CHUNK, size - received))
                    f.write(chunk)
                    received += len(chunk)
            tmp.replace(path)
            invalidate_crc(path)
        except BaseException:
            tmp.unlink(missing_ok=True)
            raise

        self.conn.send(OP_PUSH_OK)

    def on_del_remote(self) -> None:
        uid = self.conn.r_uid()
        title_id = self.conn.r_u64()
        rel = safe_rel(self.conn.r_str())

        name = self.titles.get((uid, title_id), {}).get("name", f"{title_id:016X}")
        self.ensure_backup(uid, title_id, name)

        root = self.root_for(uid, title_id)
        path = root / rel
        path.unlink(missing_ok=True)
        invalidate_crc(path)

        # Poda de directorios que queden vacios, sin salirse de root.
        parent = path.parent
        while parent != root and parent.is_dir() and not any(parent.iterdir()):
            parent.rmdir()
            parent = parent.parent

        self.conn.send(OP_DEL_OK)

    def on_commit(self) -> None:
        uid = self.conn.r_uid()
        title_id = self.conn.r_u64()
        count = self.conn.r_u32()

        manifest: dict[str, int] = {}
        for _ in range(count):
            path = safe_rel(self.conn.r_str())
            self.conn.r_u64()
            manifest[path] = self.conn.r_u32()

        root = self.root_for(uid, title_id)
        save_base(uid, title_id, manifest, root)

        # Comprobacion util: si el PC no acabo igual que la Switch, la proxima
        # sync lo vera como cambios y conviene saberlo ya.
        pc = scan_dir(root)
        if pc != manifest:
            only_pc = sorted(set(pc) - set(manifest))
            only_sw = sorted(set(manifest) - set(pc))
            diff = sorted(k for k in set(pc) & set(manifest) if pc[k] != manifest[k])
            log(f"  aviso: los dos lados no quedaron identicos "
                f"(solo PC: {len(only_pc)}, solo Switch: {len(only_sw)}, distintos: {len(diff)})")
        else:
            log("  sincronizado")

        ctx = self.titles.get((uid, title_id), {})
        self.rt.mirror_to_others(uid, title_id, root, ctx.get("name", ""))

        # La ficha del juego, para el menu del PC: que se movio, cuando, y en
        # que emulador acabo.
        plan_hecho = ctx.get("plan", [])
        cuenta = {"bajados": 0, "subidos": 0, "borrados": 0}
        for accion, _ in plan_hecho:
            if accion == ACT_PULL:                       cuenta["bajados"] += 1
            elif accion == ACT_PUSH:                     cuenta["subidos"] += 1
            elif accion in (ACT_DEL_LOCAL, ACT_DEL_REMOTE): cuenta["borrados"] += 1

        try:
            emu_idx = self.rt.last_played(uid, title_id)
        except Exception:
            emu_idx = -1

        emu_nombre = (self.rt.emus[emu_idx].name
                      if 0 <= emu_idx < len(self.rt.emus) else "")

        # Donde se jugo por ultima vez, contando tambien la consola. Se compara
        # la fecha del save mas reciente de cada lado, corrigiendo el desfase de
        # relojes igual que hace "gana el ultimo jugado": la consola y el PC no
        # tienen por que estar en hora.
        lugar = emu_nombre
        try:
            sc = ctx.get("switch_clock") or 0
            sn = ctx.get("switch_newest") or 0
            pn = newest_mtime(root)
            if sc and sn:
                desfase = time.time() - sc
                if (sn + desfase) - pn > TIE_SECONDS:
                    lugar = "Consola"
        except Exception:
            pass

        apunta_ficha(title_id, ctx.get("name", ""), uid,
                     archivos=len(manifest),
                     emulador=emu_nombre,
                     lugar=lugar,
                     carpeta=str(root),
                     **cuenta)

        if self.watcher is not None:
            with self.watcher.lock:
                self.watcher.known[(uid, title_id)] = pc
                self.watcher.pending.discard((uid, title_id))

        rt = self.rt
        rt.stats["ultima"] = datetime.now()

        if rt.on_event:
            plan = ctx.get("plan", [])
            # Solo cuentan las acciones que mueven datos. Un juego que ya estaba
            # al dia no deberia generar aviso en la bandeja.
            movidos = sum(1 for a, _ in plan if a != ACT_CONFLICT)
            rt.stats["archivos"] = rt.stats.get("archivos", 0) + movidos
            rt.on_event("sync", {
                "nombre": ctx.get("name", f"{title_id:016X}"),
                "cambios": movidos,
                "bajados": sum(1 for a, _ in plan if a == ACT_PULL),
                "subidos": sum(1 for a, _ in plan if a == ACT_PUSH),
            })

        self.conn.send(OP_COMMIT_OK)

    def on_cfg_get(self) -> None:
        items = self.rt.schema()
        body = struct.pack("<I", len(items))
        for it in items:
            body += (w_str(it["key"]) + bytes([it["type"]]) + w_str(it["label"])
                     + w_str(it["help"]) + w_str(it["value"])
                     + struct.pack("<I", len(it["options"])))
            for opt in it["options"]:
                body += w_str(opt)
        self.conn.send(OP_CFG_RES, body)
        log(f"  ajustes enviados a la consola ({len(items)} opciones)")

    def on_cfg_set(self) -> None:
        key = self.conn.r_str()
        value = self.conn.r_str()

        try:
            msg = self.rt.apply(key, value)
        except ClientError:
            raise
        except (ValueError, OSError) as e:
            raise ClientError(f"no se pudo aplicar {key}: {e}")

        save_config(self.rt.cfg)
        log(f"  ajuste desde la consola: {key} = {value!r}  ->  {msg}")
        self.conn.send(OP_CFG_OK, w_str(msg))

    HANDLERS = {
        OP_HELLO: on_hello,
        OP_DECIDE: on_decide,
        OP_CFG_GET: on_cfg_get,
        OP_CFG_SET: on_cfg_set,
        OP_EMUS_REQ: on_emus_req,
        OP_PROFILE: on_profile,
        OP_GAME_ICON: on_game_icon,
        OP_PLAN_REQ: on_plan_req,
        OP_RESOLVE: on_resolve,
        OP_PULL_REQ: on_pull_req,
        OP_PUSH: on_push,
        OP_DEL_REMOTE: on_del_remote,
        OP_COMMIT: on_commit,
        OP_SUMMARY_REQ: on_summary_req,
    }

    def serve(self) -> None:
        while True:
            try:
                op, _ = self.conn.read_header()
            except ProtocolError:
                return

            handler = self.HANDLERS.get(op)
            if handler is None:
                self.conn.drain()
                self.conn.send_error(f"opcode desconocido 0x{op:02X}")
                continue

            try:
                handler(self)
            except ClientError as e:
                self.conn.drain()
                log(f"  error: {e.message}")
                self.conn.send_error(e.message, e.code)


def policy_name(p: int) -> str:
    return {POLICY_SWITCH: "gana la Switch", POLICY_PC: "gana el PC",
            POLICY_SKIP: "no tocar nada",
            POLICY_NEWEST: "gana el ultimo jugado"}.get(p, "preguntar")


def newest_mtime(root: Path) -> float:
    """Fecha del archivo mas reciente de la carpeta. 0 si no hay ninguno."""
    ultimo = 0.0
    if not root.is_dir():
        return ultimo
    for p in root.rglob("*"):
        if p.is_file() and p.name not in IGNORED_FILES:
            try:
                ultimo = max(ultimo, p.stat().st_mtime)
            except OSError:
                pass
    return ultimo


def decide_newest(switch_clock: float, switch_newest: float,
                  root: Path) -> tuple[int, str]:
    """Quien jugo mas tarde, la consola o el PC.

    Devuelve (WINNER_*, explicacion) o (-1, motivo) si no se puede saber.

    Los dos relojes no son comparables tal cual: el de una Switch con CFW se
    desajusta y el emulador escribe con la hora del PC. Por eso la consola manda
    tambien su hora ACTUAL: con ella se calcula el desfase entre relojes y se
    traduce la fecha del save a la hora de este PC. Un reloj adelantado o
    atrasado deja de importar mientras sea constante, que es el caso normal.
    """
    if not switch_clock or not switch_newest:
        return -1, "la consola no da fechas utiles"

    pc_newest = newest_mtime(root)
    if not pc_newest:
        return WINNER_SWITCH, "el PC no tiene nada"

    ahora = time.time()
    desfase = ahora - switch_clock          # cuanto adelanta/atrasa la consola
    switch_en_hora_pc = switch_newest + desfase

    diff = switch_en_hora_pc - pc_newest

    def hace(seg: float) -> str:
        seg = abs(seg)
        if seg < 90:
            return f"hace {int(seg)} s"
        if seg < 5400:
            return f"hace {int(seg // 60)} min"
        if seg < 172800:
            return f"hace {int(seg // 3600)} h"
        return f"hace {int(seg // 86400)} dias"

    detalle = (f"consola {hace(ahora - switch_en_hora_pc)}, "
               f"PC {hace(ahora - pc_newest)}")

    if abs(diff) <= TIE_SECONDS:
        return -1, f"empate tecnico ({detalle})"

    if diff > 0:
        return WINNER_SWITCH, f"se jugo despues en la consola ({detalle})"
    return WINNER_PC, f"se jugo despues en el PC ({detalle})"


def load_base_quiet(uid: str, title_id: int, root: Path,
                    hermanas: set[str] | None = None) -> dict[str, int]:
    """Como load_base pero sin escribir en el log; para el resumen."""
    p = state_path(uid, title_id)
    if not p.is_file():
        return {}
    try:
        data = json.loads(p.read_text())
    except (json.JSONDecodeError, OSError):
        return {}
    if not isinstance(data, dict):
        return {}
    anterior = data.get("root")
    if anterior != str(root) and not (hermanas and anterior in hermanas
                                      and str(root) in hermanas):
        return {}
    try:
        return {k: int(v) for k, v in data.get("files", {}).items()}
    except (ValueError, AttributeError):
        return {}


# --------------------------------------------------------------------------
# arranque
# --------------------------------------------------------------------------


class Runtime:
    """Estado vivo del daemon.

    Existe para que los ajustes se puedan cambiar desde la consola y surtan
    efecto sin reiniciar nada: el resolver consulta estos campos en cada uso en
    vez de quedar capturado en una clausura.
    """

    def __init__(self, args, cfg: dict):
        self.args = args
        self.cfg = cfg
        self.emus: list = []
        self.profile = args.profile or cfg.get("profile")
        self.disabled: set[str] = set(cfg.get("emuladores_desactivados", []))
        self.mirror = bool(cfg.get("mirror", True))
        # Mantener los emuladores iguales entre si, con o sin consola.
        self.sin_consola = bool(cfg.get("sin_consola", False))
        self.espejo: "Espejo | None" = None
        self.watcher: Watcher | None = None
        self.disc: Discovery | None = None
        # Callback opcional (tipo, mensaje) para interfaces como la bandeja de
        # Windows. El daemon en consola simplemente no lo usa.
        self.on_event = None
        self.stats = {"conexiones": 0, "archivos": 0, "ultima": None,
                      "ultima_juegos": 0}
        self.fallback = Path(args.fallback).expanduser().resolve()

        # Rutas que el usuario puede cambiar: copias, estado y donde buscar
        # emuladores. Vacio significa "la de por defecto".
        set_rutas(cfg.get("ruta_backups"), cfg.get("ruta_estado"))
        emulators.set_extra_roots(cfg.get("carpetas_emuladores", []))

        self.refresh_emulators()

    # -- emuladores ---------------------------------------------------------

    def refresh_emulators(self) -> int:
        self.emus = [] if self.args.dir else emulators.detect(profile=self.profile)
        return len(self.emus)

    def active_emus(self) -> list:
        return [e for e in self.emus if e.name not in self.disabled]

    @property
    def emu_name(self) -> str:
        if self.args.dir:
            return "carpeta"
        act = self.active_emus()
        if not act:
            return "ninguno"
        if len(act) == 1:
            return act[0].name
        return f"{act[0].name} +{len(act) - 1}"

    def describe(self) -> str:
        if self.args.dir:
            return f"carpeta simple {Path(self.args.dir).expanduser().resolve()}"
        act = self.active_emus()
        if not act:
            return f"carpeta de reserva {self.fallback}"
        return ", ".join(e.name for e in act)

    def _emu_for(self, emu, uid: str):
        """Aplica el perfil forzado (por consola o por configuracion)."""
        forced = self.cfg.get("profiles", {}).get(uid) or self.profile
        if forced and emu.kind == "yuzu":
            return emulators.Emulator(emu.name, emu.kind, emu.base, forced)
        return emu

    # -- resolucion de carpetas --------------------------------------------

    def targets(self, uid: str, title_id: int) -> list[tuple[str, Path]]:
        """Todas las carpetas donde vive ese juego, una por emulador activo."""
        if self.args.dir:
            root = Path(self.args.dir).expanduser().resolve()
            d = root / uid[:16] / f"{title_id:016x}" if uid else root / f"{title_id:016x}"
            d.mkdir(parents=True, exist_ok=True)
            return [("carpeta", d)]

        act = self.active_emus()
        if not act:
            d = self.fallback / f"{title_id:016x}"
            d.mkdir(parents=True, exist_ok=True)
            return [("reserva", d)]

        out: list[tuple[str, Path]] = []
        for emu in act:
            d = self._emu_for(emu, uid).save_dir(title_id)
            if d is None:
                # Ryujinx sin entrada en su indice: no se puede inventar.
                continue
            d.mkdir(parents=True, exist_ok=True)
            out.append((emu.name, d))
        return out

    def hermanas(self, uid: str, title_id: int) -> set[str]:
        """Las carpetas de este mismo juego en los demas emuladores.

        Sirve para que cambiar de emulador no se confunda con que la carpeta se
        haya movido de sitio.
        """
        try:
            return {str(p) for _, p in self.targets(uid, title_id)}
        except Exception:
            return set()

    def last_played(self, uid: str, title_id: int) -> int:
        """En que emulador se jugo por ultima vez, como indice de self.emus.

        Devuelve -1 si no hay partida en ninguno. Comparar fechas entre
        emuladores del mismo PC si es fiable: comparten reloj. El problema de
        los relojes es solo entre la consola y el PC.
        """
        if self.args.dir:
            return -1

        mejor, mejor_t = -1, None
        for emu in self.active_emus():
            try:
                d = self._emu_for(emu, uid).save_dir(title_id)
            except Exception:
                d = None
            if d is None or not d.is_dir():
                continue

            files = [p for p in d.rglob("*")
                     if p.is_file() and p.name not in IGNORED_FILES]
            if not files:
                continue

            t = max(p.stat().st_mtime for p in files)
            if mejor_t is None or t > mejor_t:
                mejor_t = t
                mejor = self.emus.index(emu)
        return mejor

    def resolve(self, uid: str, title_id: int) -> Path | None:
        """Carpeta que hace de 'lado PC' para este juego.

        Con varios emuladores hay que elegir uno contra el que comparar, y el
        criterio es el mas recientemente escrito de los que tengan partida: es
        donde has jugado la ultima vez. Entre emuladores del mismo PC comparar
        fechas si es fiable (mismo reloj); el problema de los relojes es solo
        entre la consola y el PC, y ahi se sigue comparando por CRC.
        """
        targets = self.targets(uid, title_id)
        if not targets:
            return None
        if len(targets) == 1:
            return targets[0][1]

        best_path, best_key = None, None
        for _, path in targets:
            files = [p for p in path.rglob("*") if p.is_file() and p.name not in IGNORED_FILES]
            if not files:
                continue
            key = max(p.stat().st_mtime for p in files)
            if best_key is None or key > best_key:
                best_key, best_path = key, path

        # Si ninguno tiene partida todavia, vale el primero.
        return best_path or targets[0][1]

    def _replica(self, origen: Path, destino: Path) -> int:
        """Deja `destino` con exactamente el contenido de `origen`."""
        want = scan_dir(origen)
        if scan_dir(destino) == want:
            return 0

        for rel in want:
            dst = destino / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(origen / rel, dst)
            invalidate_crc(dst)

        # Quitamos lo que sobre, para que la copia sea fiel.
        for path in sorted(destino.rglob("*"), reverse=True):
            if path.is_file() and path.name not in IGNORED_FILES:
                if path.relative_to(destino).as_posix() not in want:
                    path.unlink(missing_ok=True)
                    invalidate_crc(path)
            elif path.is_dir() and not any(path.iterdir()):
                path.rmdir()

        return len(want)

    def sync_shadows(self, uid: str, title_id: int) -> None:
        """Iguala las carpetas hermanas que use cada emulador.

        Ahora mismo solo Ryujinx las necesita: reparte cada partida entre una
        carpeta confirmada y otra de trabajo, y es la de trabajo la que lee el
        juego. Sin esto la sincronizacion llega pero el juego sigue viendo su
        partida vieja, que es justo lo que parecia un fallo del emulador.
        """
        if self.args.dir:
            return

        for emu in self.active_emus():
            real = self._emu_for(emu, uid)
            d = real.save_dir(title_id)
            if d is None or not d.is_dir():
                continue
            for hermana in real.shadow_dirs(d):
                n = self._replica(d, hermana)
                if n:
                    log(f"  {emu.name}: replicado en {hermana.name}/ ({n} archivo(s))")

    def sync_shadows_de(self, emu, title_id: int) -> None:
        """Iguala las carpetas hermanas de un emulador concreto.

        Ryujinx reparte cada partida entre una carpeta confirmada y otra de
        trabajo, y es la de trabajo la que lee el juego. Sin esto, en modo sin
        consola la copia llega pero el juego sigue viendo la partida vieja.
        """
        try:
            d = emu.save_dir(title_id)
            if d is None or not d.is_dir():
                return
            for hermana in emu.shadow_dirs(d):
                n = self._replica(d, hermana)
                if n:
                    log(f"  {emu.name}: replicado en {hermana.name}/ ({n} archivo(s))")
        except Exception:
            pass

    def mirror_to_others(self, uid: str, title_id: int, source: Path, name: str) -> None:
        """Replica el resultado en el resto de emuladores.

        Sin esto tendrias la partida al dia en uno y vieja en los otros, que es
        justo el lio que aparece en cuanto tienes eden y citron a la vez.
        """
        if self.mirror:
            for emu_name, dest in self.targets(uid, title_id):
                if dest == source:
                    continue
                n = self._replica(source, dest)
                if n:
                    log(f"  replicado en {emu_name}: {n} archivo(s)")

        # Aunque el replicado entre emuladores este apagado, las carpetas
        # hermanas de un mismo emulador tienen que quedar coherentes.
        self.sync_shadows(uid, title_id)

    # -- ajustes que se editan desde la consola ----------------------------

    def schema(self) -> list[dict]:
        items: list[dict] = []

        # Un interruptor por emulador detectado: con eden + citron + Ryujinx a la
        # vez, lo normal es querer elegir cuales entran.
        for e in self.emus:
            items.append(dict(key=f"emu:{e.name}", type=CFG_BOOL,
                              label=f"Sincronizar con {e.name}",
                              help=str(e.base),
                              value="0" if e.name in self.disabled else "1",
                              options=[]))

        if len(self.emus) > 1:
            items.append(dict(key="sin_consola", type=CFG_BOOL,
                              label="Modo sin consola",
                              help="Mantiene los emuladores iguales entre si sin "
                                   "esperar a la Switch",
                              value="1" if self.sin_consola else "0", options=[]))

            items.append(dict(key="mirror", type=CFG_BOOL,
                              label="Replicar entre emuladores",
                              help="Deja la misma partida en todos los activos",
                              value="1" if self.mirror else "0", options=[]))

        # El perfil solo tiene sentido preguntarlo si algun clon de yuzu declara
        # mas de uno; con uno solo no hay nada que elegir.
        for e in self.active_emus():
            if e.kind != "yuzu":
                continue
            profs = e.declared_profiles()
            if len(profs) <= 1:
                continue
            names = [f"{n} ({u[:8]}...)" for u, n in profs]
            cur = 0
            for i, (u, _) in enumerate(profs):
                if self.profile and u.lower() == self.profile.lower():
                    cur = i
            items.append(dict(key="profile", type=CFG_CHOICE,
                              label=f"Perfil de {e.name}",
                              help="Donde escribe el emulador las partidas",
                              value=str(cur), options=names))
            break

        items.append(dict(key="watch", type=CFG_BOOL, label="Vigilar el emulador",
                          help="Detecta cuando juegas en el PC",
                          value="1" if self.watcher else "0", options=[]))

        items.append(dict(key="watch_interval", type=CFG_INT,
                          label="Cada cuantos segundos revisa",
                          help="Entre 2 y 60",
                          value=str(self.cfg.get("watch_interval", 5)), options=[]))

        items.append(dict(key="nudge", type=CFG_BOOL,
                          label="Avisar a la consola de los cambios",
                          help="Para que los recoja al momento y no en su proximo repaso",
                          value="1" if self.cfg.get("nudge", True) else "0", options=[]))

        items.append(dict(key="discovery", type=CFG_BOOL,
                          label="Dejarse encontrar en la red",
                          help="Responder al broadcast de la consola",
                          value="1" if self.disc else "0", options=[]))

        items.append(dict(key="backup_keep", type=CFG_INT,
                          label="Copias de seguridad a guardar",
                          help="Por juego y perfil, entre 1 y 50",
                          value=str(self.cfg.get("backup_keep", BACKUP_KEEP)), options=[]))

        items.append(dict(key="saves_path", type=CFG_INFO, label="Carpeta de saves",
                          help="", value=self.describe(), options=[]))

        items.append(dict(key="ruta_backups", type=CFG_INFO,
                          label="Carpeta de copias de seguridad",
                          help="Se cambia desde la app del PC",
                          value=str(backups_dir()), options=[]))

        extras = emulators.extra_roots()
        items.append(dict(key="carpetas_extra", type=CFG_INFO,
                          label="Carpetas de emuladores anadidas",
                          help="Para emuladores fuera de las rutas habituales",
                          value=(", ".join(extras) if extras else "ninguna"),
                          options=[]))

        items.append(dict(key="rescan", type=CFG_ACTION, label="Volver a buscar emuladores",
                          help="Utiil si acabas de instalar uno",
                          value="", options=[]))

        return items

    def apply(self, key: str, value: str) -> str:
        """Aplica un ajuste. Devuelve un mensaje para mostrar en la consola."""
        global BACKUP_KEEP

        if key.startswith("emu:"):
            name = key[4:]
            if value in ("0", "", "false"):
                self.disabled.add(name)
            else:
                self.disabled.discard(name)
            self.cfg["emuladores_desactivados"] = sorted(self.disabled)
            act = self.active_emus()
            return f"{len(act)} emulador(es) activo(s)" if act else "Ninguno activo"

        if key == "sin_consola":
            want = value not in ("0", "", "false")
            self.sin_consola = want
            self.cfg["sin_consola"] = want
            if want and not self.espejo:
                self.espejo = Espejo(self)
                self.espejo.start()
            return ("Los emuladores se mantendran iguales entre si"
                    if want else "Modo sin consola desactivado")

        if key == "mirror":
            self.mirror = value not in ("0", "", "false")
            self.cfg["mirror"] = self.mirror
            return "Se replicara entre emuladores" if self.mirror else "Sin replicar"

        if key == "profile":
            yuzus = [e for e in self.active_emus() if e.kind == "yuzu"]
            profs = yuzus[0].declared_profiles() if yuzus else []
            idx = int(value)
            if 0 <= idx < len(profs):
                self.profile = profs[idx][0]
                self.cfg["profile"] = self.profile
                return f"Perfil: {profs[idx][1]}"
            return "Perfil fuera de rango"

        if key == "watch":
            want = value not in ("0", "", "false")
            if want and not self.watcher:
                self.watcher = Watcher(self.resolve, int(self.cfg.get("watch_interval", 5)),
                                       nudge=bool(self.cfg.get("nudge", True)))
                self.watcher.start()
            elif not want and self.watcher:
                self.watcher.stop_flag.set()
                self.watcher = None
            self.cfg["watch"] = want
            return "Vigilancia activada" if want else "Vigilancia desactivada"

        if key == "watch_interval":
            n = max(2, min(60, int(value)))
            self.cfg["watch_interval"] = n
            if self.watcher:
                self.watcher.interval = n
            return f"Revision cada {n} s"

        if key == "nudge":
            want = value not in ("0", "", "false")
            self.cfg["nudge"] = want
            if self.watcher:
                self.watcher.nudge = want
            return "Se avisara a la consola" if want else "Sin avisos a la consola"

        if key == "discovery":
            want = value not in ("0", "", "false")
            if want and not self.disc:
                self.disc = Discovery(self.args.port, lambda: self.emu_name)
                self.disc.start()
            elif not want and self.disc:
                self.disc.stop_flag.set()
                self.disc = None
            self.cfg["discovery"] = want
            return "Descubrimiento activado" if want else "Descubrimiento desactivado"

        if key == "backup_keep":
            n = max(1, min(50, int(value)))
            self.cfg["backup_keep"] = n
            BACKUP_KEEP = n
            return f"Se guardaran {n} copias"

        if key == "ruta_backups":
            self.cfg["ruta_backups"] = value or None
            set_rutas(backups=value)
            return f"Copias en {backups_dir()}"

        if key == "ruta_estado":
            self.cfg["ruta_estado"] = value or None
            set_rutas(estado=value)
            return f"Estado en {estado_dir()}"

        if key == "carpetas_emuladores":
            rutas = [r.strip() for r in value.split("|") if r.strip()]
            self.cfg["carpetas_emuladores"] = rutas
            emulators.set_extra_roots(rutas)
            n = self.refresh_emulators()
            return f"{len(rutas)} carpeta(s), {n} emulador(es) detectado(s)"

        if key == "rescan":
            n = self.refresh_emulators()
            return f"{n} emulador(es) detectado(s)" if n else "No se encontro ninguno"

        raise ClientError(f"ajuste desconocido: {key}")


def local_ips() -> list[str]:
    ips = []
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.168.1.1", 1))  # no manda nada, solo elige la ruta
        ips.append(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    return ips


def main(argv=None, stop_event=None, rt_hook=None) -> int:
    ap = argparse.ArgumentParser(description="Servidor de sincronizacion de saves de Switch")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--dir", help="usa esta carpeta en vez de un emulador")
    ap.add_argument("--fallback", default=str(_data_dir() / "saves"),
                    help="carpeta a usar si no se detecta ningun emulador")
    ap.add_argument("--profile", help="uuid del perfil de usuario del emulador a usar")
    ap.add_argument("--no-discovery", action="store_true",
                    help="no responder al descubrimiento por UDP")
    ap.add_argument("--no-watch", action="store_true",
                    help="no vigilar la carpeta del emulador")
    ap.add_argument("--list", action="store_true", help="muestra los emuladores detectados y sale")
    args = ap.parse_args(argv)

    cfg = load_config()

    if args.list:
        emus = emulators.detect(profile=args.profile)
        if not emus:
            print("No se detecto ningun emulador de Switch.")
            return 1
        for e in emus:
            print(f"{e.name:10} {e.base}")
            declarados = {u.lower(): n for u, n in e.declared_profiles()}
            for uuid, nombre in declarados.items():
                print(f"           perfil {uuid.upper()}  \"{nombre}\"")
            for p in e.profiles():
                juegos = sum(1 for d in p.iterdir() if d.is_dir())
                if p.name.lower() in declarados:
                    print(f"           carpeta {p.name}  {juegos} juego(s)  <- en uso")
                else:
                    print(f"           carpeta {p.name}  {juegos} juego(s)  "
                          f"<- NO declarada por el emulador, se ignora")
        return 0

    rt = Runtime(args, cfg)
    if rt_hook:
        rt_hook(rt)      # deja que la interfaz se enganche antes de arrancar
    STATE_DIR.mkdir(parents=True, exist_ok=True)

    global BACKUP_KEEP
    BACKUP_KEEP = int(cfg.get("backup_keep", BACKUP_KEEP))

    for e in rt.emus:
        estado = "desactivado" if e.name in rt.disabled else "activo"
        log(f"Emulador detectado: {e}  [{estado}]")
        for uuid, nombre in e.declared_profiles():
            log(f"  perfil: {nombre} ({uuid})")
    if not rt.emus and not args.dir:
        log(f"No se detecto ningun emulador; se usara {rt.fallback}")

    if not args.no_watch and cfg.get("watch", True):
        rt.watcher = Watcher(rt.resolve, cfg.get("watch_interval", 5),
                             nudge=cfg.get("nudge", True))
        rt.watcher.start()

    if rt.sin_consola:
        rt.espejo = Espejo(rt)
        rt.espejo.start()
        log("Modo sin consola: los emuladores se mantendran iguales entre si")

    if not args.no_discovery and cfg.get("discovery", True):
        # Se pasa la funcion, no el valor: asi el anuncio refleja los
        # emuladores de ahora y no los de cuando arranco el daemon.
        rt.disc = Discovery(args.port, lambda: rt.emu_name)
        rt.disc.start()

    where = rt.describe()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if sys.platform != "win32":
        # En Windows SO_REUSEADDR permite que OTRO proceso se quede con un
        # puerto ya en uso, justo lo contrario de lo que significa en Linux.
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.bind, args.port))
    srv.listen(1)

    log(f"Escuchando en {args.bind}:{args.port}  (saves: {where})")
    log(f"Estado en {estado_dir()}")
    log(f"Copias de seguridad en {backups_dir()}")
    for ip in local_ips():
        log(f"  IP de este PC: {ip}  (la consola lo encuentra sola)")

    try:
        # Con tiempo de espera para poder atender la orden de parar; sin el,
        # accept() se quedaria bloqueado para siempre y la app no cerraria.
        srv.settimeout(1.0)
        while not (stop_event and stop_event.is_set()):
            # De uno en uno a proposito: dos Switch escribiendo el mismo save a
            # la vez es justo lo que no queremos.
            try:
                sock, addr = srv.accept()
            except socket.timeout:
                continue
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            rt.stats["conexiones"] += 1
            if rt.on_event:
                rt.on_event("conectado", addr[0])
            log(f"--- conexion desde {addr[0]} ---")
            try:
                Session(Conn(sock), rt).serve()
            except ProtocolError as e:
                log(f"conexion terminada: {e}")
            except OSError as e:
                log(f"error de red: {e}")
            finally:
                sock.close()
                if rt.on_event:
                    rt.on_event("desconectado", "")
                log("--- conexion cerrada ---")
    except KeyboardInterrupt:
        log("Adios")
        return 0
    finally:
        if rt.watcher:
            rt.watcher.stop_flag.set()
        if rt.disc:
            rt.disc.stop_flag.set()
        if rt.espejo:
            rt.espejo.stop_flag.set()
        save_config(rt.cfg)
        srv.close()


if __name__ == "__main__":
    sys.exit(main())
