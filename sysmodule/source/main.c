// NX Save Sync -- sysmodule de sincronizacion en segundo plano.
//
// Desarrollador: Angelpro09_Dev
//
// Arranca con la consola y sincroniza sin abrir la app. La regla que manda por
// encima de todo lo demas:
//
//     NUNCA se toca un savedata mientras hay un juego abierto.
//
// El sistema mantiene el savedata montado y con escrituras a medias mientras el
// juego corre; escribir ahi desde fuera corrompe la partida. Asi que todo el
// trabajo se hace solo cuando no hay ninguna aplicacion en ejecucion, que ademas
// es justo cuando el save acaba de quedar guardado y cerrado.

#include <switch.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "proto.h"
#include "net.h"
#include "sync.h"
#include "games.h"
#include "settings.h"
#include "discovery.h"
#include "notify.h"

// Un sysmodule vive con la memoria que reserva aqui y nada mas. 1 MB da de sobra
// para manifiestos de unos miles de archivos y los buffers de red reducidos.
// 1 MB y ni uno mas. La memoria de un sysmodule sale del pool del sistema, que
// es COMPARTIDO: pedir de mas no falla en el propio proceso, se lo quita a los
// demas. Subirlo a 6 MB para dibujar un aviso dejo a HID sin memoria y la
// consola arrancaba con un fatal 2001-0132 (LimitReached del kernel).
#define INNER_HEAP_SIZE 0x100000

#define LOG_PATH   CFG_DIR "/fondo.log"
#define LOG_OLD    CFG_DIR "/fondo.log.1"
#define OFF_SWITCH CFG_DIR "/fondo-apagado"
#define ASK_NOW    CFG_DIR "/sync-ahora"   // lo deja el overlay
#define LOG_MAX    (96 * 1024)

#ifdef __cplusplus
extern "C" {
#endif

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

void __libnx_initheap(void)
{
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void *fake_heap_start;
    extern void *fake_heap_end;

    fake_heap_start = inner_heap;
    fake_heap_end   = inner_heap + sizeof(inner_heap);
}

// Buffers de red pequenos: el tamano por defecto de libnx reserva megas que un
// sysmodule no se puede permitir. Va mas lento, pero esto trabaja de fondo.
static const SocketInitConfig g_sockcfg = {
    .tcp_tx_buf_size     = 0x4000,
    .tcp_rx_buf_size     = 0x4000,
    .tcp_tx_buf_max_size = 0x10000,
    .tcp_rx_buf_max_size = 0x10000,
    .udp_tx_buf_size     = 0x2400,
    .udp_rx_buf_size     = 0xA500,
    .sb_efficiency       = 2,
    .num_bsd_sessions    = 2,
    .bsd_service_type    = BsdServiceType_Auto,
};

static bool g_have_ns;
static bool g_have_account;
static bool g_have_pm;
static bool g_have_time;
static bool g_have_nifm;

void __appInit(void)
{
    Result rc = smInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }

    rc = fsInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));
    fsdevMountSdmc();

    // Los que pueden fallar sin que esto deje de funcionar se marcan y ya.
    g_have_account = R_SUCCEEDED(accountInitialize(AccountServiceType_System));
    if (!g_have_account)
        g_have_account = R_SUCCEEDED(accountInitialize(AccountServiceType_Application));

    g_have_ns = R_SUCCEEDED(nsInitialize());
    games_set_metadata_enabled(g_have_ns);

    g_have_pm = R_SUCCEEDED(pmdmntInitialize());

    // Sin esto el sysmodule se queda sin reloj y no se nota hasta que buscas
    // la causa de algo raro: el registro salia sin horas, `cuando=` valia 0, el
    // intervalo del repaso periodico no se respetaba (se repasaba cada 5 s) y
    // no habia manera de enterarse de que la consola habia estado en reposo.
    // La libreria solo lo inicializa sola cuando no le pisas __appInit.
    g_have_time = R_SUCCEEDED(timeInitialize());

    // nifm sirve para dos cosas. Saber si hay red de verdad, y sobre todo
    // pedir que se levante: al despertar del reposo la consola deja el WiFi
    // apagado hasta que alguien lo pide, y por eso solo se sincronizaba al
    // abrir la app, que lo pedia por nosotros sin querer.
    g_have_nifm = R_SUCCEEDED(nifmInitialize(NifmServiceType_System));

    socketInitialize(&g_sockcfg);
}

