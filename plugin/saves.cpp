#include "saves.h"

#include <windows.h>
#include <shlobj.h>

#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace {

fs::path profilesRoot() {
    wchar_t documents[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, documents))) return {};
    return fs::path(documents) / L"Rockstar Games" / L"Red Dead Redemption 2" / L"Profiles";
}

// Story saves are named SRDR3 followed by four digits (SRDR30000..), without an extension.
bool isSaveName(const std::wstring& name) {
    if (name.size() != 9 || name.compare(0, 5, L"SRDR3") != 0) return false;
    for (size_t i = 5; i < name.size(); ++i)
        if (!std::iswdigit(name[i])) return false;
    return true;
}

// The newest save of any profile, and the profile it lives in.
bool findLatest(fs::path& file, fs::path& profile) {
    std::error_code ec;
    fs::path root = profilesRoot();
    if (root.empty() || !fs::is_directory(root, ec)) return false;
    fs::file_time_type best{};
    bool found = false;
    for (const auto& dir : fs::directory_iterator(root, ec)) {
        if (!dir.is_directory(ec)) continue;
        for (const auto& entry : fs::directory_iterator(dir.path(), ec)) {
            if (!entry.is_regular_file(ec) || !isSaveName(entry.path().filename().wstring())) continue;
            auto time = entry.last_write_time(ec);
            if (!found || time > best) {
                best = time;
                file = entry.path();
                profile = dir.path();
                found = true;
            }
        }
    }
    return found;
}

}  // namespace

bool Rdr2SaveStore::readLatest(std::vector<uint8_t>& data, std::string& name) {
    fs::path file, profile;
    if (!findLatest(file, profile)) return false;
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    name = file.filename().string();
    return !data.empty();
}

uint64_t Rdr2SaveStore::latestStamp() {
    fs::path file, profile;
    if (!findLatest(file, profile)) return 0;
    std::error_code ec;
    auto time = fs::last_write_time(file, ec);
    if (ec) return 0;
    uint64_t stamp = static_cast<uint64_t>(time.time_since_epoch().count());
    for (wchar_t c : file.filename().wstring()) stamp = stamp * 31 + static_cast<uint64_t>(c);
    return stamp ? stamp : 1;
}

bool Rdr2SaveStore::writeCopy(const std::vector<uint8_t>& data, std::string& placedAs, std::string& error) {
    std::error_code ec;
    fs::path file, profile;
    if (!findLatest(file, profile)) {
        // No save of our own yet: use the first profile folder there is.
        fs::path root = profilesRoot();
        if (!root.empty() && fs::is_directory(root, ec))
            for (const auto& dir : fs::directory_iterator(root, ec))
                if (dir.is_directory(ec)) {
                    profile = dir.path();
                    break;
                }
    }
    if (profile.empty()) {
        error = "RDR2 save folder not found (start the story once and save)";
        return false;
    }
    // Manual save slots; never overwrite an existing save.
    for (int slot = 0; slot < 15; ++slot) {
        wchar_t name[16];
        swprintf(name, 16, L"SRDR3%04d", slot);
        fs::path target = profile / name;
        if (fs::exists(target, ec)) continue;
        std::ofstream out(target, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) {
            error = "cannot write " + target.filename().string();
            return false;
        }
        placedAs = target.filename().string();
        return true;
    }
    error = "all 15 save slots are in use - delete one first";
    return false;
}
