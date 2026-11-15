#include "config.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace coop {

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool IniFile::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream buffer;
    buffer << in.rdbuf();
    parse(buffer.str());
    return true;
}

void IniFile::parse(const std::string& text) {
    std::istringstream in(text);
    std::string line, section;
    bool first = true;
    while (std::getline(in, line)) {
        if (first && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);  // UTF-8 BOM from Notepad
        first = false;
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string value = trim(line.substr(eq + 1));
        size_t comment = value.find(" ;");
        if (comment != std::string::npos) value = trim(value.substr(0, comment));
        values_[section + "." + lower(trim(line.substr(0, eq)))] = value;
    }
}

std::string IniFile::get(const std::string& section, const std::string& key, const std::string& fallback) const {
    auto it = values_.find(lower(section) + "." + lower(key));
    return it == values_.end() || it->second.empty() ? fallback : it->second;
}
int IniFile::getInt(const std::string& section, const std::string& key, int fallback) const {
    std::string v = get(section, key);
    return v.empty() ? fallback : std::atoi(v.c_str());
}
float IniFile::getFloat(const std::string& section, const std::string& key, float fallback) const {
    std::string v = get(section, key);
    return v.empty() ? fallback : static_cast<float>(std::atof(v.c_str()));
}
bool IniFile::getBool(const std::string& section, const std::string& key, bool fallback) const {
    std::string v = lower(get(section, key));
    if (v.empty()) return fallback;
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

}  // namespace coop