void __appExit(void)
{
    socketExit();
    if (g_have_nifm)    nifmExit();
    if (g_have_time)    timeExit();
    if (g_have_pm)      pmdmntExit();
    if (g_have_ns)      nsExit();
    if (g_have_account) accountExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}

#ifdef __cplusplus
}
#endif

// --------------------------------------------------------------------------
// registro
// --------------------------------------------------------------------------

static void logf_bg(const char *fmt, ...)
{
    mkdir("sdmc:/switch", 0777);
    mkdir(CFG_DIR, 0777);

    // Rotacion simple: sin esto el log crece sin freno en la SD.
    struct stat st;
    if (stat(LOG_PATH, &st) == 0 && st.st_size > LOG_MAX) {
        remove(LOG_OLD);
        rename(LOG_PATH, LOG_OLD);
    }

    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;

    u64 ts = 0;
    if (R_SUCCEEDED(timeGetCurrentTime(TimeType_LocalSystemClock, &ts))) {
        time_t t = (time_t)ts;
        struct tm *tm = localtime(&t);
        if (tm) fprintf(f, "[%02d-%02d %02d:%02d:%02d] ",
                        tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);

    fputc('\n', f);
    fclose(f);
}

static void cb_log(void *ud, const char *fmt, ...)
{
    (void)ud;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    logf_bg("    %s", line);
}

// --------------------------------------------------------------------------
// estado del sistema
// --------------------------------------------------------------------------

// True si hay un juego abierto. Ante la duda decimos que si: es la respuesta
// segura, porque el peor caso es no sincronizar, no corromper una partida.
static bool game_running(void)
{
    if (!g_have_pm) return true;

    u64 pid = 0;
    Result rc = pmdmntGetApplicationProcessId(&pid);
    return R_SUCCEEDED(rc) && pid != 0;
}

static bool switch_off(void)
{
    struct stat st;
    return stat(OFF_SWITCH, &st) == 0;
}

// El overlay pide una sincronizacion dejando un archivo. Se consume al leerlo
// para que no se repita, pero solo si de verdad se va a sincronizar: si hay un
// juego abierto se deja pedido y se atiende al salir.
static bool asked_now(bool consumir)
{
    struct stat st;
    if (stat(ASK_NOW, &st) != 0) return false;
    if (consumir) remove(ASK_NOW);
    return true;
}

// --------------------------------------------------------------------------
// avisos del PC
// --------------------------------------------------------------------------
//
// El PC no puede sincronizar por su cuenta: la consola es siempre quien abre la
// conexion. Lo que si hace es mandar un toque por UDP cuando ve que has jugado
// en el emulador, y aqui se recoge para sincronizar al momento en vez de tener
// que esperar al repaso periodico.

static int g_nudge_fd = -1;

static void nudge_open(void)
{
    g_nudge_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_nudge_fd < 0) return;

    // Espera minima: el bucle no puede quedarse aqui parado, tiene que seguir
    // vigilando si hay un juego abierto.
    struct timeval tv = { .tv_sec = 0, .tv_usec = 2000 };
    setsockopt(g_nudge_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PROTO_NUDGE_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(g_nudge_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(g_nudge_fd);
        g_nudge_fd = -1;
        logf_bg("aviso: no se pudo escuchar los toques del PC en el %d", PROTO_NUDGE_PORT);
        return;
    }

    logf_bg("escuchando avisos del PC en UDP %d", PROTO_NUDGE_PORT);
}

// Vacia la cola y dice si llego algun aviso valido.
static bool nudge_pending(void)
{
    if (g_nudge_fd < 0) return false;

    bool got = false;
    u8 buf[64];
    size_t mlen = strlen(PROTO_NUDGE_MSG);

    for (;;) {
        ssize_t k = recv(g_nudge_fd, buf, sizeof(buf), 0);
        if (k <= 0) break;
        if ((size_t)k >= mlen + 4 && memcmp(buf, PROTO_NUDGE_MSG, mlen) == 0) {
            u32 ver = 0;
            for (int i = 0; i < 4; i++) ver |= (u32)buf[mlen + i] << (8 * i);
            if (ver == PROTO_VERSION) got = true;
        }
    }

    return got;
}

// El reloj de pared. No se usa time(): la libreria la resuelve contra un origen
// que aqui no existe y devolvia siempre 0.
static time_t wall_now(void)
{
    u64 ts = 0;
    if (R_SUCCEEDED(timeGetCurrentTime(TimeType_LocalSystemClock, &ts)))
        return (time_t)ts;
    return 0;
}

// ¿Tenemos direccion en la red local? Se mira la IP y no el estado de internet
// a proposito: mucha gente con CFW bloquea los servidores de Nintendo, ahi el
// test de internet falla para siempre y nosotros solo hablamos con un PC de
// casa. Con IP nos basta.
static bool tiene_ip(void)
{
    u32 ip = 0;
    return R_SUCCEEDED(nifmGetCurrentIpAddress(&ip)) && ip != 0;
}

static NifmRequest g_netreq;
static bool g_netreq_open;

// Suelta la peticion de red. Se llama en cuanto termina la pasada para no
// dejar el WiFi encendido a costa de la bateria.
static void network_release(void)
{
    if (!g_netreq_open) return;
    nifmRequestCancel(&g_netreq);
    nifmRequestClose(&g_netreq);
    g_netreq_open = false;
}

// Se asegura de que hay red, pidiendola si hace falta.
static bool network_up(void)
{
    // Sin nifm nos vale con intentar conectar: si no hay red, connect falla y ya.
    if (!g_have_nifm) return true;
    if (tiene_ip()) return true;

    if (!g_netreq_open) {
        if (R_FAILED(nifmCreateRequest(&g_netreq, true))) return false;
        g_netreq_open = true;
    }
    nifmRequestSubmit(&g_netreq);

    // Asociarse a la WiFi al salir del reposo lleva unos segundos. Si en diez
    // no hay IP, se deja para la vuelta siguiente en vez de bloquear el bucle.
    for (int i = 0; i < 20; i++) {
        svcSleepThread(500000000ULL);
        if (tiene_ip()) return true;
    }

    network_release();
    return false;
}

// --------------------------------------------------------------------------
// una pasada de sincronizacion
// --------------------------------------------------------------------------

static bool pick_host(host_t *out)
{
    const host_t *h = settings_host();
    if (h) { *out = *h; return true; }

    // Sin PC guardado probamos a buscarlo, por si es el primer arranque.
    hostlist_t found;
    if (discovery_scan(&found, 1200) && found.n > 0) {
        *out = found.v[0];
        settings_add_host(out, true);
        settings_save();
        logf_bg("PC encontrado por la red: %s (%s)", out->name, out->ip);
        return true;
    }
    return false;
}

// Contadores de la pasada, para el aviso del final.
static int g_pulled, g_pushed, g_deleted, g_pending, g_failed;

// Los que se quedan esperando una decision, por nombre. Se publican en la SD
// para que el overlay pueda avisar sobre el menu HOME sin hablar con nadie.
static estado_t g_estado;

static int sync_one_profile(net_t *n, AccountUid uid, const char *name)
{
    gamelist_t games;
    if (!games_list(&games, uid, false)) {
        logf_bg("  no se pudo leer la lista de partidas de %s", name);
        return 0;
    }

    int cambios = 0, saltados = 0, fallos = 0;

    for (size_t i = 0; i < games.n; i++) {
        if (settings_excluded(games.v[i].application_id)) continue;

        // Si entra un juego a mitad de la pasada, se corta y ya se seguira en
        // la siguiente. Nunca se escribe con un juego delante.
        if (game_running()) {
            logf_bg("  se abrio un juego, se deja el resto para luego");
            break;
        }

        sync_stats_t st;
        sync_ui_t ui = { .log = cb_log };   // sin ask_*: en fondo no hay a quien preguntar

        u8 policy = settings_policy_for(games.v[i].application_id);
        if (policy == POLICY_ASK) policy = g_set.bg_policy;

        bool ok = sync_title(n, uid, name,
                             games.v[i].application_id, games.v[i].name,
                             MODE_AUTO, policy, &ui, &st);
        if (!ok) {
            fallos++;
            if (net_is_dead(n)) { logf_bg("  se perdio la conexion"); break; }
            continue;
        }

        if (st.skipped) {
            saltados++;
            if (g_estado.n < ESTADO_MAX_JUEGOS) {
                snprintf(g_estado.nombre[g_estado.n], sizeof(g_estado.nombre[0]),
                         "%s", games.v[i].name);
                g_estado.estado[g_estado.n] = SUM_PC_CHANGED;
                g_estado.n++;
            }
        }
        g_pulled  += st.pulled;
        g_pushed  += st.pushed;
        g_deleted += st.del_local + st.del_remote;

        int hechos = st.pulled + st.pushed + st.del_local + st.del_remote;
        if (hechos > 0) {
            cambios += hechos;
            logf_bg("  %s: %d bajados, %d subidos, %d borrados",
                    games.v[i].name, st.pulled, st.pushed,
                    st.del_local + st.del_remote);
        }
    }

    g_pending += saltados;
    g_failed  += fallos;

    if (saltados) logf_bg("  %d juego(s) necesitan que decidas desde la app", saltados);
    if (fallos)   logf_bg("  %d juego(s) con fallos", fallos);

    games_free(&games);
    return cambios;
}

static void sync_pass(const char *motivo)
{
    if (!g_have_account) return;

    g_pulled = g_pushed = g_deleted = g_pending = g_failed = 0;
    memset(&g_estado, 0, sizeof(g_estado));

    AccountUid uids[8];
    s32 count = 0;
    if (R_FAILED(accountListAllUsers(uids, 8, &count)) || count <= 0) return;

    host_t host;
    if (!pick_host(&host)) {
        logf_bg("%s: no hay ningun PC conocido, se salta", motivo);
        return;
    }

    net_t n;
    if (!net_connect(&n, host.ip, host.port)) {
        // Que el PC este apagado es lo normal, no un error digno de log cada vez.
        return;
    }

    char server[128] = "", emu[64] = "";
    sync_ui_t ui = { .log = NULL };
    if (!sync_hello(&n, &ui, server, sizeof(server), emu, sizeof(emu))) {
        net_close(&n);
        logf_bg("%s: el PC no acepto el saludo", motivo);
        return;
    }

    logf_bg("%s: conectado a %s%s%s", motivo, server, emu[0] ? " / " : "", emu);

    int total = 0;
    for (s32 i = 0; i < count; i++) {
        if (!settings_profile_shared(uids[i])) continue;

        char nick[64];
        if (!user_nickname(uids[i], nick, sizeof(nick)))
            snprintf(nick, sizeof(nick), "perfil %d", i + 1);

        total += sync_one_profile(&n, uids[i], nick);

        if (net_is_dead(&n)) break;
    }

    net_close(&n);

    if (total > 0) logf_bg("%s: %d archivo(s) movidos", motivo, total);
    else           logf_bg("%s: todo estaba al dia", motivo);

    // Aviso: el LED del HOME para enterarte al momento, y un registro que la
    // app enseña la proxima vez que la abras con el detalle.
    notify_kind_t kind = g_failed  ? NOTIFY_FAIL
                       : g_pending ? NOTIFY_ATTENTION
                       : total > 0 ? NOTIFY_CHANGES
                                   : NOTIFY_OK;

    notify_record(kind, g_pulled, g_pushed, g_deleted, g_pending, motivo);
    notify_led(kind);

    // Y el estado de ahora, que el overlay relee cada pocos segundos.
    g_estado.pendientes = g_pending;
    estado_write(&g_estado);

}

// --------------------------------------------------------------------------

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    settings_load();
    // La fecha de compilacion va aqui a proposito: dos versiones distintas
    // pueden hablar el mismo protocolo, y sin esto no habia forma de saber
    // desde el registro cual de las dos se estaba ejecutando de verdad.
    logf_bg("--- sysmodule arrancado (v%d del protocolo, %s %s) ---",
            PROTO_VERSION, __DATE__, __TIME__);
    logf_bg("reloj: %s / red: %s",
            g_have_time ? "si" : "NO",
            g_have_nifm ? "si" : "no (a ciegas)");
    nudge_open();
    if (!g_set.bg_enabled)
        logf_bg("desactivado; se activa desde la app o poniendo fondo=1 en config.txt");

    bool was_running = false;

    // El reloj de pared, no el contador de ticks.
    //
    // armGetSystemTick se para durante el reposo. Al despertar seguia donde lo
    // dejo, asi que el repaso periodico creia que acababa de hacerse y no
    // disparaba: habia que abrir la app para que se sincronizara. El reloj de
    // pared si avanza mientras la consola duerme.
    time_t last_pass = 0;     // cuando fue el ultimo repaso
    time_t last_loop = 0;     // cuando paso la vuelta anterior
    bool   desperto  = false; // venimos del reposo y aun no hemos repasado

    for (;;) {
        svcSleepThread(5000000000ULL);   // 5 s entre comprobaciones

        // Se relee cada vuelta para que lo que cambies en la app se aplique sin
        // reiniciar la consola.
        settings_load();

        // Si entre dos vueltas ha pasado mucho mas de lo que dormimos, es que
        // la consola estuvo en reposo. Al volver hay que repasar: mientras
        // dormia han podido cambiar cosas en el PC.
        // Si por lo que fuera nos quedamos sin reloj, se tira del contador
        // monotonico: al menos se respeta el intervalo. El reposo no se notara,
        // pero es mejor que repasar cada 5 s.
        time_t wall = wall_now();
        bool hay_reloj = wall != 0;
        if (!hay_reloj) wall = (time_t)(armTicksToNs(armGetSystemTick()) / 1000000000ULL);

        if (hay_reloj && last_loop && wall - last_loop > 60) {
            desperto = true;
            logf_bg("la consola estuvo en reposo %lld s", (long long)(wall - last_loop));
        }
        last_loop = wall;

        // Se vacia siempre la cola, aunque no vayamos a usarlo: si no, se
        // acumularian avisos viejos y el primer repaso sobraria.
        bool nudged = nudge_pending() && g_set.bg_nudge;
        bool pedido = asked_now(false);

        if (!g_set.bg_enabled || switch_off()) { was_running = game_running(); continue; }

        bool running = game_running();

        if (running) {
            was_running = true;
            continue;
        }

        bool just_closed = was_running && g_set.bg_on_exit;
        bool periodic = last_pass == 0 || (wall - last_pass) >= g_set.bg_interval;

        was_running = false;

        if (!just_closed && !periodic && !nudged && !pedido && !desperto) continue;

        // Al salir del reposo la red tarda un poco en volver. Si aun no esta,
        // se deja `desperto` puesto y se reintenta en la vuelta siguiente en
        // vez de perder el aviso.
        if (!network_up()) continue;

        // Un respiro tras cerrar el juego: el sistema aun esta terminando de
        // volcar el savedata y no queremos pillarlo a medias.
        if (just_closed) svcSleepThread(3000000000ULL);
        if (game_running()) { was_running = true; continue; }

        if (pedido) asked_now(true);   // ya se va a hacer: se consume la peticion

        sync_pass(pedido      ? "pedido desde el overlay"
                : just_closed ? "al cerrar el juego"
                : desperto    ? "al salir del reposo"
                : nudged      ? "aviso del PC"
                              : "repaso periodico");
        network_release();
        desperto  = false;
        last_pass = hay_reloj ? wall_now() : wall;
    }

    return 0;
}
