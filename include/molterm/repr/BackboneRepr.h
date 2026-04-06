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
    float thickness_ = 1.0f;  // in sub-pixels
};

} // namespace molterm
