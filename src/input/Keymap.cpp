#include <memory>

#include "molterm/input/Keymap.h"

namespace molterm {

void Keymap::bind(Mode mode, const KeySequence& keys, Action action,
                  const std::string& desc) {
    if (keys.empty()) return;
    auto& root = roots_[mode];
    TrieNode* node = &root;
    for (int k : keys) {
        auto& child = node->children[k];
        if (!child) child = std::make_unique<TrieNode>();
        node = child.get();
    }
    node->action = action;
    node->description = desc;
}

void Keymap::unbind(Mode mode, const KeySequence& keys) {
    if (keys.empty()) return;
    auto it = roots_.find(mode);
    if (it == roots_.end()) return;

    TrieNode* node = &it->second;
    for (int k : keys) {
        auto child = node->children.find(k);
        if (child == node->children.end()) return;
        node = child->second.get();
    }
    node->action = Action::None;
}

Action Keymap::match(Mode mode, const KeySequence& keys, bool& partial) const {
    partial = false;
    auto it = roots_.find(mode);
    if (it == roots_.end()) return Action::None;

    const TrieNode* node = &it->second;
    for (int k : keys) {
        auto child = node->children.find(k);
        if (child == node->children.end()) return Action::None;
        node = child->second.get();
    }

    partial = !node->children.empty() && node->action == Action::None;
    return node->action;
}

std::vector<KeyBinding> Keymap::bindingsForMode(Mode mode) const {
    std::vector<KeyBinding> result;
    auto it = roots_.find(mode);
    if (it == roots_.end()) return result;

    // DFS to collect all bindings
    struct Frame {
        const TrieNode* node;
        KeySequence prefix;
    };
    std::vector<Frame> stack;
    stack.push_back({&it->second, {}});

    while (!stack.empty()) {
        auto [node, prefix] = stack.back();
        stack.pop_back();

        if (node->action != Action::None) {
            result.push_back({prefix, node->action, node->description});
        }
        for (const auto& [key, child] : node->children) {
            auto childPrefix = prefix;
            childPrefix.push_back(key);
            stack.push_back({child.get(), childPrefix});
        }
    }
    return result;
}

} // namespace molterm
