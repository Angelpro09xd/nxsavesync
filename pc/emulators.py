"""Localiza la carpeta de savedata de un juego en los emuladores de Switch.

Cada emulador guarda las partidas con una estructura distinta:

- Familia yuzu (yuzu, suyu, citron, sudachi): la ruta se deriva directamente del
  title id, asi que basta con construirla.

      <base>/nand/user/save/0000000000000000/<user_uuid>/<title_id>/

- Ryujinx: la carpeta se llama con un *save data id* interno, no con el title id.
  La correspondencia vive en una base de datos clave-valor propia de Horizon
  (imkvdb.arc), que hay que parsear.

      <base>/bis/user/save/<save_data_id>/0/
"""

from __future__ import annotations

import os
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

# --------------------------------------------------------------------------
# familia yuzu
# --------------------------------------------------------------------------

# La familia de yuzu es enorme y no para de crecer (yuzu, suyu, citron, sudachi,
# eden...). En vez de mantener una lista que siempre va por detras, buscamos por
# la forma de la carpeta: cualquier directorio con "nand/user/save" dentro es un
# emulador de esta familia. La lista solo sirve para ponerle un nombre bonito.
YUZU_MARKER = "nand/user/save"
RYUJINX_MARKER = "bis/user/save"

KNOWN_YUZU = {"yuzu", "suyu", "citron", "sudachi", "eden", "torzu", "uzuy"}
KNOWN_RYUJINX = {"ryujinx", "ryubing", "kenji-nx"}

# Ficheros que solo escribe el emulador dentro de la carpeta de un save. Son la
# unica prueba fiable de que el emulador lee de esa carpeta concreta.
EMU_MARKERS = {".yuzu_save_size", ".ryujinx_save_size"}


def _pretty_name(path: Path, known: set[str]) -> str:
    """Nombre legible a partir del directorio de datos."""
    stem = path.name
    if stem.lower() in known:
        return stem

    # Rutas de flatpak tipo ~/.var/app/dev.eden_emu.eden/data/eden
    for part in reversed(path.parts):
        if part.lower() in known:
            return part

    # Carpetas como "citron-portable" o "Ryujinx-1.1": nos quedamos con el
    # nombre conocido que las encabeza, que es lo que espera ver el usuario.
    low = stem.lower()
    for name in known:
        if low.startswith(name):
            return name

    return stem


def _search_roots(home: Path, windows: bool | None = None) -> list[Path]:
    """Sitios donde los emuladores de Switch suelen dejar sus datos."""
    if windows is None:
        windows = sys.platform == "win32"

    roots: list[Path] = []

    if windows:
        # En Windows lo normal es %APPDATA%\<emulador>. Las instalaciones
        # portables suelen acabar en Descargas, Documentos o el Escritorio.
        appdata = os.environ.get("APPDATA")
        local = os.environ.get("LOCALAPPDATA")
        roots += [Path(appdata)] if appdata else [home / "AppData/Roaming"]
        roots += [Path(local)] if local else [home / "AppData/Local"]
        roots += [
            home / "Documents", home / "Documentos",
            home / "Downloads", home / "Descargas",
            home / "Desktop", home / "Escritorio",
            home / "Games", home / "Emulators", home / "Emuladores",
        ]
    else:
        roots += [
            home / ".local/share",
            home / ".config",
            home / "Applications",
            home / "Games",
            home / "Emuladores",
            home / "Emulators",
        ]

        # Flatpak: ~/.var/app/<id>/{data,config}
        var_app = home / ".var/app"
        if var_app.is_dir():
            for app in var_app.iterdir():
                roots.append(app / "data")
                roots.append(app / "config")

    # Carpetas extra que indique el usuario, por variable de entorno o desde la
    # configuracion. Util para emuladores en otra unidad (D:\Emus, /mnt/juegos).
    extra = os.environ.get("NXSAVESYNC_EMU_DIRS", "")
    if extra:
        roots += [Path(p) for p in extra.split(os.pathsep) if p]

    roots += [Path(p) for p in (_extra_roots or []) if p]

    return [r for r in roots if r.is_dir()]


# Carpetas extra fijadas desde la configuracion.
_extra_roots: list[str] = []


def set_extra_roots(rutas) -> None:
    global _extra_roots
    _extra_roots = [str(r) for r in (rutas or []) if str(r).strip()]


