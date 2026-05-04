#include "molterm/input/InputHandler.h"

namespace molterm {

InputHandler::InputHandler(const Keymap& keymap)
    : keymap_(keymap) {}

Action InputHandler::processKey(int key) {
    // Every key — including ESC and Ctrl+C — is dispatched through the
    // keymap. Each mode (Normal/Command/Visual/Search) registers its own
    // ExitToNormal binding for {27} and {3} via KeymapManager defaults, so
    // users can rebind them per mode if they want.

    // In Command/Search mode, most keys are text input
    if (isTextInput()) {
        // Check for special keys first
        pending_.push_back(key);
        bool partial = false;
        Action action = keymap_.match(mode_, pending_, partial);
        pending_.clear();
        if (action != Action::None) return action;
        // Not a special key - it's text input, return None
        // The caller will handle it as text
        return Action::None;
    }

    // Normal/Visual mode: use trie-based key matching
    pending_.push_back(key);
    bool partial = false;
    Action action = keymap_.match(mode_, pending_, partial);

    if (action != Action::None) {
        pending_.clear();
        return action;
    }

    if (partial) {
        // Waiting for more keys
        return Action::None;
    }

    // No match at all - clear and ignore
    pending_.clear();
    return Action::None;
}

void InputHandler::setMode(Mode m) {
    mode_ = m;
    clearPending();
}

void InputHandler::clearPending() {
    pending_.clear();
}

bool InputHandler::isTextInput() const {
    return mode_ == Mode::Command || mode_ == Mode::Search;
}

} // namespace molterm
