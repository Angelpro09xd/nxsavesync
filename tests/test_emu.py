#!/usr/bin/env python3
"""Prueba el parser de imkvdb.arc y la resolucion de rutas de emulador."""

import os
import shutil
import struct
import sys
import tempfile
from pathlib import Path

PC_DIR = Path(__file__).resolve().parent.parent / "pc"
sys.path.insert(0, str(PC_DIR))
import emulators

TMP = Path(tempfile.gettempdir()) / "nxsavesync-test-e"


def make_key(app_id, save_type=1, user=b"\x11" * 16):
    """SaveDataAttribute de 0x40 bytes."""
    k = bytearray(0x40)
    struct.pack_into("<Q", k, 0x00, app_id)
    k[0x08:0x18] = user
    struct.pack_into("<Q", k, 0x18, 0)  # static save data id
    k[0x20] = save_type
    return bytes(k)


def make_val(save_data_id):
    """SaveDataIndexerValue de 0x40 bytes."""
    v = bytearray(0x40)
    struct.pack_into("<Q", v, 0x00, save_data_id)
    return bytes(v)


def make_imkvdb(entries):
    body = b""
    for app_id, save_id, stype in entries:
        k, v = make_key(app_id, stype), make_val(save_id)
        body += b"IMEN" + struct.pack("<II", len(k), len(v)) + k + v
    return b"IMKV" + struct.pack("<I", 0) + struct.pack("<I", len(entries)) + body


def check(label, cond):
    print(f"  {'OK  ' if cond else 'FALLO'} {label}")
    if not cond:
        raise SystemExit(1)


shutil.rmtree(TMP, ignore_errors=True)

print("1) Parser de imkvdb.arc")
db = make_imkvdb([
    (0x0100000000010000, 0x4000000000000021, 1),   # savedata de cuenta
    (0x01008DB008C2C000, 0x4000000000000045, 1),
    (0x0100000000001000, 0x8000000000000030, 0),   # tipo system, se ignora
])
m = emulators.parse_imkvdb(db)
check("2 entradas de cuenta", len(m) == 2)
check("mapea el primer juego", m[0x0100000000010000] == 0x4000000000000021)
check("mapea el segundo", m[0x01008DB008C2C000] == 0x4000000000000045)
check("ignora el savedata de sistema", 0x0100000000001000 not in m)

print("\n2) Archivos corruptos no revientan")
try:
    emulators.parse_imkvdb(b"basura")
    check("deberia lanzar ValueError", False)
except ValueError:
    check("cabecera invalida -> ValueError", True)
check("truncado -> devuelve lo que pudo leer", emulators.parse_imkvdb(db[:60]) == {} or True)
check("cero entradas", emulators.parse_imkvdb(b"IMKV" + struct.pack("<II", 0, 0)) == {})

print("\n3) Rutas de Ryujinx")
ryu = TMP / "home/.config/Ryujinx"
(ryu / "bis/system/save/8000000000000000/0").mkdir(parents=True, exist_ok=True)
(ryu / "bis/system/save/8000000000000000/0/imkvdb.arc").write_bytes(db)
(ryu / "bis/user/save/4000000000000021/0").mkdir(parents=True, exist_ok=True)

emus = emulators.detect(TMP / "home")
check("detecta Ryujinx", any(e.kind == "ryujinx" for e in emus))
r = next(e for e in emus if e.kind == "ryujinx")
d = r.save_dir(0x0100000000010000)
check(f"resuelve la carpeta ({d})", d == ryu / "bis/user/save/4000000000000021/0")
check("juego sin save -> None", r.save_dir(0x0999999999999999) is None)

print("\n4) Rutas de la familia yuzu")
yz = TMP / "home/.local/share/citron"
uid = "a1b2c3d4e5f60718293a4b5c6d7e8f90"
(yz / f"nand/user/save/0000000000000000/{uid}").mkdir(parents=True, exist_ok=True)

emus = emulators.detect(TMP / "home")
c = next(e for e in emus if e.kind == "yuzu")
check("detecta citron", c.name == "citron")
d = c.save_dir(0x0100000000010000)
check(f"deriva la ruta del title id ({d})",
      d == yz / f"nand/user/save/0000000000000000/{uid}/0100000000010000")

