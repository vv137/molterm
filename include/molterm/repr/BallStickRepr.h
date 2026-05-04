#pragma once

#include "molterm/repr/Representation.h"

namespace molterm {

// Ball-and-stick: filled circles for atoms + thin lines for bonds
class BallStickRepr : public Representation {
public:
    ReprType type() const override { return ReprType::BallStick; }

    void render(const MolObject& mol, const Camera& cam,
                Canvas& canvas) override;

    int ballRadius() const { return ballRadius_; }
    void setBallRadius(int r) { ballRadius_ = r; }

private:
    int ballRadius_ = 1;  // in sub-pixels
};

} // namespace molterm
