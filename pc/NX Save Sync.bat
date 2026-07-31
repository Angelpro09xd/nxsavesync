@echo off
rem Arranca NX Save Sync en la bandeja del sistema.
rem
rem Usa pythonw para que no aparezca ninguna ventana de consola. Si no tienes
rem Python instalado, bajalo de python.org y marca "Add Python to PATH".

setlocal
cd /d "%~dp0"

where pythonw >nul 2>&1
if %errorlevel%==0 (
    start "" pythonw "nxsavesync_tray.pyw"
    exit /b 0
)

where python >nul 2>&1
if %errorlevel%==0 (
    echo Aviso: no se encontro pythonw, se abrira una ventana de consola.
    start "" python "nxsavesync_tray.pyw"
    exit /b 0
)

echo No se encontro Python.
echo.
echo Instalalo desde https://www.python.org/downloads/
echo y marca la casilla "Add Python to PATH" durante la instalacion.
echo.
pause
exit /b 1
