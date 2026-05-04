#pragma once

#include "molterm/repr/Representation.h"

namespace molterm {

// Ball-and-stick: filled circles for atoms + thin lines for bonds.
//
// Two sizing modes:
//   useVdwSize_ = true  (Mol*-style, default): radius_Å = sizeFactor_ × vdw(element)
//                       so H atoms render smaller than C/N/O/S, etc.
//   useVdwSize_ = false (legacy):              radius_subpixels = ballRadius_
//                       (a fixed sub-pixel radius regardless of element)
class BallStickRepr : public Representation {
public:
    ReprType type() const override { return ReprType::BallStick; }

    void render(const MolObject& mol, const Camera& cam,
                Canvas& canvas) override;

    int  ballRadius() const { return ballRadius_; }
    void setBallRadius(int r) { ballRadius_ = r; }
    bool useVdwSize() const { return useVdwSize_; }
    void setUseVdwSize(bool v) { useVdwSize_ = v; }
    float sizeFactor() const { return sizeFactor_; }
    void  setSizeFactor(float f) { sizeFactor_ = (f < 0.01f) ? 0.01f : f; }

private:
    int   ballRadius_ = 1;       // legacy sub-pixel radius
    bool  useVdwSize_ = true;    // Mol*-aligned default
    float sizeFactor_ = 0.15f;   // multiplier on vdW (Å)
};

} // namespace molterm
