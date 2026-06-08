#pragma once

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "molterm/core/AtomData.h"
#include "molterm/core/BondData.h"

namespace molterm {

enum class ReprType {
    Wireframe,
    BallStick,
    Spacefill,
    Cartoon,    // thick tube style (ProteinView-like)
    Ribbon,     // flat ribbon with sheet arrowheads
    Backbone,
    Surface,    // Gaussian-density iso-surface (marching cubes)
};

enum class ColorScheme {
    Element,
    Chain,
    SecondaryStructure,
    BFactor,
    PLDDT,
    Rainbow,
    ResType,   // VMD-like: nonpolar/polar/acidic/basic
    SASA,      // solvent accessibility: buried → exposed
    Uniform,
};

class MolObject {
public:
    MolObject() = default;
    explicit MolObject(std::string name);

    const std::string& name() const { return name_; }
    void setName(const std::string& name) { name_ = name; }

    const std::string& sourcePath() const { return sourcePath_; }
    void setSourcePath(const std::string& p) { sourcePath_ = p; }

    // Atom/bond access
    std::vector<AtomData>& atoms() { return atoms_; }
    const std::vector<AtomData>& atoms() const { return atoms_; }
    std::vector<BondData>& bonds() { return bonds_; }
    const std::vector<BondData>& bonds() const { return bonds_; }

    // Visibility
    bool visible() const { return visible_; }
    void setVisible(bool v) { visible_ = v; }
    void toggleVisible() { visible_ = !visible_; }

    // Representations — per-object or per-atom visibility
    bool reprVisible(ReprType r) const;
    bool reprVisibleForAtom(ReprType r, int atomIdx) const;
    void showRepr(ReprType r);
    void showReprForAtoms(ReprType r, const std::vector<int>& indices);
    void hideRepr(ReprType r);
    void hideReprForAtoms(ReprType r, const std::vector<int>& indices);
    void hideAllRepr();
    void hideAllReprForAtoms(const std::vector<int>& indices);
    bool hasPerAtomRepr() const { return !reprAtomMask_.empty(); }

    // Build per-atom visibility mask for a given repr (empty if no per-atom state)
    std::vector<bool> atomVisMask(ReprType r) const;

    // Coloring
    ColorScheme colorScheme() const { return colorScheme_; }
    void setColorScheme(ColorScheme s) { colorScheme_ = s; }

    // Per-atom color overrides (-1 = use scheme)
    const std::vector<int>& atomColors() const { return atomColors_; }
    int atomColor(int idx) const {
        if (idx >= 0 && idx < static_cast<int>(atomColors_.size()))
            return atomColors_[idx];
        return -1;
    }
    void setAtomColor(int idx, int colorPair);
    void setAtomColors(const std::vector<int>& indices, int colorPair);
    void clearAtomColors();

    // Per-atom color overrides grouped by colorPair id → ascending 0-based atom
    // indices (atoms with no override are omitted; empty map if none). Shared by
    // the autosave and the PyMOL exporter so the grouping lives with the data
    // both serialize.
    std::map<int, std::vector<int>> colorGroups() const;

    // Per-atom transparency in [0, 1]. 1.0 = fully opaque (default), 0.0 =
    // fully invisible. Set via :set transparency <value> [selection]; read
    // by PixelCanvas during render to alpha-blend the atom's geometry.
    // Empty vector means "all opaque" — no per-atom storage cost until used.
    const std::vector<float>& atomAlpha() const { return atomAlpha_; }
    float atomAlpha(int idx) const {
        if (idx >= 0 && idx < static_cast<int>(atomAlpha_.size()))
            return atomAlpha_[idx];
        return 1.0f;
    }
    void setAtomAlpha(int idx, float alpha);
    void setAtomAlphas(const std::vector<int>& indices, float alpha);
    void setAtomAlphaAll(float alpha);
    void clearAtomAlpha();

    // Per-chain rainbow fractions [0,1] for each atom (N→C terminus)
    const std::vector<float>& rainbowFractions() const;

    // Geometry helpers
    void computeBoundingBox(float& minX, float& minY, float& minZ,
                            float& maxX, float& maxY, float& maxZ) const;
    void computeCenter(float& cx, float& cy, float& cz) const;

