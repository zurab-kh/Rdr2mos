// On-screen text: what is safe to hand to the game, and how a HUD line is drawn.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "natives.h"

namespace hud {

// Longest line drawn; longer text (a long server error, a notice with a long name) is cut.
constexpr size_t kMaxLineBytes = 120;

// Text that reaches the screen comes from the network (player names, server messages) and from the
// ini. The game treats '~' as the start of a formatting code (~n~, ~COLOR_RED~, ~INPUT_...~), so it
// is replaced, as are control characters and broken UTF-8. Long text is cut on a character
// boundary and ends in "...". The result is never longer than maxBytes.
inline std::string printable(const std::string& text, size_t maxBytes = kMaxLineBytes) {
    std::string out;
    out.reserve(text.size() < maxBytes ? text.size() : maxBytes);
    const size_t n = text.size();
    size_t i = 0;
    bool cut = false;
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        bool valid = len != 0 && i + len <= n;
        for (size_t k = 1; valid && k < len; ++k) valid = (static_cast<unsigned char>(text[i + k]) & 0xC0) == 0x80;
        if (valid && len == 2 && c < 0xC2) valid = false;  // overlong
        if (valid && len == 3) {
            unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
            if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0)) valid = false;  // overlong, surrogate
        }
        if (valid && len == 4) {
            unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
            if ((c == 0xF0 && c1 < 0x90) || c > 0xF4 || (c == 0xF4 && c1 >= 0x90)) valid = false;
        }
        std::string piece;
        if (!valid) {
            piece = "?";
            len = 1;
        } else if (len == 1) {
            piece = (c < 0x20 || c == 0x7F) ? std::string(" ") : c == '~' ? std::string("-") : std::string(1, static_cast<char>(c));
        } else {
            piece = text.substr(i, len);
        }
        if (out.size() + piece.size() > maxBytes) {
            cut = true;
            break;
        }
        out += piece;
        i += len;
    }
    if (cut && maxBytes >= 3) {
        // Make room for the ellipsis without splitting a character.
        while (!out.empty() && out.size() + 3 > maxBytes) {
            size_t back = out.size() - 1;
            while (back > 0 && (static_cast<unsigned char>(out[back]) & 0xC0) == 0x80) --back;
            out.resize(back);
        }
        out += "...";
    }
    return out;
}

// One line of HUD text for this frame (the game draws it once; call again every frame).
inline void drawText(const std::string& text, float x, float y, float scale, int r, int g, int b) {
    const std::string safe = printable(text);  // must outlive the native calls that read it
    native::BG_SET_TEXT_SCALE(scale, scale);
    native::BG_SET_TEXT_COLOR(r, g, b, 235);
    native::BG_DISPLAY_TEXT(native::LITERAL_STRING(safe.c_str()), x, y);
}

}  // namespace hud
