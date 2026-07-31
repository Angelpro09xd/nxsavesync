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

import struct
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
    return stem


def _search_roots(home: Path) -> list[Path]:
    """Sitios donde los emuladores de Switch suelen dejar sus datos en Linux."""
    roots = [
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

    return [r for r in roots if r.is_dir()]


def _scan(home: Path, marker: str, known: set[str]) -> list[tuple[str, Path]]:
    found: list[tuple[str, Path]] = []
    seen: set[Path] = set()

    for root in _search_roots(home):
        try:
            children = sorted(root.iterdir())
        except OSError:
            continue

        for base in children:
            if not base.is_dir() or base in seen:
                continue
            # El propio directorio, o un nivel mas abajo (instalaciones
            # portables tipo <carpeta>/Ryujinx/portable).
            for candidate in (base, base / "portable"):
                if (candidate / marker).is_dir() and candidate not in seen:
                    seen.add(candidate)
                    found.append((_pretty_name(base, known), candidate))

    return found


def _yuzu_bases(home: Path) -> list[tuple[str, Path]]:
    return _scan(home, YUZU_MARKER, KNOWN_YUZU)


def _ryujinx_bases(home: Path) -> list[tuple[str, Path]]:
    return _scan(home, RYUJINX_MARKER, KNOWN_RYUJINX)


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


def detect(home: Path | None = None, profile: str | None = None) -> list[Emulator]:
    """Busca emuladores instalados en las rutas habituales de Linux."""
    home = home or Path.home()

    emus = [Emulator(name, "yuzu", base, profile) for name, base in _yuzu_bases(home)]
    emus += [Emulator(name, "ryujinx", base) for name, base in _ryujinx_bases(home)]
    return emus
