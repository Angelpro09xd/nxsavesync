#!/usr/bin/env python3
"""Cliente simulado que habla el protocolo igual que hace el NRO, para
ejercitar el daemon sin necesidad de una Switch."""

import json
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

PC_DIR = Path(__file__).resolve().parent.parent / "pc"
sys.path.insert(0, str(PC_DIR))

OP = dict(HELLO=0x01, HELLO_OK=0x81, PLAN_REQ=0x02, PLAN_RES=0x82,
          PULL_REQ=0x03, PULL_RES=0x83, PUSH=0x04, PUSH_OK=0x84,
          DEL_REMOTE=0x05, DEL_OK=0x85, RESOLVE=0x06, RESOLVE_RES=0x86,
          COMMIT=0x07, COMMIT_OK=0x87, SUMMARY_REQ=0x08, SUMMARY_RES=0x88,
          DECIDE=0x09, DECIDE_RES=0x89, CFG_GET=0x0A, CFG_RES=0x8A,
          CFG_SET=0x0B, CFG_OK=0x8B, ERROR=0xFF)

WARN_NONE, WARN_PC_EMPTY, WARN_SWITCH_EMPTY, WARN_ROOT_CHANGED = 0, 1, 2, 3
DEC_SWITCH, DEC_PC, DEC_SKIP = 0, 1, 2

VERSION = 3
MODE_MANUAL, MODE_AUTO = 0, 1
POLICY_ASK, POLICY_SWITCH, POLICY_PC, POLICY_SKIP = 0, 1, 2, 3
SUM_SYNCED, SUM_PC_CHANGED, SUM_UNKNOWN, SUM_NO_DIR = 0, 1, 2, 3

# El uid del perfil viaja como dos u64 little-endian.
UID_HI, UID_LO = 0x1122334455667788, 0x99AABBCCDDEEFF00
UID = struct.pack("<QQ", UID_HI, UID_LO)
UID_HEX = f"{UID_HI:016X}{UID_LO:016X}"

ACT_PULL, ACT_PUSH, ACT_DEL_LOCAL, ACT_DEL_REMOTE, ACT_CONFLICT = range(5)
ACTN = {0: "PULL", 1: "PUSH", 2: "DEL_LOCAL", 3: "DEL_REMOTE", 4: "CONFLICT"}

PORT = 7899
TITLE = 0x0100000000010000

TMP = Path(tempfile.gettempdir()) / "nxsavesync-test-t"
SWITCH = TMP / "switch"
PC = TMP / "pc"
STATE = TMP / "state"


def w_str(s):
    b = s.encode()
    return struct.pack("<H", len(b)) + b


class Client:
    def __init__(self, host="127.0.0.1", port=PORT):
        self.s = socket.create_connection((host, port), timeout=10)
        self.rem = 0

    def _rx(self, n):
        buf = b""
        while len(buf) < n:
            c = self.s.recv(n - len(buf))
            if not c:
                raise RuntimeError("servidor cerro")
            buf += c
        return buf

    def send(self, op, payload=b""):
        self.s.sendall(bytes([op]) + struct.pack("<I", len(payload)) + payload)

    def send_stream(self, op, payload, extra):
        self.s.sendall(bytes([op]) + struct.pack("<I", len(payload) + extra) + payload)

    def hdr(self):
        h = self._rx(5)
        self.rem = struct.unpack_from("<I", h, 1)[0]
        return h[0]

    def take(self, n):
        self.rem -= n
        return self._rx(n)

    def body(self):
        return self.take(self.rem)


def manifest(root):
    out = {}
    for p in sorted(root.rglob("*")):
        if p.is_file():
            out[p.relative_to(root).as_posix()] = zlib.crc32(p.read_bytes())
    return out


def enc_manifest(m, root):
    b = struct.pack("<I", len(m))
    for path, crc in m.items():
        b += w_str(path) + struct.pack("<Q", (root / path).stat().st_size) + struct.pack("<I", crc)
    return b


def parse_warning(data):
    """Devuelve (codigo, mensaje, resto) del cuerpo de un PLAN_RES."""
    code = data[0]
    (ln,) = struct.unpack_from("<H", data, 1)
    msg = data[3:3 + ln].decode()
    return code, msg, data[3 + ln:]


