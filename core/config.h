// Tiny INI reader for RDR2Coop.ini.
#pragma once
#include <map>
#include <string>

namespace coop {

class IniFile {
public:
    bool load(const std::string& path);
    void parse(const std::string& text);

    std::string get(const std::string& section, const std::string& key, const std::string& fallback = "") const;
    int getInt(const std::string& section, const std::string& key, int fallback) const;
    float getFloat(const std::string& section, const std::string& key, float fallback) const;
    bool getBool(const std::string& section, const std::string& key, bool fallback) const;

private:
    std::map<std::string, std::string> values_;  // "section.key" lower-cased -> value
};

}  // namespace coop