    // Build a new MolObject containing only `keep` atom indices.
    // Bonds are kept iff BOTH endpoints survive; their indices are
    // remapped to the compacted 0..K-1 space. Per-atom state (color
    // overrides, alpha, per-repr atom masks) carries over per surviving
    // atom. Multi-state (NMR / trajectory) collapses to state 0 — the
    // subset can't roundtrip cleanly across frames since the atom set
    // is per-state. Used by :copy <sel>, :extract, :split by chain.
    std::unique_ptr<MolObject> subset(const std::vector<int>& keep,
                                      const std::string& newName) const;

    // Destructive in-place version of subset(): drop `remove` indices.
    // Bonds + per-atom state are remapped on the same rules. Backs
    // :extract — the new object is built first via subset(), then the
    // source self-trims here, so the user sees both "create new" and
    // "shrink source" as a single atomic step.
    void removeAtoms(const std::vector<int>& remove);

    // Apply smart defaults: cartoon for protein/NA, ballstick for ligands
    void applySmartDefaults();

    // Multi-state (NMR ensembles, trajectory)
    int activeState() const { return activeState_; }
    int stateCount() const { return static_cast<int>(states_.size()); }
    void addState(std::vector<AtomData> atomState);
    bool setActiveState(int idx);
    bool nextState();
    bool prevState();

    // Per-state secondary structure cache. SS labels are computed via
    // DSSP on demand and stored per state index — switching states in a
    // trajectory yields per-frame SS without re-running DSSP each time.
    //
    // Returns a reference to the cached SSType vector for `stateIdx`
    // (length == atoms in that state). On first access for a given
    // state, runs DSSP synchronously. The active state's atoms_[i].ssType
    // is also re-synced from this cache by setActiveState().
    const std::vector<SSType>& ssAtState(int stateIdx) const;
    void invalidateSSCache();   // call when SS source / settings change
    // Snapshot the current atoms_[i].ssType into the per-state cache —
    // used by the loader so returning to state 0 keeps header-derived
    // labels rather than triggering a fresh DSSP run.
    void seedSSCacheFromAtoms(int stateIdx) const;

    // Per-state SASA cache (solvent accessible surface area, Å²). Computed
    // via molterm::sasa::compute on demand and stored per state index, like
    // the DSSP cache above. Returns per-atom absolute SASA for `stateIdx`
    // (length == atoms in that state).
    const std::vector<float>& sasaAtState(int stateIdx) const;
    void invalidateSASACache();   // call when atoms / coordinates change
    // Per-atom relative accessibility [0,1] for the active state — residue
    // SASA / residue max-ASA, expanded per atom. Drives ColorScheme::SASA,
    // mirroring rainbowFractions(). Lazily built and cached.
    const std::vector<float>& sasaRelFractions() const;

private:
    std::string name_;
    std::string sourcePath_;
    std::vector<AtomData> atoms_;
    std::vector<BondData> bonds_;
    bool visible_ = true;
    int activeState_ = 0;
    std::vector<std::vector<AtomData>> states_;  // multi-state coordinates (model 0 = atoms_)
    std::unordered_map<ReprType, bool> reprVisible_ = {
        {ReprType::Wireframe, true},
    };
    // Per-atom repr mask: if empty for a repr, all atoms are visible.
    // If non-empty, only atoms with mask[i]=true are rendered for that repr.
    std::unordered_map<ReprType, std::vector<bool>> reprAtomMask_;
    ColorScheme colorScheme_ = ColorScheme::Element;
    std::vector<int> atomColors_;  // per-atom override, -1 = use scheme
    std::vector<float> atomAlpha_;  // per-atom transparency, 1.0 = opaque (lazy alloc)
    mutable std::vector<float> rainbowCache_;

    // Per-state DSSP cache: ssPerState_[stateIdx] = SS labels for that
    // state's atoms. Lazily populated by ssAtState(). Cleared by
    // invalidateSSCache() (e.g. when atoms or settings change).
    mutable std::vector<std::vector<SSType>> ssPerState_;

    // Per-state SASA cache: sasaPerState_[stateIdx] = per-atom absolute SASA
    // (Å²). Lazily populated by sasaAtState(); sasaRelCache_ holds the
    // active state's per-atom relative accessibility for colouring. Both
    // cleared by invalidateSASACache().
    mutable std::vector<std::vector<float>> sasaPerState_;
    mutable std::vector<float> sasaRelCache_;
};

} // namespace molterm
