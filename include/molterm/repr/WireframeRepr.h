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
    void setThickness(float t) { thickness_ = (t < 0.01f) ? 0.01f : (t > 1.0f ? 1.0f : t); }

    // Interface/focus mode coloring: carbons keep the scheme color
    // (chain / SS / etc.), non-carbons get element color so donors,
    // acceptors and heteroatoms read at a glance against the carbon
    // backbone. Off by default; toggled by Application around renders
    // when interface overlay or focus is active.
    bool heteroatomCarbonScheme() const { return heteroatomCarbonScheme_; }
    void setHeteroatomCarbonScheme(bool on) { heteroatomCarbonScheme_ = on; }

private:
    // Line/dot radius in Å — multiplied by the camera's projScale in
    // render() so the wireframe thickens proportionally with zoom (tracks
    // the molecule like Spacefill/BallStick-vdW). ~0.10 Å matches the
    // BallStick bond-cylinder radius; tune live via `:set wireframe_thickness`.
    float thickness_ = 0.10f;  // Å (× projScale → sub-pixels)
    bool heteroatomCarbonScheme_ = false;
};

} // namespace molterm
