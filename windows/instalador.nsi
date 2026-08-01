; Instalador de NX Save Sync para Windows.
;
; Un solo .exe que deja el PC listo y, si quieres, instala tambien en la consola.
; Se construye desde Linux con makensis; ver build.sh.
;
; Se instala por usuario, en %LOCALAPPDATA%, y no pide permisos de
; administrador. No hace falta: no toca nada del sistema, solo una carpeta del
; usuario y su propia entrada de arranque.

Unicode true
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "FileFunc.nsh"

!define NOMBRE   "NX Save Sync"
!define VERSION  "4.4"
!define AUTOR    "Angelpro09_Dev"
!define CLAVE    "Software\Microsoft\Windows\CurrentVersion\Uninstall\NXSaveSync"

Name "${NOMBRE} ${VERSION}"
OutFile "..\NXSaveSync-Instalador.exe"
InstallDir "$LOCALAPPDATA\NX Save Sync"
InstallDirRegKey HKCU "Software\NXSaveSync" "InstallDir"
RequestExecutionLevel user
ShowInstDetails show

VIProductVersion "4.4.0.0"
VIAddVersionKey "ProductName"     "${NOMBRE}"
VIAddVersionKey "FileDescription" "Instalador de ${NOMBRE}"
VIAddVersionKey "FileVersion"     "${VERSION}"
VIAddVersionKey "LegalCopyright"  "${AUTOR}"

; --------------------------------------------------------------------------
; aspecto
; --------------------------------------------------------------------------

!define MUI_ICON   "assets\nxsavesync.ico"
!define MUI_UNICON "assets\nxsavesync.ico"
!define MUI_ABORTWARNING

!define MUI_WELCOMEPAGE_TITLE "${NOMBRE} ${VERSION}"
!define MUI_WELCOMEPAGE_TEXT  "Sincroniza las partidas guardadas entre tu Switch y los emuladores de este PC.$\r$\n$\r$\nEsto instala:$\r$\n$\r$\n    - El programa que se queda junto al reloj y habla con la consola.$\r$\n    - Su menu, con los emuladores, las rutas y las copias de seguridad.$\r$\n$\r$\nNo hace falta tener Python: va incluido.$\r$\n$\r$\nAl terminar se puede instalar tambien en la consola, si la conectas por USB."

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES

; Dos casillas al terminar: abrir el menu, e instalar en la consola.
; Con una funcion propia y no con RUN_PARAMETERS: la macro de MUI mete los
; parametros sin comillas y con una ruta que lleva espacios (Local AppData) se
; los pasa a Exec como argumentos sueltos.
!define MUI_FINISHPAGE_RUN ""
!define MUI_FINISHPAGE_RUN_TEXT "Abrir ${NOMBRE} ahora"
!define MUI_FINISHPAGE_RUN_FUNCTION AbrirMenu

!define MUI_FINISHPAGE_SHOWREADME ""
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Instalar tambien en la consola (Switch por USB, con DBI en modo MTP)"
!define MUI_FINISHPAGE_SHOWREADME_FUNCTION InstalarEnConsola

!define MUI_FINISHPAGE_LINK "Manual y codigo en GitHub"
!define MUI_FINISHPAGE_LINK_LOCATION "https://github.com/Angelpro09xd/nxsavesync/wiki"

!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "Spanish"

; --------------------------------------------------------------------------
; instalacion
; --------------------------------------------------------------------------

