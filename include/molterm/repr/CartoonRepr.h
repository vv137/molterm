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

    float helixRadius() const { return helixRadius_; }
    void setHelixRadius(float r) { helixRadius_ = r; }
    float sheetRadius() const { return sheetRadius_; }
    void setSheetRadius(float r) { sheetRadius_ = r; }
    float loopRadius() const { return loopRadius_; }
    void setLoopRadius(float r) { loopRadius_ = r; }
    int subdivisions() const { return subdivisions_; }
    void setSubdivisions(int n) { subdivisions_ = n; }

private:
    float helixRadius_ = 0.8f;
    float sheetRadius_ = 0.7f;
    float loopRadius_ = 0.25f;
    int subdivisions_ = 8;
};

} // namespace molterm
