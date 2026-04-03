#pragma once

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
};

enum class ColorScheme {
    Element,
    Chain,
    SecondaryStructure,
    BFactor,
    PLDDT,
    Rainbow,
    ResType,   // VMD-like: nonpolar/polar/acidic/basic
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
    bool hasPerAtomRepr() const { return !reprAtomMask_.empty(); }

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

    // Per-chain rainbow fractions [0,1] for each atom (N→C terminus)
    const std::vector<float>& rainbowFractions() const;

    // Geometry helpers
    void computeBoundingBox(float& minX, float& minY, float& minZ,
                            float& maxX, float& maxY, float& maxZ) const;
    void computeCenter(float& cx, float& cy, float& cz) const;

    // Multi-state (NMR ensembles, trajectory)
    int activeState() const { return activeState_; }
    int stateCount() const { return static_cast<int>(states_.size()); }
    void addState(std::vector<AtomData> atomState);
    bool setActiveState(int idx);
    bool nextState();
    bool prevState();

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
    mutable std::vector<float> rainbowCache_;
};

} // namespace molterm
