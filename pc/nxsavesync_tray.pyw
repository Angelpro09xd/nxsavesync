#!/usr/bin/env python3
"""NX Save Sync para Windows: el daemon con un icono en la bandeja del sistema.

Se queda junto al reloj, sin ventana. Click derecho para las opciones, doble
click para ver el registro.

Hecho con ctypes y la API de Windows directamente, sin dependencias: asi basta
con tener Python instalado y hacer doble click, sin pasar por pip.

    pythonw nxsavesync_tray.pyw
"""

from __future__ import annotations

import ctypes
import os
import subprocess
import sys
import threading
import traceback
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import nxsavesyncd as daemon   # noqa: E402

APP_NAME = "NX Save Sync"
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
RUN_VALUE = "NXSaveSync"

# --------------------------------------------------------------------------
# constantes de la API de Windows
# --------------------------------------------------------------------------

WM_DESTROY = 0x0002
WM_COMMAND = 0x0111
WM_APP = 0x8000
WM_TRAY = WM_APP + 1          # mensaje propio para los avisos del icono

WM_LBUTTONUP = 0x0202
WM_RBUTTONUP = 0x0205
WM_LBUTTONDBLCLK = 0x0203
NIN_BALLOONUSERCLICK = 0x0405   # se pincho el globo

NIM_ADD, NIM_MODIFY, NIM_DELETE = 0, 1, 2
NIF_MESSAGE, NIF_ICON, NIF_TIP, NIF_INFO = 0x01, 0x02, 0x04, 0x10
NIIF_INFO, NIIF_WARNING, NIIF_ERROR = 0x01, 0x02, 0x03

IMAGE_ICON = 1
LR_LOADFROMFILE, LR_DEFAULTSIZE, LR_SHARED = 0x0010, 0x0040, 0x8000

MF_STRING, MF_SEPARATOR, MF_POPUP = 0x0000, 0x0800, 0x0010
MF_CHECKED, MF_GRAYED = 0x0008, 0x0001
TPM_RIGHTBUTTON = 0x0002

IDI_APPLICATION = 32512

# Todo lo de Win32 se prepara solo en Windows: en otros sistemas ni ctypes.WinDLL
# ni WINFUNCTYPE existen, y el modulo ni siquiera se podria importar.
IS_WIN = sys.platform == "win32"

if IS_WIN:
    from ctypes import wintypes

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    shell32 = ctypes.WinDLL("shell32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

    LRESULT = ctypes.c_ssize_t
    WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wintypes.HWND, wintypes.UINT,
                                 wintypes.WPARAM, wintypes.LPARAM)
else:
    user32 = shell32 = kernel32 = None
    LRESULT = ctypes.c_ssize_t
    WNDPROC = None


if IS_WIN:
    class WNDCLASS(ctypes.Structure):
        _fields_ = [
            ("style", wintypes.UINT),
            ("lpfnWndProc", WNDPROC),
            ("cbClsExtra", ctypes.c_int),
            ("cbWndExtra", ctypes.c_int),
            ("hInstance", wintypes.HINSTANCE),
            ("hIcon", wintypes.HICON),
            ("hCursor", wintypes.HANDLE),
            ("hbrBackground", wintypes.HBRUSH),
            ("lpszMenuName", wintypes.LPCWSTR),
            ("lpszClassName", wintypes.LPCWSTR),
        ]


    class NOTIFYICONDATA(ctypes.Structure):
        _fields_ = [
            ("cbSize", wintypes.DWORD),
            ("hWnd", wintypes.HWND),
            ("uID", wintypes.UINT),
            ("uFlags", wintypes.UINT),
            ("uCallbackMessage", wintypes.UINT),
            ("hIcon", wintypes.HICON),
            ("szTip", wintypes.WCHAR * 128),
            ("dwState", wintypes.DWORD),
            ("dwStateMask", wintypes.DWORD),
            ("szInfo", wintypes.WCHAR * 256),
            ("uTimeoutOrVersion", wintypes.UINT),
            ("szInfoTitle", wintypes.WCHAR * 64),
            ("dwInfoFlags", wintypes.DWORD),
            ("guidItem", ctypes.c_byte * 16),
            ("hBalloonIcon", wintypes.HICON),
        ]


    class POINT(ctypes.Structure):
        _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


