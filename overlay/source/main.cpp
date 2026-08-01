// NX Save Sync -- overlay para Tesla / Ultrahand.
//
// Desarrollador: Angelpro09_Dev
//
// El sysmodule no puede sacar nada en pantalla: los servicios de applet no
// estan disponibles para un proceso de fondo. Un overlay si puede, incluso con
// un juego abierto, asi que aqui se ve el estado y se puede actuar sin salir
// de lo que estes haciendo.
//
// No habla el protocolo ni toca savedata: solo lee y escribe los mismos
// archivos de la SD que usan la app y el sysmodule. Asi no hay dos sitios
// distintos decidiendo sobre tus partidas.

#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include "aviso.hpp"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <string>
#include <vector>

namespace {

// En un overlay la raiz de la tarjeta es "/", no "sdmc:/".
const char *CFG_DIR   = "/switch/nxsavesync";
const char *CFG_FILE  = "/switch/nxsavesync/config.txt";
const char *ULTIMA    = "/switch/nxsavesync/ultima-sync.txt";
const char *AHORA     = "/switch/nxsavesync/sync-ahora";
const char *APAGADO   = "/switch/nxsavesync/fondo-apagado";

// En un overlay la SD NO esta montada: libtesla la monta y la desmonta
// alrededor de cada acceso. Hacer fopen a pelo devuelve siempre NULL, en
// silencio, y el panel se quedaba sin datos sin dar ninguna pista.
bool existe(const char *ruta) {
    bool hay = false;
    tsl::hlp::doWithSDCardHandle([&] {
        FILE *f = fopen(ruta, "rb");
        if (!f) return;
        hay = true;
        fclose(f);
    });
    return hay;
}

// Lee un archivo de "clave=valor" por lineas.
std::map<std::string, std::string> lee_ini(const char *ruta) {
    std::map<std::string, std::string> out;
    tsl::hlp::doWithSDCardHandle([&] {
    FILE *f = fopen(ruta, "r");
    if (!f) return;

    char linea[256];
    while (fgets(linea, sizeof(linea), f)) {
        if (linea[0] == '#' || linea[0] == '\n') continue;
        char *eq = strchr(linea, '=');
        if (!eq) continue;
        *eq = '\0';
        std::string v = eq + 1;
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
        out[linea] = v;
    }
    fclose(f);
    });
    return out;
}

int entero(const std::map<std::string, std::string> &m, const char *k, int def = 0) {
    auto it = m.find(k);
    if (it == m.end()) return def;
    return atoi(it->second.c_str());
}

// Cambia una sola clave de config.txt conservando el resto. Se reescribe entero
// porque son cuatro lineas; no compensa nada mas fino.
bool escribe_clave(const char *clave, const char *valor) {
    std::vector<std::string> lineas;
    bool encontrada = false, ok = false;

    tsl::hlp::doWithSDCardHandle([&] {
    FILE *f = fopen(CFG_FILE, "r");
    if (f) {
        char l[256];
        while (fgets(l, sizeof(l), f)) {
            std::string s = l;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();

            if (s.rfind(std::string(clave) + "=", 0) == 0) {
                s = std::string(clave) + "=" + valor;
                encontrada = true;
            }
            lineas.push_back(s);
        }
        fclose(f);
    }

    if (!encontrada) lineas.push_back(std::string(clave) + "=" + valor);

    f = fopen(CFG_FILE, "w");
    if (!f) return;
    for (const auto &s : lineas) fprintf(f, "%s\n", s.c_str());
    fclose(f);
    ok = true;
    });
    return ok;
}

// Hace cuanto, en texto corto.
std::string hace(u64 cuando) {
    if (!cuando) return "nunca";

    u64 ahora = 0;
    if (R_FAILED(timeGetCurrentTime(TimeType_LocalSystemClock, &ahora)) || ahora < cuando)
        return "hace un momento";

    u64 s = ahora - cuando;
    char buf[48];
    if (s < 90)          snprintf(buf, sizeof(buf), "hace %llu s", s);
    else if (s < 5400)   snprintf(buf, sizeof(buf), "hace %llu min", s / 60);
    else if (s < 172800) snprintf(buf, sizeof(buf), "hace %llu h", s / 3600);
    else                 snprintf(buf, sizeof(buf), "hace %llu dias", s / 86400);
    return buf;
}

bool hay_juego_abierto() {
    u64 pid = 0;
    // Si pm:dmnt no responde asumimos que si, igual que hace el sysmodule.
    if (R_FAILED(pmdmntInitialize())) return true;
    Result rc = pmdmntGetApplicationProcessId(&pid);
    pmdmntExit();
    return R_SUCCEEDED(rc) && pid != 0;
}

}  // namespace


