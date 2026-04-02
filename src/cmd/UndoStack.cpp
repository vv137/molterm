#include "molterm/cmd/UndoStack.h"

namespace molterm {

void UndoStack::push(UndoEntry entry) {
    // Discard any redo entries ahead of current position
    if (pos_ > 0) {
        stack_.erase(stack_.end() - static_cast<ptrdiff_t>(pos_), stack_.end());
        pos_ = 0;
    }
    stack_.push_back(std::move(entry));

    // Limit stack size
    if (stack_.size() > 100) {
        stack_.erase(stack_.begin());
    }
}

bool UndoStack::canUndo() const {
    return !stack_.empty() && pos_ < stack_.size();
}

bool UndoStack::canRedo() const {
    return pos_ > 0;
}

std::string UndoStack::undo() {
    if (!canUndo()) return "";
    size_t idx = stack_.size() - 1 - pos_;
    stack_[idx].undo();
    ++pos_;
    return "Undone: " + stack_[idx].description;
}

std::string UndoStack::redo() {
    if (!canRedo()) return "";
    --pos_;
    size_t idx = stack_.size() - 1 - pos_;
    stack_[idx].redo();
    return "Redone: " + stack_[idx].description;
}

void UndoStack::clear() {
    stack_.clear();
    pos_ = 0;
}

} // namespace molterm
