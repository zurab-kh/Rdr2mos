// Story save files: Documents\Rockstar Games\Red Dead Redemption 2\Profiles\<profile>\SRDR3nnnn.
// Portable (std::filesystem only, every call with an error code), so the tests can check it on a
// temporary folder. plugin/saves.cpp points it at the real profiles folder.
#pragma once
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "protocol.h"

namespace saves {

namespace fs = std::filesystem;

constexpr int kSlots = 15;  // SRDR30000..SRDR30014: the manual save slots

// Story saves are named SRDR3 followed by four digits, without an extension.
inline bool isSaveName(const fs::path& file) {
    const fs::path::string_type name = file.filename().native();
    static const char kPrefix[] = "SRDR3";
    if (name.size() != 9) return false;
    for (size_t i = 0; i < 5; ++i)
        if (name[i] != static_cast<typename fs::path::value_type>(kPrefix[i])) return false;
    for (size_t i = 5; i < 9; ++i)
        if (name[i] < '0' || name[i] > '9') return false;
    return true;
}

// A profile folder is named with eight hex digits; anything else (a user's "backup" folder) is
// only used when there is nothing better.
inline bool looksLikeProfile(const fs::path& dir) {
    const fs::path::string_type name = dir.filename().native();
    if (name.size() != 8) return false;
    for (auto c : name)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    return true;
}

struct Found {
    fs::path file, profile;
    fs::file_time_type time{};
    uintmax_t size = 0;
};

// Iterates a directory without ever throwing: unreadable entries and folders are skipped.
template <typename F>
void eachEntry(const fs::path& dir, F&& f) {
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) f(*it);
}

// The newest save of any profile, and the profile it lives in.
inline bool findLatest(const fs::path& root, Found& out) {
    bool found = false;
    eachEntry(root, [&](const fs::directory_entry& dir) {
        std::error_code ec;
        if (!dir.is_directory(ec)) return;
        eachEntry(dir.path(), [&](const fs::directory_entry& entry) {
            std::error_code e;
            if (!isSaveName(entry.path()) || !entry.is_regular_file(e)) return;
            auto time = entry.last_write_time(e);
            if (e) return;
            uintmax_t size = entry.file_size(e);
            if (e) return;
            if (!found || time > out.time) {
                out.file = entry.path();
                out.profile = dir.path();
                out.time = time;
                out.size = size;
                found = true;
            }
        });
    });
    return found;
}

// Where a copy goes when there is no save yet: a profile-looking folder first, the newest first.
inline fs::path pickProfile(const fs::path& root) {
    fs::path best;
    bool bestLooks = false;
    fs::file_time_type bestTime{};
    eachEntry(root, [&](const fs::directory_entry& dir) {
        std::error_code ec;
        if (!dir.is_directory(ec)) return;
        bool looks = looksLikeProfile(dir.path());
        auto time = dir.last_write_time(ec);
        if (ec) time = {};
        if (best.empty() || (looks && !bestLooks) || (looks == bestLooks && time > bestTime)) {
            best = dir.path();
            bestLooks = looks;
            bestTime = time;
        }
    });
    return best;
}

inline uint64_t stampOf(const Found& f) {
    uint64_t stamp = static_cast<uint64_t>(f.time.time_since_epoch().count());
    stamp = stamp * 1000003u + static_cast<uint64_t>(f.size);
    const fs::path::string_type name = f.file.filename().native();
    for (auto c : name) stamp = stamp * 31 + static_cast<uint64_t>(c);
    return stamp ? stamp : 1;
}

class SaveFolder {
public:
    explicit SaveFolder(fs::path root = {}) : root_(std::move(root)) {}
    void setRoot(fs::path root) { root_ = std::move(root); }
    const fs::path& root() const { return root_; }

    // A save written less than this long ago may still be in the middle of being written.
    std::chrono::milliseconds settleTime{2000};

    bool readLatest(std::vector<uint8_t>& data, std::string& name) {
        Found f;
        if (root_.empty() || !findLatest(root_, f) || f.size == 0 || f.size > coop::kMaxSaveBytes) return false;
        std::ifstream in(f.file, std::ios::binary);
        if (!in) return false;
        data.resize(static_cast<size_t>(f.size));
        in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (in.gcount() != static_cast<std::streamsize>(data.size())) {  // shrank while we read it
            data.clear();
            return false;
        }
        name = narrow(f.file.filename());
        return true;
    }

    // Changes when a newer save appears, but only once it is complete: a save still being written
    // (younger than settleTime and changed since the last call) keeps the previous answer, so the
    // story never uploads half a file. Called every few seconds.
    uint64_t latestStamp() {
        Found f;
        if (root_.empty() || !findLatest(root_, f)) return settled_ = pending_ = 0;
        uint64_t stamp = stampOf(f);
        if (stamp == settled_) return stamp;
        bool old = fs::file_time_type::clock::now() - f.time >= settleTime;
        if (old || stamp == pending_ || stamp == ownWrite_) {
            settled_ = stamp;
            pending_ = 0;
        } else {
            pending_ = stamp;
        }
        return settled_;
    }

    // Stores a copy in the first free manual slot of the newest profile, never overwriting a file:
    // the data goes to a temporary file first and is then linked to a name that must not exist
    // yet, so neither a crash nor the game saving at the same moment can leave a broken or lost save.
    bool writeCopy(const std::vector<uint8_t>& data, std::string& placedAs, std::string& error) {
        std::error_code ec;
        if (root_.empty() || !fs::is_directory(root_, ec)) {
            error = "RDR2 save folder not found (start the story once and save)";
            return false;
        }
        Found latest;
        fs::path profile = findLatest(root_, latest) ? latest.profile : pickProfile(root_);
        if (profile.empty()) {
            error = "RDR2 save folder not found (start the story once and save)";
            return false;
        }
        const fs::path temp = profile / "RDR2Coop-incoming.tmp";
        fs::remove(temp, ec);  // a leftover from a crash
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            out.close();  // the write is only complete (or known to have failed) once flushed
            if (!out || fs::file_size(temp, ec) != data.size() || ec) {
                fs::remove(temp, ec);
                error = "cannot write to the save folder (disk full or no permission?)";
                return false;
            }
        }
        for (int slot = 0; slot < kSlots; ++slot) {
            char name[16];
            std::snprintf(name, sizeof name, "SRDR3%04d", slot);
            fs::path target = profile / name;
            if (fs::exists(target, ec) || ec) continue;
            std::error_code linkError;
            fs::create_hard_link(temp, target, linkError);  // fails if the name exists by now
            if (linkError) {
                // No hard links on this drive (FAT): move it, after checking once more.
                if (fs::exists(target, ec) || ec) continue;
                fs::rename(temp, target, linkError);
                if (linkError) continue;
            }
            fs::remove(temp, ec);
            Found mine;
            mine.file = target;
            mine.time = fs::last_write_time(target, ec);
            mine.size = data.size();
            if (!ec) ownWrite_ = stampOf(mine);  // our own copy is complete: no need to wait for it
            placedAs = name;
            return true;
        }
        fs::remove(temp, ec);
        error = "all 15 save slots are in use - delete one first";
        return false;
    }

private:
    static std::string narrow(const fs::path& p) {
        std::string out;
        for (auto c : p.native()) out += c >= 32 && c < 127 ? static_cast<char>(c) : '?';
        return out;
    }

    fs::path root_;
    uint64_t settled_ = 0, pending_ = 0, ownWrite_ = 0;
};

}  // namespace saves
