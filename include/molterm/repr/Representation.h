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
    const std::vector<float>* rainbowData;  // nullptr if not Rainbow
    std::vector<bool> atomVis;               // per-atom visibility mask (empty = all visible)

    float rainbowFrac(int i) const {
        return rainbowData ? (*rainbowData)[i] : -1.0f;
    }

    bool visible(int i) const {
        return atomVis.empty() || atomVis[i];
    }

    int colorFor(int i) const {
        return ColorMapper::colorForAtom(atoms[i], scheme,
                                         mol.atomColor(i), rainbowFrac(i));
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

protected:
    // Build a RenderContext for the given mol and repr type.
    // Returns nullopt-like empty context if repr should not render.
    // Call at the start of every render() to replace the boilerplate.
    static RenderContext makeContext(const MolObject& mol, ReprType repr) {
        auto scheme = mol.colorScheme();
        const std::vector<float>* rbw =
            (scheme == ColorScheme::Rainbow) ? &mol.rainbowFractions() : nullptr;
        return {mol, mol.atoms(), scheme, rbw, mol.atomVisMask(repr)};
    }

    // Convert a repr parameter (thickness, radius) to sub-pixels, clamped to min.
    static int toSubPixels(float value, int scale, int minVal = 1) {
        int r = static_cast<int>(value * static_cast<float>(scale) + 0.5f);
        return std::max(minVal, r);
    }

    // Adjust subdivision level based on atom count (for LOD).
    static int adjustLOD(int baseSub, size_t atomCount,
                         size_t mediumThreshold = 5000,
                         size_t lowThreshold = 20000) {
        if (atomCount > lowThreshold) return std::max(2, baseSub / 4);
        if (atomCount > mediumThreshold) return std::max(3, baseSub / 2);
        return baseSub;
    }
};

} // namespace molterm
