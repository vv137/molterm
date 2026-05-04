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
    // 0.3 = ~3 px on a 1920-wide pixel canvas (cellPixW≈10), ~1 px on
    // braille (scaleX=2). Thinner default than the previous 1.0 — at
    // 1920×1080 the old setting drew 10-pixel circles per atom.
    float thickness_ = 0.3f;  // sub-pixels per cell
};

} // namespace molterm
