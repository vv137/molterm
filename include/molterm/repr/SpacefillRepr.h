#pragma once

#include "molterm/repr/Representation.h"
#include "molterm/render/ColorMapper.h"

namespace molterm {

// Spacefill / CPK: van der Waals spheres for each atom
class SpacefillRepr : public Representation {
public:
    ReprType type() const override { return ReprType::Spacefill; }

    void render(const MolObject& mol, const Camera& cam,
                Canvas& canvas) override;

    float scale() const { return scale_; }
    void setScale(float s) { scale_ = (s < 0.1f) ? 0.1f : s; }

private:
    float scale_ = 0.5f;  // VDW radius scale factor for terminal rendering

    static float vdwRadius(const std::string& element);
};

} // namespace molterm
