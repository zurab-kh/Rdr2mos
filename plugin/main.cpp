// RDR2 Co-op: play Red Dead Redemption 2's story mode together.
// ScriptHookRDR2 plugin entry point: configuration, the optional built-in server, hotkeys, HUD
// and the per-frame loop.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "hud.h"
#include "link.h"
#include "natives.h"
#include "rdr2_game.h"
#include "saves.h"
#include "server.h"
#include "session.h"
#include "settings.h"

using namespace coop;

namespace {

HMODULE g_module = nullptr;
// Script Hook restarts the script after every save load; this survives it so the co-op session
// reconnects on its own instead of making the player press the key again.
bool g_reconnect = false;

std::string moduleDirectory() {
    char path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(g_module, path, MAX_PATH);
    std::string dir(path, n < MAX_PATH ? n : 0);
    size_t slash = dir.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : dir.substr(0, slash);
}

// Called from the script thread and from the built-in server's thread.
void logLine(const char* format, ...) {
    static std::mutex mutex;
    static const std::string path = moduleDirectory() + "\\RDR2Coop.log";
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof text, format, args);
    va_end(args);
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::lock_guard<std::mutex> lock(mutex);
    FILE* file = std::fopen(path.c_str(), "a");
    if (!file) return;
    std::fprintf(file, "[%02d:%02d:%02d] %s\n", t.wHour, t.wMinute, t.wSecond, text);
    std::fclose(file);
}

// Millisecond clock from the performance counter. GetTickCount() only advances every 10-16 ms,
// which would show up as visible stutter in the interpolation. Used by both threads.
uint32_t clockMs() {
    static const double ticksPerMs = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint32_t>(static_cast<uint64_t>(static_cast<double>(c.QuadPart) / ticksPerMs));
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
        bool down = vk_ && (GetAsyncKeyState(vk_) & 0x8000) && gameHasFocus();
        bool edge = down && !wasDown_;
        wasDown_ = down;
        return edge;
    }

private:
    int vk_;
    bool wasDown_ = false;
};

// The built-in server runs on its own thread: it needs nothing from the game, so it keeps
// serving the others even while this game sits in a loading screen (when scripts are paused).
// It shares nothing with the script thread but logLine() and clockMs().
class BuiltInServer {
public:
    bool start(const settings::Settings& s, std::string& error) {
        if (thread_.joinable()) return true;  // already running (survives script restarts)
        if (!socket_.open(static_cast<uint16_t>(s.port), &error)) return false;
        storage_ = std::make_unique<FileStorage>(moduleDirectory() + "\\RDR2Coop-server");
        server_ = std::make_unique<Server>(socket_, s.server, *storage_);
        stop_ = false;
        running_ = true;
        thread_ = std::thread([this] {
            uint32_t failures = 0;
            while (!stop_) {
                try {
                    socket_.waitReadable(5);
                    server_->pump(clockMs());
                    failures = 0;
                } catch (const std::exception& e) {
                    // A full disk or an unreadable campaign folder must not take the game down:
                    // log it, back off, keep serving.
                    if (failures++ < 5) logLine("built-in server error: %s", e.what());
                    Sleep(failures < 5 ? 50 : 1000);
                } catch (...) {
                    if (failures++ < 5) logLine("built-in server error");
                    Sleep(failures < 5 ? 50 : 1000);
                }
            }
            running_ = false;
        });
        logLine("built-in server running on UDP port %d", s.port);
        return true;
    }

    // From DllMain, under the loader lock: the thread can never be joined there (its exit needs
    // that lock).
    void stop(bool processExit) {
        stop_ = true;
        if (!thread_.joinable()) return;
        if (processExit) {
            // Windows has already killed the thread, perhaps in the middle of a file write:
            // nothing it owned may be touched any more, so it is abandoned, not destroyed.
            thread_.detach();
            (void)server_.release();
            (void)storage_.release();
            return;
        }
        // The DLL is being unloaded: wait for the thread to leave our code first.
        for (int i = 0; i < 200 && running_; ++i) Sleep(5);
        if (!running_) Sleep(20);  // let it get from the loop into the thread exit
        thread_.detach();
        if (running_) {  // stuck: leak it rather than free what it uses
            (void)server_.release();
            (void)storage_.release();
        }
    }

private:
    UdpSocket socket_;
    std::unique_ptr<FileStorage> storage_;
    std::unique_ptr<Server> server_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
};
BuiltInServer g_server;

