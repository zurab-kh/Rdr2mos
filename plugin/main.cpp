// RDR2 Co-op: play Red Dead Redemption 2's story mode together.
// ScriptHookRDR2 plugin entry point: configuration, the optional built-in server, hotkeys, HUD
// and the per-frame loop.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "config.h"
#include "link.h"
#include "natives.h"
#include "rdr2_game.h"
#include "saves.h"
#include "server.h"
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
    std::string serverAddress;
    bool hostServer = false;
    int port = kDefaultPort;
    std::string campaign = "default";
    std::string password;
    bool wantsWorld = false;
    std::string name = "Player";
    std::string avatar = "player_zero";
    bool autoConnect = false;
    bool showHud = true;
    SessionConfig session;
    ServerConfig server;  // used by the built-in server
    std::string toggleKey = "F7", teleportKey = "F8", hudKey = "F9", downloadKey = "F10", uploadKey = "F11";
    std::string problem;  // configuration error to show on screen

    void load(const std::string& path) {
        IniFile ini;
        if (!ini.load(path)) {
            problem = "RDR2Coop.ini not found next to RDR2Coop.asi";
            return;
        }
        serverAddress = ini.get("network", "server");
        hostServer = ini.getBool("network", "host_server", false);
        port = ini.getInt("network", "port", kDefaultPort);
        if (port <= 0 || port > 65535) problem = "RDR2Coop.ini: bad port";
        campaign = ini.get("network", "campaign", "default");
        password = ini.get("network", "password");
        autoConnect = ini.getBool("network", "auto_connect", false);
        wantsWorld = ini.getBool("network", "world_owner", hostServer);
        if (!hostServer && serverAddress.empty())
            problem = "RDR2Coop.ini: set server = <address> or host_server = 1";

        name = ini.get("player", "name", "Player").substr(0, 24);
        avatar = ini.get("player", "avatar", wantsWorld ? "player_zero" : "player_three");

        session.worldSync = ini.getBool("sync", "world_sync", true);
        session.friendlyFire = ini.getBool("sync", "friendly_fire", false);
        session.syncClock = ini.getBool("sync", "sync_clock", true);
        session.syncWeather = ini.getBool("sync", "sync_weather", true);
        session.softCorrection = ini.getBool("sync", "soft_correction", true);
        session.npcRadius = std::clamp(ini.getFloat("sync", "radius", 150.0f), 30.0f, 400.0f);
        session.maxPeds = std::clamp(ini.getInt("sync", "max_peds", 64), 0, 200);
        session.maxVehicles = std::clamp(ini.getInt("sync", "max_vehicles", 16), 0, 64);
        session.autoRegroup = ini.getBool("story", "auto_regroup", true);
        session.spectate = ini.getBool("story", "spectate", true);
        session.syncWaypoints = ini.getBool("story", "sync_waypoints", true);
        session.autoUploadSaves = ini.getBool("story", "auto_upload_saves", true);

        std::string mode = ini.get("server", "mode", "shared");
        if (mode == "shared") server.storyMode = StoryMode::Shared;
        else if (mode == "host") server.storyMode = StoryMode::HostWorld;
        else problem = "RDR2Coop.ini: [server] mode must be shared or host";
        server.password = password;
        server.maxPlayers = std::clamp(ini.getInt("server", "max_players", 8), 1, static_cast<int>(kMaxPlayers));
        server.cutsceneBarrier = ini.getBool("server", "cutscene_barrier", true);
        server.barrierTimeoutMs = static_cast<uint32_t>(std::clamp(ini.getInt("server", "barrier_timeout", 120), 10, 1800)) * 1000u;
        server.sharedFail = ini.getBool("server", "shared_fail", true);
        server.regroupDistance = std::clamp(ini.getFloat("server", "regroup_distance", 150.0f), 20.0f, 5000.0f);

        toggleKey = ini.get("keys", "toggle", "F7");
        teleportKey = ini.get("keys", "teleport_to_friend", "F8");
        hudKey = ini.get("keys", "hud", "F9");
        downloadKey = ini.get("keys", "download_campaign_save", "F10");
        uploadKey = ini.get("keys", "upload_save", "F11");
        showHud = ini.getBool("hud", "show", true);
    }
};

