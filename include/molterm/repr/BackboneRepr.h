#pragma once

#include "molterm/repr/Representation.h"

namespace molterm {

// Backbone trace: connects Cα atoms with thick lines
class BackboneRepr : public Representation {
public:
    ReprType type() const override { return ReprType::Backbone; }

    void render(const MolObject& mol, const Camera& cam,
                Canvas& canvas) override;

    float thickness() const { return thickness_; }
    void setThickness(float t) { thickness_ = (t < 0.1f) ? 0.1f : t; }

private:
    // 0.5 = ~5 px on a 1920-wide pixel canvas, ~1 px on braille.
    // Slightly thicker than wireframe so the CA trace reads clearly
    // against an empty background.
    float thickness_ = 0.5f;  // sub-pixels per cell
};

} // namespace molterm