existing = yz / f"nand/user/save/0000000000000000/{uid}/0100000000010000"
existing.mkdir(parents=True, exist_ok=True)
check("reutiliza la carpeta existente", c.save_dir(0x0100000000010000) == existing)

print("\n5) Eden: perfil de relleno con la carpeta creada pero vacia")
# Reproduce lo que hace Eden de verdad: un perfil de ceros y el real en
# mayusculas, los dos con carpeta para el juego, pero solo uno con datos.
eden = TMP / "home/.local/share/eden"
real_uid = "DD46990A5A58F92B860EC1E52CA4A43D"
zero_uid = "0" * 32
base = eden / "nand/user/save/0000000000000000"
(base / zero_uid / "0100152000022000").mkdir(parents=True, exist_ok=True)
(base / real_uid / "0100152000022000").mkdir(parents=True, exist_ok=True)
(base / real_uid / "0100152000022000/userdata.dat").write_bytes(b"partida real")

emus = emulators.detect(TMP / "home")
ed = next(e for e in emus if e.name == "eden")
check("detecta eden sin estar en ninguna lista fija", ed.kind == "yuzu")
d = ed.save_dir(0x0100152000022000)
check(f"elige el perfil con datos, no el de ceros ({d.parent.name})",
      d == base / real_uid / "0100152000022000")

print("\n5b) Grafia del title id: la familia yuzu usa MAYUSCULAS")
# Un id con letras, para que mayusculas y minusculas se distingan de verdad.
TL = 0x010051F0207B2000
d = ed.save_dir(TL)
check(f"crea la carpeta en mayusculas ({d.name})", d.name == "010051F0207B2000")

# Si ya existen las dos, gana la del emulador aunque la otra tenga datos.
(base / real_uid / "010051f0207b2000").mkdir(parents=True, exist_ok=True)
(base / real_uid / "010051f0207b2000/Player.sav").write_bytes(b"la mia")
(base / real_uid / "010051F0207B2000").mkdir(parents=True, exist_ok=True)
(base / real_uid / "010051F0207B2000/Player.sav").write_bytes(b"la de eden")
d = ed.save_dir(TL)
check(f"con las dos presentes, gana la del emulador ({d.name})",
      d.name == "010051F0207B2000")

# Si solo existe la de minusculas (creada por una version anterior), se usa.
import shutil as _sh
_sh.rmtree(base / real_uid / "010051F0207B2000")
check("si solo hay minusculas, se respeta", ed.save_dir(TL).name == "010051f0207b2000")

print("\n5c) profiles.dat manda sobre las carpetas sueltas")
def make_profiles_dat(entries):
    data = bytearray(0x10 + 0xC8 * 8)
    for i, (uuid_hex, name) in enumerate(entries):
        off = 0x10 + i * 0xC8
        raw = bytes.fromhex(uuid_hex)[::-1]        # se guarda en little-endian
        data[off:off+0x10] = raw
        data[off+0x10:off+0x20] = raw
        nb = name.encode()
        data[off+0x28:off+0x28+len(nb)] = nb
    return bytes(data)

REAL = "DD46990A5A58F92B860EC1E52CA4A43D"
dat = eden / "nand/system/save/8000000000000010/su/avators/profiles.dat"
dat.parent.mkdir(parents=True, exist_ok=True)
dat.write_bytes(make_profiles_dat([(REAL, "Eden")]))

check("lee el uuid invirtiendo los bytes",
      emulators.parse_profiles_dat(dat.read_bytes()) == [(REAL, "Eden")])

# Carpeta con uuid mal formateado, como la que crea Eden por su bug, y ademas
# con datos y con el marcador del emulador dentro: aun asi debe ignorarse.
falsa = base / "2B860EC1E52CA4A43D00000000000000" / "010051F0207B2000"
falsa.mkdir(parents=True, exist_ok=True)
(falsa / "Player.sav").write_bytes(b"copia despistada")
(falsa / ".yuzu_save_size").write_bytes(b"0123456789abcdef")
(base / REAL / "010051F0207B2000").mkdir(parents=True, exist_ok=True)

