#pragma once

#include "molterm/repr/Representation.h"
#include <string>
#include <vector>

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
    struct CaAtom {
        int idx; float x, y, z; SSType ss; std::string chain;
        // Carbonyl C→O direction hint (proteins only); used to orient
        // β-sheet ribbons consistently. hasHint=false for nucleic acids
        // or residues missing C/O atoms.
        float hintX, hintY, hintZ;
        bool hasHint;
    };
    struct SplinePoint {
        float x, y, z; SSType ss; int color; float arrowFrac;
        float hintX, hintY, hintZ;
        bool hasHint;
    };

    void renderChain(const std::vector<CaAtom>& cas, size_t start, size_t end,
                     int subdiv, int coilSegs, const RenderContext& ctx,
                     const Camera& cam, Canvas& canvas) const;
    void renderNucleicBases(const MolObject& mol, const RenderContext& ctx,
                            const Camera& cam, Canvas& canvas) const;

    float helixRadius_ = 0.8f;
    float sheetRadius_ = 0.7f;
    float loopRadius_ = 0.25f;
    int subdivisions_ = 8;
};

} // namespace molterm