def extra_roots() -> list[str]:
    return list(_extra_roots)


def _scan(home: Path, marker: str, known: set[str],
          windows: bool | None = None) -> list[tuple[str, Path]]:
    found: list[tuple[str, Path]] = []
    seen: set[Path] = set()

    for root in _search_roots(home, windows):
        try:
            children = sorted(root.iterdir())
        except OSError:
            continue

        for base in children:
            if not base.is_dir() or base in seen:
                continue
            # El propio directorio, o un nivel mas abajo. Las instalaciones
            # portables usan <carpeta>/portable en Ryujinx y <carpeta>/user en
            # la familia de yuzu.
            for candidate in (base, base / "portable", base / "user"):
                if (candidate / marker).is_dir() and candidate not in seen:
                    seen.add(candidate)
                    found.append((_pretty_name(base, known), candidate))

    return found


def _yuzu_bases(home: Path, windows: bool | None = None) -> list[tuple[str, Path]]:
    return _scan(home, YUZU_MARKER, KNOWN_YUZU, windows)


def _ryujinx_bases(home: Path, windows: bool | None = None) -> list[tuple[str, Path]]:
    return _scan(home, RYUJINX_MARKER, KNOWN_RYUJINX, windows)


# --------------------------------------------------------------------------
# imkvdb.arc (Ryujinx)
# --------------------------------------------------------------------------


def parse_imkvdb(data: bytes) -> dict[int, int]:
    """Devuelve {application_id: save_data_id} leyendo un imkvdb.arc.

    Formato (KeyValueArchive de Horizon):

        cabecera: "IMKV" | u32 reservado | u32 numero_de_entradas
        entrada:  "IMEN" | u32 tam_clave | u32 tam_valor | clave | valor

    La clave es un SaveDataAttribute: application_id en el offset 0, user id en
    el 8 y el tipo en el 0x20. El valor es un SaveDataIndexerValue con el
    save_data_id en el offset 0.
    """
    out: dict[int, int] = {}

    if len(data) < 12 or data[:4] != b"IMKV":
        raise ValueError("no es un imkvdb.arc valido (falta la cabecera IMKV)")

    (count,) = struct.unpack_from("<I", data, 8)
    pos = 12

    for _ in range(count):
        if pos + 12 > len(data) or data[pos : pos + 4] != b"IMEN":
            break
        key_size, val_size = struct.unpack_from("<II", data, pos + 4)
        pos += 12

        if pos + key_size + val_size > len(data):
            break
        key = data[pos : pos + key_size]
        val = data[pos + key_size : pos + key_size + val_size]
        pos += key_size + val_size

        if key_size < 0x28 or val_size < 8:
            continue

        (application_id,) = struct.unpack_from("<Q", key, 0)
        save_type = key[0x20]
        (save_data_id,) = struct.unpack_from("<Q", val, 0)

        # Tipo 1 = SaveDataType.Account, que es el savedata de partida.
        if save_type == 1 and application_id != 0:
            out[application_id] = save_data_id

    return out


# --------------------------------------------------------------------------
# API
# --------------------------------------------------------------------------


def parse_profiles_dat(data: bytes) -> list[tuple[str, str]]:
    """Devuelve [(uuid, nombre)] leyendo el profiles.dat de la familia yuzu.

    Es la lista real de perfiles del emulador, asi que evita tener que adivinar
    a partir de las carpetas que haya por ahi. Importa porque algunos clones
    crean carpetas con el uuid mal formateado, y una carpeta suelta puede
    parecer un perfil sin serlo.

    Formato: 0x10 bytes de relleno y luego 8 entradas de 0xC8:
        uuid (0x10) | uuid repetido (0x10) | timestamp (8) | nombre (0x20) | extra (0x80)

    El uuid se guarda en little-endian respecto al nombre de la carpeta, por eso
    hay que invertir los bytes.
    """
    HEADER, ENTRY, MAX_USERS = 0x10, 0xC8, 8
    out: list[tuple[str, str]] = []

    for i in range(MAX_USERS):
        off = HEADER + i * ENTRY
        if off + ENTRY > len(data):
            break

        raw = data[off : off + 0x10]
        if not any(raw):
            continue

        uuid = raw[::-1].hex().upper()
        name = data[off + 0x28 : off + 0x48].split(b"\0")[0].decode("utf-8", "replace")
        out.append((uuid, name))

    return out


