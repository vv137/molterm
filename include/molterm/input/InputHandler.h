#pragma once

#include <functional>
#include <vector>

#include "molterm/input/Action.h"
#include "molterm/input/Keymap.h"
#include "molterm/input/Mode.h"

namespace molterm {

class InputHandler {
public:
    explicit InputHandler(const Keymap& keymap);

    // Process a raw key, returns the resolved action (or None if pending/unbound)
    Action processKey(int key);

    Mode mode() const { return mode_; }
    void setMode(Mode m);

    // Clear pending key sequence
    void clearPending();

    // For command/search mode: get the raw character
    bool isTextInput() const;

private:
    const Keymap& keymap_;
    Mode mode_ = Mode::Normal;
    KeySequence pending_;
};

} // namespace molterm