def _declara_prototipos() -> None:
    """Fija los tipos de cada funcion de Win32 que se usa.

    Sin esto ctypes asume que todo es int de 32 bits, y en Windows de 64 los
    handles (HWND, HMENU, HICON...) se truncan a la mitad. Es un fallo que no
    da error de compilacion: simplemente la aplicacion se cae o no pinta nada.
    """
    HMENU, HICON, HWND = wintypes.HMENU, wintypes.HICON, wintypes.HWND
    UINT_PTR = ctypes.c_size_t

    user32.CreatePopupMenu.restype = HMENU
    user32.CreatePopupMenu.argtypes = []

    user32.AppendMenuW.restype = wintypes.BOOL
    user32.AppendMenuW.argtypes = [HMENU, wintypes.UINT, UINT_PTR, wintypes.LPCWSTR]

    user32.TrackPopupMenu.restype = wintypes.BOOL
    user32.TrackPopupMenu.argtypes = [HMENU, wintypes.UINT, ctypes.c_int, ctypes.c_int,
                                      ctypes.c_int, HWND, ctypes.c_void_p]

    user32.DestroyMenu.restype = wintypes.BOOL
    user32.DestroyMenu.argtypes = [HMENU]

    user32.LoadImageW.restype = wintypes.HANDLE
    user32.LoadImageW.argtypes = [wintypes.HINSTANCE, wintypes.LPCWSTR, wintypes.UINT,
                                  ctypes.c_int, ctypes.c_int, wintypes.UINT]

    user32.LoadIconW.restype = HICON
    user32.LoadIconW.argtypes = [wintypes.HINSTANCE, wintypes.LPCWSTR]

    user32.CreateWindowExW.restype = HWND
    user32.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR,
                                       wintypes.DWORD, ctypes.c_int, ctypes.c_int,
                                       ctypes.c_int, ctypes.c_int, HWND, HMENU,
                                       wintypes.HINSTANCE, ctypes.c_void_p]

    user32.RegisterClassW.restype = wintypes.ATOM
    user32.RegisterClassW.argtypes = [ctypes.POINTER(WNDCLASS)]

    user32.DefWindowProcW.restype = LRESULT
    user32.DefWindowProcW.argtypes = [HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]

    user32.GetMessageW.restype = wintypes.BOOL
    user32.GetMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), HWND,
                                   wintypes.UINT, wintypes.UINT]

    user32.DispatchMessageW.restype = LRESULT
    user32.DispatchMessageW.argtypes = [ctypes.POINTER(wintypes.MSG)]

    user32.TranslateMessage.argtypes = [ctypes.POINTER(wintypes.MSG)]
    user32.SetForegroundWindow.argtypes = [HWND]
    user32.DestroyWindow.argtypes = [HWND]
    user32.PostMessageW.argtypes = [HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.GetCursorPos.argtypes = [ctypes.POINTER(POINT)]
    user32.PostQuitMessage.argtypes = [ctypes.c_int]

    kernel32.GetModuleHandleW.restype = wintypes.HMODULE
    kernel32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]

    kernel32.CreateMutexW.restype = wintypes.HANDLE
    kernel32.CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR]

    kernel32.ReleaseMutex.argtypes = [wintypes.HANDLE]

    shell32.Shell_NotifyIconW.restype = wintypes.BOOL
    shell32.Shell_NotifyIconW.argtypes = [wintypes.DWORD, ctypes.POINTER(NOTIFYICONDATA)]


# --------------------------------------------------------------------------
# identificadores del menu
# --------------------------------------------------------------------------

ID_ESTADO = 1000
ID_LOG = 1001
ID_BACKUPS = 1002
ID_RESCAN = 1003
ID_AUTOSTART = 1004
ID_SALIR = 1005
ID_MIRROR = 1006
ID_EMU_BASE = 1100          # 1100 + n para cada emulador


def resume_tanda(nombres: list[str], archivos: int = 0,
                 limite: int = 200) -> tuple[str, str]:
    """Titulo y texto del aviso a partir de los juegos sincronizados.

    Aparte de la clase y sin tocar Win32 para poder probarlo en cualquier
    sistema: el globo de Windows corta a 255 caracteres y conviene asegurarse
    de que el recorte cae bien.
    """
    n = len(nombres)
    titulo = "1 partida sincronizada" if n == 1 else f"{n} partidas sincronizadas"
    if archivos:
        titulo += f"  ({archivos} archivo" + ("" if archivos == 1 else "s") + ")"

    texto, mostrados = "", 0
    for nombre in nombres:
        trozo = (", " if texto else "") + nombre
        if len(texto) + len(trozo) > limite:
            break
        texto += trozo
        mostrados += 1

    if mostrados < n:
        texto += f"  y {n - mostrados} mas"

    return titulo, texto


