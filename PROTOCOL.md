# Protocolo NXSaveSync v3

Transporte: **TCP** en el 7878, todo en **little-endian**. La Switch es el cliente,
el PC el servidor. El descubrimiento va por **UDP** en el 7879.

## Aviso del PC a la consola

El PC **no puede sincronizar por su cuenta**: la consola es siempre quien abre la
conexión. Lo que sí puede es dar un toque por UDP cuando su vigilante detecta que
has jugado en el emulador:

```
broadcast a 255.255.255.255:7880    "NXSN" | u32 version
```

El sysmodule lo recoge y sincroniza al momento, en vez de esperar a su repaso
periódico. Es un **aviso, no una orden**: la consola decide si sincroniza y
cuándo, y si no hay ninguna escuchando el datagrama se pierde sin más.

## Descubrimiento

La Switch manda un datagrama a `255.255.255.255:7879`:

```
"NXSS?" | u32 version
```

Cada daemon de la red contesta en unicast:

```
"NXSS!" | u32 version | u16 puerto_tcp | str hostname | str emulador | u32 flags
```

La consola espera ~800 ms, junta las respuestas y: si hay **un** host lo usa sin
preguntar, si hay **varios** deja elegir. Los ya conocidos se recuerdan.

## Cambios en v3

- `PLAN_RES` empieza ahora por `u8 aviso` + `str mensaje`. Cuando el PC ve algo
  ambiguo (una de las dos carpetas vacía, la ruta cambiada) **no decide ni se
  niega**: manda el aviso con el plan vacío y espera un `DECIDE`.
- Nuevo `DECIDE` / `DECIDE_RES`: la consola responde al aviso y recibe el plan
  recalculado **ignorando la base anterior**, que es lo que permite regenerar en
  un lado lo que se perdió en el otro.
- Nuevo `CFG_GET` / `CFG_SET`: el daemon publica sus ajustes como una lista
  genérica y la consola los edita sin tocar el PC.

## Cambios respecto a v1

- `PLAN_REQ`, `PULL_REQ`, `PUSH`, `DEL_REMOTE`, `RESOLVE` y `COMMIT` llevan el
  **AccountUid** (16 bytes) del perfil de la consola. El estado y el destino se
  guardan por perfil, así que dos usuarios de la misma Switch no se pisan.
- `PLAN_REQ` lleva un **modo**: en `MODE_AUTO` el PC resuelve los conflictos con
  la política configurada y nunca devuelve `CONFLICT`.
- Nuevo `SUMMARY_REQ` / `SUMMARY_RES` para pintar en la GUI qué juegos tienen
  cambios pendientes en el PC sin tener que sincronizar.

## Trama

```
[u8 opcode][u32 payload_len][payload...]
```

`payload_len` no incluye la cabecera de 5 bytes. Máximo 64 MiB por trama.

Tipos auxiliares:

- `str`  = `[u16 len][len bytes UTF-8]`, sin NUL final.
- `entry` = `[str path][u64 size][u32 crc32]`
  - `path` es relativo a la raíz del savedata, siempre con `/` como separador,
    sin `/` inicial. Ej: `system/data_001.sav`.

## Opcodes

`uid` = 16 bytes crudos del `AccountUid` del perfil de la consola.

| Op     | Nombre        | Sentido | Payload |
|--------|---------------|---------|---------|
| `0x01` | HELLO         | S→PC    | `u32 version`, `str client_name`, `str device_id` |
| `0x81` | HELLO_OK      | PC→S    | `u32 version`, `str server_name`, `str emulador`, `u32 flags` |
| `0x02` | PLAN_REQ      | S→PC    | `uid`, `str user_name`, `u64 title_id`, `str title_name`, `u8 mode`, `u8 policy`, `u32 n`, `n × entry` |
| `0x82` | PLAN_RES      | PC→S    | `u8 aviso`, `str mensaje`, `u32 n`, `n × [u8 action][str path]` |
| `0x03` | PULL_REQ      | S→PC    | `uid`, `u64 title_id`, `str path` |
| `0x83` | PULL_RES      | PC→S    | `u64 size`, `size bytes` |
| `0x04` | PUSH          | S→PC    | `uid`, `u64 title_id`, `str path`, `u64 size`, `size bytes` |
| `0x84` | PUSH_OK       | PC→S    | — |
| `0x05` | DEL_REMOTE    | S→PC    | `uid`, `u64 title_id`, `str path` |
| `0x85` | DEL_OK        | PC→S    | — |
| `0x06` | RESOLVE       | S→PC    | `uid`, `u64 title_id`, `u8 winner` (0=switch, 1=pc) |
| `0x86` | RESOLVE_RES   | PC→S    | `u32 n`, `n × [u8 action][str path]` |
| `0x07` | COMMIT        | S→PC    | `uid`, `u64 title_id`, `u32 n`, `n × entry` |
| `0x87` | COMMIT_OK     | PC→S    | — |
| `0x08` | SUMMARY_REQ   | S→PC    | `uid`, `u32 n`, `n × u64 title_id` |
| `0x88` | SUMMARY_RES   | PC→S    | `u32 n`, `n × [u64 title_id][u8 estado]` |
| `0x09` | DECIDE        | S→PC    | `uid`, `u64 title_id`, `u8 decision` |
| `0x89` | DECIDE_RES    | PC→S    | `u32 n`, `n × [u8 action][str path]` |
| `0x0A` | CFG_GET       | S→PC    | — |
| `0x8A` | CFG_RES       | PC→S    | `u32 n`, `n × ajuste` |
| `0x0B` | CFG_SET       | S→PC    | `str clave`, `str valor` |
| `0x8B` | CFG_OK        | PC→S    | `str mensaje` |
| `0xFF` | ERROR         | PC→S    | `u32 code`, `str message` |

