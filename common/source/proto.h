// Protocolo NXSaveSync v2 -- ver PROTOCOL.md en la raiz del repo.
#pragma once

#include <switch.h>

#define PROTO_VERSION      7
#define PROTO_DEFAULT_PORT 7878
#define PROTO_DISC_PORT    7879   // descubrimiento por UDP
#define PROTO_NUDGE_PORT   7880   // toque del PC a la consola

// Limite de una trama. Un savedata individual rara vez pasa de unos MB, pero
// dejamos margen porque algunos juegos guardan capturas dentro del save.
#define PROTO_MAX_FRAME    (64u * 1024u * 1024u)

// Rutas dentro del savedata. La Switch admite hasta 0x301 bytes de ruta.
#define PROTO_MAX_PATH     768

#define PROTO_DISC_PROBE   "NXSS?"
#define PROTO_DISC_REPLY   "NXSS!"

// El PC no puede sincronizar por su cuenta: la consola es siempre quien abre la
// conexion. Lo que si puede es dar un toque por UDP cuando detecta que has
// jugado en el emulador, para que el sysmodule lo recoja al momento en vez de
// esperar a su repaso periodico.
#define PROTO_NUDGE_MSG    "NXSN"

enum {
    OP_HELLO       = 0x01,
    OP_HELLO_OK    = 0x81,
    OP_PLAN_REQ    = 0x02,
    OP_PLAN_RES    = 0x82,
    OP_PULL_REQ    = 0x03,
    OP_PULL_RES    = 0x83,
    OP_PUSH        = 0x04,
    OP_PUSH_OK     = 0x84,
    OP_DEL_REMOTE  = 0x05,
    OP_DEL_OK      = 0x85,
    OP_RESOLVE     = 0x06,
    OP_RESOLVE_RES = 0x86,
    OP_COMMIT      = 0x07,
    OP_COMMIT_OK   = 0x87,
    OP_SUMMARY_REQ = 0x08,   // v2: estado de todos los juegos de un perfil
    OP_SUMMARY_RES = 0x88,
    OP_DECIDE      = 0x09,   // v3: respuesta a un aviso del PC
    OP_DECIDE_RES  = 0x89,
    OP_CFG_GET     = 0x0A,   // v3: ajustes del daemon, para editarlos desde aqui
    OP_CFG_RES     = 0x8A,
    OP_CFG_SET     = 0x0B,
    OP_CFG_OK      = 0x8B,
    OP_EMUS_REQ    = 0x0C,   // v5: que emuladores hay en el PC
    OP_EMUS_RES    = 0x8C,
    OP_PROFILE     = 0x0D,   // v6: clonar el perfil de la consola en un emulador
    OP_PROFILE_RES = 0x8D,
    OP_GAME_ICON   = 0x0E,   // v7: la caratula de un juego, para el menu del PC
    OP_GAME_ICON_OK = 0x8E,
    OP_ERROR       = 0xFF,
};

// Avisos que el PC devuelve en PLAN_RES cuando la situacion es ambigua y
// prefiere que decida el usuario en vez de elegir por su cuenta.
enum {
    WARN_NONE         = 0,
    WARN_PC_EMPTY     = 1,   // la carpeta del PC esta vacia pero habia archivos
    WARN_SWITCH_EMPTY = 2,   // la Switch aparece vacia pero habia archivos
    WARN_ROOT_CHANGED = 3,   // la carpeta de saves del PC ha cambiado de sitio
};

// Que hacer ante un aviso. Se recalcula el plan desde cero, sin base previa.
enum {
    DEC_SWITCH = 0,   // manda la consola: regenera en el PC lo que falte
    DEC_PC     = 1,   // manda el PC: aplica en la consola lo que haya (o falte)
    DEC_SKIP   = 2,   // no tocar nada
};

// Tipos de ajuste que expone el daemon.
enum {
    CFG_BOOL   = 0,
    CFG_INT    = 1,
    CFG_CHOICE = 2,
    CFG_INFO   = 3,   // solo lectura
    CFG_ACTION = 4,   // se "pulsa", no tiene valor
};

enum {
    ACT_PULL       = 0,
    ACT_PUSH       = 1,
    ACT_DEL_LOCAL  = 2,
    ACT_DEL_REMOTE = 3,
    ACT_CONFLICT   = 4,
};

enum {
    WINNER_SWITCH = 0,
    WINNER_PC     = 1,
};

// Modo con el que la Switch pide el plan.
enum {
    MODE_MANUAL = 0,   // los conflictos vuelven como ACT_CONFLICT y se preguntan
    MODE_AUTO   = 1,   // el PC los resuelve con la politica y nunca pregunta
};

// Politica que aplica el PC en MODE_AUTO (y por juego en manual).
enum {
    POLICY_ASK        = 0,   // solo valido en manual
    POLICY_SWITCH     = 1,   // ante la duda gana la consola
    POLICY_PC         = 2,   // ante la duda gana el PC
    POLICY_SKIP       = 3,   // no tocar nada si hay conflicto
    POLICY_NEWEST     = 4,   // gana donde se jugo mas tarde
};

// POLICY_NEWEST necesita comparar cuando se jugo en cada lado, y eso choca con
// la regla general de no comparar fechas entre consola y PC: el reloj de una
// Switch con CFW se desajusta y el emulador escribe con la hora del PC.
//
// Se resuelve mandando dos marcas en PLAN_REQ: la hora que tiene la consola
// AHORA y la del archivo mas reciente del savedata. Con la primera el PC calcula
// el desfase entre los dos relojes y con el traduce la segunda a su propia hora.
// Un reloj adelantado o atrasado deja de importar mientras sea constante, que es
// justo el caso de una consola desajustada.
//
// Si la consola no da fechas utiles (algunos sistemas de archivos devuelven 0)
// se manda 0 y el PC lo trata como "no se sabe" en vez de inventarse un ganador.
#define PROTO_TIME_UNKNOWN 0

// Margen por debajo del cual los dos lados se consideran empatados: no merece la
// pena decidir una partida por unos segundos de diferencia.
#define PROTO_TIME_TIE_S   90

// Estado que devuelve OP_SUMMARY_RES por juego.
enum {
    SUM_SYNCED     = 0,   // los dos lados coinciden con la ultima base
    SUM_PC_CHANGED = 1,   // el PC tiene cambios sin sincronizar
    SUM_UNKNOWN    = 2,   // nunca sincronizado, o sin base
    SUM_NO_DIR     = 3,   // el emulador aun no tiene carpeta para ese juego
};