@dataclass
class Emulator:
    name: str
    kind: str  # "yuzu" o "ryujinx"
    base: Path
    profile: str | None = None  # uuid fijado a mano, si el usuario lo indica

    def __str__(self) -> str:
        return f"{self.name} ({self.base})"

    def save_dir(self, title_id: int) -> Path | None:
        """Carpeta de savedata del juego, o None si el emulador aun no la creo."""
        if self.kind == "yuzu":
            return self._yuzu_save_dir(title_id)
        return self._ryujinx_save_dir(title_id)

    # -- yuzu ---------------------------------------------------------------

    def _yuzu_save_dir(self, title_id: int) -> Path | None:
        root = self.base / "nand/user/save/0000000000000000"
        if not root.is_dir():
            return None

        users = [p for p in root.iterdir() if p.is_dir()]
        if not users:
            return None

        if self.profile:
            # Fijado a mano: manda, aunque la carpeta aun no exista.
            match = [u for u in users if u.name.lower() == self.profile.lower()]
            users = match or [root / self.profile]
        else:
            # El emulador dice cuales son sus perfiles de verdad. Cualquier otra
            # carpeta que haya suelta se descarta: hay clones que crean alguna
            # con el uuid mal formateado, y colarse ahi significa sincronizar
            # contra una carpeta que el emulador no lee nunca.
            declared = {uuid.lower() for uuid, _ in self.declared_profiles()}
            if declared:
                real = [u for u in users if u.name.lower() in declared]
                if real:
                    users = real
                else:
                    # Declarados pero sin carpeta todavia: la creamos nosotros.
                    users = [root / next(iter(sorted(declared))).upper()]

        # yuzu y sus clones formatean el title id en MAYUSCULAS ("{:016X}").
        # En Linux el sistema de archivos distingue mayusculas, asi que crear
        # la carpeta en minusculas hace que el emulador no la vea: arranca el
        # juego, no encuentra partida y crea una vacia al lado. Y con ids como
        # 0100152000022000, que son solo digitos, el error no se nota.
        prefer, other = f"{title_id:016X}", f"{title_id:016x}"

        # El uuid del perfil lo genera el emulador y no se puede derivar del
        # title id, asi que hay que elegir entre los que existan. Ojo: varios
        # emuladores (Eden entre ellos) crean ademas un perfil de ceros de
        # relleno, y llegan a crear la carpeta del juego en los dos. Quedarse
        # con el primero por orden alfabetico da justo el vacio.
        best = max(users, key=lambda u: self._profile_rank(u, title_id))

        # Si ya existen las dos grafias, manda la que usa el emulador: es la
        # que va a leer al arrancar el juego.
        existing = [best / v for v in (prefer, other) if (best / v).is_dir()]
        if existing:
            with_files = [d for d in existing
                          if any(p.is_file() for p in d.rglob("*"))]
            pool = with_files or existing
            for candidate in (best / prefer, best / other):
                if candidate in pool:
                    return candidate

        return best / prefer

    @staticmethod
    def _profile_rank(user_dir: Path, title_id: int) -> tuple[int, int, int, float]:
        """Ordena perfiles de mas a menos fiable para este juego.

        La senal buena es EMU_MARKERS: son ficheros que solo escribe el
        emulador, asi que su presencia demuestra que lee de ahi. Tener datos
        no basta, porque los datos los puede haber puesto cualquiera (nosotros,
        sin ir mas lejos) en una carpeta que el emulador no mira.
        """
        marker = 0
        has_files = 0
        has_dir = 0
        mtime = 0.0

        for candidate in _title_variants(title_id):
            d = user_dir / candidate
            if not d.is_dir():
                continue

            has_dir = 1
            try:
                mtime = max(mtime, d.stat().st_mtime)
                for p in d.rglob("*"):
                    if not p.is_file():
                        continue
                    if p.name in EMU_MARKERS:
                        marker = 1
                    else:
                        has_files = 1
            except OSError:
                pass

        # Un uuid todo ceros es el perfil de relleno, nunca el bueno.
        real_profile = 0 if set(user_dir.name) <= {"0"} else 1

        return (marker, has_files + real_profile, has_dir, mtime)

    def declared_profiles(self) -> list[tuple[str, str]]:
        """Perfiles que el emulador declara en su profiles.dat."""
        if self.kind != "yuzu":
            return []
        db = self.base / "nand/system/save/8000000000000010/su/avators/profiles.dat"
        if not db.is_file():
            return []
        try:
            return parse_profiles_dat(db.read_bytes())
        except (OSError, ValueError):
            return []

    def shadow_dirs(self, save_dir: Path) -> list[Path]:
        """Carpetas hermanas que deben acabar con el mismo contenido.

        Ryujinx guarda cada partida con el esquema de LibHac, que reparte los
        datos en dos carpetas:

            <save_data_id>/0/   confirmada  (lo que queda al cerrar el juego)
            <save_data_id>/1/   de trabajo  (lo que el juego lee y escribe)

        Escribir solo en la confirmada no basta: mientras exista la de trabajo,
        el juego sigue viendo su contenido viejo y parece que la sincronizacion
        no ha hecho nada. Hay que dejar las dos iguales.
        """
        if self.kind != "ryujinx":
            return []

        pareja = {"0": "1", "1": "0"}.get(save_dir.name)
        if not pareja:
            return []

        hermana = save_dir.parent / pareja
        return [hermana] if hermana.is_dir() else []

    def profiles(self) -> list[Path]:
        """Carpetas de perfil presentes en disco."""
        root = self.base / "nand/user/save/0000000000000000"
        return sorted(p for p in root.iterdir() if p.is_dir()) if root.is_dir() else []

    # -- Ryujinx ------------------------------------------------------------

    def _ryujinx_save_dir(self, title_id: int) -> Path | None:
        db = self.base / "bis/system/save/8000000000000000/0/imkvdb.arc"
        if not db.is_file():
            return None

        try:
            mapping = parse_imkvdb(db.read_bytes())
        except (ValueError, struct.error):
            return None

        save_data_id = mapping.get(title_id)
        if save_data_id is None:
            return None

        for candidate in (f"{save_data_id:016x}", f"{save_data_id:016X}"):
            d = self.base / "bis/user/save" / candidate / "0"
            if d.is_dir():
                return d

        return self.base / "bis/user/save" / f"{save_data_id:016x}" / "0"


