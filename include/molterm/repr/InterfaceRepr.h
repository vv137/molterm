#pragma once

#include <vector>

#include "molterm/analysis/ContactMap.h"   // InterfaceContact, InteractionType

namespace molterm {

class MolObject;
class Camera;
class Canvas;

// Color pair ID for an interface interaction type — same mapping the
// dashed line renderer uses, exposed so the legend overlay stays in
// sync with the viewport palette.
int interactionColor(InteractionType t);

// Overlay renderer for interface analysis. Not a Representation subclass —
// its inputs (per-atom interface mask + classified inter-chain contact
// list) come from Application, not from MolObject, so it sits outside
// the per-object representation map. Application calls render() once per
// frame after the regular reprs draw, when the interface feature is
// active.
//
// The contact list reuses ContactMap's `InterfaceContact` so we share
// classification with `:contactmap` rather than duplicating chemistry
// helpers.
class InterfaceRepr {
public:
    // `interfaceMask`: per-atom flag, true for atoms in interface residues
    // (typically built by expanding `interfaceContacts` to whole residues).
    void setData(std::vector<bool> interfaceMask,
                 std::vector<InterfaceContact> contacts) {
        mask_ = std::move(interfaceMask);
        contacts_ = std::move(contacts);
    }
    void clear() {
        mask_.clear();
        contacts_.clear();
    }
    bool hasData() const { return !mask_.empty() || !contacts_.empty(); }

    const std::vector<bool>&             mask()     const { return mask_; }
    const std::vector<InterfaceContact>& contacts() const { return contacts_; }

    // Draws sidechain bonds for atoms in `mask_` (element-colored, thin)
    // and dashed lines between every contact's atom pair (color by type).
    // Stamps active atom indices on the canvas so focus-dim can protect
    // these pixels from desaturation.
    void render(const MolObject& mol, const Camera& cam, Canvas& canvas);

    void setDashLen(int v)     { dashLen_ = v; }
    void setGapLen(int v)      { gapLen_  = v; }
    void setDrawSidechains(bool v) { drawSidechains_ = v; }
    // Thickness in sub-pixels for sidechain bonds and interaction dashes.
    // 1 = thin (Bresenham line); >=2 stamps a filled circle per Bresenham
    // step, so the overlay reads at typical zoom.
    void setLineThickness(int v)        { lineThickness_     = v; }
    void setInteractionThickness(int v) { interactionThickness_ = v; }
    // Bitmask of InteractionType bits — only contacts whose type bit is
    // set get a dashed line. Sidechain wireframe is unaffected so the
    // viewer still sees which residues form the interface.
    void setShowMask(std::uint8_t mask) { showMask_ = mask; }

private:
    std::vector<bool>             mask_;
    std::vector<InterfaceContact> contacts_;
    // Dashed interaction lines: gap > dash so individual H-bond / salt
    // bridge segments read as separate marks rather than a near-solid
    // line. Mol*-like.
    int  dashLen_              = 5;
    int  gapLen_               = 9;
    int  lineThickness_        = 2;     // sidechain bonds
    int  interactionThickness_ = 4;     // dashed interaction lines (chunky overlay)
    bool drawSidechains_       = true;
    std::uint8_t showMask_     = kInterfaceShowAll;
};

} // namespace molterm
