#pragma once

#include <cstdint>
#include <vector>

#include "molterm/repr/Representation.h"

namespace molterm {

// Molecular surface meshed with marching cubes on a uniform scalar grid.
//
// Four surface flavours, selected by Mode:
//   Ses (default) — solvent-excluded (Connolly): the face a probe sphere
//                   traces when rolled over the atoms; fills crevices the
//                   probe can't enter. Built from a probe-accessibility
//                   distance field.
//   Sas           — solvent-accessible: locus of the probe *centre*
//                   (vdW expanded by the probe radius).
//   Vdw           — plain van der Waals union of spheres.
//   Gaussian      — smooth metaball blobs; each atom is a Gaussian kernel
//                   exp(-k*(d^2/r^2 - 1)) whose iso=1.0 sits at scale*vdW.
//
// The mesh is camera-independent, so it is cached and only rebuilt when the
// atoms, visibility, colours, or parameters change. Per frame we merely
// re-project the cached world-space triangles into the canvas.
class SurfaceRepr : public Representation {
public:
    enum class Mode { Ses, Sas, Vdw, Gaussian };

    ReprType type() const override { return ReprType::Surface; }
    void render(const MolObject& mol, const Camera& cam,
                Canvas& canvas) override;

    Mode mode() const { return mode_; }
    void setMode(Mode m) { mode_ = m; }

    // Probe sphere radius (Å) for the SES/SAS modes. 1.4 ≈ water.
    float probe() const { return probe_; }
    void setProbe(float v) { probe_ = clampf(v, 0.0f, 3.0f); }

    // Grid spacing in Å. Smaller = finer mesh, ~cubically more cost.
    float resolution() const { return resolution_; }
    void setResolution(float v) { resolution_ = clampf(v, 0.2f, 3.0f); }

    // vdW radius multiplier for each atom.
    float scale() const { return scale_; }
    void setScale(float v) { scale_ = clampf(v, 0.2f, 3.0f); }

    // Gaussian blobbiness: higher = tighter to vdW, lower = more merged.
    float smoothness() const { return smoothness_; }
    void setSmoothness(float v) { smoothness_ = clampf(v, 0.5f, 8.0f); }

    // Gaussian iso-level. 1.0 places an isolated atom's surface at scale*vdW.
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

    // Extract the iso-surface of `field` (inside = field >= iso) into mesh_,
    // colouring each triangle by its owning atom.
    void marchingCubes(const std::vector<float>& field,
                       const std::vector<int>& owner,
                       int nx, int ny, int nz,
                       float minX, float minY, float minZ,
                       float spacing, float iso,
                       const RenderContext& ctx, int fallbackAtom);

    // Parameters
    Mode  mode_       = Mode::Ses;
    float probe_      = 1.4f;
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