class App {
public:
    void run() {
        settings_.load(moduleDirectory() + "\\RDR2Coop.ini");
        toggle_ = Hotkey(settings_.toggleKey.vk);
        teleport_ = Hotkey(settings_.teleportKey.vk);
        hud_ = Hotkey(settings_.hudKey.vk);
        download_ = Hotkey(settings_.downloadKey.vk);
        upload_ = Hotkey(settings_.uploadKey.vk);
        showHud_ = settings_.showHud;
        logLine("RDR2 Co-op started: server=%s campaign=%s%s%s",
                settings_.hostServer ? "built-in" : settings_.serverAddress.c_str(), settings_.campaign.c_str(),
                settings_.problem.empty() ? "" : " CONFIG ERROR: ", settings_.problem.c_str());
        for (const std::string& w : settings_.warnings) logLine("%s", w.c_str());
        uint32_t start = clockMs();
        if (!settings_.problem.empty()) notify(start, settings_.problem);
        else if (!settings_.warnings.empty()) notify(start, settings_.warnings.front());

        wantAutoConnect_ = settings_.autoConnect || g_reconnect;
        for (;;) {
            uint32_t now = clockMs();
            LARGE_INTEGER workStart;
            QueryPerformanceCounter(&workStart);
            uint32_t nativesBefore = native::g_nativeCalls;
            try {
                tick(now);
            } catch (const std::exception& e) {
                // Never let an error escape into Script Hook (that ends the game): drop the
                // session, say so, and keep the game running.
                logLine("error: %s", e.what());
                notify(now, "Co-op stopped after an error (see RDR2Coop.log) - press the toggle key to reconnect");
                safeDisconnect();
            } catch (...) {
                logLine("error");
                notify(now, "Co-op stopped after an error (see RDR2Coop.log)");
                safeDisconnect();
            }
            measureWork(workStart, native::g_nativeCalls - nativesBefore);
            if (showHud_) drawHud(now);
            scriptWait(0);
        }
    }

private:
    void tick(uint32_t now) {
        // Connecting happens before the game layer's frame, so a fresh session starts on a frame
        // that has all of the player's data (the offline frame only does the safety checks).
        if (wantAutoConnect_ && game_.ready()) {
            wantAutoConnect_ = false;
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
        game_.frame(now, online_);

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
    }

    void connect(uint32_t now) {
        if (!settings_.problem.empty()) {
            notify(now, settings_.problem);
            return;
        }
        std::string error;
        if (settings_.hostServer && !g_server.start(settings_, error)) {
            notify(now, "Cannot start the server: " + error);
            logLine("cannot start the built-in server: %s", error.c_str());
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

    void safeDisconnect() {
        try {
            disconnect();
        } catch (...) {
            session_.reset();
            link_.reset();
            socket_.close();
            online_ = false;
        }
        g_reconnect = false;
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

    void drawHud(uint32_t now) {
        const float x = 0.012f;
        float y = 0.012f;
        char line[256];
        const char* toggle = settings_.toggleKey.name.c_str();
        if (!online_) {
            std::snprintf(line, sizeof(line), "Co-op: offline  [%s] connect", toggle);
            hud::drawText(line, x, y, 0.30f, 200, 200, 200);
        } else if (link_->state() == Link::State::Failed) {
            hud::drawText("Co-op: " + link_->error(), x, y, 0.30f, 255, 110, 90);
        } else if (!session_->status().denied.empty()) {
            hud::drawText("Co-op: the server refused: " + session_->status().denied, x, y, 0.30f, 255, 110, 90);
        } else if (link_->state() != Link::State::Connected || !session_->status().welcomed) {
            std::snprintf(line, sizeof(line), "Co-op: connecting to %s...",
                          settings_.hostServer ? "the built-in server" : settings_.serverAddress.c_str());
            hud::drawText(line, x, y, 0.30f, 240, 220, 120);
        } else {
            const SessionStatus& st = session_->status();
            std::snprintf(line, sizeof(line), "Co-op \"%s\": %d player%s  |  world: %s  |  ping %d ms  |  up %.1f / down %.1f KB/s",
                          settings_.campaign.c_str(), st.players, st.players == 1 ? "" : "s",
                          st.worldOwner ? "you" : st.ownerName.c_str(), link_->pingMs() < 0 ? 0 : link_->pingMs(),
                          link_->sendRate() / 1024.0f, link_->receiveRate() / 1024.0f);
            hud::drawText(line, x, y, 0.30f, 140, 230, 140);
            if (st.players > 1 && (st.peerVisible || st.peerDistance > 0)) {
                std::snprintf(line, sizeof(line), "nearest: %s %.0f m%s%s  |  smoothing %d ms  [%s] teleport",
                              st.peerName.c_str(), st.peerDistance, st.peerDead ? "  |  DEAD" : "",
                              st.peerInMission ? "  |  in mission" : "", st.bufferMs, settings_.teleportKey.name.c_str());
                hud::drawText(line, x, y += 0.026f, 0.28f, 220, 220, 220);
            }
            if (settings_.session.worldSync) {
                std::snprintf(line, sizeof(line),
                              st.worldOwner ? "World: %d streamed  |  mod %.2f ms, %.0f calls/frame"
                                            : "World: %d shown  |  mod %.2f ms, %.0f calls/frame",
                              st.syncedEntities, workUs_ / 1000.0, natives_);
                hud::drawText(line, x, y += 0.026f, 0.28f, 220, 220, 220);
            }
            const char* storyLine = nullptr;
            if (st.holding) storyLine = "Waiting for the others - the world is paused";
            else if (st.spectating) storyLine = "Watching the world owner's cutscene";
            else if (st.peerInCutscene) storyLine = "Someone is in a cutscene";
            if (storyLine) hud::drawText(storyLine, x, y += 0.026f, 0.28f, 240, 220, 120);
            if (st.missionMismatch)
                hud::drawText("Different missions! Load the campaign save to play the story together", x, y += 0.026f,
                              0.28f, 255, 110, 90);
            if (st.saveProgress >= 0) {
                std::snprintf(line, sizeof(line), "Save transfer: %d%%", st.saveProgress);
                hud::drawText(line, x, y += 0.026f, 0.28f, 240, 220, 120);
            }
        }
        if (!notice_.empty() && static_cast<int32_t>(noticeUntil_ - now) > 0)
            hud::drawText(notice_, x, y + 0.03f, 0.32f, 255, 255, 255);
    }

    settings::Settings settings_;
    Rdr2Game game_;
    Rdr2SaveStore saves_;
    UdpSocket socket_;
    std::unique_ptr<Link> link_;
    std::unique_ptr<CoopSession> session_;
    bool online_ = false;
    bool showHud_ = true;
    bool wantAutoConnect_ = false;
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

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    static bool hooked = false;
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        hooked = scripthook::load();  // Script Hook loads .asi plugins, so it is already in the process
        if (hooked) scriptRegister(module, scriptMain);
        else logLine("ScriptHookRDR2.dll not found or too old: the mod stays inactive");
    } else if (reason == DLL_PROCESS_DETACH) {
        bool processExit = reserved != nullptr;  // the process ends (vs. FreeLibrary)
        g_server.stop(processExit);
        if (hooked) scriptUnregister(module);  // Script Hook loaded us, so it is detached after us
        if (processExit) (void)g_app.release();  // its script fiber is gone: nothing to tidy up
    }
    return TRUE;
}
