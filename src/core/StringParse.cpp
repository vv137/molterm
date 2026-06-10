#include "molterm/core/StringParse.h"

#include <cerrno>
#include <cstdlib>
#include <limits>

namespace molterm {

namespace {

// Shared tail check for the strto* family: reject a parse that consumed
// nothing (no digits) or left non-whitespace characters behind, and surface
// ERANGE as failure. `begin` is the original c_str, `end` is strto*'s output.
bool fullyConsumed(const char* begin, const char* end) {
    if (end == begin) return false;          // no conversion happened
    while (*end == ' ' || *end == '\t' || *end == '\n' ||
           *end == '\r' || *end == '\f' || *end == '\v') ++end;
    return *end == '\0';                      // only trailing whitespace allowed
}

}  // namespace

std::optional<long> parseLong(const std::string& s) {
    if (s.empty()) return std::nullopt;
    const char* begin = s.c_str();
    char* end = nullptr;
    errno = 0;
    long v = std::strtol(begin, &end, 10);
    if (errno == ERANGE || !fullyConsumed(begin, end)) return std::nullopt;
    return v;
}

std::optional<int> parseInt(const std::string& s) {
    auto v = parseLong(s);
    if (!v) return std::nullopt;
    if (*v < std::numeric_limits<int>::min() ||
        *v > std::numeric_limits<int>::max()) return std::nullopt;
    return static_cast<int>(*v);
}

std::optional<double> parseDouble(const std::string& s) {
    if (s.empty()) return std::nullopt;
    const char* begin = s.c_str();
    char* end = nullptr;
    errno = 0;
    double v = std::strtod(begin, &end);
    if (errno == ERANGE || !fullyConsumed(begin, end)) return std::nullopt;
    return v;
}

std::optional<float> parseFloat(const std::string& s) {
    if (s.empty()) return std::nullopt;
    const char* begin = s.c_str();
    char* end = nullptr;
    errno = 0;
    float v = std::strtof(begin, &end);
    if (errno == ERANGE || !fullyConsumed(begin, end)) return std::nullopt;
    return v;
}

}  // namespace molterm