def parse_plan(data):
    (n,) = struct.unpack_from("<I", data, 0)
    pos = 4
    plan = []
    for _ in range(n):
        action = data[pos]
        (ln,) = struct.unpack_from("<H", data, pos + 1)
        path = data[pos + 3: pos + 3 + ln].decode()
        pos += 3 + ln
        plan.append((action, path))
    return plan


def sync(conflict_winner=None, expect_error=False, mode=MODE_MANUAL,
         policy=POLICY_ASK, decision=None):
    """Una sesion completa de sincronizacion de un juego."""
    c = Client()
    c.send(OP["HELLO"], struct.pack("<I", VERSION) + w_str("test") + w_str("dev"))
    assert c.hdr() == OP["HELLO_OK"], "handshake fallo"
    c.body()

    m = manifest(SWITCH)
    c.send(OP["PLAN_REQ"], UID + w_str("Angel") + struct.pack("<Q", TITLE)
           + w_str("Juego De Prueba") + bytes([mode, policy]) + enc_manifest(m, SWITCH))

    op = c.hdr()
    if expect_error:
        assert op == OP["ERROR"], f"esperaba ERROR, llego 0x{op:02X}"
        return None
    assert op == OP["PLAN_RES"], f"esperaba PLAN_RES, llego 0x{op:02X}"
    warn_code, warn_msg, rest = parse_warning(c.body())
    plan = parse_plan(rest)

    if warn_code != WARN_NONE:
        if decision is None:
            c.s.close()
            return ("AVISO", warn_code, warn_msg)
        c.send(OP["DECIDE"], UID + struct.pack("<Q", TITLE) + bytes([decision]))
        assert c.hdr() == OP["DECIDE_RES"]
        plan = parse_plan(c.body())

    if any(a == ACT_CONFLICT for a, _ in plan):
        assert conflict_winner is not None, f"conflicto inesperado: {plan}"
        c.send(OP["RESOLVE"], UID + struct.pack("<Q", TITLE) + bytes([conflict_winner]))
        assert c.hdr() == OP["RESOLVE_RES"]
        plan = parse_plan(c.body())

    for action, path in plan:
        if action == ACT_PULL:
            c.send(OP["PULL_REQ"], UID + struct.pack("<Q", TITLE) + w_str(path))
            assert c.hdr() == OP["PULL_RES"]
            (size,) = struct.unpack("<Q", c.take(8))
            data = c.take(size) if size else b""
            dest = SWITCH / path
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
        elif action == ACT_PUSH:
            data = (SWITCH / path).read_bytes()
            c.send_stream(OP["PUSH"], UID + struct.pack("<Q", TITLE) + w_str(path)
                          + struct.pack("<Q", len(data)), len(data))
            c.s.sendall(data)
            assert c.hdr() == OP["PUSH_OK"]
            c.body()
        elif action == ACT_DEL_LOCAL:
            (SWITCH / path).unlink(missing_ok=True)
        elif action == ACT_DEL_REMOTE:
            c.send(OP["DEL_REMOTE"], UID + struct.pack("<Q", TITLE) + w_str(path))
            assert c.hdr() == OP["DEL_OK"]
            c.body()

    final = manifest(SWITCH)
    c.send(OP["COMMIT"], UID + struct.pack("<Q", TITLE) + enc_manifest(final, SWITCH))
    assert c.hdr() == OP["COMMIT_OK"]
    c.body()
    c.s.close()
    return [(ACTN[a], p) for a, p in plan]


def check(label, cond):
    print(f"  {'OK  ' if cond else 'FALLO'} {label}")
    if not cond:
        raise SystemExit(1)


