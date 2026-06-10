#pragma once

#include <optional>
#include <string>

namespace molterm {

// No-throw, whole-string numeric parsing.
//
// Unlike std::stoi/std::stol/std::stof/std::stod these never throw
// (std::invalid_argument / std::out_of_range) and never silently accept
// trailing garbage — std::stoi("3abc") returns 3, whereas parseInt("3abc")
// returns std::nullopt. Leading and trailing ASCII whitespace is tolerated.
//
// Returns std::nullopt when the string is empty, not numeric, has leftover
// non-space characters, or names a value outside the target type's range.
//
// This is the single home for "parse a user/file/script-supplied number
// without crashing the process"; prefer it over a raw std::sto* + ad-hoc
// try/catch at every call site. Lives in core/ (a leaf with no molterm
// dependencies) so every layer — core, io, cmd, app — can use it.
std::optional<int>    parseInt(const std::string& s);
std::optional<long>   parseLong(const std::string& s);
std::optional<float>  parseFloat(const std::string& s);
std::optional<double> parseDouble(const std::string& s);

}  // namespace molterm