def _title_variants(title_id: int) -> list[str]:
    return [f"{title_id:016x}", f"{title_id:016X}"]


def detect(home: Path | None = None, profile: str | None = None,
           windows: bool | None = None) -> list[Emulator]:
    """Busca emuladores instalados en las rutas habituales del sistema.

    `windows` fuerza el juego de rutas; sirve para las pruebas, que simulan una
    instalacion de Windows sin necesidad de un Windows.
    """
    home = home or Path.home()

    emus = [Emulator(name, "yuzu", base, profile)
            for name, base in _yuzu_bases(home, windows)]
    emus += [Emulator(name, "ryujinx", base)
             for name, base in _ryujinx_bases(home, windows)]
    return emus


# --------------------------------------------------------------------------
# clonar un perfil de la consola
# --------------------------------------------------------------------------
#
# Escribir profiles.dat es lo mas delicado que hace este programa en el PC: es
# la lista de cuentas del emulador, y dejarla mal significa que el emulador no
# arranca o pierde de vista sus partidas. Por eso:
#
#   - se reconstruye el archivo entero desde lo que ya habia, entrada a entrada,
#   - se respeta el tamano exacto del formato,
#   - y siempre se guarda una copia antes de tocar nada.

PROFILES_HEADER = 0x10
PROFILES_ENTRY  = 0xC8
PROFILES_MAX    = 8


