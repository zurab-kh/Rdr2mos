#include "server_storage.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace coop {

namespace {
std::string timestamp(bool forFile) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), forFile ? "%Y%m%d-%H%M%S" : "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}
}  // namespace

std::string sanitizeCampaign(const std::string& name) {
    std::string out;
    for (char c : name) {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '-' || c == '_') out += static_cast<char>(std::tolower(u));
        if (out.size() >= 32) break;
    }
    return out;
}

std::string FileStorage::dir(const std::string& campaign) const {
    fs::path p = fs::path(root_) / sanitizeCampaign(campaign);
    std::error_code ec;
    fs::create_directories(p / "saves", ec);
    return p.string();
}

bool FileStorage::loadSave(const std::string& campaign, std::vector<uint8_t>& data) {
    fs::path saves = fs::path(dir(campaign)) / "saves";
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(saves, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".sav") files.push_back(e.path());
    if (files.empty()) return false;
    std::sort(files.begin(), files.end());  // names carry the time: the last one is the newest
    std::ifstream in(files.back(), std::ios::binary);
    if (!in) return false;
    data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !data.empty();
}

bool FileStorage::storeSave(const std::string& campaign, const std::vector<uint8_t>& data, const std::string& by) {
    fs::path saves = fs::path(dir(campaign)) / "saves";
    std::error_code ec;
    // save-<number>-<time>.sav: the running number keeps the order right even within one second.
    std::vector<fs::path> files;
    unsigned long next = 1;
    for (const auto& e : fs::directory_iterator(saves, ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ".sav") continue;
        files.push_back(e.path());
        std::string stem = e.path().stem().string();  // save-000012-20261116-101500
        if (stem.size() > 11 && stem.compare(0, 5, "save-") == 0)
            next = std::max(next, std::strtoul(stem.c_str() + 5, nullptr, 10) + 1);
    }
    char name[64];
    std::snprintf(name, sizeof(name), "save-%06lu-%s.sav", next, timestamp(true).c_str());
    fs::path file = saves / name;
    {
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) return false;
    }
    files.push_back(file);
    // Keep a history, but not forever.
    std::sort(files.begin(), files.end());
    while (files.size() > keep_) {
        fs::remove(files.front(), ec);
        files.erase(files.begin());
    }
    journal(campaign, "save stored (" + std::to_string(data.size()) + " bytes, from " + by + ")");
    return true;
}

void FileStorage::journal(const std::string& campaign, const std::string& line) {
    std::ofstream out(fs::path(dir(campaign)) / "journal.log", std::ios::app);
    out << "[" << timestamp(false) << "] " << line << "\n";
}

void FileStorage::storePlayer(const std::string& campaign, const std::string& name, const Vec3& lastPos) {
    // players.ini: one line per player, rewritten in full (small file).
    fs::path file = fs::path(dir(campaign)) / "players.ini";
    std::map<std::string, std::string> lines;
    {
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line)) {
            size_t eq = line.find('=');
            if (eq != std::string::npos) lines[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "last_seen %s, position %.1f %.1f %.1f", timestamp(false).c_str(), lastPos.x,
                  lastPos.y, lastPos.z);
    lines[name] = buf;
    std::ofstream out(file, std::ios::trunc);
    for (const auto& [k, v] : lines) out << k << "=" << v << "\n";
}

}  // namespace coop