Section "Programa" SecPrograma
    SectionIn RO

    ; Si ya estaba corriendo, hay que pararlo o los archivos estan en uso. Se
    ; buscan solo los procesos que salen de esta carpeta: matar todos los
    ; pythonw del sistema se llevaria por delante programas de otros.
    DetailPrint "Cerrando la version anterior, si la hubiera..."
    nsExec::ExecToLog 'powershell -NoProfile -ExecutionPolicy Bypass -Command \
        "Get-Process pythonw,python -ErrorAction SilentlyContinue | \
         Where-Object { $$_.Path -like \"$INSTDIR\*\" } | Stop-Process -Force"'
    Pop $0
    Sleep 700

    SetOutPath "$INSTDIR\python"
    File /r "build\python\*.*"

    SetOutPath "$INSTDIR\app"
    File "build\app\*.*"

    SetOutPath "$INSTDIR\consola"
    File "build\consola\*.*"

    ; --- accesos directos ---
    ;
    ; Apuntan a abrir_menu, no al programa: si ya esta en marcha abre su menu, y
    ; si no lo arranca. Asi pulsar el icono dos veces no deja dos copias.
    CreateDirectory "$SMPROGRAMS\${NOMBRE}"
    CreateShortCut "$SMPROGRAMS\${NOMBRE}\${NOMBRE}.lnk" \
        "$INSTDIR\python\pythonw.exe" '"$INSTDIR\app\abrir_menu.pyw"' \
        "$INSTDIR\app\nxsavesync.ico" 0
    CreateShortCut "$DESKTOP\${NOMBRE}.lnk" \
        "$INSTDIR\python\pythonw.exe" '"$INSTDIR\app\abrir_menu.pyw"' \
        "$INSTDIR\app\nxsavesync.ico" 0
    CreateShortCut "$SMPROGRAMS\${NOMBRE}\Instalar en la consola.lnk" \
        "powershell.exe" \
        '-NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\consola\consola.ps1"' \
        "$INSTDIR\app\nxsavesync.ico" 0
    CreateShortCut "$SMPROGRAMS\${NOMBRE}\Desinstalar.lnk" "$INSTDIR\Desinstalar.exe"

    ; --- arranque con la sesion ---
    ;
    ; Un sincronizador que hay que abrir a mano no sincroniza. Se puede quitar
    ; desde el propio menu, en Estado.
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "NXSaveSync" \
        '"$INSTDIR\python\pythonw.exe" "$INSTDIR\app\nxsavesync_tray.pyw"'

    ; --- registro de programas instalados ---
    WriteRegStr HKCU "Software\NXSaveSync" "InstallDir" "$INSTDIR"
    WriteRegStr HKCU "${CLAVE}" "DisplayName"     "${NOMBRE}"
    WriteRegStr HKCU "${CLAVE}" "DisplayVersion"  "${VERSION}"
    WriteRegStr HKCU "${CLAVE}" "Publisher"       "${AUTOR}"
    WriteRegStr HKCU "${CLAVE}" "DisplayIcon"     "$INSTDIR\app\nxsavesync.ico"
    WriteRegStr HKCU "${CLAVE}" "UninstallString" '"$INSTDIR\Desinstalar.exe"'
    WriteRegDWORD HKCU "${CLAVE}" "NoModify" 1
    WriteRegDWORD HKCU "${CLAVE}" "NoRepair" 1

    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    IntFmt $0 "0x%08X" $0
    WriteRegDWORD HKCU "${CLAVE}" "EstimatedSize" "$0"

    WriteUninstaller "$INSTDIR\Desinstalar.exe"

    ; Y se deja en marcha, para que la consola pueda encontrarlo ya.
    DetailPrint "Arrancando ${NOMBRE}..."
    Exec '"$INSTDIR\python\pythonw.exe" "$INSTDIR\app\nxsavesync_tray.pyw"'
SectionEnd

; --------------------------------------------------------------------------
; la consola
; --------------------------------------------------------------------------

Function AbrirMenu
    Exec '"$INSTDIR\python\pythonw.exe" "$INSTDIR\app\abrir_menu.pyw"'
FunctionEnd

Function InstalarEnConsola
    MessageBox MB_OKCANCEL|MB_ICONINFORMATION \
"Para instalar en la consola:$\r$\n$\r$\n\
    1. Conecta la Switch a este PC con el cable USB.$\r$\n\
    2. Abre DBI en la consola.$\r$\n\
    3. Elige 'Run MTP responder'.$\r$\n$\r$\n\
Cuando lo tengas, pulsa Aceptar y se copiaran la app, el sysmodule y el overlay." \
        IDOK seguir
    Return

    seguir:
    ; En una ventana visible a proposito: la copia por MTP puede tardar y el
    ; script va diciendo que archivo va, que es mejor que una barra parada.
    ExecShell "open" "powershell.exe" \
        '-NoProfile -NoExit -ExecutionPolicy Bypass -File "$INSTDIR\consola\consola.ps1"'
FunctionEnd

; --------------------------------------------------------------------------
; desinstalacion
; --------------------------------------------------------------------------

Section "Uninstall"
    nsExec::ExecToLog 'powershell -NoProfile -ExecutionPolicy Bypass -Command \
        "Get-Process pythonw,python -ErrorAction SilentlyContinue | \
         Where-Object { $$_.Path -like \"$INSTDIR\*\" } | Stop-Process -Force"'
    Pop $0
    Sleep 700

    Delete "$DESKTOP\${NOMBRE}.lnk"
    RMDir /r "$SMPROGRAMS\${NOMBRE}"

    DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "NXSaveSync"
    DeleteRegKey HKCU "${CLAVE}"
    DeleteRegKey HKCU "Software\NXSaveSync"

    RMDir /r "$INSTDIR\python"
    RMDir /r "$INSTDIR\app"
    RMDir /r "$INSTDIR\consola"
    Delete "$INSTDIR\Desinstalar.exe"
    RMDir "$INSTDIR"

    ; Los ajustes y las copias de seguridad viven aparte y NO se borran: son
    ; datos del usuario, y una desinstalacion no es una peticion de tirarlos.
    MessageBox MB_OK|MB_ICONINFORMATION \
"${NOMBRE} se ha quitado.$\r$\n$\r$\n\
Las copias de seguridad de tus partidas siguen donde estaban: no se borran al \
desinstalar. Si quieres quitarlas, estan en la carpeta que veias en el menu, \
dentro de Estado."
SectionEnd
