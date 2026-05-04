#pragma once

#include <cstddef>
#include <cstdint>

#include "molterm/repr/Representation.h"
#include <string>
#include <vector>

namespace molterm {

// Cartoon: chunky Catmull-Rom spline tube with SS-dependent radius.
// Helices and sheets are fat; coils are thin. Uses filled circles for 3D effect.
class CartoonRepr : public Representation {
public:
    // Which nucleic-acid backbone atom drives the cartoon spline.
    // C4' tracks the sugar ring centre and gives smoother curves;
    // P matches the standard PDB phosphate-trace convention.
    enum class NucleicBackbone : std::uint8_t {
        C4 = 0,
        P,
    };

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
    NucleicBackbone nucleicBackbone() const { return nucleicBackbone_; }
    void setNucleicBackbone(NucleicBackbone b) { nucleicBackbone_ = b; }

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

    void renderNucleicBases(const MolObject& mol, const RenderContext& ctx,
                            const Camera& cam, Canvas& canvas) const;

    // ── Camera-independent geometry cache ────────────────────────────────
    // Catmull-Rom spline + parallel-transport frame + sheet-hint blend
    // depend only on atom positions, SS, coloring, and repr params — not
    // on the camera. We rebuild this when a fingerprint key changes, so a
    // spinning animation reuses the same buffers across all frames.
    struct CachedSpine {
        std::vector<float> x, y, z;
        std::vector<SSType> ss;
        std::vector<int>   color;
        std::vector<float> arrowFrac;
        std::vector<float> tx, ty, tz;
        std::vector<float> nx, ny, nz;
        std::vector<float> bx, by, bz;
    };
    struct ChainSpan { std::string chainId; int start, end; };
    struct CacheKey {
        const MolObject* mol = nullptr;
        int activeState = -1;
        std::size_t atomCount = 0;
        int subdiv = 0;
        int coilSegments = 0;
        float helixR = 0.0f, sheetR = 0.0f, loopR = 0.0f;
        NucleicBackbone nb = NucleicBackbone::C4;
        int colorScheme = -1;
        std::size_t atomColorsSize = 0;
        std::uint64_t atomColorsHash = 0;
        bool perAtomRepr = false;
        std::uint64_t visMaskHash = 0;
        bool operator==(const CacheKey& o) const;
        bool operator!=(const CacheKey& o) const { return !(*this == o); }
    };

    mutable CacheKey cacheKey_;
    mutable CachedSpine cachedSpine_;
    mutable std::vector<ChainSpan> cachedChains_;
    mutable bool cacheValid_ = false;

    void rebuildCache(const MolObject& mol, const RenderContext& ctx,
                      int subdiv, int coilSegments) const;
    void drawChainCached(int chainStart, int chainEnd, int coilSegments,
                         const Camera& cam, Canvas& canvas,
                         std::vector<TriangleSpan>* triBatch) const;

    // Cartoon defaults follow ProteinView's chunkier sizing so helices
    // and sheets read as solid 3-D ribbons rather than thin strips.
    // All four are overridable via `:set cartoon_helix/sheet/loop/subdiv`.
    float helixRadius_ = 1.30f;   // half-width  of helix ribbon, Å
    float sheetRadius_ = 1.50f;   // half-width  of sheet ribbon, Å
    float loopRadius_  = 0.40f;   // tube radius of coil section, Å
    int subdivisions_  = 14;
    NucleicBackbone nucleicBackbone_ = NucleicBackbone::C4;
};

} // namespace molterm
