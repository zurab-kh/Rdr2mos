// RDR2Coop.ini: the plugin's settings, checked so a typo shows up as a message instead of odd
// behaviour (a NaN radius, a key that silently does nothing, a name that breaks the HUD).
// Portable (no Windows headers), so the tests can check it.
#pragma once
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "config.h"
#include "hud.h"
#include "server.h"
#include "session_config.h"

namespace settings {

constexpr size_t kMaxNameBytes = 24;  // what the server keeps of a name

// "F7", "K", "5", "NUMPAD3", "INSERT" ... -> Windows virtual key code (0 = unknown or empty).
inline int parseKey(std::string name) {
    for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c); }), name.end());
    if (name.size() == 1 && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9'))) return name[0];
    // The number after a prefix ("F12" -> 12), -1 unless it is all digits and within lo..hi.
    auto number = [&](const char* prefix, int lo, int hi) {
        size_t from = std::char_traits<char>::length(prefix);
        if (name.rfind(prefix, 0) != 0 || name.size() <= from || name.size() > from + 2) return -1;
        for (size_t i = from; i < name.size(); ++i)
            if (!std::isdigit(static_cast<unsigned char>(name[i]))) return -1;
        int v = std::atoi(name.c_str() + from);
        return v >= lo && v <= hi ? v : -1;
    };
    if (int f = number("F", 1, 24); f >= 0) return 0x70 + f - 1;       // VK_F1..VK_F24
    if (int d = number("NUMPAD", 0, 9); d >= 0) return 0x60 + d;     // VK_NUMPAD0..9
    static const struct { const char* name; int vk; } kNamed[] = {
        {"INSERT", 0x2D}, {"DELETE", 0x2E}, {"HOME", 0x24}, {"END", 0x23}, {"PAGEUP", 0x21}, {"PAGEDOWN", 0x22},
        {"PAUSE", 0x13}, {"SCROLLLOCK", 0x91},
    };
    for (const auto& k : kNamed)
        if (name == k.name) return k.vk;
    return 0;
}

// Whole-string number parsing: "27960x" or "abc" is an error, not 27960 or 0.
inline bool parseInt(const std::string& text, long& out) {
    if (text.empty()) return false;
    errno = 0;
    char* end = nullptr;
    long v = std::strtol(text.c_str(), &end, 10);
    if (errno || !end || *end) return false;
    out = v;
    return true;
}
inline bool parseFloat(const std::string& text, float& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    float v = std::strtof(text.c_str(), &end);
    if (!end || *end || !std::isfinite(v)) return false;
    out = v;
    return true;
}

struct Key {
    std::string name;
    int vk = 0;
};

struct Settings {
    std::string serverAddress;
    bool hostServer = false;
    int port = coop::kDefaultPort;
    std::string campaign = "default";
    std::string password;
    bool wantsWorld = false;
    std::string name = "Player";
    std::string avatar = "player_zero";
    bool autoConnect = false;
    bool showHud = true;
    coop::SessionConfig session;
    coop::ServerConfig server;  // used by the built-in server
    Key toggleKey{"F7", 0}, teleportKey{"F8", 0}, hudKey{"F9", 0}, downloadKey{"F10", 0}, uploadKey{"F11", 0};
    std::string problem;                // configuration error: shown on screen, connecting is refused
    std::vector<std::string> warnings;  // fixed up with a default: shown once

    bool load(const std::string& path) {
        coop::IniFile ini;
        if (!ini.load(path)) {
            problem = "RDR2Coop.ini not found next to RDR2Coop.asi";
            apply(ini);  // defaults, so the HUD and the keys still work
            return false;
        }
        apply(ini);
        return true;
    }

