<div align="center">

<img src="switch/nxsavesync.jpg" width="128" alt="NX Save Sync">

# NX Save Sync

**Tus partidas guardadas, iguales en la Switch y en el PC. Por wifi, en los dos sentidos, sin cables ni copias a mano.**

[![versión](https://img.shields.io/badge/versión-5.4-2ecc71?style=for-the-badge)](https://github.com/Angelpro09xd/nxsavesync/releases/latest)
[![Switch](https://img.shields.io/badge/Switch-Atmosphère-e60012?style=for-the-badge&logo=nintendoswitch&logoColor=white)](https://github.com/Atmosphere-NX/Atmosphere)
[![Windows](https://img.shields.io/badge/Windows-un_solo_.exe-0078d6?style=for-the-badge&logo=windows&logoColor=white)](#-instalación)
[![wiki](https://img.shields.io/badge/wiki-18_páginas-8e44ad?style=for-the-badge&logo=readthedocs&logoColor=white)](https://github.com/Angelpro09xd/nxsavesync/wiki)

**Desarrollador: Angelpro09_Dev**

<img src="docs/capturas/01-juegos.jpg" width="90%" alt="La pantalla de juegos">

</div>

---

## ✨ Qué hace

Juegas en la consola, la apagas, sigues en el emulador del PC. O al revés. **La partida está donde la dejaste**, sin acordarte de nada.

|  | |
|---|---|
| 🔄 **En los dos sentidos** | Sube, baja y detecta cuándo has jugado en los dos lados |
| 🧠 **Compara contenido, no fechas** | CRC32 archivo a archivo: el reloj desajustado de una Switch con CFW da igual |
| 😴 **Sin abrir nada** | Un sysmodule sincroniza solo al cerrar un juego, al avisar el PC o cada X minutos |
| 🎮 **Varios emuladores** | eden, suyu, citron, sudachi, torzu, Ryujinx, Ryubing, Kenji‑NX… detectados solos |
| 👥 **Perfiles separados** | Cada usuario de la consola tiene su estado y sus copias |
| 🖥️ **Menú en el PC** | La misma interfaz de cristal, en el navegador |
| 🪟 **Un solo .exe** | Sin Python, sin administrador, e instala también en la consola por USB |
| 🛟 **Copia antes de tocar** | Las 10 últimas versiones de cada juego, comprimidas |

---

## 📸 Cómo se ve

### En la consola

<table>
<tr>
<td width="50%"><img src="docs/capturas/01-juegos.jpg" alt="Juegos"><br><sub><b>Juegos</b> — el estado de cada uno de un vistazo</sub></td>
<td width="50%"><img src="docs/capturas/06-sincronizando.jpg" alt="Sincronizando"><br><sub><b>Sincronizando</b> — qué se mueve y hacia dónde</sub></td>
</tr>
<tr>
<td><img src="docs/capturas/08-opciones-juego.jpg" alt="Opciones del juego"><br><sub><b>Opciones del juego</b> — política propia, o excluirlo</sub></td>
<td><img src="docs/capturas/05-dialogo.jpg" alt="Conflicto"><br><sub><b>Conflicto</b> — has jugado en los dos lados</sub></td>
</tr>
<tr>
<td><img src="docs/capturas/02-perfiles.jpg" alt="Perfiles"><br><sub><b>Perfiles</b> — cada uno por separado</sub></td>
<td><img src="docs/capturas/03-pcs.jpg" alt="PCs"><br><sub><b>PCs</b> — los encuentra solos en la red</sub></td>
</tr>
<tr>
<td><img src="docs/capturas/04-ajustes.jpg" alt="Ajustes"><br><sub><b>Ajustes</b> — modo, políticas, segundo plano, sonido</sub></td>
<td><img src="docs/capturas/09-ajustes-pc.jpg" alt="Ajustes del PC"><br><sub><b>Ajustes del PC</b> — configura el daemon sin levantarte</sub></td>
</tr>
</table>

> Las carátulas de estas capturas son de relleno. En tu consola cada tarjeta lleva **el icono real del juego**, y de él sale el color de acento de toda la interfaz.

### En el PC

<div align="center">
<img src="docs/capturas/10-menu-pc.jpg" width="90%" alt="El menú del PC">
</div>

<table>
<tr>
<td width="50%"><img src="docs/capturas/11-menu-juegos.jpg" alt="Rejilla de juegos"><br><sub>Rejilla con el tamaño que elijas, y se sincroniza desde ahí</sub></td>
<td width="50%"><img src="docs/capturas/12-menu-ajustes.jpg" alt="Ajustes del menú"><br><sub>Los mismos ajustes que ves en la consola</sub></td>
</tr>
</table>

---

## 🚀 Instalación

### 🪟 Windows — un solo archivo

Descarga **`NXSaveSync-Instalador.exe`** de la [última release](https://github.com/Angelpro09xd/nxsavesync/releases/latest) y ábrelo.

**No hace falta Python ni permisos de administrador**: se instala en `%LOCALAPPDATA%` y lleva dentro la distribución embebida oficial de Python.

- 🕐 Deja el programa junto al reloj, con su icono, y lo arranca ya.
- 🖱️ Crea accesos directos en el escritorio y en el menú de inicio.
- ▶️ Lo pone a arrancar con la sesión (se puede quitar desde el propio menú).
- 🎮 Al terminar, ofrece **instalar también en la consola**: conectas la Switch por USB, abres DBI en modo MTP, y copia la app, el sysmodule y el overlay.

### 🎮 En la consola

Copia `nxsavesync.nro` a la carpeta `/switch` de la SD y ábrelo desde el menú de homebrew.

> ⚠️ **Ábrelo con acceso completo**: mantén **R** pulsado mientras abres un juego para entrar en el menú de homebrew. Si lo abres desde el álbum, la app se queda sin memoria y sin permisos para leer los savedata.

Para que sincronice **sin abrir la app**, activa el sysmodule:

```bash
touch /ruta/a/la/SD/atmosphere/contents/420000000000534E/flags/boot2.flag
```

Y luego en la app: **Ajustes → Segundo plano**. **Reinicia la consola** — un sysmodule solo se carga al arrancar.

### 🐧 Linux

Funciona (el daemon es el mismo Python y hay servicio de systemd), pero **ya no tiene soporte oficial**: lo que se prueba y se publica es Windows.

```bash
./install.sh
systemctl --user enable --now nxsavesync
journalctl --user -u nxsavesync -f
```

---

## 📖 Documentación

La **[wiki](https://github.com/Angelpro09xd/nxsavesync/wiki)** tiene 18 páginas con todo explicado paso a paso:

| | |
|---|---|
| 🏁 [Primer uso](../../wiki/Primer-uso) | 🪟 [Instalación en Windows](../../wiki/Instalación-en-Windows) |
| 🎮 [Instalación en la Switch](../../wiki/Instalación-en-la-Switch) | 🖥️ [El menú del PC](../../wiki/El-menú-del-PC) |
| 😴 [Sincronización en segundo plano](../../wiki/Sincronización-en-segundo-plano) | 🧭 [Cómo decide qué sincronizar](../../wiki/Cómo-decide-qué-sincronizar) |
| 👤 [Clonar un perfil](../../wiki/Clonar-un-perfil) | 🔁 [Modo sin consola](../../wiki/Modo-sin-consola) |
| 🎨 [La interfaz](../../wiki/La-interfaz) | 🧩 [Overlay de Tesla](../../wiki/Overlay-de-Tesla) |
| 🕹️ [Emuladores compatibles](../../wiki/Emuladores-compatibles) | ⚙️ [Todos los ajustes](../../wiki/Todos-los-ajustes) |
| 🛠️ [Problemas frecuentes](../../wiki/Problemas-frecuentes) | 📡 [El protocolo](../../wiki/El-protocolo) |

---

## 🗂️ Cómo está montado

| Carpeta | Qué es |
|---------|--------|
| **`common/`** | El motor: protocolo, red, manifiestos, ajustes. Lo comparten la app y el sysmodule, así que los dos se comportan igual. |
| **`switch/`** | Homebrew (`.nro`) con libnx, SDL2 y SDL2_mixer. Interfaz gráfica y sonido. |
| **`sysmodule/`** | Proceso de Atmosphère que sincroniza **sin abrir la app**. |
| **`overlay/`** | Overlay de Tesla/Ultrahand para ver el estado sin salir del juego. **Requiere ovlloader.** |
| **`pc/`** | Daemon en Python. Escribe directo en la carpeta de saves del emulador, sin copias intermedias que puedan quedar desfasadas. |
| **`tools/preview/`** | Compila la interfaz en el PC para poder mirarla sin la consola. |
| **`tests/`** | Pruebas del protocolo y de la detección de emuladores. |
| **`PROTOCOL.md`** | El protocolo, por si quieres tocarlo. |

---

## 🎨 La interfaz

Cristal líquido, y no como metáfora: **los paneles enseñan de verdad lo que tienen detrás, desenfocado**.

Cada fotograma se dibuja en dos capas. Primero el fondo: el icono del juego elegido ampliado hasta perder la forma, girando muy despacio, sobre tres luces de color que se mueven en órbitas distintas. Ese fondo se reduce a una cadena de miniaturas —1280, 640, 320, 160, 80— promediando de dos en dos, y de ahí sale el desenfoque. Luego cada panel copia el trozo que le toca, lo tiñe, le dobla la luz en el canto y se recorta con una máscara de alfa suavizada.

Por eso el cristal reacciona: no es un color pintado que lo imita, es el fondo real. Mueve la selección y el interior de cada tarjeta cambia con él.

| Pieza | Qué aporta |
|-------|-----------|
| **Desenfoque del fondo** | Lo que hace que sea cristal y no plástico |
| **Refracción del canto** | Arrastra hacia dentro lo que hay fuera: da grosor |
| **Grano de esmerilado** | Sin él parece un degradado pintado |
| **Brillo especular** | Solo arriba y a la izquierda, apagándose en los extremos |
| **Sombra proyectada** | Separa el panel del fondo |
| **Máscara de alfa** | Recorte redondo de verdad, también encima de otro panel |

El acento sale del **color medio del icono del juego elegido**, con la saturación con tope y el brillo fijo para que siempre contraste. Fondo, halos, botón principal, anillo de selección e insignias se tiñen con él a la vez.

El menú es un **dock flotante** con una gota que persigue la sección elegida con un muelle y se estira según lo rápido que vaya. El muelle se integra a pasos fijos de 1/120 s: de un salto por fotograma era estable a 60 fps pero se disparaba en cualquier bache.

### 🎛️ Controles

| Botón | Acción |
|-------|--------|
| **A** | Sincroniza el juego seleccionado |
| **Y** | Sincroniza todos los del perfil |
| **X** | Opciones de ese juego (política de conflicto, excluir) |
| **L / R** | Cambia de sección |
| **ZL / ZR** | Cambia de perfil de la consola |
| **+** | Salir |

Toca una tarjeta para seleccionarla y tócala otra vez para sincronizar. El dock, los botones y las filas también responden al tacto, y cada toque deja una onda.

### 💫 Lo que se mueve

Nada aparece de golpe. Los menús entran creciendo y fundiéndose, y al cerrarse se van antes de dejar de existir: pulsar B no borra la hoja, pide que se cierre.

Para poder fundir un menú entero hizo falta una **capa de composición** en el motor. Sin ella habría que dar opacidad a cada panel, cada icono y cada texto por separado; con ella se dibuja todo a una textura aparte y se vuelca una vez con su opacidad y su escala.

| Qué | Cómo entra |
|-----|-----------|
| Menús y diálogos | Crecen desde el 95 % y suben 14 px, fundiéndose |
| Cambio de sección | El contenido sube 18 px y se funde; el título entra desde la izquierda |
| Barra de pistas | Se estira o se encoge hasta el ancho del texto nuevo |
| Tarjeta elegida | El cristal crece 5 px; el contenido no se mueve |
| Gota del menú | Muelle con inercia, y se estira según lo rápido que vaya |
| Cada toque | Deja una onda que se expande y se apaga |

### 👀 Ver la interfaz sin la consola

El dibujo vive en `switch/source/screens.c` y no sabe nada de red ni de savedata: entra el estado ya masticado y sale una pantalla. Eso permite compilar los mismos `ui.c` y `screens.c` en el PC y **mirar el resultado** — de ahí salen las capturas de arriba:

```bash
cd tools/preview
make
./preview salida/            # un PNG por pantalla
./preview salida/ --debug    # ademas dibuja el contorno de cada caja
./preview --live             # ventana navegable con teclado y raton
```

Con `--debug` cada caja registrada sale contorneada, **en rojo si se cruza con otra**. La versión anterior acumuló seis solapamientos que hubo que cazar de uno en uno a base de fotos de la consola; ahora salen todos de golpe y antes de compilar nada.

El propio previsualizador cazó dos fallos que en la consola habrían tardado en aparecer: la caché de tamaños de letra se quedaba en diez y a partir de ahí los títulos salían con el cuerpo del texto, y la de esquinas redondeadas se llenaba a las veinte y devolvía la curva de otro radio.

---

## 🖼️ El logo

Una **lente de cristal** con el ciclo de sincronización dentro: la flecha de arriba baja hacia la consola, la de abajo sube hacia el PC.

La lente no es adorno. Es la misma idea que la interfaz —algo transparente con luz detrás, brillo especular arriba a la izquierda y un destello cruzado—, así que el icono y la app se leen como la misma cosa.

Lo genera `switch/make_icon.py` con PIL, dibujando a 4x y reduciendo al final para tener bordes suaves. De ahí salen el `nxsavesync.jpg` de 256x256 que usa el menú de homebrew y el `.ico` multi-tamaño de Windows, cada tamaño reducido desde el original en vez de dejar que lo escale el sistema.

El mismo dibujo se pinta **por código** dentro de la app (arcos, círculos y halos), así que se tiñe con el color del juego elegido y no hace falta empaquetar la imagen dentro del binario.

---

## 🔊 Sonido

Los efectos se **generan por síntesis al arrancar**, no hay ficheros de audio: son ondas seno y cuadrada con envolvente de ataque y caída, mezcladas a mano. Así el `.nro` sigue siendo un solo archivo sin carpeta de recursos al lado.

Hay sonidos distintos para moverse, aceptar, volver, cambiar un ajuste, error, aviso, empezar y terminar. Se apagan en Ajustes → Sonidos.

### 🎵 La música de fondo

También sintetizada, y a juego con el cristal. Tres capas:

1. **El acorde**, cuatro voces rotando por **Do – Sol – Lam – Fa** (I–V–vi–IV, la progresión más luminosa que existe), cada nota con dos osciladores desafinados un pelín entre sí para que flote.
2. **Un arpegio** de corcheas que sube y baja por las notas del acorde. Es lo que le da alegría: sin él, cuatro voces largas suenan a sala de espera.
3. **Repiques** con parciales de campana —relaciones 1, 2, 2.76 y 5.4, que es lo que distingue un cristal de una flauta— y debajo una capa de aire.

El bucle dura 32 segundos y **se cose consigo mismo**: la cola se pliega sobre el principio con un cruce, así que al repetir no hay golpe.

Que cierre bien obliga a que **todo lo que se repite quepa un número entero de veces en esos 32 segundos**. Con corcheas de 0,3125 s entraban 102,4 y al dar la vuelta el arpegio saltaba de nota a media envolvente: un chasquido cada 32 segundos. Con 0,32 s entran 100 exactas. Lo mismo vale para las oscilaciones lentas de cada voz, que van en múltiplos de 1/32 Hz.

Se genera en un hilo aparte y empieza a sonar cuando está lista, para no retrasar el arranque. Se apaga en Ajustes → Música de fondo.

Para escucharla en el PC antes de copiar nada a la SD:

```bash
cd tools/preview && make musica
./musica ambiente.wav
```

Usa el mismo generador que la consola, así que lo que suene ahí es exactamente lo que sonará allí.

---

## 🚦 Estado de cada juego

La barrita bajo el icono de cada tarjeta dice cómo está:

| Color | Significado |
|-------|-------------|
| 🟢 verde | Al día |
| 🟠 naranja | El PC tiene cambios sin sincronizar |
| 🔴 rojo | El emulador no tiene carpeta para ese juego todavía |
| ⚪ gris | Sin sincronizar nunca, o excluido |

Ese estado se pide al PC sin transferir nada (`SUMMARY` del protocolo), así que la lista se pinta al momento.

---

## ⚖️ Modos y políticas

**Manual** (por defecto): cuando los dos lados han cambiado, sale un diálogo y eliges. **Automático**: no pregunta nunca y aplica la política configurada.

| Política | Qué hace ante un conflicto |
|----------|----------------------------|
| Gana la Switch | Se queda la versión de la consola |
| Gana el PC | Se queda la del emulador |
| **Gana el último jugado** | Se queda donde jugaste más tarde |
| No tocar nada | Deja el juego como está y sigue con el resto |

Están disponibles en los tres sitios: el ajuste general, el de cada juego y el del segundo plano.

### ⏱️ Cómo puede saberse dónde se jugó más tarde

Esto choca de frente con la regla de no comparar fechas, así que merece explicación. La consola manda **dos** marcas: la hora del save más reciente y **su hora actual**. Con la segunda el PC calcula el desfase entre los dos relojes y traduce la primera a su propia hora.

Un reloj adelantado o atrasado deja de importar mientras sea **constante**, que es justo el caso de una consola desajustada. Está probado con un desvío simulado de cinco días en las dos direcciones.

Si la consola no da fechas utilizables, o si la diferencia es menor de 90 segundos, **no se elige a ciegas**: se deja el juego sin tocar. El log dice siempre qué se decidió y por qué (*"consola hace 3 min, PC hace 2 h"*).

La política se puede fijar **por juego** desde la pantalla de opciones (X), y ahí mismo se puede **excluir** un juego para que no se sincronice nunca. Un juego excluido se salta también en modo automático.

En Ajustes está además *"Sincronizar todo al abrir la app"*, que junto al modo automático deja la sincronización sin ninguna intervención: abres la app y se pone al día sola.

---

## 😴 Sincronizar sin abrir la app

`sysmodule/` es un proceso que arranca con la consola y sincroniza solo. El instalador lo copia a `/atmosphere/contents/420000000000534E/` pero **no lo activa**: un proceso que corre siempre se enciende a conciencia.

### 🛑 La regla que manda sobre todo lo demás

> **Nunca se toca un savedata mientras hay un juego abierto.**

El sistema mantiene el savedata montado y con escrituras a medias mientras juegas; escribir ahí desde fuera corrompe la partida. El sysmodule consulta a `pm:dmnt` si hay alguna aplicación en ejecución y, si la hay, no hace nada. Ante cualquier duda (por ejemplo si `pm:dmnt` no responde) asume que **sí** hay un juego abierto: el peor caso de equivocarse por ese lado es no sincronizar, y por el otro es corromperte una partida.

Además comprueba otra vez justo antes de cada juego dentro de la misma pasada, por si abres algo a mitad.

### ⏰ Cuándo trabaja

- **Al cerrar un juego en la consola**, esperando 3 segundos a que el sistema termine de volcar el savedata. Es el mejor momento: la partida acaba de guardarse.
- **Al terminar de jugar en el emulador.** El PC no puede sincronizar por su cuenta —la consola es siempre quien abre la conexión— pero cuando su vigilante detecta el cambio manda un aviso por la red, y el sysmodule lo recoge al momento.
- **Al despertar del reposo**, porque mientras dormía han podido cambiar cosas en el PC.
- **Cada X minutos** estando en el menú HOME, como red de seguridad por si algún aviso se pierde.

Si el PC está apagado no pasa nada ni ensucia el log: reintenta a la siguiente.

### 📶 Dos servicios que hay que pedir a mano

Un sysmodule que define su propio `__appInit` **se queda sin lo que la librería inicializa sola**, y ninguno de los dos casos avisa de nada:

- **Sin el servicio de tiempo**, `time()` devuelve siempre 0. Eso deja el registro sin horas, la fecha de última sincronización a cero, el intervalo del repaso sin respetar y cualquier cuenta de tiempo transcurrido muerta por dentro.
- **Sin nifm** no se puede *pedir* la red. Al despertar del reposo la consola deja el wifi apagado hasta que alguien lo pide, así que las pasadas fallaban en silencio y parecía que hacía falta abrir la app a mano — cuando lo que hacía la app era, sin querer, encender la wifi.

Se mira la **IP** y no el estado de internet a propósito: con CFW mucha gente bloquea los servidores de Nintendo, ahí el test de internet fallaría siempre, y aquí solo se habla con un PC de casa.

### 🤫 Sin nadie delante, no se pregunta

Los conflictos y los avisos no se pueden consultar con la consola en el bolsillo, así que el sysmodule usa su propia política, **por defecto *no tocar nada***. Los juegos que necesiten una decisión se quedan pendientes y te los pregunta la app la próxima vez que la abras.

### 🧩 El overlay

Si tienes **ovlloader** (Ultrahand o Tesla), `overlay/` añade un panel que se abre con el combo de siempre y muestra el estado sin salir del juego: última sincronización, archivos movidos, cuántos esperan decisión, el interruptor del segundo plano y un *"Sincronizar ahora"*.

Con un juego abierto ese botón pone *"al salir del juego"*, y es a propósito: la regla de no tocar un savedata mientras se juega no se salta por nada.

> **Sin ovlloader el `.ovl` no hace nada.** No es un homebrew que se pueda abrir por su cuenta. El resto de NX Save Sync funciona igual sin él.

En un overlay **la SD no está montada**: libtesla la monta y la desmonta alrededor de cada acceso. Hacer `fopen` a pelo devuelve siempre `NULL`, en silencio, y el panel se queda sin datos sin dar ninguna pista.

### 🧠 Cuánta memoria puede usar un sysmodule

> **1 MB, y ni uno más.** No es un capricho.

La memoria de un sysmodule sale del **pool del sistema, que es compartido con los servicios de Horizon**. Pedir de más no falla en tu proceso: **se lo quita a los demás**.

Aprendido a base de romperlo: subir el heap a 6 MB para dibujar una imagen de aviso dejó a **HID** sin memoria, y la consola arrancaba con un fatal `2001-0132` (`LimitReached` del kernel) señalando a `0100000000000013`, que no tenía culpa de nada. El proceso que revienta no es el culpable, y eso lo hace especialmente difícil de diagnosticar.

Si necesitas mostrar algo en pantalla, el sitio es el **overlay**, no el sysmodule.

### 🔔 Cómo avisa

Un sysmodule **no puede sacar un mensaje en pantalla**: los servicios de applet no están disponibles para un proceso de fondo. Así que avisa por dos vías:

- **El LED del botón HOME de los mandos**, con un patrón por resultado: dos pulsos suaves = se movieron archivos · tres rápidos = algo espera tu decisión · uno largo y tenue = falló. Sin novedades no molesta.
- **Un aviso en la app** la próxima vez que la abras, con el detalle. Se lee de `sdmc:/switch/nxsavesync/ultima-sync.txt` y se borra al mostrarlo, para no repetirlo.

### 🔌 Cómo pararlo

Crea el archivo `sdmc:/switch/nxsavesync/fondo-apagado` y se queda quieto sin desinstalar nada. También vale desactivarlo en Ajustes, o borrar el `boot2.flag`.

Deja un registro en `sdmc:/switch/nxsavesync/fondo.log`, con rotación a los 96 KB. Cada arranque apunta **la fecha de compilación**: dos versiones distintas pueden hablar el mismo protocolo, y sin eso no hay forma de saber cuál se está ejecutando de verdad.

---

## 🖥️ El menú del PC

Se abre desde el icono, y es una página servida solo en `127.0.0.1`. Que sea una página y no una ventana de escritorio tiene dos motivos, y el primero pesa más de lo que parece:

1. **Tiene el mismo aspecto que el homebrew.** El cristal necesita desenfocar lo que hay detrás, y eso ningún kit de ventanas de escritorio lo hace. Un navegador sí, con `backdrop-filter`.
2. **Cabe en el Python reducido que lleva el instalador**, que no trae tkinter.

Las opciones no se escriben ahí: salen de `Runtime.schema()`, que es **la misma lista que se le manda a la consola**. Así el menú del PC y el de la Switch no pueden acabar diciendo cosas distintas.

La dirección lleva un testigo aleatorio. Sin él cualquier página abierta en el navegador podría llamar a ese servidor y cambiar ajustes o rutas.

Los accesos directos abren `abrir_menu.pyw`, no el programa: si ya está en marcha solo abre su menú, y si no lo está lo arranca. Pulsar el icono dos veces no deja dos copias peleándose por el mismo puerto.

### 👤 Clonar el perfil entero

Copia de la consola al emulador **el nombre, la foto y las partidas** de un perfil, eligiendo a qué emulador. Reconstruye el `profiles.dat` de la familia de yuzu (cabecera de 0x10 + 8 entradas de 0xC8, con el UUID en little-endian) y deja el avatar en `avators/<UUID>.jpg`.

### 🔁 Modo sin consola

Mantiene los emuladores iguales **entre sí**, sin que la Switch entre en la ecuación. Útil si juegas en dos emuladores del mismo PC.

Lleva su propia base, y no por gusto: **borrar un archivo no cambia la fecha de los que quedan**, así que sin base el lado que borró parecía el más viejo y el archivo resucitaba en la siguiente pasada.

---

## 🎮 Emuladores

| Familia | Cómo encuentra los saves | Estado |
|---------|--------------------------|--------|
| yuzu y clones (**eden**, suyu, citron, sudachi, torzu…) | La ruta se deriva del title id | ✅ probado |
| **Ryujinx** y forks (Ryubing, Kenji‑NX…) | La carpeta usa un id interno; hay que leer `imkvdb.arc`, y la partida se reparte entre una carpeta confirmada (`0`) y una de trabajo (`1`) | ✅ probado |

> **Ryujinx guarda cada partida en dos carpetas.** `0` es la confirmada y `1` la de trabajo, que es **la que lee el juego**. Escribir solo en `0` hace que la sincronización llegue pero el juego siga viendo su partida vieja. Las dos se dejan iguales tras cada sincronización.

**No hay lista fija de emuladores**: se detectan por la forma de la carpeta. Todo lo que tenga `nand/user/save` dentro es de la familia de yuzu, y todo lo que tenga `bis/user/save` es de la de Ryujinx. Un clon que salga mañana funcionará sin tocar nada.

### 🔠 Mayúsculas en el title id

La familia de yuzu formatea el title id en **MAYÚSCULAS** (`{:016X}`). En Linux el sistema de archivos distingue mayúsculas, así que escribir `010051f0207b2000` en vez de `010051F0207B2000` crea una carpeta paralela que el emulador **no mira**: arranca el juego, no encuentra partida y crea una vacía al lado.

Cuesta detectarlo porque muchos title ids son solo dígitos (`0100152000022000`), donde las dos grafías son idénticas y todo parece funcionar.

### 👥 Elegir el perfil correcto

En la familia de yuzu el uuid del perfil lo genera el emulador y no se puede deducir del title id. Eden (y otros) crean **un perfil de relleno todo ceros además del real**, y llegan a crear la carpeta del juego en los dos, con datos solo en uno:

```
save/0000000000000000/00000000000000000000000000000000/0100152000022000/   <- vacía
save/0000000000000000/DD46990A5A58F92B860EC1E52CA4A43D/0100152000022000/userdata.dat
```

Coger el primero por orden alfabético da justo el vacío. Peor aún: **una carpeta con datos no demuestra que el emulador la lea**. Eden llegó a crear una carpeta con el uuid mal formateado que parecía un perfil legítimo y no lo era.

Por eso no se adivina: se lee la lista real de perfiles del emulador en `nand/system/save/8000000000000010/su/avators/profiles.dat`. Las carpetas que no aparezcan ahí se ignoran.

```bash
./pc/nxsavesyncd.py --list    # perfiles declarados y carpetas en disco
./pc/nxsavesyncd.py --profile DD46990A5A58F92B860EC1E52CA4A43D
```

### 🔀 Varios emuladores a la vez

Cuando hay más de uno, el lado PC de la comparación es **el que se escribió más tarde**: es donde jugaste la última vez. Entre emuladores del mismo PC comparar fechas sí es fiable, porque comparten reloj.

El panel de detalle de cada juego dice **en cuál se jugó la última vez**, que con dos o tres emuladores es la pregunta que uno se hace todo el rato.

#### Alternar de emulador no puede parecer una mudanza

La base —el estado en que quedaron los dos lados tras la última sincronización correcta— describe **el acuerdo entre la consola y el PC**, no el contenido de una carpeta concreta. La distinción parece teórica y no lo es.

Como el lado PC se elige por fecha, jugar en Ryujinx después de haber jugado en eden cambia la carpeta comparada sin que haya cambiado nada del acuerdo. La primera versión lo tomaba por *«la carpeta de saves ha cambiado de sitio»*, tiraba la base y dejaba el juego esperando una decisión. En cada pasada, y sin sincronizar nunca.

Ahora, si la carpeta vieja y la nueva son **las dos destinos actuales de ese juego**, es solo un cambio de emulador y la base sigue valiendo. Se exigen las dos: comprobar solo la vieja daba por buena una carpeta ajena.

### ⚠️ Cierra el emulador antes de sincronizar

Si el emulador está abierto puede reescribir el save justo después de recibirlo, y esa versión suya es la que se propagaría a la consola en la siguiente pasada.

El emulador tiene que **haber creado ya la carpeta del juego**. Si nunca has abierto ese juego en el PC, arráncalo y guarda partida una vez; si no, el daemon no sabe dónde poner el save y te lo dirá.

---

## 🔍 Qué hace exactamente

Cada archivo se compara por **CRC32 del contenido**, nunca por fecha: el reloj de una Switch con CFW se desajusta con facilidad y el emulador escribe con la hora del PC, así que comparar fechas daría falsos cambios constantemente.

El PC recuerda, tras cada sincronización correcta, en qué estado quedaron los dos lados. Con esa base puede distinguir tres cosas que a simple vista se parecen:

- has jugado en la Switch → **sube** al PC
- has jugado en el emulador → **baja** a la Switch
- has jugado en los dos → **conflicto**, y te pregunta con qué lado quedarte

El conflicto se decide por juego entero, no archivo a archivo: mezclar archivos de dos partidas distintas corrompe la partida en la mayoría de juegos.

Detalles en [PROTOCOL.md](PROTOCOL.md).

### ❓ Cuando el PC trae cambios, pregunta

Aunque la comparación diga que el que cambió fue el PC, bajar esos archivos **sobrescribe lo que hay en la consola**, y eso no siempre es lo que quieres.

| Opción | Qué hace |
|--------|----------|
| **Los del PC** | Baja lo del emulador (lo que ya proponía la comparación) |
| **Los de la Switch** | Da la vuelta al plan y sube lo de la consola |
| **No tocar nada** | Lo deja pendiente |

Quedarse con lo de la Switch no es tan simple como "no hacer nada": sin la base previa, el archivo de la consola se leería como *sin cambios* y no se subiría. Por eso esa opción recalcula el plan **ignorando la base**.

### ❗ Cuando algo no cuadra, pregunta

Si una de las dos carpetas aparece vacía cuando debería tener partida, el PC **no decide ni se niega**: avisa y te da las dos salidas reales.

| Situación | Opciones |
|-----------|----------|
| La carpeta del PC está vacía | *Volver a subirla* · *Borrar también aquí* |
| La consola aparece sin partida | *Aceptar el borrado* · *Recuperarla del PC* |
| La carpeta del PC cambió de sitio | Quedarte con un lado u otro |

Y siempre hay un tercer botón, **no tocar nada**, para decidirlo más tarde.

---

## 🛡️ Seguridad de tus partidas

- Antes de tocar nada en el PC, el daemon comprime la carpeta del juego en `backups/<perfil>/<title_id>/`. Guarda las **10 últimas**.
- En el PC se escribe a un archivo temporal y se renombra al final, así que un corte de conexión no deja un save a medio pisar.
- En la Switch se llama a `fsdevCommitDevice` después de escribir. Sin eso el sistema descarta los cambios al desmontar, que es el error clásico de los gestores de saves caseros.
- Las rutas que llegan por la red se validan: nada de `..` ni rutas absolutas.
- Si una carpeta aparece vacía cuando el estado dice que tenía archivos, el daemon **avisa y te deja elegir** en vez de propagar el borrado.

> 💾 Aun así, **haz una copia de tus partidas con JKSV la primera vez**. Es código nuevo tocando tus saves.

---

## 🧪 Pruebas

```bash
python3 tests/test_sync.py   # protocolo completo contra el daemon real (67 comprobaciones)
python3 tests/test_emu.py    # emuladores, perfiles y rutas (38 comprobaciones)
```

`test_sync.py` levanta el daemon y simula una Switch: primera sincronización, cambios en cada lado, borrados, conflictos en ambos sentidos, un archivo de 3 MB, rutas maliciosas, versión incompatible, modo automático con sus tres políticas, el resumen de estados, aislamiento entre perfiles, el descubrimiento por UDP, los tres avisos con sus tres decisiones, y la configuración remota del daemon.

`test_emu.py` cubre el parser de `imkvdb.arc` de Ryujinx, el de `profiles.dat` de la familia yuzu, la grafía del title id, el perfil de relleno de Eden, la detección de un clon inventado, la convivencia de eden + citron + Ryujinx, la elección del primario por fecha y el replicado entre emuladores.

---

## 🔨 Compilar

```bash
source /etc/profile.d/devkit-env.sh
make -C switch       # la app
make -C sysmodule    # el proceso de segundo plano
make -C overlay      # el overlay de Tesla
./windows/build.sh   # el instalador de Windows (se compila desde Linux)
```

Necesita devkitPro y SDL2:

```bash
sudo apt install devkitpro-pacman
sudo dkp-pacman -S switch-dev switch-sdl2 switch-sdl2_ttf switch-sdl2_image \
                   switch-sdl2_gfx switch-sdl2_mixer
```

El instalador de Windows se construye **desde Linux**: NSIS compila instaladores de Windows en cualquier sistema, y el Python que va dentro se descarga tal cual, sin compilar nada.

El texto usa las fuentes del sistema de la consola. Como ninguna las cubre todas (la estándar no tiene kanji, la china no tiene los símbolos de Nintendo), se cargan varias y cada cadena se parte en tramos según qué fuente tenga cada glifo. Por eso un nombre como 妖怪ウォッチ4++ se ve bien y no en cuadraditos.

---

## 🚧 Limitaciones

- Un juego a la vez por conexión y una Switch a la vez, a propósito: dos clientes escribiendo el mismo save simultáneamente es justo lo que no quieres.
- **Sin cifrado ni autenticación.** Está pensado para tu red local; no lo expongas a internet abriendo los puertos en el router.
- El mapeo perfil de la Switch → perfil del emulador usa el único perfil que suele tener el emulador. Si tienes varios y quieres separarlos, se configura con `{"profiles": {"<uid_switch>": "<uuid_emu>"}}`.
- Linux funciona pero ya no tiene soporte oficial.

---

<div align="center">

**[📖 Wiki](https://github.com/Angelpro09xd/nxsavesync/wiki)** · **[⬇️ Descargas](https://github.com/Angelpro09xd/nxsavesync/releases/latest)** · **[🐛 Informar de un fallo](https://github.com/Angelpro09xd/nxsavesync/issues)**

Hecho por **Angelpro09_Dev**

</div>
