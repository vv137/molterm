#pragma once

#include "molterm/repr/Representation.h"

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
    // Mol*-aligned: full vdW radius (CPK convention). Override via
    // `:set spacefill_scale` (which clamps to [0.1, ...]).
    float scale_ = 1.0f;

    // Cached depth-sorted indices (re-sort only when camera dirty)
    mutable std::vector<int> sortedIndices_;
    mutable bool sortDirty_ = true;

    static float vdwRadius(const std::string& element);
};

} // namespace molterm
