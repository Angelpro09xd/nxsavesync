# Overlay de Tesla / Ultrahand

Muestra el estado de la sincronización y deja actuar **sin salir del juego**.

> **Necesita ovlloader instalado** (Ultrahand o Tesla). Sin cargador de overlays
> el `.ovl` no hace nada: no es un homebrew que se pueda abrir por su cuenta.

## Compilar

`libtesla` no viene con devkitPro y no se guarda en este repositorio, para no
duplicar código de terceros con su propia licencia. Se descarga una vez:

```bash
mkdir -p libs/libtesla/include
curl -L -o libs/libtesla/include/tesla.hpp \
    https://raw.githubusercontent.com/WerWolv/libtesla/master/include/tesla.hpp
curl -L -o libs/libtesla/include/stb_truetype.h \
    https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h

make
```

Sale `nxsavesync.ovl`, que va en `SD:/switch/.overlays/`.

`../install.sh` y `../deploy-mtp.sh` se encargan de compilarlo y copiarlo, si las
cabeceras ya están descargadas.

## Cómo se comunica

El overlay **no habla el protocolo ni toca ningún savedata**. Solo lee y escribe
los mismos archivos de la SD que usan la app y el sysmodule:

| Archivo | Para qué |
|---------|----------|
| `config.txt` | Lee y cambia el interruptor de segundo plano |
| `ultima-sync.txt` | Lee el resultado de la última sincronización |
| `sync-ahora` | Lo crea al pulsar "Sincronizar ahora"; el sysmodule lo consume |
| `fondo-apagado` | Lo respeta, y lo borra si activas desde aquí |

Es deliberado: si el overlay decidiera por su cuenta habría **dos sitios
distintos** tomando decisiones sobre las partidas, que es justo lo que no quieres
en algo que las sobrescribe.

## Licencias

`libtesla` es GPLv2+ (WerWolv) y `stb_truetype.h` es de dominio público
(Sean Barrett). Ninguno se redistribuye aquí; se descargan al compilar.
