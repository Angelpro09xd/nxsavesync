# Copia el homebrew, el sysmodule y el overlay a la SD de la Switch por USB.
#
# La consola tiene que estar conectada con DBI abierto en modo MTP. Windows la
# ve entonces como un "dispositivo portatil", que no es una unidad con letra: no
# se puede usar copy ni robocopy, hay que pasar por el shell.
#
# Se ejecuta solo: no toca nada mas que las tres rutas de NX Save Sync.

param(
    [string]$Origen = $PSScriptRoot
)

$ErrorActionPreference = "Stop"
$TID = "420000000000534E"

function Titulo($t) { Write-Host ""; Write-Host $t -ForegroundColor Cyan }
function Bien($t)   { Write-Host "  $t" -ForegroundColor Green }
function Aviso($t)  { Write-Host "  $t" -ForegroundColor Yellow }
function Malo($t)   { Write-Host "  $t" -ForegroundColor Red }

$shell = New-Object -ComObject Shell.Application

# --- encontrar la consola ---------------------------------------------------
#
# 17 es "Este equipo". Un dispositivo portatil aparece ahi como carpeta que no
# es del sistema de archivos, que es justo como se distingue de un disco.

Titulo "Buscando la consola..."

$equipo = $shell.NameSpace(17)
$portatiles = @($equipo.Items() | Where-Object { $_.IsFolder -and -not $_.IsFileSystem })

if ($portatiles.Count -eq 0) {
    Malo "No hay ningun dispositivo portatil conectado."
    Write-Host ""
    Write-Host "  Comprueba que:"
    Write-Host "    1. La Switch esta conectada al PC por USB."
    Write-Host "    2. Has abierto DBI en la consola."
    Write-Host "    3. En DBI has elegido 'Run MTP responder'."
    Write-Host ""
    exit 1
}

# La SD es el almacen que tiene dentro una carpeta 'switch'.
$sd = $null
foreach ($dev in $portatiles) {
    $carpeta = $dev.GetFolder
    foreach ($alm in @($carpeta.Items() | Where-Object { $_.IsFolder })) {
        $dentro = $alm.GetFolder
        if ($dentro.ParseName("switch")) { $sd = $dentro; $nombreDev = $dev.Name; break }
    }
    if ($sd) { break }
}

if (-not $sd) {
    Malo "Se ve un dispositivo conectado, pero no encuentro la tarjeta SD."
    Aviso "En DBI, asegurate de que el almacen 'SD Card' esta compartido."
    exit 1
}

Bien "Consola encontrada: $nombreDev"

# --- utilidades -------------------------------------------------------------

function Subcarpeta($padre, $nombre) {
    <#  Devuelve la carpeta, creandola si no existe.  #>
    $it = $padre.ParseName($nombre)
    if ($it -and $it.IsFolder) { return $it.GetFolder }

    $padre.NewFolder($nombre)
    Start-Sleep -Milliseconds 400
    $it = $padre.ParseName($nombre)
    if (-not $it) { throw "no se pudo crear la carpeta '$nombre' en la SD" }
    return $it.GetFolder
}

function Copia($destino, [string]$archivo) {
    <#  MTP copia en segundo plano y CopyHere no espera, asi que hay que
        comprobar que el archivo ha llegado y con el tamano correcto.  #>
    $nombre = Split-Path $archivo -Leaf
    $tam = (Get-Item $archivo).Length

    $previo = $destino.ParseName($nombre)
    if ($previo) { $previo.InvokeVerb("delete"); Start-Sleep -Milliseconds 500 }

    $carpetaOrigen = $shell.NameSpace((Split-Path $archivo -Parent))
    $item = $carpetaOrigen.ParseName($nombre)

    # 16 = responder que si a todo, 4 = sin ventana de progreso.
    $destino.CopyHere($item, 16 -bor 4)

    for ($i = 0; $i -lt 120; $i++) {
        Start-Sleep -Milliseconds 500
        $llegado = $destino.ParseName($nombre)
        if ($llegado) {
            $size = $destino.GetDetailsOf($llegado, 1)
            if ($size) { Bien "$nombre  ->  copiado"; return $true }
        }
    }

    Malo "$nombre  ->  no llego (tamano esperado: $tam bytes)"
    return $false
}

# --- copiar -----------------------------------------------------------------

$fallos = 0

Titulo "Copiando la app..."
$carpetaSwitch = Subcarpeta $sd "switch"
if (-not (Copia $carpetaSwitch (Join-Path $Origen "nxsavesync.nro"))) { $fallos++ }

Titulo "Copiando el sysmodule..."
$atm  = Subcarpeta $sd "atmosphere"
$cont = Subcarpeta $atm "contents"
$mod  = Subcarpeta $cont $TID
if (-not (Copia $mod (Join-Path $Origen "exefs.nsp")))    { $fallos++ }
if (-not (Copia $mod (Join-Path $Origen "toolbox.json"))) { $fallos++ }

# El flag solo tiene que existir; Atmosphere no mira lo que hay dentro. Lleva
# una linea de texto porque algunos stacks MTP se atragantan con los archivos
# de cero bytes.
$flags = Subcarpeta $mod "flags"
if (-not (Copia $flags (Join-Path $Origen "boot2.flag"))) { $fallos++ }

Titulo "Copiando el overlay..."
Aviso "Solo sirve si tienes ovlloader (Ultrahand o Tesla) instalado."
try {
    $ovl = Subcarpeta $carpetaSwitch ".overlays"
    if (-not (Copia $ovl (Join-Path $Origen "nxsavesync.ovl"))) { $fallos++ }
} catch {
    Aviso "No se pudo crear switch\.overlays; el overlay se queda sin copiar."
}

# --- final ------------------------------------------------------------------

Write-Host ""
if ($fallos -eq 0) {
    Write-Host "  Listo. Reinicia la consola para que arranque el sysmodule." -ForegroundColor Green
} else {
    Malo "Quedaron $fallos archivo(s) sin copiar."
    Write-Host "  Puedes copiarlos a mano: estan en" -ForegroundColor Yellow
    Write-Host "    $Origen" -ForegroundColor Yellow
}
Write-Host ""
Write-Host "  Pulsa una tecla para cerrar."
$null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
