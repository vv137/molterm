#pragma once

#include "molterm/repr/Representation.h"

namespace molterm {

// Cartoon: chunky Catmull-Rom spline tube with SS-dependent radius.
// Helices and sheets are fat; coils are thin. Uses filled circles for 3D effect.
class CartoonRepr : public Representation {
public:
    ReprType type() const override { return ReprType::Cartoon; }

    void render(const MolObject& mol, const Camera& cam,
                Canvas& canvas) override;

private:
    float helixRadius_ = 0.8f;
    float sheetRadius_ = 0.7f;
    float loopRadius_ = 0.25f;
    int subdivisions_ = 8;

    static float catmullRom(float p0, float p1, float p2, float p3, float t);
};

} // namespace molterm
