#pragma once

#include "molterm/input/Keymap.h"

#include <string>

namespace molterm {

class KeymapManager {
public:
    KeymapManager();

    Keymap& keymap() { return keymap_; }
    const Keymap& keymap() const { return keymap_; }

    // Load defaults
    void loadDefaults();

    // Load overrides from ~/.molterm/keymap.toml (Phase 4)
    void loadFromFile();

private:
    Keymap keymap_;

    void bindNormalDefaults();
    void bindCommandDefaults();
    void bindVisualDefaults();
    void bindSearchDefaults();

    static Mode modeFromName(const std::string& name);
};

} // namespace molterm