### Modos (`PLAN_REQ.mode`)

| Valor | Modo | Comportamiento ante conflicto |
|-------|------|-------------------------------|
| `0` | `MANUAL` | Devuelve `CONFLICT` y la consola pregunta |
| `1` | `AUTO`   | El PC aplica `policy` y nunca devuelve `CONFLICT` |

### Políticas (`PLAN_REQ.policy`)

`0` preguntar (solo manual) · `1` gana la Switch · `2` gana el PC · `3` no tocar nada.

### Avisos (`PLAN_RES.aviso`)

| Valor | Situación | Qué ofrece la consola |
|-------|-----------|------------------------|
| `0` | Nada raro | — |
| `1` | La carpeta del PC está vacía pero había archivos | Volver a subirla / borrar también aquí |
| `2` | La consola aparece vacía pero había archivos | Aceptar el borrado / recuperarla del PC |
| `3` | La carpeta del PC cambió de sitio | Quedarse con un lado u otro |

Decisión (`DECIDE.decision`): `0` manda la consola · `1` manda el PC · `2` no tocar nada.

En `MODE_AUTO` con política *gana la Switch* o *gana el PC* el aviso se resuelve
solo con esa regla, sin preguntar: es lo que se pidió al elegir ese modo.

### Formato de un ajuste (`CFG_RES`)

```
str clave | u8 tipo | str etiqueta | str ayuda | str valor | u32 n_opciones | n × str opcion
```

Tipos: `0` sí/no · `1` entero · `2` lista de opciones · `3` solo lectura · `4` acción.

La consola los pinta de forma genérica, así que añadir un ajuste nuevo en el
daemon no requiere tocar el homebrew.

### Estados de `SUMMARY_RES`

`0` al día · `1` el PC tiene cambios · `2` nunca sincronizado · `3` el emulador no
tiene carpeta para ese juego todavía.

### Acciones de PLAN_RES / RESOLVE_RES

| Valor | Acción        | Qué hace la Switch |
|-------|---------------|--------------------|
| `0`   | `PULL`        | Pide el archivo al PC y lo escribe en su savedata |
| `1`   | `PUSH`        | Envía su copia del archivo al PC |
| `2`   | `DEL_LOCAL`   | Borra el archivo de su savedata |
| `3`   | `DEL_REMOTE`  | Pide al PC que borre el archivo |
| `4`   | `CONFLICT`    | Informativo: ambos lados cambiaron. No se toca nada hasta un `RESOLVE` |

## Flujo de una sincronización

```
HELLO           →
                ← HELLO_OK
por cada juego:
  PLAN_REQ      →     (manifiesto completo del savedata de la Switch)
                ← PLAN_RES
  si hay CONFLICT y el usuario elige un lado:
  RESOLVE       →
                ← RESOLVE_RES   (plan sin conflictos para esos archivos)
  ejecuta cada acción (PULL / PUSH / DEL_LOCAL / DEL_REMOTE)
  fsdevCommitDevice()           ← imprescindible, si no la Switch pierde los datos
  COMMIT        →     (manifiesto final, se guarda como base para la próxima vez)
                ← COMMIT_OK
```

## Detección de cambios: por qué CRC32 y no fechas

El reloj de una Switch con CFW se desajusta con facilidad (y los emuladores escriben
`mtime` con la hora del PC). Comparar fechas entre los dos lados da falsos positivos
constantes, así que **el protocolo no compara `mtime` en ningún momento**: la unidad
de comparación es el CRC32 del contenido.

## Resolución a tres vías

El PC guarda, tras cada `COMMIT` correcto, el manifiesto acordado en
`state.json`. Eso da una **base** para comparar, de modo que se distingue
"este lado cambió" de "el otro lado borró".

Para cada ruta de `union(switch, pc, base)`, con `s`/`p`/`b` los CRC de cada lado
(`None` = el archivo no existe ahí):

| Condición                      | Resultado |
|--------------------------------|-----------|
| `s == p`                       | nada que hacer |
| `s == b`, `p` existe           | `PULL` (cambió el PC) |
| `s == b`, `p` no existe        | `DEL_LOCAL` (el PC lo borró) |
| `p == b`, `s` existe           | `PUSH` (cambió la Switch) |
| `p == b`, `s` no existe        | `DEL_REMOTE` (la Switch lo borró) |
| `s != b` y `p != b`            | `CONFLICT` (cambiaron los dos) |

Sin base (primera sincronización) y con el archivo en los dos lados con CRC distinto,
el resultado es `CONFLICT`: es lo correcto, porque no hay forma de saber cuál es más
reciente.

El conflicto se resuelve **por juego, no por archivo**: mezclar archivos de dos
partidas distintas corrompe la partida en la mayoría de juegos.
