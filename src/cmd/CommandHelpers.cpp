#include "molterm/cmd/CommandHelpers.h"

#include <cctype>
#include <cstdio>

#include "molterm/render/ColorMapper.h"
#include "molterm/render/PixelCanvas.h"

namespace molterm {

void trimWhitespace(std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) { s.clear(); return; }
    size_t b = s.find_last_not_of(" \t\r");
    s = s.substr(a, b - a + 1);
}

std::string joinArgs(const std::vector<std::string>& args, size_t lo, size_t hi) {
    std::string out;
    for (size_t i = lo; i < hi && i < args.size(); ++i) {
        if (i > lo) out += ' ';
        out += args[i];
    }
    return out;
}

std::pair<std::vector<std::string>, std::string>
splitAtToken(const std::vector<std::string>& args, std::string_view token) {
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == token) {
            return {std::vector<std::string>(args.begin(), args.begin() + i),
                    joinArgs(args, i + 1, args.size())};
        }
    }
    return {args, ""};
}

std::pair<std::vector<std::string>, std::string>
splitAtEqToken(const std::vector<std::string>& args) {
    return splitAtToken(args, "=");
}

bool isValidEnvName(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_'))
        return false;
    for (size_t k = 1; k < s.size(); ++k) {
        char c = s[k];
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
            return false;
    }
    return true;
}

bool isBareName(const std::string& s) {
    return !s.empty() &&
           (std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_') &&
           s.find_first_of(" \t") == std::string::npos;
}

std::optional<std::array<uint8_t, 3>> parseHexColor(std::string s) {
    auto trim = [](std::string& v) {
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.erase(v.begin());
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back())))  v.pop_back();
    };
    trim(s);
    if (s.empty()) return std::nullopt;

    // rgb(R,G,B) form: optional whitespace inside, decimal channels 0..255.
    if (s.size() > 4 &&
        (s[0] == 'r' || s[0] == 'R') && (s[1] == 'g' || s[1] == 'G') &&
        (s[2] == 'b' || s[2] == 'B') && s[3] == '(' && s.back() == ')') {
        std::string body = s.substr(4, s.size() - 5);
        int r = -1, g = -1, b = -1;
        if (std::sscanf(body.c_str(), " %d , %d , %d ", &r, &g, &b) != 3) return std::nullopt;
        if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) return std::nullopt;
        return std::array<uint8_t, 3>{static_cast<uint8_t>(r),
                                      static_cast<uint8_t>(g),
                                      static_cast<uint8_t>(b)};
    }

    // #RGB / #RRGGBB form.
    if (s[0] != '#') return std::nullopt;
    std::string hex = s.substr(1);
    auto fromHex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    if (hex.size() == 3) {
        int r = fromHex(hex[0]), g = fromHex(hex[1]), b = fromHex(hex[2]);
        if (r < 0 || g < 0 || b < 0) return std::nullopt;
        return std::array<uint8_t, 3>{static_cast<uint8_t>(r * 17),
                                      static_cast<uint8_t>(g * 17),
                                      static_cast<uint8_t>(b * 17)};
    }
    if (hex.size() == 6) {
        int rh = fromHex(hex[0]), rl = fromHex(hex[1]);
        int gh = fromHex(hex[2]), gl = fromHex(hex[3]);
        int bh = fromHex(hex[4]), bl = fromHex(hex[5]);
        if (rh < 0 || rl < 0 || gh < 0 || gl < 0 || bh < 0 || bl < 0) return std::nullopt;
        return std::array<uint8_t, 3>{static_cast<uint8_t>((rh << 4) | rl),
                                      static_cast<uint8_t>((gh << 4) | gl),
                                      static_cast<uint8_t>((bh << 4) | bl)};
    }
    return std::nullopt;
}

std::optional<std::array<uint8_t, 3>> parseColorSpec(const std::string& s) {
    if (s.empty()) return std::nullopt;
    if (auto rgb = parseHexColor(s)) return rgb;
    int pair = ColorMapper::colorByName(s);
    if (pair > 0) {
        auto rgb = PixelCanvas::colorPairToRGB(pair);
        return std::array<uint8_t, 3>{rgb.r, rgb.g, rgb.b};
    }
    return std::nullopt;
}

}  // namespace molterm
