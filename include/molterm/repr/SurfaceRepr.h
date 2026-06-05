#pragma once

#include <cstdint>
#include <vector>

#include "molterm/repr/Representation.h"

namespace molterm {

// Gaussian-density molecular surface, meshed with marching cubes.
//
// Each atom contributes a Gaussian "blob" whose width tracks its vdW radius.
// The summed density is sampled on a uniform grid and the iso-surface at
// `isoValue_` is extracted as a triangle mesh. With the normalized kernel
// exp(-k*(d^2/r^2 - 1)), an isolated atom's iso=1.0 surface sits exactly at
// scale*vdW; overlapping atoms blend smoothly (metaball union).
//
// The mesh is camera-independent, so it is cached and only rebuilt when the
// atoms, visibility, colours, or parameters change. Per frame we merely
// re-project the cached world-space triangles into the canvas.
class SurfaceRepr : public Representation {
public:
    ReprType type() const override { return ReprType::Surface; }
    void render(const MolObject& mol, const Camera& cam,
                Canvas& canvas) override;

    // Grid spacing in Å. Smaller = finer mesh, ~cubically more cost.
    float resolution() const { return resolution_; }
    void setResolution(float v) { resolution_ = clampf(v, 0.2f, 3.0f); }

    // vdW radius multiplier for each Gaussian blob.
    float scale() const { return scale_; }
    void setScale(float v) { scale_ = clampf(v, 0.2f, 3.0f); }

    // Blobbiness: higher = tighter to vdW, lower = smoother/more merged.
    float smoothness() const { return smoothness_; }
    void setSmoothness(float v) { smoothness_ = clampf(v, 0.5f, 8.0f); }

    // Iso-level. 1.0 places an isolated atom's surface at scale*vdW.
    float isoValue() const { return isoValue_; }
    void setIsoValue(float v) { isoValue_ = clampf(v, 0.05f, 5.0f); }

private:
    static float clampf(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // One meshed triangle in world space (camera-independent), pre-coloured.
    struct WorldTri {
        float x[3], y[3], z[3];
        int colorPair;
        int atomIdx;   // owning atom, for per-triangle alpha lookup
    };

    void rebuildIfNeeded(const MolObject& mol);

    // Parameters
    float resolution_ = 0.7f;
    float scale_      = 1.0f;
    float smoothness_ = 2.0f;
    float isoValue_   = 1.0f;

    // Cached mesh + invalidation key (a hash over inputs that affect geometry
    // or colour: atom coords, visibility, colours, scheme, and parameters).
    std::vector<WorldTri> mesh_;
    std::uint64_t cacheKey_ = 0;
    bool cacheValid_ = false;
};

} // namespace molterm
