// Save games on disk: Documents\Rockstar Games\Red Dead Redemption 2\Profiles\<profile>\SRDR3nnnn
#pragma once
#include "game_api.h"
#include "save_files.h"

class Rdr2SaveStore final : public coop::ISaveStore {
public:
    bool readLatest(std::vector<uint8_t>& data, std::string& name) override;
    uint64_t latestStamp() override;
    uint64_t latestTime() override;
    bool writeCopy(const std::vector<uint8_t>& data, std::string& placedAs, std::string& error) override;

private:
    saves::SaveFolder& folder();
    saves::SaveFolder folder_;
    bool located_ = false;
};