def build_profiles_dat(entradas: list[tuple[str, str]], original: bytes = b"") -> bytes:
    """Arma un profiles.dat con `entradas` = [(uuid, nombre)].

    `original` sirve para conservar los bytes que no entendemos: la cabecera y
    el relleno de cada entrada. Reescribirlos a cero funciona en las pruebas
    pero es tentar a la suerte con un formato que no esta documentado.
    """
    total = PROFILES_HEADER + PROFILES_ENTRY * PROFILES_MAX
    out = bytearray(original[:total].ljust(total, b"\0"))

    for i in range(PROFILES_MAX):
        off = PROFILES_HEADER + i * PROFILES_ENTRY

        if i >= len(entradas):
            # Hueco libre: se limpia solo el uuid y el nombre, que es lo que
            # mira el emulador para saber si la entrada existe.
            out[off : off + 0x20] = b"\0" * 0x20
            out[off + 0x28 : off + 0x48] = b"\0" * 0x20
            continue

        uuid, nombre = entradas[i]
        raw = bytes.fromhex(uuid)[::-1]          # el uuid va al reves en disco
        if len(raw) != 0x10:
            raise ValueError(f"uuid de 16 bytes esperado, llego {len(raw)}")

        out[off : off + 0x10] = raw
        out[off + 0x10 : off + 0x20] = raw       # se repite, asi es el formato

        nb = nombre.encode("utf-8")[:0x1F]
        out[off + 0x28 : off + 0x48] = nb.ljust(0x20, b"\0")

    return bytes(out)


def clona_perfil(emu: "Emulator", uuid: str, nombre: str,
                 avatar: bytes | None) -> str:
    """Deja el perfil de la consola dentro del emulador.

    Devuelve un mensaje para ensenar en la consola. Lanza si no se puede.
    """
    if emu.kind != "yuzu":
        raise RuntimeError(
            f"{emu.name} no guarda los perfiles como la familia de yuzu; "
            f"todavia no se sabe escribir los suyos")

    carpeta = emu.base / "nand/system/save/8000000000000010/su/avators"
    carpeta.mkdir(parents=True, exist_ok=True)
    db = carpeta / "profiles.dat"

    original = db.read_bytes() if db.is_file() else b""
    actuales = parse_profiles_dat(original) if original else []

    uuid = uuid.upper()
    ya_estaba = any(u.upper() == uuid for u, _ in actuales)

    # Si ya existe, se le cambia el nombre; si no, se anade al final.
    nuevas: list[tuple[str, str]] = []
    for u, n in actuales:
        nuevas.append((uuid, nombre) if u.upper() == uuid else (u, n))
    if not ya_estaba:
        if len(nuevas) >= PROFILES_MAX:
            raise RuntimeError(f"{emu.name} ya tiene {PROFILES_MAX} perfiles, "
                               f"que es el maximo; borra alguno")
        nuevas.append((uuid, nombre))

    datos = build_profiles_dat(nuevas, original)

    # Se relee lo escrito antes de dar nada por bueno.
    if parse_profiles_dat(datos) != [(u.upper(), n) for u, n in nuevas]:
        raise RuntimeError("el profiles.dat resultante no se relee igual; "
                           "no se toca nada")

    if original:
        (carpeta / "profiles.dat.antes-de-nxsavesync").write_bytes(original)

    db.write_bytes(datos)

    if avatar:
        (carpeta / f"{uuid}.jpg").write_bytes(avatar)

    return (f"{nombre} {'actualizado' if ya_estaba else 'creado'} en {emu.name}"
            f"{' con su foto' if avatar else ''}")


def titulos(emu: "Emulator") -> dict[int, Path]:
    """Los juegos que tiene guardados ese emulador: title_id -> carpeta.

    Es el camino inverso a save_dir, y hace falta para el modo sin consola: sin
    una Switch que diga que juegos hay, la unica forma de saberlo es mirar lo
    que ya tiene cada emulador.
    """
    out: dict[int, Path] = {}

    if emu.kind == "ryujinx":
        # Ryujinx guarda la correspondencia en su indice; sin el no se puede
        # adivinar que carpeta es de que juego.
        for tid in _ryujinx_titles(emu):
            d = emu.save_dir(tid)
            if d is not None and d.is_dir():
                out[tid] = d
        return out

    for perfil in emu.profiles():
        if not perfil.is_dir():
            continue
        for d in perfil.iterdir():
            if not d.is_dir():
                continue
            try:
                tid = int(d.name, 16)
            except ValueError:
                continue
            if tid:
                out[tid] = d
    return out


def _ryujinx_titles(emu: "Emulator") -> list[int]:
    idx = emu.base / "bis/system/save/8000000000000000/0/imkvdb.arc"
    if not idx.is_file():
        idx = emu.base / "system/save/8000000000000000/0/imkvdb.arc"
    if not idx.is_file():
        return []
    try:
        return list(parse_imkvdb(idx.read_bytes()).keys())
    except Exception:
        return []
