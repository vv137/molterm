#pragma once

#include <string>
#include <utility>

#include "molterm/analysis/ContactMap.h"
#include "molterm/tui/DensityMap.h"
#include "molterm/tui/Widget.h"
#include "molterm/tui/Window.h"

namespace molterm {

class ContactMapPanel : public Widget {
public:
    // Recompute contact map for the given object
    void update(const MolObject& mol, float cutoff = 8.0f);

    // Render the heatmap into the provided window
    void render(Window& win);

    // Scroll controls
    void scrollUp(int n = 2)    { scrollRow_ = std::max(0, scrollRow_ - n); }
    void scrollDown(int n = 2)  { scrollRow_ += n; }
    void scrollLeft(int n = 1)  { scrollCol_ = std::max(0, scrollCol_ - n); }
    void scrollRight(int n = 1) { scrollCol_ += n; }

    // Mouse click: returns (resSeq1, resSeq2) at terminal position, or (-1,-1)
    std::pair<int,int> residueAtCell(int ty, int tx) const;

    const ContactMap& contactMap() const { return contactMap_; }
    ContactMap& contactMap() { return contactMap_; }

    bool valid() const { return contactMap_.valid(); }

private:
    ContactMap contactMap_;
    DensityMap densityMap_;
    int scrollRow_ = 0, scrollCol_ = 0;
    std::string lastObjName_;
    int lastAtomCount_ = 0;
    float lastCutoff_ = 0.0f;
};

} // namespace molterm
