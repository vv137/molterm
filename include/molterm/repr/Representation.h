#pragma once

#include <algorithm>
#include <vector>

#include "molterm/core/MolObject.h"
#include "molterm/render/Camera.h"
#include "molterm/render/Canvas.h"
#include "molterm/render/ColorMapper.h"

namespace molterm {

// Shared context for rendering — eliminates boilerplate in each repr.
struct RenderContext {
    const MolObject& mol;
    const std::vector<AtomData>& atoms;
    ColorScheme scheme;
    // Per-atom scalar channel for gradient schemes (nullptr otherwise).
    // See Representation::scalarChannel for which schemes feed it.
    const std::vector<float>* scalarData;
    std::vector<bool> atomVis;               // per-atom visibility mask (empty = all visible)

    float scalarFrac(int i) const {
        return scalarData ? (*scalarData)[i] : -1.0f;
    }

    bool visible(int i) const {
        return atomVis.empty() || atomVis[i];
    }

    int colorFor(int i) const {
        return ColorMapper::colorForAtom(atoms[i], scheme,
                                         mol.atomColor(i), scalarFrac(i));
    }
};

// Abstract representation: knows what to draw from a MolObject.
// Uses Canvas for backend-agnostic sub-pixel drawing.
class Representation {
public:
    virtual ~Representation() = default;

    virtual ReprType type() const = 0;

    // Render this representation of mol onto canvas
    virtual void render(const MolObject& mol, const Camera& cam,
                        Canvas& canvas) = 0;

public:
    // The per-atom scalar channel a colour scheme reads, or nullptr if it
    // uses none. Rainbow feeds N→C fractions; SASA feeds relative
    // accessibility. Single source of truth so every renderer routes the
    // same way (used by makeContext and by reprs that build their own ctx).
    static const std::vector<float>* scalarChannel(const MolObject& mol,
                                                   ColorScheme scheme) {
        switch (scheme) {
            case ColorScheme::Rainbow: return &mol.rainbowFractions();
            case ColorScheme::SASA:    return &mol.sasaRelFractions();
            default:                   return nullptr;
        }
    }

protected:
    // Build a RenderContext for the given mol and repr type.
    // Returns nullopt-like empty context if repr should not render.
    // Call at the start of every render() to replace the boilerplate.
    static RenderContext makeContext(const MolObject& mol, ReprType repr) {
        auto scheme = mol.colorScheme();
        return {mol, mol.atoms(), scheme, scalarChannel(mol, scheme),
                mol.atomVisMask(repr)};
    }

    // Convert a repr parameter (thickness, radius) to sub-pixels, clamped to min.
    static int toSubPixels(float value, int scale, int minVal = 1) {
        int r = static_cast<int>(value * static_cast<float>(scale) + 0.5f);
        return std::max(minVal, r);
    }

    // Frustum check: is a projected point within canvas bounds + margin?
    static bool inFrustum(float sx, float sy, int canvasW, int canvasH, int margin) {
        return sx >= -margin && sx < canvasW + margin &&
               sy >= -margin && sy < canvasH + margin;
    }

    // Adjust subdivision level based on atom count (for LOD).
    static int adjustLOD(int baseSub, size_t atomCount) {
        if (atomCount > lodLowThreshold) return std::max(2, baseSub / 4);
        if (atomCount > lodMediumThreshold) return std::max(3, baseSub / 2);
        return baseSub;
    }

public:
    // Global rendering thresholds (configurable via :set)
    static size_t lodMediumThreshold;   // default 5000
    static size_t lodLowThreshold;      // default 20000
    static size_t backboneCutoff;       // default 200000
};

} // namespace molterm
