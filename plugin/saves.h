// Save games on disk: Documents\Rockstar Games\Red Dead Redemption 2\Profiles\<profile>\SRDR3nnnn
#pragma once
#include "game_api.h"

class Rdr2SaveStore final : public coop::ISaveStore {
public:
    bool readLatest(std::vector<uint8_t>& data, std::string& name) override;
    bool writeCopy(const std::vector<uint8_t>& data, std::string& placedAs, std::string& error) override;
};
