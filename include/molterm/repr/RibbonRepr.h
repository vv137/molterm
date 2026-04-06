#pragma once

#include "molterm/repr/Representation.h"

namespace molterm {

// Ribbon: flat Catmull-Rom spline ribbon with C→O guide vectors.
// Helices/sheets are flat wide ribbons; sheet ends have arrowheads; coils are thin.
class RibbonRepr : public Representation {
public:
    ReprType type() const override { return ReprType::Ribbon; }

    void render(const MolObject& mol, const Camera& cam,
                Canvas& canvas) override;

    float helixWidth() const { return helixWidth_; }
    void setHelixWidth(float w) { helixWidth_ = w; }
    float sheetWidth() const { return sheetWidth_; }
    void setSheetWidth(float w) { sheetWidth_ = w; }
    float loopWidth() const { return loopWidth_; }
    void setLoopWidth(float w) { loopWidth_ = w; }
    int subdivisions() const { return subdivisions_; }
    void setSubdivisions(int n) { subdivisions_ = n; }

private:
    float helixWidth_ = 3.5f;
    float sheetWidth_ = 4.0f;
    float loopWidth_ = 1.0f;
    int subdivisions_ = 8;
};

} // namespace molterm
