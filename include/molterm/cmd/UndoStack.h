#pragma once

#include <functional>
#include <string>
#include <cstddef>
#include <vector>

namespace molterm {

struct UndoEntry {
    std::string description;
    std::function<void()> undo;
    std::function<void()> redo;
};

class UndoStack {
public:
    void push(UndoEntry entry);

    bool canUndo() const;
    bool canRedo() const;

    std::string undo();  // returns description of undone action
    std::string redo();  // returns description of redone action

    void clear();
    size_t undoCount() const { return stack_.size() - pos_; }
    size_t redoCount() const { return pos_; }

private:
    std::vector<UndoEntry> stack_;
    size_t pos_ = 0;  // points past the last undone entry (0 = nothing undone)
};

} // namespace molterm
