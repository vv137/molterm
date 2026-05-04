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
    // 0.35 ≈ 3.5 px on a 1920-wide pixel canvas (cellPixW≈10), 1 px on
    // braille (scaleX=2). Tunable live via `:set wf_thickness <float>`;
    // also scales gently with camera zoom in render() so close-ups
    // stay readable without the default looking chunky.
    float thickness_ = 0.35f;  // sub-pixels per cell
};

} // namespace molterm
