// The character menu: every character of the catalog the game has, which ones the other players
// of the campaign have taken, and ours. Portable (no Windows or game calls), so tests can drive it.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "characters.h"
#include "session.h"

namespace coop {

class CharacterMenu {
public:
    struct Item {
        std::string avatar, name, takenBy;  // takenBy: another player has it
        bool mine = false;
    };

    // `exists`: whether the game has a model (the catalog names a few the game may not know).
    void open(const SessionStatus& status, const std::function<bool(const std::string&)>& exists) {
        items_.clear();
        for (const CharacterInfo& c : characterCatalog()) {
            if (exists && !exists(c.model)) continue;
            Item item{c.model, c.name, "", normalizeAvatar(status.character) == c.model};
            for (const SessionStatus::Taken& t : status.characters)
                if (!item.mine && normalizeAvatar(t.avatar) == c.model) item.takenBy = t.by;
            items_.push_back(item);
        }
        cursor_ = 0;
        for (size_t i = 0; i < items_.size(); ++i)
            if (items_[i].mine) cursor_ = i;
        open_ = !items_.empty();
    }
    void close() { open_ = false; }
    bool isOpen() const { return open_; }

    void move(int step) {
        if (items_.empty()) return;
        int n = static_cast<int>(items_.size());
        cursor_ = static_cast<size_t>(((static_cast<int>(cursor_) + step) % n + n) % n);
    }
    // The character to ask the server for, or "" (ours already, or someone else's).
    std::string pick(std::string& why) const {
        if (!open_ || cursor_ >= items_.size()) return {};
        const Item& item = items_[cursor_];
        if (item.mine) {
            why = "You already are " + item.name;
            return {};
        }
        if (!item.takenBy.empty()) {
            why = item.name + " is taken by " + item.takenBy;
            return {};
        }
        return item.avatar;
    }

    // A window of `rows` lines around the cursor, for drawing.
    std::vector<std::string> lines(size_t rows) const {
        std::vector<std::string> out;
        if (!open_) return out;
        size_t first = cursor_ >= rows / 2 ? cursor_ - rows / 2 : 0;
        if (first + rows > items_.size()) first = items_.size() > rows ? items_.size() - rows : 0;
        for (size_t i = first; i < items_.size() && out.size() < rows; ++i) {
            const Item& item = items_[i];
            std::string line = (i == cursor_ ? "> " : "  ") + item.name;
            if (item.mine) line += "  (you)";
            else if (!item.takenBy.empty()) line += "  (" + item.takenBy + ")";
            out.push_back(line);
        }
        return out;
    }
    const std::vector<Item>& items() const { return items_; }
    size_t cursor() const { return cursor_; }

private:
    std::vector<Item> items_;
    size_t cursor_ = 0;
    bool open_ = false;
};

}  // namespace coop
