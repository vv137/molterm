#pragma once

namespace molterm {

// Base interface for UI components with dirty-tracking.
// Concrete widgets inherit this for a uniform dirty protocol.
// render() is NOT virtual — each component has its own typed signature.
class Widget {
public:
    virtual ~Widget() = default;

    bool needsRender() const { return dirty_; }
    void markDirty() { dirty_ = true; }
    void markClean() { dirty_ = false; }

protected:
    bool dirty_ = true;
};

} // namespace molterm
