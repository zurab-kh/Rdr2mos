// Where the server keeps a campaign: save games (with history), the journal, players.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "bytes.h"

namespace coop {

class IServerStorage {
public:
    virtual ~IServerStorage() = default;
    virtual bool loadSave(const std::string& campaign, std::vector<uint8_t>& data) = 0;
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
    bool storeSave(const std::string& campaign, const std::vector<uint8_t>& data, const std::string&) override {
        saves[campaign].push_back(data);
        return true;
    }
    void journal(const std::string& campaign, const std::string& line) override { journals[campaign].push_back(line); }
    void storePlayer(const std::string& campaign, const std::string& name, const Vec3& lastPos) override {
        players[campaign][name] = lastPos;
    }

    std::map<std::string, std::vector<std::vector<uint8_t>>> saves;
    std::map<std::string, std::vector<std::string>> journals;
    std::map<std::string, std::map<std::string, Vec3>> players;
};

// On disk: <root>/<campaign>/saves/save-<number>-<time>.sav (highest number = latest), journal.log, players.ini.
class FileStorage : public IServerStorage {
public:
    explicit FileStorage(std::string root, size_t keepSaves = 20) : root_(std::move(root)), keep_(keepSaves) {}
    bool loadSave(const std::string& campaign, std::vector<uint8_t>& data) override;
    bool storeSave(const std::string& campaign, const std::vector<uint8_t>& data, const std::string& by) override;
    void journal(const std::string& campaign, const std::string& line) override;
    void storePlayer(const std::string& campaign, const std::string& name, const Vec3& lastPos) override;

private:
    std::string dir(const std::string& campaign) const;
    std::string root_;
    size_t keep_;
};

// Campaign names become folder names: keep letters, digits, '-' and '_' only.
std::string sanitizeCampaign(const std::string& name);

}  // namespace coop