class GuiPrincipal : public tsl::Gui {
private:
    tsl::elm::ListItem *m_ultima = nullptr;
    tsl::elm::ListItem *m_movidos = nullptr;
    tsl::elm::ListItem *m_pendientes = nullptr;
    tsl::elm::ListItem *m_sincronizar = nullptr;

public:
    virtual tsl::elm::Element *createUI() override {
        auto *frame = new tsl::elm::OverlayFrame("NX Save Sync", "por Angelpro09_Dev");
        auto *lista = new tsl::elm::List();

        auto cfg = lee_ini(CFG_FILE);
        auto ult = lee_ini(ULTIMA);

        lista->addItem(new tsl::elm::CategoryHeader("Estado"));

        m_ultima = new tsl::elm::ListItem("Ultima sincronizacion");
        m_ultima->setValue(hace(strtoull(ult.count("cuando") ? ult["cuando"].c_str() : "0",
                                         nullptr, 10)));
        lista->addItem(m_ultima);

        int bajados = entero(ult, "bajados");
        int subidos = entero(ult, "subidos");
        m_movidos = new tsl::elm::ListItem("Archivos movidos");
        {
            char v[64];
            snprintf(v, sizeof(v), "%d / %d", bajados, subidos);
            m_movidos->setValue(v);
        }
        lista->addItem(m_movidos);

        int pendientes = entero(ult, "pendientes");
        m_pendientes = new tsl::elm::ListItem("Esperando decision");
        {
            char v[48];
            snprintf(v, sizeof(v), "%d juego%s", pendientes, pendientes == 1 ? "" : "s");
            // El color avisa sin tener que leer: naranja si hay algo pendiente.
            m_pendientes->setValue(v, pendientes > 0);
        }
        lista->addItem(m_pendientes);

        lista->addItem(new tsl::elm::CategoryHeader("Segundo plano"));

        bool fondo = entero(cfg, "fondo", 0) != 0 && !existe(APAGADO);
        auto *tog = new tsl::elm::ToggleListItem("Sincronizar solo", fondo);
        tog->setStateChangedListener([](bool activo) {
            escribe_clave("fondo", activo ? "1" : "0");
            // El interruptor de emergencia manda sobre el ajuste, asi que al
            // activar desde aqui hay que quitarlo o no serviria de nada.
            if (activo) tsl::hlp::doWithSDCardHandle([] { remove(APAGADO); });
        });
        lista->addItem(tog);

        m_sincronizar = new tsl::elm::ListItem("Sincronizar ahora");
        m_sincronizar->setValue(existe(AHORA) ? "pedido" : "");
        m_sincronizar->setClickListener([this](u64 keys) {
            if (!(keys & HidNpadButton_A)) return false;

            tsl::hlp::doWithSDCardHandle([] {
                mkdir(CFG_DIR, 0777);
                FILE *f = fopen(AHORA, "w");
                if (f) {
                    fputs("1\n", f);
                    fclose(f);
                }
            });

            // Con un juego abierto no se toca ningun savedata: se queda pedido
            // y el sysmodule lo hara en cuanto salgas.
            m_sincronizar->setValue(hay_juego_abierto() ? "al salir del juego" : "pedido");
            return true;
        });
        lista->addItem(m_sincronizar);

        if (!fondo) {
            lista->addItem(new tsl::elm::CategoryHeader("Aviso"));
            lista->addItem(new tsl::elm::ListItem("El segundo plano esta apagado"));
        }

        frame->setContent(lista);
        return frame;
    }

    // Se refresca solo mientras el overlay esta abierto.
    virtual void update() override {
        static u32 tick = 0;
        if (++tick % 60) return;

        auto ult = lee_ini(ULTIMA);
        if (m_ultima)
            m_ultima->setValue(hace(strtoull(ult.count("cuando") ? ult["cuando"].c_str() : "0",
                                             nullptr, 10)));
        if (m_pendientes) {
            int p = entero(ult, "pendientes");
            char v[48];
            snprintf(v, sizeof(v), "%d juego%s", p, p == 1 ? "" : "s");
            m_pendientes->setValue(v, p > 0);
        }
        if (m_sincronizar && !existe(AHORA))
            m_sincronizar->setValue("");
    }
};


class OverlayNXSaveSync : public tsl::Overlay {
public:
    virtual void initServices() override {
        timeInitialize();

        // Marca de arranque. Si esta linea no aparece en el registro, es que
        // este overlay no se esta ejecutando siquiera: el problema estaria en
        // el cargador, no en nuestro codigo.
        tsl::hlp::doWithSDCardHandle([] {
            mkdir(CFG_DIR, 0777);
            FILE *f = fopen("/switch/nxsavesync/aviso.log", "a");
            if (!f) return;
            fprintf(f, "--- overlay arrancado ---\n");
            fclose(f);
        });
    }

    virtual void exitServices() override {
        nxss_aviso_exit();
        timeExit();
    }

    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<GuiPrincipal>();
    }
};


int main(int argc, char **argv) {
    return tsl::loop<OverlayNXSaveSync>(argc, argv);
}
