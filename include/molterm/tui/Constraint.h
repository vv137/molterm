#pragma once

#include <vector>

#include "molterm/tui/Layout.h"  // for Rect

namespace molterm {

enum class SizePolicy {
    Fixed,   // Exact size in rows/cols
    Fill,    // Take remaining space (weight = value, default 1)
};

struct LayoutSlot {
    SizePolicy policy = SizePolicy::Fill;
    int value = 1;         // Fixed: exact size. Fill: weight.
    bool visible = true;   // invisible slots get 0 size
    int minSize = 0;       // hard minimum (only for Fill)
    int maxSize = 0;       // hard maximum (0 = unlimited)
};

enum class SplitDir { Vertical, Horizontal };

// Solve a 1D strip allocation: distribute `total` among slots by policy.
std::vector<int> solveStrip(int total, const std::vector<LayoutSlot>& slots);

// Split a rect along an axis according to slot constraints.
std::vector<Rect> solveLayout(const Rect& area, SplitDir dir,
                               const std::vector<LayoutSlot>& slots);

} // namespace molterm
