#include "molterm/app/PathPatterns.h"

#include <algorithm>
#include <filesystem>
#include <glob.h>

namespace molterm {

namespace {

// Expand a single brace range "{lo..hi}". Only one brace per token, no nesting.
// Returns the original pattern unchanged if no valid range is present.
std::vector<std::string> expandBrace(const std::string& p) {
    auto lb = p.find('{');
    if (lb == std::string::npos) return {p};
    auto rb = p.find('}', lb);
    if (rb == std::string::npos) return {p};
    auto inner = p.substr(lb + 1, rb - lb - 1);
    auto dots = inner.find("..");
    if (dots == std::string::npos) return {p};
    try {
        int lo = std::stoi(inner.substr(0, dots));
        int hi = std::stoi(inner.substr(dots + 2));
        if (hi < lo) std::swap(lo, hi);
        std::vector<std::string> out;
        out.reserve(static_cast<size_t>(hi - lo + 1));
        for (int i = lo; i <= hi; ++i) {
            out.push_back(p.substr(0, lb) + std::to_string(i) +
                          p.substr(rb + 1));
        }
        return out;
    } catch (...) {
        return {p};
    }
}

bool hasGlobMeta(const std::string& s) {
    return s.find_first_of("*?[") != std::string::npos;
}

}  // namespace

std::vector<std::string>
expandPathPatterns(const std::vector<std::string>& patterns) {
    std::vector<std::string> matches;
    for (const auto& raw : patterns) {
        for (const auto& pat : expandBrace(raw)) {
            if (!hasGlobMeta(pat)) {
                // Literal path (possibly brace-expanded) — pass through.
                matches.push_back(pat);
                continue;
            }
            glob_t g{};
            int rc = glob(pat.c_str(), GLOB_NOSORT, nullptr, &g);
            if (rc == 0) {
                for (size_t i = 0; i < g.gl_pathc; ++i)
                    matches.emplace_back(g.gl_pathv[i]);
            } else if (rc == GLOB_NOMATCH && std::filesystem::exists(pat)) {
                // Defensive: a file literally named like a glob token.
                matches.push_back(pat);
            }
            globfree(&g);
        }
    }
    std::sort(matches.begin(), matches.end());
    matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
    return matches;
}

}  // namespace molterm
