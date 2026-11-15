// RDR2 Co-op: play Red Dead Redemption 2's story mode together with a friend.
// ScriptHookRDR2 plugin entry point: configuration, hotkeys, HUD and the per-frame loop.
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <string>

#include "config.h"
#include "link.h"
#include "natives.h"
#include "rdr2_game.h"
#include "session.h"

using namespace coop;

namespace {

HMODULE g_module = nullptr;
// Script Hook restarts the script after every save load; this survives it so the co-op session
// reconnects on its own instead of making the player press the key again.
bool g_reconnect = false;

std::string moduleDirectory() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(g_module, path, MAX_PATH);
    std::string dir = path;
    size_t slash = dir.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : dir.substr(0, slash);
}

void logLine(const char* format, ...) {
    static std::string path = moduleDirectory() + "\\RDR2Coop.log";
    FILE* file = std::fopen(path.c_str(), "a");
    if (!file) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(file, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fputc('\n', file);
    std::fclose(file);
}

// "F7", "K", "5" ... -> virtual key code (0 = unknown / disabled).
int parseKey(std::string name) {
    for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (name.size() >= 2 && name[0] == 'F') {
        int number = std::atoi(name.c_str() + 1);
        if (number >= 1 && number <= 24) return VK_F1 + number - 1;
    }
    if (name.size() == 1 && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9'))) return name[0];
    return 0;
}

// Millisecond clock with sub-millisecond precision. GetTickCount() only advances every
// 10-16 ms, which would show up as visible stutter in the interpolation.
uint32_t clockMs() {
    static const double ticksPerMs = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint32_t>(static_cast<uint64_t>(c.QuadPart / ticksPerMs));
}

bool gameHasFocus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

class Hotkey {
public:
    explicit Hotkey(int vk = 0) : vk_(vk) {}
    // True once per key press.
    bool pressed() {
        bool down = vk_ && gameHasFocus() && (GetAsyncKeyState(vk_) & 0x8000);
        bool edge = down && !wasDown_;
        wasDown_ = down;
        return edge;
    }

private:
    int vk_;
    bool wasDown_ = false;
};

struct Settings {
    Role role = Role::Host;
    bool viaRelay = false;
    std::string address;
    int port = kDefaultPort;
    std::string room;
    std::string name = "Player";
    std::string avatar;
    bool autoConnect = false;
    bool showHud = true;
    SessionConfig session;
    std::string toggleKey = "F7", teleportKey = "F8", hudKey = "F9";
    std::string problem;  // configuration error to show on screen

    void load(const std::string& path) {
        IniFile ini;
        if (!ini.load(path)) {
            problem = "RDR2Coop.ini not found next to RDR2Coop.asi";
            return;
        }
        std::string roleName = ini.get("network", "role", "host");
        if (roleName == "host") role = Role::Host;
        else if (roleName == "guest") role = Role::Guest;
        else problem = "RDR2Coop.ini: role must be host or guest";

        std::string connection = ini.get("network", "connection", "direct");
        if (connection == "relay") viaRelay = true;
        else if (connection != "direct") problem = "RDR2Coop.ini: connection must be direct or relay";

        address = ini.get("network", "address");
        port = ini.getInt("network", "port", kDefaultPort);
        if (port <= 0 || port > 65535) problem = "RDR2Coop.ini: bad port";
        room = ini.get("network", "room");
        autoConnect = ini.getBool("network", "auto_connect", false);
        if (viaRelay && room.empty()) problem = "RDR2Coop.ini: relay mode needs a room name";
        if ((viaRelay || role == Role::Guest) && address.empty())
            problem = viaRelay ? "RDR2Coop.ini: relay mode needs the relay address"
                               : "RDR2Coop.ini: guest needs the host's address";

        name = ini.get("player", "name", "Player").substr(0, 24);
        avatar = ini.get("player", "avatar", role == Role::Host ? "player_zero" : "player_three");

        session.worldSync = ini.getBool("sync", "world_sync", true);
        session.friendlyFire = ini.getBool("sync", "friendly_fire", false);
        session.syncClock = ini.getBool("sync", "sync_clock", true);
        session.syncWeather = ini.getBool("sync", "sync_weather", true);
        session.softCorrection = ini.getBool("sync", "soft_correction", true);
        session.autoRegroup = ini.getBool("sync", "auto_regroup", true);
        session.regroupDistance = std::clamp(ini.getFloat("sync", "regroup_distance", 150.0f), 20.0f, 5000.0f);
        session.npcRadius = std::clamp(ini.getFloat("sync", "radius", 150.0f), 30.0f, 400.0f);
        session.maxPeds = std::clamp(ini.getInt("sync", "max_peds", 64), 0, 200);
        session.maxVehicles = std::clamp(ini.getInt("sync", "max_vehicles", 16), 0, 64);

        toggleKey = ini.get("keys", "toggle", "F7");
        teleportKey = ini.get("keys", "teleport_to_friend", "F8");
        hudKey = ini.get("keys", "hud", "F9");
        showHud = ini.getBool("hud", "show", true);
    }
};

class App {
public:
    void run() {
        settings_.load(moduleDirectory() + "\\RDR2Coop.ini");
        toggle_ = Hotkey(parseKey(settings_.toggleKey));
        teleport_ = Hotkey(parseKey(settings_.teleportKey));
        hud_ = Hotkey(parseKey(settings_.hudKey));
        showHud_ = settings_.showHud;
        logLine("RDR2 Co-op started: role=%s connection=%s%s%s", settings_.role == Role::Host ? "host" : "guest",
                settings_.viaRelay ? "relay" : "direct", settings_.problem.empty() ? "" : " CONFIG ERROR: ",
                settings_.problem.c_str());

        bool wantAutoConnect = settings_.autoConnect || g_reconnect;
        for (;;) {
            uint32_t now = clockMs();
            LARGE_INTEGER workStart;
            QueryPerformanceCounter(&workStart);
            uint32_t nativesBefore = native::g_nativeCalls;
            game_.frame(now);

            if (wantAutoConnect && game_.ready()) {
                wantAutoConnect = false;
                connect(now);
            }
            if (toggle_.pressed()) {
                if (online_) {
                    disconnect();
                    g_reconnect = false;
                } else {
                    connect(now);
                }
            }
            if (teleport_.pressed() && session_) {
                if (!session_->teleportToPeer()) notify(now, "Friend's position is not known yet");
            }
            if (hud_.pressed()) showHud_ = !showHud_;

            if (online_) {
                session_->tick(now);
                trackLinkState(now);
                std::string text;
                while (session_->popNotice(text)) notify(now, text);
            }
            measureWork(workStart, native::g_nativeCalls - nativesBefore);
            if (showHud_) drawHud(now);
            scriptWait(0);
        }
    }

    void shutdown() {
        if (online_) disconnect();
    }

private:
    void connect(uint32_t now) {
        if (!settings_.problem.empty()) {
            notify(now, settings_.problem);
            return;
        }
        LinkConfig config;
        config.local.role = settings_.role;
        config.local.name = settings_.name;
        config.local.avatar = settings_.avatar;
        config.viaRelay = settings_.viaRelay;
        config.room = settings_.room;
        config.listen = !settings_.viaRelay && settings_.role == Role::Host;
        if (!config.listen &&
            !UdpSocket::resolve(settings_.address, static_cast<uint16_t>(settings_.port), config.remote)) {
            notify(now, "Cannot resolve address: " + settings_.address);
            return;
        }
        std::string error;
        if (!socket_.open(config.listen ? static_cast<uint16_t>(settings_.port) : 0, &error)) {
            notify(now, error);
            return;
        }
        link_ = std::make_unique<Link>();
        link_->start(&socket_, config, now);
        session_ = std::make_unique<CoopSession>(game_, *link_, settings_.session);
        online_ = true;
        g_reconnect = true;
        lastState_ = Link::State::Idle;
        logLine("connecting (%s)", config.listen ? "listening" : config.remote.toString().c_str());
    }

    void disconnect() {
        if (session_) session_->cleanup();
        if (link_) link_->stop();
        session_.reset();
        link_.reset();
        socket_.close();
        online_ = false;
        logLine("disconnected");
    }

    void trackLinkState(uint32_t now) {
        Link::State state = link_->state();
        if (state == lastState_) return;
        if (state == Link::State::Connected) {
            notify(now, link_->peer().name + " joined");
            logLine("connected to %s", link_->peer().name.c_str());
        } else if (lastState_ == Link::State::Connected) {
            notify(now, "Friend disconnected");
            logLine("peer lost");
        } else if (state == Link::State::Failed) {
            logLine("link failed: %s", link_->error().c_str());
        }
        lastState_ = state;
    }

    // Average cost of the mod per frame (script time and native calls), shown on the HUD.
    void measureWork(const LARGE_INTEGER& start, uint32_t natives) {
        static const double ticksPerUs = [] {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return static_cast<double>(f.QuadPart) / 1e6;
        }();
        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        double us = static_cast<double>(end.QuadPart - start.QuadPart) / ticksPerUs;
        workUs_ += (us - workUs_) * 0.05;
        natives_ += (natives - natives_) * 0.05;
    }

    void notify(uint32_t now, const std::string& text) {
        notice_ = text;
        noticeUntil_ = now + 6000;
    }

    static void drawText(const std::string& text, float x, float y, float scale, int r, int g, int b) {
        native::SET_TEXT_SCALE(scale, scale);
        native::SET_TEXT_COLOR(r, g, b, 235);
        native::DISPLAY_TEXT(native::LITERAL_STRING(text.c_str()), x, y);
    }

    void drawHud(uint32_t now) {
        const float x = 0.012f;
        float y = 0.012f;
        char line[256];
        if (!online_) {
            std::snprintf(line, sizeof(line), "Co-op: offline  [%s] connect", settings_.toggleKey.c_str());
            drawText(line, x, y, 0.30f, 200, 200, 200);
        } else if (link_->state() == Link::State::Failed) {
            drawText("Co-op: " + link_->error(), x, y, 0.30f, 255, 110, 90);
        } else if (link_->state() != Link::State::Connected) {
            if (settings_.viaRelay) {
                std::snprintf(line, sizeof(line), "Co-op: %s, room \"%s\"",
                              link_->relayPeers() == 0 ? "contacting relay" : "waiting for friend", settings_.room.c_str());
            } else if (settings_.role == Role::Host) {
                std::snprintf(line, sizeof(line), "Co-op: waiting for friend on UDP port %d", settings_.port);
            } else {
                std::snprintf(line, sizeof(line), "Co-op: connecting to %s", settings_.address.c_str());
            }
            drawText(line, x, y, 0.30f, 240, 220, 120);
            if (!link_->error().empty()) drawText(link_->error(), x, y += 0.026f, 0.28f, 255, 110, 90);
        } else {
            const SessionStatus& st = session_->status();
            std::snprintf(line, sizeof(line), "Co-op: %s  |  ping %d ms  |  up %.1f / down %.1f KB/s",
                          link_->peer().name.c_str(), link_->pingMs() < 0 ? 0 : link_->pingMs(),
                          link_->sendRate() / 1024.0f, link_->receiveRate() / 1024.0f);
            drawText(line, x, y, 0.30f, 140, 230, 140);
            if (st.peerVisible || st.peerDistance > 0) {
                std::snprintf(line, sizeof(line), "%.0f m away%s%s  |  smoothing %d ms  [%s] teleport", st.peerDistance,
                              st.peerDead ? "  |  DEAD" : "", st.peerInMission ? "  |  in mission" : "", st.bufferMs,
                              settings_.teleportKey.c_str());
                drawText(line, x, y += 0.026f, 0.28f, 220, 220, 220);
            }
            if (settings_.session.worldSync) {
                std::snprintf(line, sizeof(line),
                              settings_.role == Role::Guest ? "Host world: %d shown  |  mod %.2f ms, %.0f calls/frame"
                                                            : "World: %d streamed  |  mod %.2f ms, %.0f calls/frame",
                              st.syncedEntities, workUs_ / 1000.0, natives_);
                drawText(line, x, y += 0.026f, 0.28f, 220, 220, 220);
            }
            if (st.peerInCutscene) drawText("Host is in a cutscene", x, y += 0.026f, 0.28f, 240, 220, 120);
        }
        if (!notice_.empty() && static_cast<int32_t>(noticeUntil_ - now) > 0)
            drawText(notice_, x, y + 0.03f, 0.32f, 255, 255, 255);
    }

    Settings settings_;
    Rdr2Game game_;
    UdpSocket socket_;
    std::unique_ptr<Link> link_;
    std::unique_ptr<CoopSession> session_;
    bool online_ = false;
    bool showHud_ = true;
    Link::State lastState_ = Link::State::Idle;
    Hotkey toggle_, teleport_, hud_;
    std::string notice_;
    uint32_t noticeUntil_ = 0;
    double workUs_ = 0, natives_ = 0;
};

std::unique_ptr<App> g_app;

// Script Hook calls this on its script thread, again after every save load.
void scriptMain() {
    // Entity handles from before a reload are gone; only the network side needs a goodbye.
    g_app = std::make_unique<App>();
    g_app->run();
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        scriptRegister(module, scriptMain);
    } else if (reason == DLL_PROCESS_DETACH) {
        scriptUnregister(module);
    }
    return TRUE;
}
