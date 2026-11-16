#include "saves.h"

#include <windows.h>
#include <shlobj.h>

namespace {

std::filesystem::path profilesRoot() {
    wchar_t documents[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, documents))) return {};
    return std::filesystem::path(documents) / L"Rockstar Games" / L"Red Dead Redemption 2" / L"Profiles";
}

}  // namespace

saves::SaveFolder& Rdr2SaveStore::folder() {
    if (!located_) {  // the Documents folder can be redirected (OneDrive): ask Windows, once found
        std::filesystem::path root = profilesRoot();
        located_ = !root.empty();
        folder_.setRoot(root);
    }
    return folder_;
}

bool Rdr2SaveStore::readLatest(std::vector<uint8_t>& data, std::string& name) { return folder().readLatest(data, name); }

uint64_t Rdr2SaveStore::latestStamp() { return folder().latestStamp(); }

bool Rdr2SaveStore::writeCopy(const std::vector<uint8_t>& data, std::string& placedAs, std::string& error) {
    return folder().writeCopy(data, placedAs, error);
}
