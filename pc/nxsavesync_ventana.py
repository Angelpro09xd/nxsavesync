"""Ventana de ajustes de NX Save Sync para Windows.

Se abre desde el icono de la bandeja. Usa tkinter, que viene con Python, para no
obligar a instalar nada con pip.

Está separada de la bandeja a propósito: la bandeja tiene que seguir respondiendo
aunque esta ventana no se pueda crear.
"""

from __future__ import annotations

import os
import subprocess
import sys
import threading
from pathlib import Path

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

# Paleta, para que no dependa del tema del sistema y se vea igual siempre.
BG = "#14171C"
PANEL = "#1E232B"
PANEL_HI = "#2A313C"
ACCENT = "#2EC4D3"
TEXT = "#ECEFF2"
DIM = "#8A93A0"
OK = "#5FD07A"
WARN = "#F0B43C"
ERR = "#E85D5D"


def abrir_en_explorador(ruta: Path) -> None:
    try:
        ruta.mkdir(parents=True, exist_ok=True)
        if sys.platform == "win32":
            os.startfile(str(ruta))          # noqa: S606
        else:
            subprocess.Popen(["xdg-open", str(ruta)])
    except Exception:
        pass


class Ventana:
    """Ventana principal. `tray` es la app de bandeja, para leer su estado."""

    def __init__(self, tray, daemon):
        self.tray = tray
        self.daemon = daemon
        self.rt = tray.rt
        self.root: tk.Tk | None = None
        self._cerrada = False

    # -- construccion --------------------------------------------------------

    def abrir(self) -> None:
        if self.root is not None:
            try:
                self.root.deiconify()
                self.root.lift()
                self.root.focus_force()
                return
            except tk.TclError:
                self.root = None

        self.root = tk.Tk()
        self.root.title("NX Save Sync")
        self.root.geometry("880x620")
        self.root.minsize(760, 520)
        self.root.configure(bg=BG)

        ico = Path(__file__).resolve().parent / "nxsavesync.ico"
        if ico.is_file():
            try:
                self.root.iconbitmap(str(ico))
            except tk.TclError:
                pass

        self._estilos()
        self._cabecera()

        nb = ttk.Notebook(self.root)
        nb.pack(fill="both", expand=True, padx=14, pady=(0, 14))

        self._pestana_estado(nb)
        self._pestana_emuladores(nb)
        self._pestana_rutas(nb)
        self._pestana_ajustes(nb)
        self._pestana_copias(nb)

        self.root.protocol("WM_DELETE_WINDOW", self.cerrar)
        self._refresca()
        self.root.mainloop()

    def _estilos(self) -> None:
        st = ttk.Style(self.root)
        try:
            st.theme_use("clam")
        except tk.TclError:
            pass

        st.configure(".", background=BG, foreground=TEXT, fieldbackground=PANEL)
        st.configure("TNotebook", background=BG, borderwidth=0)
        st.configure("TNotebook.Tab", background=PANEL, foreground=DIM,
                     padding=(18, 9), borderwidth=0)
        st.map("TNotebook.Tab",
               background=[("selected", PANEL_HI)],
               foreground=[("selected", TEXT)])
        st.configure("TFrame", background=BG)
        st.configure("Panel.TFrame", background=PANEL)
        st.configure("TLabel", background=BG, foreground=TEXT)
        st.configure("Dim.TLabel", background=BG, foreground=DIM)
        st.configure("Panel.TLabel", background=PANEL, foreground=TEXT)
        st.configure("Titulo.TLabel", background=BG, foreground=TEXT,
                     font=("Segoe UI", 15, "bold"))
        st.configure("Cabecera.TLabel", background=BG, foreground=ACCENT,
                     font=("Segoe UI", 11, "bold"))
        st.configure("TButton", background=PANEL_HI, foreground=TEXT,
                     borderwidth=0, padding=(12, 6))
        st.map("TButton", background=[("active", ACCENT)],
               foreground=[("active", BG)])
        st.configure("TCheckbutton", background=BG, foreground=TEXT)
        st.map("TCheckbutton", background=[("active", BG)])
        st.configure("TEntry", fieldbackground=PANEL_HI, foreground=TEXT,
                     insertcolor=TEXT, borderwidth=0)
        st.configure("Treeview", background=PANEL, fieldbackground=PANEL,
                     foreground=TEXT, borderwidth=0, rowheight=26)
        st.configure("Treeview.Heading", background=PANEL_HI, foreground=DIM,
                     borderwidth=0)
        st.map("Treeview", background=[("selected", ACCENT)],
               foreground=[("selected", BG)])

    def _cabecera(self) -> None:
        f = ttk.Frame(self.root)
        f.pack(fill="x", padx=18, pady=(16, 12))

        ttk.Label(f, text="NX Save Sync", style="Titulo.TLabel").pack(side="left")
        ttk.Label(f, text="  por Angelpro09_Dev", style="Dim.TLabel").pack(side="left")

        self.lbl_estado = ttk.Label(f, text="", style="Cabecera.TLabel")
        self.lbl_estado.pack(side="right")

    # -- pestanas ------------------------------------------------------------

    def _pestana_estado(self, nb) -> None:
        f = ttk.Frame(nb, padding=16)
        nb.add(f, text="  Estado  ")

        tarjetas = ttk.Frame(f)
        tarjetas.pack(fill="x", pady=(0, 14))

        self.contadores = {}
        for clave, titulo in [("conexiones", "Conexiones"),
                              ("archivos", "Archivos movidos"),
                              ("ultima", "Última sincronización")]:
            caja = ttk.Frame(tarjetas, style="Panel.TFrame", padding=14)
            caja.pack(side="left", fill="x", expand=True, padx=(0, 10))
            ttk.Label(caja, text=titulo, style="Panel.TLabel",
                      foreground=DIM, font=("Segoe UI", 9)).pack(anchor="w")
            v = ttk.Label(caja, text="—", style="Panel.TLabel",
                          font=("Segoe UI", 14, "bold"))
            v.pack(anchor="w")
            self.contadores[clave] = v

        ttk.Label(f, text="Registro", style="Cabecera.TLabel").pack(anchor="w")

        caja = ttk.Frame(f, style="Panel.TFrame")
        caja.pack(fill="both", expand=True, pady=(6, 0))

        self.txt_log = tk.Text(caja, bg=PANEL, fg=DIM, insertbackground=TEXT,
                               borderwidth=0, highlightthickness=0, wrap="none",
                               font=("Consolas", 9))
        sb = ttk.Scrollbar(caja, orient="vertical", command=self.txt_log.yview)
        self.txt_log.configure(yscrollcommand=sb.set, state="disabled")
        sb.pack(side="right", fill="y")
        self.txt_log.pack(side="left", fill="both", expand=True, padx=8, pady=8)

    def _pestana_emuladores(self, nb) -> None:
        f = ttk.Frame(nb, padding=16)
        nb.add(f, text="  Emuladores  ")

        ttk.Label(f, text="Desmarca los que no quieras sincronizar.",
                  style="Dim.TLabel").pack(anchor="w", pady=(0, 10))

        self.lista_emus = ttk.Frame(f)
        self.lista_emus.pack(fill="both", expand=True)

        botones = ttk.Frame(f)
        botones.pack(fill="x", pady=(12, 0))
        ttk.Button(botones, text="Volver a buscar",
                   command=self._rescan).pack(side="left")

        self.var_mirror = tk.BooleanVar(value=bool(self.rt and self.rt.mirror))
        ttk.Checkbutton(botones, text="Replicar la partida entre todos los activos",
                        variable=self.var_mirror,
                        command=self._toggle_mirror).pack(side="left", padx=14)

    def _pestana_rutas(self, nb) -> None:
        f = ttk.Frame(nb, padding=16)
        nb.add(f, text="  Rutas  ")

        ttk.Label(f, text="Dónde guardar, y dónde buscar emuladores.",
                  style="Dim.TLabel").pack(anchor="w", pady=(0, 12))

        self.var_backups = tk.StringVar(value=str(self.daemon.backups_dir()))
        self.var_estado = tk.StringVar(value=str(self.daemon.estado_dir()))

        self._fila_ruta(f, "Copias de seguridad", self.var_backups,
                        lambda: self._elige_carpeta(self.var_backups, "ruta_backups"))
        self._fila_ruta(f, "Estado de sincronización", self.var_estado,
                        lambda: self._elige_carpeta(self.var_estado, "ruta_estado"))

        ttk.Label(f, text="Carpetas donde buscar emuladores",
                  style="Cabecera.TLabel").pack(anchor="w", pady=(18, 4))
        ttk.Label(f, text="Además de las habituales. Útil si tienes un emulador "
                          "en otra unidad.", style="Dim.TLabel").pack(anchor="w")

        caja = ttk.Frame(f, style="Panel.TFrame", padding=10)
        caja.pack(fill="both", expand=True, pady=(8, 0))

        self.lst_extra = tk.Listbox(caja, bg=PANEL_HI, fg=TEXT, borderwidth=0,
                                    highlightthickness=0, selectbackground=ACCENT,
                                    selectforeground=BG, font=("Segoe UI", 9))
        self.lst_extra.pack(fill="both", expand=True, side="left")

        bot = ttk.Frame(caja, style="Panel.TFrame")
        bot.pack(side="right", fill="y", padx=(10, 0))
        ttk.Button(bot, text="Añadir…", command=self._add_extra).pack(fill="x", pady=2)
        ttk.Button(bot, text="Quitar", command=self._del_extra).pack(fill="x", pady=2)

        self._recarga_extras()

    def _fila_ruta(self, padre, titulo, var, cmd) -> None:
        caja = ttk.Frame(padre, style="Panel.TFrame", padding=12)
        caja.pack(fill="x", pady=4)
        ttk.Label(caja, text=titulo, style="Panel.TLabel",
                  font=("Segoe UI", 10, "bold")).pack(anchor="w")

        fila = ttk.Frame(caja, style="Panel.TFrame")
        fila.pack(fill="x", pady=(6, 0))
        ttk.Entry(fila, textvariable=var, state="readonly").pack(
            side="left", fill="x", expand=True)
        ttk.Button(fila, text="Cambiar…", command=cmd).pack(side="left", padx=(8, 0))
        ttk.Button(fila, text="Abrir",
                   command=lambda: abrir_en_explorador(Path(var.get()))).pack(
            side="left", padx=(6, 0))

    def _pestana_ajustes(self, nb) -> None:
        f = ttk.Frame(nb, padding=16)
        nb.add(f, text="  Ajustes  ")

        cfg = self.rt.cfg if self.rt else {}

        self.var_watch = tk.BooleanVar(value=bool(self.rt and self.rt.watcher))
        self.var_disc = tk.BooleanVar(value=bool(self.rt and self.rt.disc))
        self.var_nudge = tk.BooleanVar(value=bool(cfg.get("nudge", True)))
        self.var_inicio = tk.BooleanVar(value=self.tray.autoarranque_activo())

        for var, titulo, ayuda, cmd in [
            (self.var_watch, "Vigilar los emuladores",
             "Detecta cuándo has jugado en el PC", self._toggle_watch),
            (self.var_nudge, "Avisar a la consola de los cambios",
             "Para que los recoja al momento y no en su próximo repaso",
             self._toggle_nudge),
            (self.var_disc, "Dejarse encontrar en la red",
             "Responder al broadcast de la consola", self._toggle_disc),
            (self.var_inicio, "Iniciar con Windows",
             "Arranca solo al encender el PC", self._toggle_inicio),
        ]:
            caja = ttk.Frame(f, style="Panel.TFrame", padding=12)
            caja.pack(fill="x", pady=4)
            ttk.Checkbutton(caja, text=titulo, variable=var, command=cmd,
                            style="TCheckbutton").pack(anchor="w")
            ttk.Label(caja, text=ayuda, style="Panel.TLabel",
                      foreground=DIM, font=("Segoe UI", 9)).pack(anchor="w", padx=22)

        # numeros
        caja = ttk.Frame(f, style="Panel.TFrame", padding=12)
        caja.pack(fill="x", pady=4)
        ttk.Label(caja, text="Copias de seguridad a guardar por juego",
                  style="Panel.TLabel").pack(side="left")
        self.var_keep = tk.IntVar(value=int(cfg.get("backup_keep", 10)))
        tk.Spinbox(caja, from_=1, to=50, width=5, textvariable=self.var_keep,
                   command=self._set_keep, bg=PANEL_HI, fg=TEXT,
                   borderwidth=0, highlightthickness=0).pack(side="right")

        caja = ttk.Frame(f, style="Panel.TFrame", padding=12)
        caja.pack(fill="x", pady=4)
        ttk.Label(caja, text="Cada cuántos segundos revisa los emuladores",
                  style="Panel.TLabel").pack(side="left")
        self.var_int = tk.IntVar(value=int(cfg.get("watch_interval", 5)))
        tk.Spinbox(caja, from_=2, to=60, width=5, textvariable=self.var_int,
                   command=self._set_int, bg=PANEL_HI, fg=TEXT,
                   borderwidth=0, highlightthickness=0).pack(side="right")

        ttk.Label(f, text="Las políticas de conflicto se configuran desde la "
                          "consola, en Ajustes.", style="Dim.TLabel").pack(
            anchor="w", pady=(14, 0))

    def _pestana_copias(self, nb) -> None:
        f = ttk.Frame(nb, padding=16)
        nb.add(f, text="  Copias  ")

        ttk.Label(f, text="Antes de tocar nada, la carpeta del juego se guarda "
                          "en un zip.", style="Dim.TLabel").pack(anchor="w", pady=(0, 10))

        cols = ("juego", "copias", "ultima")
        self.tree = ttk.Treeview(f, columns=cols, show="headings", height=12)
        for c, t, w in [("juego", "Juego", 380), ("copias", "Copias", 80),
                        ("ultima", "Más reciente", 180)]:
            self.tree.heading(c, text=t)
            self.tree.column(c, width=w, anchor="w")
        self.tree.pack(fill="both", expand=True)

        bot = ttk.Frame(f)
        bot.pack(fill="x", pady=(10, 0))
        ttk.Button(bot, text="Abrir la carpeta",
                   command=lambda: abrir_en_explorador(self.daemon.backups_dir())).pack(side="left")
        ttk.Button(bot, text="Actualizar", command=self._recarga_copias).pack(side="left", padx=8)

    # -- acciones ------------------------------------------------------------

    def _aplica(self, clave, valor) -> None:
        if not self.rt:
            return
        try:
            msg = self.rt.apply(clave, str(valor))
            self.daemon.save_config(self.rt.cfg)
            self.lbl_estado.configure(text=msg)
        except Exception as e:
            messagebox.showerror("NX Save Sync", f"No se pudo aplicar: {e}")

    def _toggle_mirror(self):  self._aplica("mirror", "1" if self.var_mirror.get() else "0")
    def _toggle_watch(self):   self._aplica("watch", "1" if self.var_watch.get() else "0")
    def _toggle_disc(self):    self._aplica("discovery", "1" if self.var_disc.get() else "0")
    def _toggle_nudge(self):   self._aplica("nudge", "1" if self.var_nudge.get() else "0")
    def _set_keep(self):       self._aplica("backup_keep", self.var_keep.get())
    def _set_int(self):        self._aplica("watch_interval", self.var_int.get())

    def _toggle_inicio(self):
        self.tray.alterna_autoarranque()
        self.var_inicio.set(self.tray.autoarranque_activo())

    def _rescan(self):
        self._aplica("rescan", "1")
        self._recarga_emus()

    def _elige_carpeta(self, var, clave):
        elegida = filedialog.askdirectory(initialdir=var.get(),
                                          title="Elige una carpeta")
        if elegida:
            var.set(elegida)
            self._aplica(clave, elegida)

    def _add_extra(self):
        elegida = filedialog.askdirectory(title="Carpeta donde buscar emuladores")
        if not elegida:
            return
        import emulators
        rutas = emulators.extra_roots() + [elegida]
        self._aplica("carpetas_emuladores", "|".join(rutas))
        self._recarga_extras()
        self._recarga_emus()

    def _del_extra(self):
        sel = self.lst_extra.curselection()
        if not sel:
            return
        import emulators
        rutas = emulators.extra_roots()
        del rutas[sel[0]]
        self._aplica("carpetas_emuladores", "|".join(rutas))
        self._recarga_extras()
        self._recarga_emus()

    # -- refrescos -----------------------------------------------------------

    def _recarga_extras(self):
        import emulators
        self.lst_extra.delete(0, "end")
        for r in emulators.extra_roots():
            self.lst_extra.insert("end", r)

    def _recarga_emus(self):
        for w in self.lista_emus.winfo_children():
            w.destroy()

        if not self.rt or not self.rt.emus:
            ttk.Label(self.lista_emus, text="No se ha detectado ningún emulador.",
                      style="Dim.TLabel").pack(anchor="w")
            return

        for e in self.rt.emus:
            caja = ttk.Frame(self.lista_emus, style="Panel.TFrame", padding=12)
            caja.pack(fill="x", pady=3)

            var = tk.BooleanVar(value=e.name not in self.rt.disabled)
            ttk.Checkbutton(
                caja, text=e.name, variable=var,
                command=lambda n=e.name, v=var: self._aplica(
                    f"emu:{n}", "1" if v.get() else "0")).pack(anchor="w")
            ttk.Label(caja, text=str(e.base), style="Panel.TLabel",
                      foreground=DIM, font=("Segoe UI", 9)).pack(anchor="w", padx=22)

            perfiles = e.declared_profiles()
            if perfiles:
                txt = ", ".join(f"{n} ({u[:8]}…)" for u, n in perfiles)
                ttk.Label(caja, text=f"perfil: {txt}", style="Panel.TLabel",
                          foreground=DIM, font=("Segoe UI", 9)).pack(anchor="w", padx=22)

    def _recarga_copias(self):
        from datetime import datetime
        for i in self.tree.get_children():
            self.tree.delete(i)

        raiz = self.daemon.backups_dir()
        if not raiz.is_dir():
            return

        for perfil in sorted(raiz.iterdir()):
            if not perfil.is_dir():
                continue
            for juego in sorted(perfil.iterdir()):
                if not juego.is_dir():
                    continue
                zips = sorted(juego.glob("*.zip"))
                if not zips:
                    continue
                ultima = datetime.fromtimestamp(zips[-1].stat().st_mtime)
                self.tree.insert("", "end", values=(
                    juego.name, len(zips), ultima.strftime("%d/%m/%Y %H:%M")))

    def _refresca(self):
        if self._cerrada or not self.root:
            return

        self.rt = self.tray.rt
        st = self.rt.stats if self.rt else {}

        self.lbl_estado.configure(text=self.tray.estado)
        self.contadores["conexiones"].configure(text=str(st.get("conexiones", 0)))
        self.contadores["archivos"].configure(text=str(st.get("archivos", 0)))
        ult = st.get("ultima")
        self.contadores["ultima"].configure(
            text=ult.strftime("%H:%M:%S") if ult else "—")

        # El registro solo se repinta si ha cambiado, para no robar el foco ni
        # el desplazamiento mientras lo estas leyendo.
        lineas = self.tray.lineas[-400:]
        if getattr(self, "_ultimo_log", None) != len(self.tray.lineas):
            self._ultimo_log = len(self.tray.lineas)
            self.txt_log.configure(state="normal")
            self.txt_log.delete("1.0", "end")
            self.txt_log.insert("1.0", "\n".join(lineas))
            self.txt_log.see("end")
            self.txt_log.configure(state="disabled")

        if not hasattr(self, "_emus_pintados"):
            self._emus_pintados = True
            self._recarga_emus()
            self._recarga_copias()

        self.root.after(1000, self._refresca)

    def cerrar(self):
        # Cerrar la ventana no cierra la app: sigue en la bandeja.
        self._cerrada = True
        try:
            self.root.destroy()
        except tk.TclError:
            pass
        self.root = None
        self._cerrada = False
        if hasattr(self, "_emus_pintados"):
            del self._emus_pintados


def abrir_ventana(tray, daemon) -> None:
    """Abre la ventana en su propio hilo, para no bloquear la bandeja."""
    def run():
        try:
            Ventana(tray, daemon).abrir()
        except Exception:
            import traceback
            tray.anota("No se pudo abrir la ventana:")
            for l in traceback.format_exc().splitlines():
                tray.anota("  " + l)

    threading.Thread(target=run, daemon=True).start()
