#pragma once

#include "molterm/repr/Representation.h"
#include "molterm/render/ColorMapper.h"

namespace molterm {

// Wireframe: bond lines + atom dots at element color
class WireframeRepr : public Representation {
public:
    ReprType type() const override { return ReprType::Wireframe; }

    void render(const MolObject& mol, const Camera& cam,
                Canvas& canvas) override;

    float thickness() const { return thickness_; }
    void setThickness(float t) { thickness_ = (t < 0.1f) ? 0.1f : t; }

private:
    float thickness_ = 1.0f;  // in sub-pixels
};

} // namespace molterm
