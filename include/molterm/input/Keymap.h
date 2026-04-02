#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "molterm/input/Action.h"
#include "molterm/input/Mode.h"

namespace molterm {

// A key sequence (e.g., "g" then "t" for next_tab)
using KeySequence = std::vector<int>;

struct KeyBinding {
    KeySequence keys;
    Action action;
    std::string description;
};

class Keymap {
public:
    void bind(Mode mode, const KeySequence& keys, Action action,
              const std::string& desc = "");
    void unbind(Mode mode, const KeySequence& keys);

    // Try to match a key sequence in the given mode
    // Returns Action::None if no match
    // Sets 'partial' to true if the sequence could be a prefix of a binding
    Action match(Mode mode, const KeySequence& keys, bool& partial) const;

    // Get all bindings for a mode
    std::vector<KeyBinding> bindingsForMode(Mode mode) const;

private:
    // Trie node for multi-key sequences
    struct TrieNode {
        Action action = Action::None;
        std::string description;
        std::unordered_map<int, std::unique_ptr<TrieNode>> children;
    };

    std::unordered_map<Mode, TrieNode> roots_;
};

} // namespace molterm
