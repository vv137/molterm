#pragma once

#include "molterm/input/Keymap.h"

namespace molterm {

class KeymapManager {
public:
    KeymapManager();

    Keymap& keymap() { return keymap_; }
    const Keymap& keymap() const { return keymap_; }

    // Load defaults
    void loadDefaults();

    // TODO Phase 4: load from TOML
    // void loadFromFile(const std::string& path);

private:
    Keymap keymap_;

    void bindNormalDefaults();
    void bindCommandDefaults();
    void bindVisualDefaults();
};

} // namespace molterm