class Tray:
    def __init__(self):
        self.hwnd = None
        self.hicon = None
        self.rt = None
        self.stop = threading.Event()
        self.lineas: list[str] = []
        self.estado = "arrancando"
        self.log_path = daemon.STATE_DIR / "nxsavesync.log"

        # Lo sincronizado en la conexion actual. Se avisa al final y de una vez:
        # un globo por juego serian once seguidos y no se lee ninguno.
        self.tanda: list[str] = []
        self.tanda_archivos = 0

    # -- registro -----------------------------------------------------------

    def anota(self, linea: str) -> None:
        self.lineas.append(linea)
        if len(self.lineas) > 500:
            del self.lineas[:250]
        try:
            self.log_path.parent.mkdir(parents=True, exist_ok=True)
            with self.log_path.open("a", encoding="utf-8") as f:
                f.write(linea + "\n")
        except OSError:
            pass

    # -- eventos del daemon -------------------------------------------------

    def evento(self, tipo: str, dato) -> None:
        if tipo == "conectado":
            self.estado = f"sincronizando con {dato}"
            self.tanda = []
            self.tanda_archivos = 0
            self.actualiza_tip()

        elif tipo == "sync":
            # Solo se apunta lo que movio algo; un juego ya al dia no es noticia.
            if isinstance(dato, dict):
                if dato.get("cambios", 0) > 0:
                    self.tanda.append(dato.get("nombre") or "?")
                    self.tanda_archivos += dato["cambios"]
            elif dato:
                self.tanda.append(str(dato))

        elif tipo == "desconectado":
            self.estado = "esperando"
            self.avisa_tanda()
            self.actualiza_tip()

    def avisa_tanda(self) -> None:
        """Un aviso con los juegos sincronizados en esta conexion."""
        if not self.tanda:
            return

        titulo, texto = resume_tanda(self.tanda, self.tanda_archivos)

        if self.rt:
            self.rt.stats["ultima_juegos"] = len(self.tanda)

        self.globo(titulo, texto, NIIF_INFO)
        self.tanda = []
        self.tanda_archivos = 0

    # -- icono de la bandeja -------------------------------------------------

    def carga_icono(self) -> int:
        ico = Path(__file__).resolve().parent / "nxsavesync.ico"
        if ico.is_file():
            h = user32.LoadImageW(None, str(ico), IMAGE_ICON, 0, 0,
                                  LR_LOADFROMFILE | LR_DEFAULTSIZE)
            if h:
                return h
        # Sin el .ico al lado seguimos funcionando, con el icono generico. El id
        # numerico se pasa como puntero (MAKEINTRESOURCE), no como cadena.
        return user32.LoadIconW(None,
                                ctypes.cast(ctypes.c_void_p(IDI_APPLICATION),
                                            wintypes.LPCWSTR))

    def _nid(self, flags: int) -> NOTIFYICONDATA:
        nid = NOTIFYICONDATA()
        nid.cbSize = ctypes.sizeof(NOTIFYICONDATA)
        nid.hWnd = self.hwnd
        nid.uID = 1
        nid.uFlags = flags
        return nid

    def añade_icono(self) -> None:
        nid = self._nid(NIF_MESSAGE | NIF_ICON | NIF_TIP)
        nid.uCallbackMessage = WM_TRAY
        nid.hIcon = self.hicon
        nid.szTip = f"{APP_NAME} — {self.estado}"[:127]
        shell32.Shell_NotifyIconW(NIM_ADD, ctypes.byref(nid))

    def actualiza_tip(self) -> None:
        if not self.hwnd:
            return
        nid = self._nid(NIF_TIP)
        nid.szTip = f"{APP_NAME} — {self.estado}"[:127]
        shell32.Shell_NotifyIconW(NIM_MODIFY, ctypes.byref(nid))

    def globo(self, titulo: str, texto: str, icono: int = NIIF_INFO) -> None:
        if not self.hwnd:
            return
        nid = self._nid(NIF_INFO)
        nid.szInfoTitle = titulo[:63]
        nid.szInfo = texto[:255]
        nid.dwInfoFlags = icono
        shell32.Shell_NotifyIconW(NIM_MODIFY, ctypes.byref(nid))

    def quita_icono(self) -> None:
        if not self.hwnd:
            return
        nid = self._nid(0)
        shell32.Shell_NotifyIconW(NIM_DELETE, ctypes.byref(nid))

    # -- inicio automatico ---------------------------------------------------

    def autoarranque_activo(self) -> bool:
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as k:
                winreg.QueryValueEx(k, RUN_VALUE)
            return True
        except OSError:
            return False

    def alterna_autoarranque(self) -> None:
        import winreg
        if self.autoarranque_activo():
            try:
                with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0,
                                    winreg.KEY_SET_VALUE) as k:
                    winreg.DeleteValue(k, RUN_VALUE)
                self.globo(APP_NAME, "Ya no arrancara con Windows")
            except OSError as e:
                self.globo(APP_NAME, f"No se pudo quitar: {e}", NIIF_ERROR)
            return

        # pythonw evita que aparezca una ventana de consola al arrancar.
        pyw = Path(sys.executable).with_name("pythonw.exe")
        exe = str(pyw if pyw.is_file() else Path(sys.executable))
        cmd = f'"{exe}" "{Path(__file__).resolve()}"'
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0,
                                winreg.KEY_SET_VALUE) as k:
                winreg.SetValueEx(k, RUN_VALUE, 0, winreg.REG_SZ, cmd)
            self.globo(APP_NAME, "Arrancara con Windows")
        except OSError as e:
            self.globo(APP_NAME, f"No se pudo activar: {e}", NIIF_ERROR)

    # -- menu ---------------------------------------------------------------

    def menu(self) -> None:
        hmenu = user32.CreatePopupMenu()

        ultima = self.rt.stats.get("ultima") if self.rt else None
        cabecera = f"{APP_NAME} — {self.estado}"
        if ultima:
            cabecera += f"   (ultima: {ultima:%H:%M}"
            juegos = self.rt.stats.get("ultima_juegos", 0)
            if juegos:
                cabecera += f", {juegos} juego" + ("" if juegos == 1 else "s")
            cabecera += ")"
        user32.AppendMenuW(hmenu, MF_STRING | MF_GRAYED, ID_ESTADO, cabecera)
        user32.AppendMenuW(hmenu, MF_SEPARATOR, 0, None)

        # Un submenu con los emuladores detectados y su interruptor.
        if self.rt and self.rt.emus:
            sub = user32.CreatePopupMenu()
            for i, e in enumerate(self.rt.emus):
                marca = MF_CHECKED if e.name not in self.rt.disabled else 0
                user32.AppendMenuW(sub, MF_STRING | marca, ID_EMU_BASE + i, e.name)
            if len(self.rt.emus) > 1:
                user32.AppendMenuW(sub, MF_SEPARATOR, 0, None)
                marca = MF_CHECKED if self.rt.mirror else 0
                user32.AppendMenuW(sub, MF_STRING | marca, ID_MIRROR,
                                   "Replicar entre emuladores")
            user32.AppendMenuW(hmenu, MF_POPUP, sub, "Emuladores")
        else:
            user32.AppendMenuW(hmenu, MF_STRING | MF_GRAYED, 0,
                               "Sin emuladores detectados")

        user32.AppendMenuW(hmenu, MF_STRING, ID_RESCAN, "Volver a buscar emuladores")
        user32.AppendMenuW(hmenu, MF_SEPARATOR, 0, None)
        user32.AppendMenuW(hmenu, MF_STRING, ID_BACKUPS, "Abrir copias de seguridad")
        user32.AppendMenuW(hmenu, MF_STRING, ID_LOG, "Ver registro")
        user32.AppendMenuW(hmenu, MF_SEPARATOR, 0, None)

        marca = MF_CHECKED if self.autoarranque_activo() else 0
        user32.AppendMenuW(hmenu, MF_STRING | marca, ID_AUTOSTART, "Iniciar con Windows")
        user32.AppendMenuW(hmenu, MF_SEPARATOR, 0, None)
        user32.AppendMenuW(hmenu, MF_STRING, ID_SALIR, "Salir")

        pt = POINT()
        user32.GetCursorPos(ctypes.byref(pt))
        # Sin esto el menu se queda abierto al hacer click fuera; es un requisito
        # documentado de TrackPopupMenu.
        user32.SetForegroundWindow(self.hwnd)
        user32.TrackPopupMenu(hmenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, self.hwnd, None)
        user32.PostMessageW(self.hwnd, 0, 0, 0)
        user32.DestroyMenu(hmenu)

    def comando(self, ident: int) -> None:
        if ident == ID_SALIR:
            user32.DestroyWindow(self.hwnd)

        elif ident == ID_LOG:
            self.abre(self.log_path)

        elif ident == ID_BACKUPS:
            self.abre(daemon.STATE_DIR / "backups")

        elif ident == ID_AUTOSTART:
            self.alterna_autoarranque()

        elif ident == ID_RESCAN and self.rt:
            n = self.rt.refresh_emulators()
            self.globo(APP_NAME, f"{n} emulador(es) detectado(s)")

        elif ident == ID_MIRROR and self.rt:
            self.rt.mirror = not self.rt.mirror
            self.rt.cfg["mirror"] = self.rt.mirror
            daemon.save_config(self.rt.cfg)

        elif ID_EMU_BASE <= ident < ID_EMU_BASE + 64 and self.rt:
            i = ident - ID_EMU_BASE
            if i < len(self.rt.emus):
                nombre = self.rt.emus[i].name
                if nombre in self.rt.disabled:
                    self.rt.disabled.discard(nombre)
                else:
                    self.rt.disabled.add(nombre)
                self.rt.cfg["emuladores_desactivados"] = sorted(self.rt.disabled)
                daemon.save_config(self.rt.cfg)

    def abre(self, ruta: Path) -> None:
        try:
            ruta.parent.mkdir(parents=True, exist_ok=True)
            if ruta.suffix and not ruta.exists():
                ruta.touch()
            elif not ruta.suffix:
                ruta.mkdir(parents=True, exist_ok=True)
            os.startfile(str(ruta))          # noqa: S606  (solo existe en Windows)
        except Exception as e:
            self.globo(APP_NAME, f"No se pudo abrir: {e}", NIIF_ERROR)

    # -- ventana -------------------------------------------------------------

    def wndproc(self, hwnd, msg, wparam, lparam):
        if msg == WM_TRAY:
            evento = lparam & 0xFFFF
            if evento == WM_RBUTTONUP:
                self.menu()
            elif evento in (WM_LBUTTONUP, WM_LBUTTONDBLCLK, NIN_BALLOONUSERCLICK):
                self.abre(self.log_path)
            return 0

        if msg == WM_COMMAND:
            self.comando(wparam & 0xFFFF)
            return 0

        if msg == WM_DESTROY:
            self.stop.set()
            self.quita_icono()
            user32.PostQuitMessage(0)
            return 0

        return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

    def crea_ventana(self) -> None:
        # Ventana solo para recibir mensajes: no se dibuja nada en pantalla.
        # La referencia a self.proc se guarda a proposito: si el objeto WNDPROC
        # se recolectara, Windows llamaria a memoria liberada.
        self.proc = WNDPROC(self.wndproc)

        wc = WNDCLASS()
        wc.lpfnWndProc = self.proc
        wc.lpszClassName = "NXSaveSyncTray"
        wc.hInstance = kernel32.GetModuleHandleW(None)
        user32.RegisterClassW(ctypes.byref(wc))

        self.hwnd = user32.CreateWindowExW(
            0, "NXSaveSyncTray", APP_NAME, 0, 0, 0, 0, 0,
            None, None, wc.hInstance, None)

    # -- arranque ------------------------------------------------------------

    def engancha(self, rt) -> None:
        self.rt = rt
        rt.on_event = self.evento

    def hilo_daemon(self) -> None:
        try:
            daemon.main(argv=[], stop_event=self.stop, rt_hook=self.engancha)
        except Exception:
            self.anota("El daemon se detuvo por un error:")
            for l in traceback.format_exc().splitlines():
                self.anota("  " + l)
            self.estado = "detenido por un error"
            self.actualiza_tip()
            self.globo(APP_NAME, "El daemon se detuvo. Mira el registro.", NIIF_ERROR)

    def run(self) -> int:
        _declara_prototipos()
        daemon.set_log_sink(self.anota)
        self.anota(f"=== {APP_NAME} arrancado {datetime.now():%Y-%m-%d %H:%M:%S} ===")

        self.crea_ventana()
        self.hicon = self.carga_icono()
        self.estado = "esperando"
        self.añade_icono()

        hilo = threading.Thread(target=self.hilo_daemon, daemon=True)
        hilo.start()

        msg = wintypes.MSG()
        while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))

        self.stop.set()
        hilo.join(timeout=3)
        return 0


def main() -> int:
    if sys.platform != "win32":
        print("Esta version con icono en la bandeja es solo para Windows.")
        print("En Linux usa el servicio de systemd:")
        print("  systemctl --user enable --now nxsavesync")
        return 1

    # Una sola instancia: si no, dos daemons pelearian por el puerto 7878.
    mutex = kernel32.CreateMutexW(None, True, "NXSaveSync_TrayMutex")
    if kernel32.GetLastError() == 183:   # ERROR_ALREADY_EXISTS
        user32.MessageBoxW(None, "NX Save Sync ya se esta ejecutando.",
                           APP_NAME, 0x40)
        return 0

    try:
        return Tray().run()
    finally:
        kernel32.ReleaseMutex(mutex)


if __name__ == "__main__":
    sys.exit(main())
