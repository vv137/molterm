#pragma once

#include <string>

namespace molterm {

enum class Mode {
    Normal,
    Command,
    Visual,
    Search,
};

inline std::string modeName(Mode m) {
    switch (m) {
        case Mode::Normal:  return "NORMAL";
        case Mode::Command: return "COMMAND";
        case Mode::Visual:  return "VISUAL";
        case Mode::Search:  return "SEARCH";
    }
    return "UNKNOWN";
}

} // namespace molterm
