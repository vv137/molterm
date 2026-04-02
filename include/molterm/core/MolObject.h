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
    Cartoon,
    Backbone,
};

enum class ColorScheme {
    Element,
    Chain,
    SecondaryStructure,
    BFactor,
    Residue,
    Uniform,
};

class MolObject {
public:
    MolObject() = default;
    explicit MolObject(std::string name);

    const std::string& name() const { return name_; }
    void setName(const std::string& name) { name_ = name; }

    // Atom/bond access
    std::vector<AtomData>& atoms() { return atoms_; }
    const std::vector<AtomData>& atoms() const { return atoms_; }
    std::vector<BondData>& bonds() { return bonds_; }
    const std::vector<BondData>& bonds() const { return bonds_; }

    // Visibility
    bool visible() const { return visible_; }
    void setVisible(bool v) { visible_ = v; }
    void toggleVisible() { visible_ = !visible_; }

    // Representations
    bool reprVisible(ReprType r) const;
    void showRepr(ReprType r);
    void hideRepr(ReprType r);
    void hideAllRepr();

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

    // Geometry helpers
    void computeBoundingBox(float& minX, float& minY, float& minZ,
                            float& maxX, float& maxY, float& maxZ) const;
    void computeCenter(float& cx, float& cy, float& cz) const;

    // Multi-state (future: NMR, trajectory)
    int activeState() const { return activeState_; }

private:
    std::string name_;
    std::vector<AtomData> atoms_;
    std::vector<BondData> bonds_;
    bool visible_ = true;
    int activeState_ = 0;
    std::unordered_map<ReprType, bool> reprVisible_ = {
        {ReprType::Wireframe, true},
    };
    ColorScheme colorScheme_ = ColorScheme::Element;
    std::vector<int> atomColors_;  // per-atom override, -1 = use scheme
};

} // namespace molterm