// The built-in server runs on its own thread: it needs nothing from the game, so it keeps
// serving the others even while this game sits in a loading screen (when scripts are paused).
class BuiltInServer {
public:
    bool start(const Settings& s, std::string& error) {
        if (thread_.joinable()) return true;  // already running (survives script restarts)
        if (!socket_.open(static_cast<uint16_t>(s.port), &error)) return false;
        storage_ = std::make_unique<FileStorage>(moduleDirectory() + "\\RDR2Coop-server");
        server_ = std::make_unique<Server>(socket_, s.server, *storage_);
        stop_ = false;
        thread_ = std::thread([this] {
            while (!stop_) {
                socket_.waitReadable(5);
                server_->pump(clockMs());
            }
        });
        logLine("built-in server running on UDP port %d", s.port);
        return true;
    }
    void stop() {
        stop_ = true;
        if (thread_.joinable()) thread_.detach();  // never block the loader lock in DllMain
    }

private:
    UdpSocket socket_;
    std::unique_ptr<FileStorage> storage_;
    std::unique_ptr<Server> server_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};
BuiltInServer g_server;

class App {
public:
    void run() {
        settings_.load(moduleDirectory() + "\\RDR2Coop.ini");
        toggle_ = Hotkey(parseKey(settings_.toggleKey));
        teleport_ = Hotkey(parseKey(settings_.teleportKey));
        hud_ = Hotkey(parseKey(settings_.hudKey));
        download_ = Hotkey(parseKey(settings_.downloadKey));
        upload_ = Hotkey(parseKey(settings_.uploadKey));
        showHud_ = settings_.showHud;
        logLine("RDR2 Co-op started: server=%s campaign=%s%s%s",
                settings_.hostServer ? "built-in" : settings_.serverAddress.c_str(), settings_.campaign.c_str(),
                settings_.problem.empty() ? "" : " CONFIG ERROR: ", settings_.problem.c_str());

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
            if (teleport_.pressed() && session_ && !session_->teleportToPeer())
                notify(now, "Nobody else's position is known yet");
            if (hud_.pressed()) showHud_ = !showHud_;
            if (download_.pressed()) {
                if (!session_ || !link_->connected()) notify(now, "Connect to the server first");
                else if (!session_->requestCampaignSave()) notify(now, "A download is already running");
            }
            if (upload_.pressed()) {
                if (!session_ || !link_->connected()) notify(now, "Connect to the server first");
                else session_->uploadSave();
            }

            if (online_) {
                session_->tick(now);
                trackLinkState(now);
                std::string text;
                while (session_->popNotice(text)) {
                    notify(now, text);
                    logLine("%s", text.c_str());
                }
            }
            measureWork(workStart, native::g_nativeCalls - nativesBefore);
            if (showHud_) drawHud(now);
            scriptWait(0);
        }
    }

