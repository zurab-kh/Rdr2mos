// Where the server keeps a campaign: save games (with history), the journal, players.
#pragma once
#include <cstdint>
#include <ctime>
#include <map>
#include <string>
#include <vector>
#include "bytes.h"

namespace coop {

class IServerStorage {
public:
    virtual ~IServerStorage() = default;
    virtual bool loadSave(const std::string& campaign, std::vector<uint8_t>& data) = 0;
    // True when the campaign has stored saves, readable or not. A campaign whose saves exist but
    // cannot be read (loadSave() false) is not a new one: nobody may start it over from their own save.
    virtual bool hasSaveFiles(const std::string& campaign) {
        (void)campaign;
        return false;
    }
    // When the save that the last loadSave() returned was stored, in Unix seconds (0: not known).
    virtual uint64_t loadedSaveTime(const std::string& campaign) {
        (void)campaign;
        return 0;
    }
    virtual bool storeSave(const std::string& campaign, const std::vector<uint8_t>& data, const std::string& by) = 0;
    virtual void journal(const std::string& campaign, const std::string& line) = 0;
    virtual void storePlayer(const std::string& campaign, const std::string& name, const Vec3& lastPos) = 0;
};

// Keeps everything in memory (tests, or a server started without a data folder).
class MemoryStorage : public IServerStorage {
public:
    bool loadSave(const std::string& campaign, std::vector<uint8_t>& data) override {
        auto it = saves.find(campaign);
        if (it == saves.end() || it->second.empty()) return false;
        data = it->second.back();
        return true;
    }
    uint64_t loadedSaveTime(const std::string& campaign) override {
        auto it = savedAt.find(campaign);
        return it == savedAt.end() ? 0 : it->second;
    }
    bool hasSaveFiles(const std::string& campaign) override {
        auto it = saves.find(campaign);
        return it != saves.end() && !it->second.empty();
    }
    bool storeSave(const std::string& campaign, const std::vector<uint8_t>& data, const std::string&) override {
        auto& history = saves[campaign];
        history.push_back(data);
        savedAt[campaign] = static_cast<uint64_t>(std::time(nullptr));
        // Saves can be large; a long-running server without a data folder keeps only recent ones.
        if (history.size() > keep) history.erase(history.begin());
        return true;
    }
    void journal(const std::string& campaign, const std::string& line) override { journals[campaign].push_back(line); }
    void storePlayer(const std::string& campaign, const std::string& name, const Vec3& lastPos) override {
        players[campaign][name] = lastPos;
    }

    size_t keep = 5;
    std::map<std::string, std::vector<std::vector<uint8_t>>> saves;
    std::map<std::string, uint64_t> savedAt;  // when the latest of each campaign was stored
    std::map<std::string, std::vector<std::string>> journals;
    std::map<std::string, std::map<std::string, Vec3>> players;
};

// On disk: <root>/<campaign>/saves/save-<number>-<time>.sav (highest number = latest), journal.log, players.ini.
class FileStorage : public IServerStorage {
public:
    explicit FileStorage(std::string root, size_t keepSaves = 20) : root_(std::move(root)), keep_(keepSaves) {}
    // The newest save that is whole and readable: an empty or broken newest file (a power cut, a
    // full disk) does not hide the older ones.
    bool loadSave(const std::string& campaign, std::vector<uint8_t>& data) override;
    bool hasSaveFiles(const std::string& campaign) override;
    uint64_t loadedSaveTime(const std::string& campaign) override;
    bool storeSave(const std::string& campaign, const std::vector<uint8_t>& data, const std::string& by) override;
    void journal(const std::string& campaign, const std::string& line) override;
    void storePlayer(const std::string& campaign, const std::string& name, const Vec3& lastPos) override;

private:
    std::map<std::string, uint64_t> loadedTime_;  // modification time of the file loadSave() returned
    std::string dir(const std::string& campaign) const;
    std::string root_;
    size_t keep_;
};

// Campaign names become folder names: keep letters, digits, '-' and '_' only (lower case, at most 32),
// and never a Windows device name.
std::string sanitizeCampaign(const std::string& name);

}  // namespace coop
