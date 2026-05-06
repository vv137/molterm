#pragma once

#include <string>
#include <vector>

namespace molterm {

// Expand shell-style globs and brace ranges across a list of patterns.
// Supports POSIX glob (`*`, `?`, `[abc]`) and a single brace range (`{1..5}`)
// per token. Multiple patterns are concatenated, then deduplicated and sorted.
//
// Tokens that contain no glob metacharacters and no brace range pass through
// untouched (existence is not checked here — let the caller's loader fail
// loudly on missing files), so callers can mix literal paths and patterns
// freely.
std::vector<std::string>
expandPathPatterns(const std::vector<std::string>& patterns);

}  // namespace molterm