    void apply(const coop::IniFile& ini) {
        serverAddress = trimmed(ini.get("network", "server"));
        hostServer = ini.getBool("network", "host_server", false);
        long portValue = coop::kDefaultPort;
        std::string portText = ini.get("network", "port");
        if (!portText.empty() && (!parseInt(portText, portValue) || portValue <= 0 || portValue > 65535)) {
            fail("RDR2Coop.ini: bad port '" + portText + "'");
            portValue = coop::kDefaultPort;
        }
        port = static_cast<int>(portValue);
        campaign = trimmed(ini.get("network", "campaign", "default"));
        if (campaign.empty()) campaign = "default";
        password = ini.get("network", "password");
        autoConnect = ini.getBool("network", "auto_connect", false);
        wantsWorld = ini.getBool("network", "world_owner", hostServer);
        if (!hostServer && serverAddress.empty()) fail("RDR2Coop.ini: set server = <address> or host_server = 1");

        // The name is shown in other players' games: one line, no formatting codes, whole characters.
        std::string rawName = ini.get("player", "name", "Player");
        rawName.erase(std::remove(rawName.begin(), rawName.end(), '='), rawName.end());
        name = trimmed(hud::printable(trimmed(rawName), kMaxNameBytes));
        if (name.empty()) name = "Player";
        const std::string defaultAvatar = wantsWorld ? "player_zero" : "player_three";
        avatar = trimmed(ini.get("player", "avatar", defaultAvatar));
        bool modelName = !avatar.empty() && avatar.size() <= 48 &&
                         std::all_of(avatar.begin(), avatar.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
        if (!modelName) {
            warn("RDR2Coop.ini: avatar '" + hud::printable(avatar, 48) + "' is not a model name, using " + defaultAvatar);
            avatar = defaultAvatar;
        }

        session.worldSync = ini.getBool("sync", "world_sync", true);
        session.friendlyFire = ini.getBool("sync", "friendly_fire", false);
        session.syncClock = ini.getBool("sync", "sync_clock", true);
        session.syncWeather = ini.getBool("sync", "sync_weather", true);
        session.softCorrection = ini.getBool("sync", "soft_correction", true);
        session.npcRadius = number(ini, "sync", "radius", 150.0f, 30.0f, 400.0f);
        session.maxPeds = integer(ini, "sync", "max_peds", 64, 0, 200);
        session.maxVehicles = integer(ini, "sync", "max_vehicles", 16, 0, 64);
        session.autoRegroup = ini.getBool("story", "auto_regroup", true);
        session.spectate = ini.getBool("story", "spectate", true);
        session.syncWaypoints = ini.getBool("story", "sync_waypoints", true);
        session.autoUploadSaves = ini.getBool("story", "auto_upload_saves", true);

        std::string mode = ini.get("server", "mode", "shared");
        for (char& c : mode) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (mode == "shared") server.storyMode = coop::StoryMode::Shared;
        else if (mode == "host") server.storyMode = coop::StoryMode::HostWorld;
        else if (hostServer) fail("RDR2Coop.ini: [server] mode must be shared or host");
        server.password = password;
        server.maxPlayers = integer(ini, "server", "max_players", 8, 1, static_cast<int>(coop::kMaxPlayers));
        server.cutsceneBarrier = ini.getBool("server", "cutscene_barrier", true);
        server.barrierTimeoutMs = static_cast<uint32_t>(integer(ini, "server", "barrier_timeout", 120, 10, 1800)) * 1000u;
        server.sharedFail = ini.getBool("server", "shared_fail", true);
        server.regroupDistance = number(ini, "server", "regroup_distance", 150.0f, 20.0f, 5000.0f);

        Key* keys[] = {&toggleKey, &teleportKey, &hudKey, &downloadKey, &uploadKey};
        const char* entries[] = {"toggle", "teleport_to_friend", "hud", "download_campaign_save", "upload_save"};
        const char* defaults[] = {"F7", "F8", "F9", "F10", "F11"};
        for (int k = 0; k < 5; ++k) {
            Key& key = *keys[k];
            key.name = trimmed(ini.get("keys", entries[k], defaults[k]));
            key.vk = parseKey(key.name);
            if (!key.vk) {
                warn("RDR2Coop.ini: [keys] " + std::string(entries[k]) + " = '" + hud::printable(key.name, 16) +
                     "' is not a key (F1..F24, A..Z, 0..9, NUMPAD0..9, INSERT, HOME ...): using " + defaults[k]);
                key.name = defaults[k];
                key.vk = parseKey(key.name);
            }
            for (int j = 0; j < k; ++j)
                if (keys[j]->vk == key.vk) {
                    warn("RDR2Coop.ini: [keys] " + std::string(entries[k]) + " uses the same key as " + entries[j] +
                         ": it is disabled");
                    key.vk = 0;
                }
        }
        showHud = ini.getBool("hud", "show", true);
    }

private:
    static std::string trimmed(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return {};
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }
    void fail(const std::string& text) {
        if (problem.empty()) problem = text;
    }
    void warn(const std::string& text) { warnings.push_back(text); }
    float number(const coop::IniFile& ini, const char* section, const char* key, float fallback, float lo, float hi) {
        std::string text = ini.get(section, key);
        float v = fallback;
        if (!text.empty() && !parseFloat(text, v)) {
            warn("RDR2Coop.ini: " + std::string(key) + " = '" + hud::printable(text, 16) + "' is not a number");
            v = fallback;
        }
        return std::clamp(v, lo, hi);
    }
    int integer(const coop::IniFile& ini, const char* section, const char* key, int fallback, int lo, int hi) {
        std::string text = ini.get(section, key);
        long v = fallback;
        if (!text.empty() && !parseInt(text, v)) {
            warn("RDR2Coop.ini: " + std::string(key) + " = '" + hud::printable(text, 16) + "' is not a whole number");
            v = fallback;
        }
        return static_cast<int>(std::clamp<long>(v, lo, hi));
    }
};

}  // namespace settings