private:
    void connect(uint32_t now) {
        if (!settings_.problem.empty()) {
            notify(now, settings_.problem);
            return;
        }
        std::string error;
        if (settings_.hostServer && !g_server.start(settings_, error)) {
            notify(now, "Cannot start the server: " + error);
            return;
        }
        LinkConfig config;
        config.local.role = Role::Guest;
        config.local.name = settings_.name;
        config.local.avatar = settings_.avatar;
        config.local.campaign = settings_.campaign;
        config.local.password = settings_.password;
        config.local.wantsWorld = settings_.wantsWorld ? 1 : 0;
        std::string address = settings_.hostServer ? "127.0.0.1" : settings_.serverAddress;
        if (!UdpSocket::resolve(address, static_cast<uint16_t>(settings_.port), config.remote)) {
            notify(now, "Cannot resolve server address: " + address);
            return;
        }
        if (!socket_.open(0, &error)) {
            notify(now, error);
            return;
        }
        link_ = std::make_unique<Link>();
        link_->start(&socket_, config, now);
        session_ = std::make_unique<CoopSession>(game_, *link_, settings_.session);
        session_->setSaveStore(&saves_);
        online_ = true;
        g_reconnect = true;
        lastState_ = Link::State::Idle;
        logLine("connecting to %s (campaign %s)", config.remote.toString().c_str(), settings_.campaign.c_str());
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
            notify(now, "Connected to the server");
            logLine("connected to the server");
        } else if (lastState_ == Link::State::Connected) {
            notify(now, "Lost the server - reconnecting...");
            logLine("server lost");
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
        } else if (!session_->status().denied.empty()) {
            drawText("Co-op: the server refused: " + session_->status().denied, x, y, 0.30f, 255, 110, 90);
        } else if (link_->state() != Link::State::Connected || !session_->status().welcomed) {
            std::snprintf(line, sizeof(line), "Co-op: connecting to %s...",
                          settings_.hostServer ? "the built-in server" : settings_.serverAddress.c_str());
            drawText(line, x, y, 0.30f, 240, 220, 120);
        } else {
            const SessionStatus& st = session_->status();
            std::snprintf(line, sizeof(line), "Co-op \"%s\": %d player%s  |  world: %s  |  ping %d ms  |  up %.1f / down %.1f KB/s",
                          settings_.campaign.c_str(), st.players, st.players == 1 ? "" : "s",
                          st.worldOwner ? "you" : st.ownerName.c_str(), link_->pingMs() < 0 ? 0 : link_->pingMs(),
                          link_->sendRate() / 1024.0f, link_->receiveRate() / 1024.0f);
            drawText(line, x, y, 0.30f, 140, 230, 140);
            if (st.players > 1 && (st.peerVisible || st.peerDistance > 0)) {
                std::snprintf(line, sizeof(line), "nearest: %s %.0f m%s%s  |  smoothing %d ms  [%s] teleport",
                              st.peerName.c_str(), st.peerDistance, st.peerDead ? "  |  DEAD" : "",
                              st.peerInMission ? "  |  in mission" : "", st.bufferMs, settings_.teleportKey.c_str());
                drawText(line, x, y += 0.026f, 0.28f, 220, 220, 220);
            }
            if (settings_.session.worldSync) {
                std::snprintf(line, sizeof(line),
                              st.worldOwner ? "World: %d streamed  |  mod %.2f ms, %.0f calls/frame"
                                            : "World: %d shown  |  mod %.2f ms, %.0f calls/frame",
                              st.syncedEntities, workUs_ / 1000.0, natives_);
                drawText(line, x, y += 0.026f, 0.28f, 220, 220, 220);
            }
            const char* storyLine = nullptr;
            if (st.holding) storyLine = "Waiting for the others - the world is paused";
            else if (st.spectating) storyLine = "Watching the world owner's cutscene";
            else if (st.peerInCutscene) storyLine = "Someone is in a cutscene";
            if (storyLine) drawText(storyLine, x, y += 0.026f, 0.28f, 240, 220, 120);
            if (st.missionMismatch)
                drawText("Different missions! Load the campaign save to play the story together", x, y += 0.026f, 0.28f,
                         255, 110, 90);
            if (st.saveProgress >= 0) {
                std::snprintf(line, sizeof(line), "Save transfer: %d%%", st.saveProgress);
                drawText(line, x, y += 0.026f, 0.28f, 240, 220, 120);
            }
        }
        if (!notice_.empty() && static_cast<int32_t>(noticeUntil_ - now) > 0)
            drawText(notice_, x, y + 0.03f, 0.32f, 255, 255, 255);
    }

    Settings settings_;
    Rdr2Game game_;
    Rdr2SaveStore saves_;
    UdpSocket socket_;
    std::unique_ptr<Link> link_;
    std::unique_ptr<CoopSession> session_;
    bool online_ = false;
    bool showHud_ = true;
    Link::State lastState_ = Link::State::Idle;
    Hotkey toggle_, teleport_, hud_, download_, upload_;
    std::string notice_;
    uint32_t noticeUntil_ = 0;
    double workUs_ = 0, natives_ = 0;
};

std::unique_ptr<App> g_app;

// Script Hook calls this on its script thread, again after every save load.
void scriptMain() {
    g_app = std::make_unique<App>();
    g_app->run();
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        scriptRegister(module, scriptMain);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_server.stop();
        scriptUnregister(module);
    }
    return TRUE;
}
