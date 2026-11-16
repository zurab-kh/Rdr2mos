#include "server_storage.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "protocol.h"

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


// save-<number>-<time>.sav -> number (0 for anything else).
unsigned long saveNumber(const fs::path& file) {
    std::string stem = file.stem().string();
    if (stem.size() <= 5 || stem.compare(0, 5, "save-") != 0) return 0;
    return std::strtoul(stem.c_str() + 5, nullptr, 10);
}

// The stored saves, oldest first: by running number (the name alone would put save-1000000 first).
std::vector<fs::path> listSaves(const fs::path& saves) {
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(saves, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".sav") files.push_back(e.path());
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
        unsigned long na = saveNumber(a), nb = saveNumber(b);
        return na != nb ? na < nb : a < b;
    });
    return files;
}
}  // namespace

std::string sanitizeCampaign(const std::string& name) {
    std::string out;
    for (char c : name) {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '-' || c == '_') out += static_cast<char>(std::tolower(u));
        if (out.size() >= 32) break;
    }
    // Windows cannot create folders with device names (con, nul, com1, ...): keep them apart.
    static const char* const reserved[] = {"con", "prn", "aux", "nul"};
    bool device = std::any_of(std::begin(reserved), std::end(reserved), [&](const char* r) { return out == r; });
    if (out.size() == 4 && (out.compare(0, 3, "com") == 0 || out.compare(0, 3, "lpt") == 0) && out[3] >= '1' &&
        out[3] <= '9')
        device = true;
    if (device) out += '_';
    return out;
}

std::string FileStorage::dir(const std::string& campaign) const {
    fs::path p = fs::path(root_) / sanitizeCampaign(campaign);
    std::error_code ec;
    fs::create_directories(p / "saves", ec);
    return p.string();
}

bool FileStorage::loadSave(const std::string& campaign, std::vector<uint8_t>& data) {
    std::vector<fs::path> files = listSaves(fs::path(dir(campaign)) / "saves");
    if (files.empty()) return false;
    std::error_code ec;
    uintmax_t size = fs::file_size(files.back(), ec);
    if (ec || size == 0 || size > kMaxSaveBytes) return false;  // not something a player could load
    std::ifstream in(files.back(), std::ios::binary);
    if (!in) return false;
    data.resize(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    if (in.gcount() != static_cast<std::streamsize>(size)) {
        data.clear();
        return false;
    }
    return true;
}

bool FileStorage::storeSave(const std::string& campaign, const std::vector<uint8_t>& data, const std::string& by) {
    fs::path saves = fs::path(dir(campaign)) / "saves";
    std::error_code ec;
    // save-<number>-<time>.sav: the running number keeps the order right even within one second.
    std::vector<fs::path> files = listSaves(saves);
    unsigned long next = files.empty() ? 1 : saveNumber(files.back()) + 1;
    char name[64];
    std::snprintf(name, sizeof(name), "save-%06lu-%s.sav", next, timestamp(true).c_str());
    fs::path file = saves / name;
    // Written aside and renamed when complete: a crash or a full disk mid-write must not leave a
    // truncated file that would become the campaign save everyone downloads.
    fs::path part = file;
    part += ".part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) {
            out.close();
            fs::remove(part, ec);
            return false;
        }
    }
    fs::rename(part, file, ec);
    if (ec) {
        fs::remove(part, ec);
        return false;
    }
    files.push_back(file);
    // Keep a history, but not forever.
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