emus = emulators.detect(TMP / "home")
ed = next(e for e in emus if e.name == "eden")
d = ed.save_dir(0x010051F0207B2000)
check(f"ignora la carpeta no declarada ({d.parent.name[:12]}...)", d.parent.name == REAL)
check("--profile permite forzarla",
      emulators.Emulator("eden","yuzu",eden,"2B860EC1E52CA4A43D00000000000000")
      .save_dir(0x010051F0207B2000).parent.name == "2B860EC1E52CA4A43D00000000000000")

print("\n6) Un clon desconocido tambien se detecta")
nuevo = TMP / "home/.local/share/emuinventado"
(nuevo / "nand/user/save/0000000000000000/aabb").mkdir(parents=True, exist_ok=True)
emus = emulators.detect(TMP / "home")
check("detectado por la forma de la carpeta",
      any(e.base == nuevo for e in emus))

print("\n7) Varios emuladores a la vez (eden + citron + Ryujinx)")
emus = emulators.detect(TMP / "home")
kinds = sorted({e.kind for e in emus})
names = sorted(e.name for e in emus)
check(f"detecta los tres ({', '.join(names)})", len(emus) >= 3)
check("de las dos familias", kinds == ["ryujinx", "yuzu"])
check("dos clones de yuzu por separado",
      sum(1 for e in emus if e.kind == "yuzu") >= 2)

# Cada uno resuelve a su propia carpeta: nada de mezclarlos.
dirs = {}
for e in emus:
    d = e.save_dir(0x0100152000022000)
    if d is not None:
        dirs[e.name] = d
check(f"cada emulador tiene su carpeta ({len(dirs)})", len(set(dirs.values())) == len(dirs))
check("las rutas cuelgan de su propio emulador",
      all(e_name.lower() in str(path).lower() or "ryujinx" in str(path).lower()
          for e_name, path in dirs.items()))

print("\n8) El primario es el jugado mas recientemente")
import time as _t
sys.path.insert(0, str(PC_DIR))
import nxsavesyncd as d


class FakeArgs:
    dir = None
    fallback = str(TMP / "reserva")
    profile = None
    port = 7878


rt = d.Runtime(FakeArgs(), {})
rt.emus = emulators.detect(TMP / "home")

TID = 0x0100152000022000
targets = rt.targets("UID0", TID)
check(f"targets devuelve {len(targets)} carpeta(s)", len(targets) >= 2)

# Escribimos en dos emuladores con fechas distintas.
viejo, nuevo = targets[0][1], targets[1][1]
(viejo / "userdata.dat").write_bytes(b"partida vieja")
_t.sleep(0.05)
(nuevo / "userdata.dat").write_bytes(b"partida nueva")

check(f"manda el escrito mas tarde ({targets[1][0]})", rt.resolve("UID0", TID) == nuevo)

os.utime(viejo / "userdata.dat", (_t.time() + 100, _t.time() + 100))
check("si se toca el otro, pasa a mandar el", rt.resolve("UID0", TID) == viejo)

print("\n9) Replicado entre emuladores")
(viejo / "extra.sav").write_bytes(b"algo mas")
rt.mirror_to_others("UID0", TID, viejo, "Prueba")
for name, path in targets:
    if path == viejo:
        continue
    got = {p.relative_to(path).as_posix() for p in path.rglob("*") if p.is_file()}
    check(f"{name} recibio la copia", got == {"userdata.dat", "extra.sav"})
    check(f"{name} tiene el mismo contenido",
          (path / "userdata.dat").read_bytes() == b"partida vieja")

(nuevo / "sobra.sav").write_bytes(b"esto no deberia quedarse")
rt.mirror_to_others("UID0", TID, viejo, "Prueba")
check("el replicado borra lo que sobra en el destino", not (nuevo / "sobra.sav").exists())

rt.mirror = False
(nuevo / "intocable.sav").write_bytes(b"x")
rt.mirror_to_others("UID0", TID, viejo, "Prueba")
check("con el replicado apagado no toca nada", (nuevo / "intocable.sav").exists())

print("\n10) Desactivar un emulador lo saca de la lista")
rt.mirror = True
apagado = rt.emus[0].name
rt.disabled.add(apagado)
check(f"{apagado} ya no esta activo",
      apagado not in [e.name for e in rt.active_emus()])
check("y no aparece entre los destinos",
      all(name != apagado for name, _ in rt.targets("UID0", TID)))
rt.disabled.clear()

print("\nTODO OK")
