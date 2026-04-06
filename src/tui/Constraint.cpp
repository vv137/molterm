#include "molterm/tui/Constraint.h"

namespace molterm {

std::vector<int> solveStrip(int total, const std::vector<LayoutSlot>& slots) {
    std::vector<int> sizes(slots.size(), 0);

    int remaining = total;
    int totalWeight = 0;
    for (size_t i = 0; i < slots.size(); ++i) {
        if (!slots[i].visible) continue;
        if (slots[i].policy == SizePolicy::Fixed) {
            sizes[i] = slots[i].value;
            remaining -= sizes[i];
        } else {
            totalWeight += slots[i].value;
        }
    }
    if (remaining < 0) remaining = 0;

    if (totalWeight > 0) {
        int distributed = 0;
        int lastFill = -1;
        for (size_t i = 0; i < slots.size(); ++i) {
            if (!slots[i].visible || slots[i].policy != SizePolicy::Fill) continue;
            sizes[i] = remaining * slots[i].value / totalWeight;

            if (slots[i].minSize > 0 && sizes[i] < slots[i].minSize)
                sizes[i] = slots[i].minSize;
            if (slots[i].maxSize > 0 && sizes[i] > slots[i].maxSize)
                sizes[i] = slots[i].maxSize;

            distributed += sizes[i];
            lastFill = static_cast<int>(i);
        }
        if (lastFill >= 0) {
            sizes[lastFill] += remaining - distributed;
            if (sizes[lastFill] < 0) sizes[lastFill] = 0;
        }
    }

    return sizes;
}

std::vector<Rect> solveLayout(const Rect& area, SplitDir dir,
                               const std::vector<LayoutSlot>& slots) {
    bool vert = (dir == SplitDir::Vertical);
    int total = vert ? area.h : area.w;
    auto sizes = solveStrip(total, slots);

    std::vector<Rect> result(slots.size());
    int offset = 0;
    for (size_t i = 0; i < slots.size(); ++i) {
        if (vert) {
            result[i] = {area.y + offset, area.x, sizes[i], area.w};
        } else {
            result[i] = {area.y, area.x + offset, area.h, sizes[i]};
        }
        offset += sizes[i];
    }
    return result;
}

} // namespace molterm