def main():
    shutil.rmtree(TMP, ignore_errors=True)
    (SWITCH / "system").mkdir(parents=True)
    PC.mkdir(parents=True)

    env = {"HOME": str(TMP), "XDG_DATA_HOME": str(STATE),
           "XDG_CONFIG_HOME": str(TMP / ".config"), "PATH": "/usr/bin:/bin"}
    srv = subprocess.Popen(
        [sys.executable, "nxsavesyncd.py", "--port", str(PORT), "--dir", str(PC)],
        cwd=str(PC_DIR), env=env,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(1.5)
    if srv.poll() is not None:
        print(srv.stdout.read())
        raise SystemExit("el daemon no arranco")

    pcdir = PC / UID_HEX[:16] / f"{TITLE:016x}"

    try:
        print("\n1) Primera sync: la Switch tiene datos, el PC no")
        (SWITCH / "system/data_001.sav").write_bytes(b"partida inicial")
        (SWITCH / "config.bin").write_bytes(b"ajustes")
        plan = sync()
        check("plan = 2 PUSH", sorted(plan) == [("PUSH", "config.bin"), ("PUSH", "system/data_001.sav")])
        check("el PC recibio los archivos", manifest(pcdir) == manifest(SWITCH))

        print("\n2) Nada cambia")
        check("plan vacio", sync() == [])

        print("\n3) Cambia solo el PC (como si hubieras jugado en el emulador)")
        (pcdir / "system/data_001.sav").write_bytes(b"avance en el emulador")
        plan = sync()
        check("plan = 1 PULL", plan == [("PULL", "system/data_001.sav")])
        check("la Switch se actualizo",
              (SWITCH / "system/data_001.sav").read_bytes() == b"avance en el emulador")

        print("\n4) Cambia solo la Switch")
        (SWITCH / "system/data_001.sav").write_bytes(b"avance en la switch")
        plan = sync()
        check("plan = 1 PUSH", plan == [("PUSH", "system/data_001.sav")])
        check("el PC se actualizo",
              (pcdir / "system/data_001.sav").read_bytes() == b"avance en la switch")

        print("\n5) Archivo nuevo solo en el PC")
        (pcdir / "extra.dat").write_bytes(b"nuevo")
        plan = sync()
        check("plan = 1 PULL", plan == [("PULL", "extra.dat")])
        check("llego a la Switch", (SWITCH / "extra.dat").read_bytes() == b"nuevo")

        print("\n6) Borrado en el PC -> se borra en la Switch")
        (pcdir / "extra.dat").unlink()
        plan = sync()
        check("plan = 1 DEL_LOCAL", plan == [("DEL_LOCAL", "extra.dat")])
        check("desaparecio de la Switch", not (SWITCH / "extra.dat").exists())

        print("\n7) Borrado en la Switch -> se borra en el PC")
        (SWITCH / "config.bin").unlink()
        plan = sync()
        check("plan = 1 DEL_REMOTE", plan == [("DEL_REMOTE", "config.bin")])
        check("desaparecio del PC", not (pcdir / "config.bin").exists())

        print("\n8) Conflicto: los dos lados cambian -> gana la Switch")
        (SWITCH / "system/data_001.sav").write_bytes(b"version switch")
        (pcdir / "system/data_001.sav").write_bytes(b"version pc")
        plan = sync(conflict_winner=0)
        check("resuelto como PUSH", plan == [("PUSH", "system/data_001.sav")])
        check("el PC tiene la version de la Switch",
              (pcdir / "system/data_001.sav").read_bytes() == b"version switch")

        print("\n9) Conflicto -> gana el PC")
        (SWITCH / "system/data_001.sav").write_bytes(b"switch otra vez")
        (pcdir / "system/data_001.sav").write_bytes(b"pc otra vez")
        plan = sync(conflict_winner=1)
        check("resuelto como PULL", plan == [("PULL", "system/data_001.sav")])
        check("la Switch tiene la version del PC",
              (SWITCH / "system/data_001.sav").read_bytes() == b"pc otra vez")

        print("\n10) Archivo grande (3 MB, varios chunks)")
        big = bytes(range(256)) * 12000
        (SWITCH / "big.bin").write_bytes(big)
        plan = sync()
        check("plan = 1 PUSH", plan == [("PUSH", "big.bin")])
        check("llego intacto", (pcdir / "big.bin").read_bytes() == big)
        (pcdir / "big.bin").write_bytes(big[::-1])
        sync()
        check("vuelve intacto", (SWITCH / "big.bin").read_bytes() == big[::-1])

        print("\n11) La carpeta del PC se vacia -> avisa en vez de borrar")
        antes = manifest(SWITCH)
        shutil.rmtree(pcdir)
        pcdir.mkdir(parents=True)

        res = sync()   # sin decision: debe devolver el aviso
        check("devuelve un aviso, no un error", isinstance(res, tuple) and res[0] == "AVISO")
        check("el aviso es 'el PC esta vacio'", res[1] == WARN_PC_EMPTY)
        check("el mensaje explica que pasa", "vacia" in res[2] and str(len(antes)) in res[2])
        check("sin decidir, no se toca la Switch", manifest(SWITCH) == antes)

        # Esta es la salida que faltaba: regenerar en el PC desde la consola.
        plan = sync(decision=DEC_SWITCH)
        check("al elegir 'manda la Switch' se resube todo",
              plan and all(a == "PUSH" for a, _ in plan))
        check("el PC recupera los archivos", manifest(pcdir) == antes)

        print("\n11b) La Switch se queda vacia -> tambien avisa, y se puede recuperar")
        guardado = manifest(pcdir)
        for p in list(SWITCH.rglob("*")):
            if p.is_file():
                p.unlink()
        res = sync()
        check("aviso 'la consola esta vacia'",
              isinstance(res, tuple) and res[1] == WARN_SWITCH_EMPTY)
        check("el PC no se toca sin decidir", manifest(pcdir) == guardado)

        plan = sync(decision=DEC_PC)
        check("al elegir 'manda el PC' se regenera en la consola",
              plan and all(a == "PULL" for a, _ in plan))
        check("la Switch recupera sus archivos", manifest(SWITCH) == guardado)

        print("\n11c) 'No tocar nada' deja las dos partes intactas")
        shutil.rmtree(pcdir)
        pcdir.mkdir(parents=True)
        antes_sw = manifest(SWITCH)
        res = sync(decision=DEC_SKIP)
        check("plan vacio", res == [])
        check("la Switch queda igual", manifest(SWITCH) == antes_sw)
        check("el PC sigue vacio", manifest(pcdir) == {})
        sync(decision=DEC_SWITCH)   # lo dejamos coherente

        print("\n11d) Cambia la carpeta de destino (como al instalar un emulador)")
        # La base antigua describe otra carpeta. Si se aplicara, un save nuevo
        # en el destino nuevo pareceria "cambio del PC" y machacaria la Switch.
        st_file = STATE / f"nxsavesync/state/{UID_HEX}/{TITLE:016x}.json"
        data = json.loads(st_file.read_text())
        check("el estado recuerda su carpeta", "root" in data and "files" in data)
        data["root"] = "/otra/carpeta/de/antes"
        st_file.write_text(json.dumps(data))

        antes_sw = manifest(SWITCH)
        (pcdir / "system/data_001.sav").write_bytes(b"save recien creado por el emulador")

        res = sync()
        check("avisa del cambio de carpeta",
              isinstance(res, tuple) and res[1] == WARN_ROOT_CHANGED)
        check("no machaca nada mientras tanto", manifest(SWITCH) == antes_sw)

        plan = sync(decision=DEC_SWITCH)
        check("al decidir, manda la consola",
              plan and all(a == "PUSH" for a, _ in plan))
        check("la Switch conserva su partida", manifest(SWITCH) == antes_sw)

        print("\n11b) Rutas maliciosas")
        c = Client()
        c.send(OP["HELLO"], struct.pack("<I", VERSION) + w_str("test") + w_str("dev"))
        c.hdr(); c.body()
        evil = struct.pack("<I", 1) + w_str("../../../../etc/pwned") + struct.pack("<Q", 3) + struct.pack("<I", 0)
        c.send(OP["PLAN_REQ"], UID + w_str("Angel") + struct.pack("<Q", TITLE)
                           + w_str("Malo") + bytes([MODE_MANUAL, POLICY_ASK]) + evil)
        check("el servidor rechaza '..'", c.hdr() == OP["ERROR"])
        c.body()
        c.s.close()

        print("\n12) Se hicieron copias de seguridad")
        backups = list((STATE / "nxsavesync/backups").rglob("*.zip"))
        check(f"hay {len(backups)} copia(s)", len(backups) > 0)

        print("\n13) Version de protocolo incompatible")
        c = Client()
        c.send(OP["HELLO"], struct.pack("<I", 99) + w_str("viejo") + w_str("dev"))
        check("rechazada", c.hdr() == OP["ERROR"])
        c.s.close()

        print("\n14) Modo automatico: resuelve sin preguntar")
        (SWITCH / "system/data_001.sav").write_bytes(b"auto switch")
        (pcdir / "system/data_001.sav").write_bytes(b"auto pc")
        plan = sync(mode=MODE_AUTO, policy=POLICY_SWITCH)
        check("no devuelve CONFLICT", all(a != "CONFLICT" for a, _ in plan))
        check("aplica gana-la-Switch", plan == [("PUSH", "system/data_001.sav")])
        check("el PC recibio la version de la Switch",
              (pcdir / "system/data_001.sav").read_bytes() == b"auto switch")

        (SWITCH / "system/data_001.sav").write_bytes(b"auto switch 2")
        (pcdir / "system/data_001.sav").write_bytes(b"auto pc 2")
        plan = sync(mode=MODE_AUTO, policy=POLICY_PC)
        check("aplica gana-el-PC", plan == [("PULL", "system/data_001.sav")])

        print("\n15) Politica 'no tocar nada' deja el conflicto pendiente")
        antes_sw = manifest(SWITCH)
        antes_pc = manifest(pcdir)
        (SWITCH / "system/data_001.sav").write_bytes(b"intacto switch")
        (pcdir / "system/data_001.sav").write_bytes(b"intacto pc")
        esperado_sw = manifest(SWITCH)
        esperado_pc = manifest(pcdir)
        plan = sync(mode=MODE_AUTO, policy=POLICY_SKIP)
        check("plan vacio", plan == [])
        check("la Switch queda igual", manifest(SWITCH) == esperado_sw)
        check("el PC queda igual", manifest(pcdir) == esperado_pc)
        # lo dejamos coherente para las pruebas siguientes
        sync(mode=MODE_AUTO, policy=POLICY_SWITCH)

        print("\n16) Resumen de estados sin sincronizar nada")
        def summary(ids):
            c = Client()
            c.send(OP["HELLO"], struct.pack("<I", VERSION) + w_str("test") + w_str("dev"))
            c.hdr(); c.body()
            body = UID + struct.pack("<I", len(ids))
            for t in ids:
                body += struct.pack("<Q", t)
            c.send(OP["SUMMARY_REQ"], body)
            assert c.hdr() == OP["SUMMARY_RES"]
            data = c.body()
            (n,) = struct.unpack_from("<I", data, 0)
            out = {}
            for i in range(n):
                tid, st = struct.unpack_from("<QB", data, 4 + i * 9)
                out[tid] = st
            c.s.close()
            return out

        st = summary([TITLE, 0x0100AAAABBBB0000])
        check("el juego sincronizado sale al dia", st[TITLE] == SUM_SYNCED)
        check("un juego desconocido sale sin carpeta",
              st[0x0100AAAABBBB0000] in (SUM_NO_DIR, SUM_UNKNOWN))

        (pcdir / "system/data_001.sav").write_bytes(b"tocado por el emulador")
        st = summary([TITLE])
        check("tras tocar el PC sale 'cambios en el PC'", st[TITLE] == SUM_PC_CHANGED)
        sync(mode=MODE_AUTO, policy=POLICY_SWITCH)

        print("\n17) Dos perfiles no se pisan")
        OTHER = struct.pack("<QQ", 0xAAAA, 0xBBBB)
        other_hex = f"{0xAAAA:016X}{0xBBBB:016X}"
        c = Client()
        c.send(OP["HELLO"], struct.pack("<I", VERSION) + w_str("test") + w_str("dev"))
        c.hdr(); c.body()
        # El otro perfil no tiene nada: su plan debe salir vacio, no heredar
        # el estado ni los archivos del primero.
        c.send(OP["PLAN_REQ"], OTHER + w_str("Otro") + struct.pack("<Q", TITLE)
               + w_str("Juego De Prueba") + bytes([MODE_MANUAL, POLICY_ASK])
               + struct.pack("<I", 0))
        assert c.hdr() == OP["PLAN_RES"]
        otro_plan = parse_plan(c.body())
        c.s.close()
        check("el segundo perfil parte de cero", otro_plan == [])
        check("cada perfil tiene su propio estado",
              (STATE / f"nxsavesync/state/{UID_HEX}").is_dir()
              and not (STATE / f"nxsavesync/state/{other_hex}/{TITLE:016x}.json").exists())
        check("y su propia carpeta de saves",
              (PC / UID_HEX[:16]).is_dir() and not (PC / other_hex[:16] / f"{TITLE:016x}"
                                                    ).joinpath("system").exists())

        print("\n18) Descubrimiento por UDP")
        us = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        us.settimeout(3)
        us.sendto(b"NXSS?" + struct.pack("<I", VERSION), ("127.0.0.1", 7879))
        try:
            data, _ = us.recvfrom(512)
            ok_disc = data.startswith(b"NXSS!")
            (ver,) = struct.unpack_from("<I", data, 5)
            (port,) = struct.unpack_from("<H", data, 9)
            pos = 11
            (ln,) = struct.unpack_from("<H", data, pos)
            host = data[pos + 2: pos + 2 + ln].decode()
        except socket.timeout:
            ok_disc, ver, port, host = False, 0, 0, ""
        us.close()
        check("el daemon responde a la sonda", ok_disc)
        check(f"version correcta ({ver})", ver == VERSION)
        check(f"anuncia su puerto y nombre ({port}, {host!r})", port == PORT and bool(host))

        print("\n19) Version antigua del protocolo se ignora en descubrimiento")
        us = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        us.settimeout(1.5)
        us.sendto(b"NXSS?" + struct.pack("<I", 1), ("127.0.0.1", 7879))
        try:
            us.recvfrom(512)
            respondio = True
        except socket.timeout:
            respondio = False
        us.close()
        check("no contesta a v1", not respondio)

        print("\n20) Ajustes del daemon leidos y cambiados desde la consola")
        def cfg_get():
            c = Client()
            c.send(OP["HELLO"], struct.pack("<I", VERSION) + w_str("test") + w_str("dev"))
            c.hdr(); c.body()
            c.send(OP["CFG_GET"])
            assert c.hdr() == OP["CFG_RES"]
            data = c.body(); c.s.close()

            (n,) = struct.unpack_from("<I", data, 0)
            pos = 4
            items = []
            def rstr(p):
                (ln,) = struct.unpack_from("<H", data, p)
                return data[p + 2:p + 2 + ln].decode(), p + 2 + ln
            for _ in range(n):
                key, pos = rstr(pos)
                typ = data[pos]; pos += 1
                label, pos = rstr(pos)
                help_, pos = rstr(pos)
                value, pos = rstr(pos)
                (nopt,) = struct.unpack_from("<I", data, pos); pos += 4
                opts = []
                for _ in range(nopt):
                    o, pos = rstr(pos)
                    opts.append(o)
                items.append(dict(key=key, type=typ, label=label, value=value, options=opts))
            return items

        def cfg_set(key, value):
            c = Client()
            c.send(OP["HELLO"], struct.pack("<I", VERSION) + w_str("test") + w_str("dev"))
            c.hdr(); c.body()
            c.send(OP["CFG_SET"], w_str(key) + w_str(value))
            op = c.hdr(); data = c.body(); c.s.close()
            (ln,) = struct.unpack_from("<H", data, 4 if op == OP["ERROR"] else 0)
            off = 6 if op == OP["ERROR"] else 2
            return op == OP["CFG_OK"], data[off:off + ln].decode()

        items = cfg_get()
        keys = {i["key"] for i in items}
        check(f"el PC expone sus ajustes ({len(items)})", len(items) > 0)
        check("incluye vigilancia y copias",
              "watch" in keys and "backup_keep" in keys)
        check("incluye la carpeta de saves como informacion",
              any(i["key"] == "saves_path" and i["type"] == 3 for i in items))

        ok, msg = cfg_set("backup_keep", "7")
        check(f"cambia un entero ({msg})", ok and "7" in msg)
        check("y se ve reflejado",
              next(i["value"] for i in cfg_get() if i["key"] == "backup_keep") == "7")

        ok, msg = cfg_set("watch", "0")
        check(f"apaga la vigilancia ({msg})", ok)
        check("queda apagada",
              next(i["value"] for i in cfg_get() if i["key"] == "watch") == "0")
        cfg_set("watch", "1")

        ok, msg = cfg_set("inventado", "1")
        check("rechaza un ajuste desconocido", not ok and "desconocido" in msg)

        check("el cambio se guardo en disco",
              json.loads((TMP / ".config/nxsavesync/config.json").read_text())
              .get("backup_keep") == 7)

        print("\n21) El PC trae cambios: se puede quedar cualquiera de los dos lados")
        # Estado de partida limpio y conocido para las dos pruebas.
        (SWITCH / "system/data_001.sav").write_bytes(b"base comun")
        sync(mode=MODE_AUTO, policy=POLICY_SWITCH)

        # a) Cambia solo el PC y la consola acepta lo del PC (el plan tal cual).
        (pcdir / "system/data_001.sav").write_bytes(b"jugado en el emulador")
        plan = sync()
        check("el plan propone bajarlo", plan == [("PULL", "system/data_001.sav")])
        check("la Switch tiene lo del PC",
              (SWITCH / "system/data_001.sav").read_bytes() == b"jugado en el emulador")

        # b) Cambia solo el PC pero se decide quedarse con lo de la consola.
        #    Es el caso nuevo: sin recalcular ignorando la base, el archivo de la
        #    Switch se leeria como "sin cambios" y no se subiria nada.
        (SWITCH / "system/data_001.sav").write_bytes(b"lo que quiero conservar")
        sync(mode=MODE_AUTO, policy=POLICY_SWITCH)          # deja base coherente
        (pcdir / "system/data_001.sav").write_bytes(b"el emulador lo piso")

        c = Client()
        c.send(OP["HELLO"], struct.pack("<I", VERSION) + w_str("test") + w_str("dev"))
        c.hdr(); c.body()
        m = manifest(SWITCH)
        c.send(OP["PLAN_REQ"], UID + w_str("Angel") + struct.pack("<Q", TITLE)
               + w_str("Juego De Prueba") + bytes([MODE_MANUAL, POLICY_ASK])
               + enc_manifest(m, SWITCH))
        assert c.hdr() == OP["PLAN_RES"]
        _, _, rest = parse_warning(c.body())
        propuesto = parse_plan(rest)
        check("el PC propone bajar", [ACTN[a] for a, _ in propuesto] == ["PULL"])

        # La consola dice "me quedo con lo mio": DECIDE con DEC_SWITCH.
        c.send(OP["DECIDE"], UID + struct.pack("<Q", TITLE) + bytes([DEC_SWITCH]))
        assert c.hdr() == OP["DECIDE_RES"]
        invertido = parse_plan(c.body())
        check("se da la vuelta al plan", [ACTN[a] for a, _ in invertido] == ["PUSH"])

        for action, path in invertido:
            data = (SWITCH / path).read_bytes()
            c.send_stream(OP["PUSH"], UID + struct.pack("<Q", TITLE) + w_str(path)
                          + struct.pack("<Q", len(data)), len(data))
            c.s.sendall(data)
            assert c.hdr() == OP["PUSH_OK"]
            c.body()
        final = manifest(SWITCH)
        c.send(OP["COMMIT"], UID + struct.pack("<Q", TITLE) + enc_manifest(final, SWITCH))
        assert c.hdr() == OP["COMMIT_OK"]
        c.body(); c.s.close()

        check("la Switch conserva lo suyo",
              (SWITCH / "system/data_001.sav").read_bytes() == b"lo que quiero conservar")
        check("y el PC recibe esa version",
              (pcdir / "system/data_001.sav").read_bytes() == b"lo que quiero conservar")
        check("los dos lados quedan iguales", manifest(pcdir) == manifest(SWITCH))

        print("\nTODO OK")
    finally:
        srv.terminate()
        try:
            out = srv.communicate(timeout=5)[0]
        except subprocess.TimeoutExpired:
            srv.kill()
            out = ""
        print("\n--- log del daemon ---")
        print(out)


main()
