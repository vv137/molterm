#pragma once

// Helpers shared by command handlers across the src/cmd/commands/ modules
// (and by the script runner in Application.cpp). These used to live in an
// anonymous namespace in Application.cpp; they were lifted here so the
// per-area command translation units can reuse them without duplication.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace molterm {

// Join args[lo..hi) with single-space separators.
std::string joinArgs(const std::vector<std::string>& args, size_t lo, size_t hi);

// Split args at the first token equal to `token`: returns {left, right} where
// `left` is the args before it and `right` is everything after, space-joined.
// When the token is absent, `right` is empty and `left` is the original args.
// Used by :label and the :measure/:angle/:dihedral caption parsers.
std::pair<std::vector<std::string>, std::string>
splitAtToken(const std::vector<std::string>& args, std::string_view token);

// Back-compat wrapper for the original "=" caller.
std::pair<std::vector<std::string>, std::string>
splitAtEqToken(const std::vector<std::string>& args);

// `[A-Za-z_][A-Za-z0-9_]*` — accepted in both ${NAME} expansion and :setenv.
bool isValidEnvName(const std::string& s);

// A bare binding name for `:let <name> =` / `:select <name> =`: starts with a
// letter or underscore and holds no whitespace. Looser than isValidEnvName()
// (the rest of the character set is unconstrained).
bool isBareName(const std::string& s);

// Parse "#RGB" / "#RRGGBB" / "rgb(R,G,B)" to an RGB triple (forgiving on
// whitespace and case). std::nullopt on malformed input.
std::optional<std::array<uint8_t, 3>> parseHexColor(std::string s);

// Resolve a color spec — named ("red"), hex ("#RRGGBB"/"#RGB"), or rgb(R,G,B)
// — to an RGB triple, so every color-accepting option shares one accept-list.
std::optional<std::array<uint8_t, 3>> parseColorSpec(const std::string& s);

}  // namespace molterm
