#include "molterm/core/MolObject.h"
#include "molterm/core/BondTable.h"
#include "molterm/core/DSSP.h"
#include "molterm/core/SASA.h"
#include "molterm/repr/Representation.h"
#include <algorithm>
#include <limits>

namespace molterm {

MolObject::MolObject(std::string name) : name_(std::move(name)) {}

bool MolObject::reprVisible(ReprType r) const {
    auto it = reprVisible_.find(r);
    return it != reprVisible_.end() && it->second;
}

void MolObject::showRepr(ReprType r) {
    reprVisible_[r] = true;
    reprAtomMask_.erase(r);  // clear per-atom mask → show all
}

void MolObject::showReprForAtoms(ReprType r, const std::vector<int>& indices) {
    reprVisible_[r] = true;
    auto& mask = reprAtomMask_[r];
    if (mask.empty()) mask.assign(atoms_.size(), false);
    for (int idx : indices) {
        if (idx >= 0 && idx < static_cast<int>(mask.size()))
            mask[idx] = true;
    }
}

void MolObject::hideRepr(ReprType r) {
    reprVisible_[r] = false;
    reprAtomMask_.erase(r);
}

void MolObject::hideReprForAtoms(ReprType r, const std::vector<int>& indices) {
    auto maskIt = reprAtomMask_.find(r);
    if (maskIt == reprAtomMask_.end()) {
        // Currently showing all → create mask with all true, then turn off selected
        reprAtomMask_[r].assign(atoms_.size(), true);
        maskIt = reprAtomMask_.find(r);
    }
    for (int idx : indices) {
        if (idx >= 0 && idx < static_cast<int>(maskIt->second.size()))
            maskIt->second[idx] = false;
    }
}

void MolObject::hideAllRepr() {
    for (auto& [k, v] : reprVisible_) v = false;
    reprAtomMask_.clear();
}

void MolObject::hideAllReprForAtoms(const std::vector<int>& indices) {
    for (auto& [r, visible] : reprVisible_) {
        if (!visible) continue;
        hideReprForAtoms(r, indices);
    }
}

bool MolObject::reprVisibleForAtom(ReprType r, int atomIdx) const {
    if (!reprVisible(r)) return false;
    auto it = reprAtomMask_.find(r);
    if (it == reprAtomMask_.end() || it->second.empty()) return true;  // no mask → all visible
    if (atomIdx < 0 || atomIdx >= static_cast<int>(it->second.size())) return false;
    return it->second[atomIdx];
}

void MolObject::computeBoundingBox(float& minX, float& minY, float& minZ,
                                    float& maxX, float& maxY, float& maxZ) const {
    minX = minY = minZ = std::numeric_limits<float>::max();
    maxX = maxY = maxZ = std::numeric_limits<float>::lowest();
    for (const auto& a : atoms_) {
        minX = std::min(minX, a.x); maxX = std::max(maxX, a.x);
        minY = std::min(minY, a.y); maxY = std::max(maxY, a.y);
        minZ = std::min(minZ, a.z); maxZ = std::max(maxZ, a.z);
    }
}

void MolObject::computeCenter(float& cx, float& cy, float& cz) const {
    cx = cy = cz = 0.0f;
    if (atoms_.empty()) return;
    for (const auto& a : atoms_) {
        cx += a.x; cy += a.y; cz += a.z;
    }
    float n = static_cast<float>(atoms_.size());
    cx /= n; cy /= n; cz /= n;
}

namespace {
// Build a dense 0..N-1 → newIdx map from a list of indices to keep.
// Returns a vector of size atomCount where entry[i] is the new index
// of original atom i, or -1 if i isn't kept. Used by subset() and
// removeAtoms() to remap bonds + per-atom state in one pass.
std::vector<int> buildKeepMap(const std::vector<int>& keep, int atomCount) {
    std::vector<int> map(atomCount, -1);
    int next = 0;
    // Sort+unique keep so the new ordering is deterministic and the
    // surviving atoms keep their original relative order — mirrors how
    // Selection::indices() comes back sorted.
    std::vector<int> sortedKeep = keep;
    std::sort(sortedKeep.begin(), sortedKeep.end());
    sortedKeep.erase(std::unique(sortedKeep.begin(), sortedKeep.end()), sortedKeep.end());
    for (int idx : sortedKeep) {
        if (idx >= 0 && idx < atomCount) map[idx] = next++;
    }
    return map;
}

// Compact a per-atom vector<T> based on the keep map. Entries with
// map[i] == -1 are dropped; survivors land at their new index. `fill`
// only seeds slots that fall off the end of `src` (e.g. lazy per-atom
// state shorter than atoms_); every kept slot within range is then
// overwritten from src, so this is a safety net rather than a true
// default.
template <typename T>
std::vector<T> remapPerAtom(const std::vector<T>& src,
                            const std::vector<int>& keepMap,
                            T fill) {
    if (src.empty()) return {};
    int newSize = 0;
    for (int idx : keepMap) if (idx >= 0) ++newSize;
    std::vector<T> out(newSize, fill);
    for (size_t i = 0; i < keepMap.size() && i < src.size(); ++i) {
        if (keepMap[i] >= 0) out[keepMap[i]] = src[i];
    }
    return out;
}
}  // namespace

std::unique_ptr<MolObject> MolObject::subset(const std::vector<int>& keep,
                                              const std::string& newName) const {
    auto out = std::make_unique<MolObject>(newName);
    out->sourcePath_ = sourcePath_;
    out->visible_ = visible_;
    out->colorScheme_ = colorScheme_;
    out->reprVisible_ = reprVisible_;

    auto keepMap = buildKeepMap(keep, static_cast<int>(atoms_.size()));
    int newSize = 0;
    for (int v : keepMap) if (v >= 0) ++newSize;
    out->atoms_.resize(newSize);
    for (size_t i = 0; i < keepMap.size(); ++i) {
        if (keepMap[i] >= 0) out->atoms_[keepMap[i]] = atoms_[i];
    }

    // Bonds: keep only those with both endpoints surviving; remap indices.
    out->bonds_.reserve(bonds_.size());
    for (const auto& b : bonds_) {
        if (b.atom1 < 0 || b.atom1 >= static_cast<int>(keepMap.size())) continue;
        if (b.atom2 < 0 || b.atom2 >= static_cast<int>(keepMap.size())) continue;
        int n1 = keepMap[b.atom1];
        int n2 = keepMap[b.atom2];
        if (n1 < 0 || n2 < 0) continue;
        BondData nb = b;
        nb.atom1 = n1;
        nb.atom2 = n2;
        out->bonds_.push_back(nb);
    }

    out->atomColors_ = remapPerAtom<int>(atomColors_, keepMap, -1);
    out->atomAlpha_  = remapPerAtom<float>(atomAlpha_, keepMap, 1.0f);
    for (const auto& [r, mask] : reprAtomMask_) {
        out->reprAtomMask_[r] = remapPerAtom<bool>(mask, keepMap, false);
    }
    // states_, ssPerState_, rainbowCache_ deliberately not propagated:
    // multi-state subsetting is ambiguous (per-state atom counts can
    // differ); SS + rainbow are derivable on demand from the new atom
    // sequence.
    return out;
}

void MolObject::removeAtoms(const std::vector<int>& remove) {
    if (remove.empty()) return;
    std::vector<bool> drop(atoms_.size(), false);
    for (int idx : remove) {
        if (idx >= 0 && idx < static_cast<int>(atoms_.size())) drop[idx] = true;
    }
    std::vector<int> keep;
    keep.reserve(atoms_.size());
    for (int i = 0; i < static_cast<int>(atoms_.size()); ++i) {
        if (!drop[i]) keep.push_back(i);
    }
    auto trimmed = subset(keep, name_);
    atoms_      = std::move(trimmed->atoms_);
    bonds_      = std::move(trimmed->bonds_);
    atomColors_ = std::move(trimmed->atomColors_);
    atomAlpha_  = std::move(trimmed->atomAlpha_);
    reprAtomMask_ = std::move(trimmed->reprAtomMask_);
    rainbowCache_.clear();
    invalidateSSCache();
    invalidateSASACache();
    // states_ is dropped — see subset() rationale.
    states_.clear();
}

const std::vector<float>& MolObject::rainbowFractions() const {
    if (rainbowCache_.size() == atoms_.size()) return rainbowCache_;

    int n = static_cast<int>(atoms_.size());
    rainbowCache_.resize(n, 0.0f);
    if (n == 0) return rainbowCache_;

    // Single pass: find chain boundaries and assign fractions
    int chainStart = 0;
    for (int i = 1; i <= n; ++i) {
        if (i == n || atoms_[i].chainId != atoms_[chainStart].chainId) {
            int chainLen = i - chainStart;
            for (int j = chainStart; j < i; ++j) {
                rainbowCache_[j] = (chainLen <= 1)
                    ? 0.5f
                    : static_cast<float>(j - chainStart) / static_cast<float>(chainLen - 1);
            }
            chainStart = i;
        }
    }
    return rainbowCache_;
}

void MolObject::setAtomColor(int idx, int colorPair) {
    if (idx < 0 || idx >= static_cast<int>(atoms_.size())) return;
    if (atomColors_.size() != atoms_.size())
        atomColors_.assign(atoms_.size(), -1);
    atomColors_[idx] = colorPair;
}

void MolObject::setAtomColors(const std::vector<int>& indices, int colorPair) {
    if (atomColors_.size() != atoms_.size())
        atomColors_.assign(atoms_.size(), -1);
    for (int idx : indices) {
        if (idx >= 0 && idx < static_cast<int>(atoms_.size()))
            atomColors_[idx] = colorPair;
    }
}

void MolObject::clearAtomColors() {
    atomColors_.clear();
}

void MolObject::setAtomAlpha(int idx, float alpha) {
    if (idx < 0 || idx >= static_cast<int>(atoms_.size())) return;
    if (atomAlpha_.size() != atoms_.size())
        atomAlpha_.assign(atoms_.size(), 1.0f);
    atomAlpha_[idx] = alpha;
}

void MolObject::setAtomAlphas(const std::vector<int>& indices, float alpha) {
    if (atomAlpha_.size() != atoms_.size())
        atomAlpha_.assign(atoms_.size(), 1.0f);
    for (int idx : indices) {
        if (idx >= 0 && idx < static_cast<int>(atoms_.size()))
            atomAlpha_[idx] = alpha;
    }
}

void MolObject::setAtomAlphaAll(float alpha) {
    atomAlpha_.assign(atoms_.size(), alpha);
}

void MolObject::clearAtomAlpha() {
    atomAlpha_.clear();
}

void MolObject::addState(std::vector<AtomData> atomState) {
    states_.push_back(std::move(atomState));
}

bool MolObject::setActiveState(int idx) {
    if (idx < 0 || idx >= static_cast<int>(states_.size())) return false;
    activeState_ = idx;
    atoms_ = states_[idx];
    rainbowCache_.clear();
    sasaRelCache_.clear();   // relative accessibility is per-state
    // Sync atoms_[i].ssType from the per-state DSSP cache (computed on
    // demand). Trajectory frames may have different SS, so the cartoon
    // mesh cache (keyed on activeState) will pick up the change.
    const auto& ss = ssAtState(idx);
    if (ss.size() == atoms_.size()) {
        for (size_t i = 0; i < atoms_.size(); ++i) atoms_[i].ssType = ss[i];
    }
    return true;
}

bool MolObject::nextState() {
    if (states_.size() <= 1) return false;
    return setActiveState((activeState_ + 1) % static_cast<int>(states_.size()));
}

bool MolObject::prevState() {
    if (states_.size() <= 1) return false;
    int next = activeState_ - 1;
    if (next < 0) next = static_cast<int>(states_.size()) - 1;
    return setActiveState(next);
}

const std::vector<SSType>& MolObject::ssAtState(int stateIdx) const {
    // Empty fallback for invalid state indices.
    static const std::vector<SSType> kEmpty;

    // Single-state structures: stateIdx 0 maps to atoms_, no states_
    // entry exists. Cache slot 0 is still used.
    int nStates = std::max<int>(1, static_cast<int>(states_.size()));
    if (stateIdx < 0 || stateIdx >= nStates) return kEmpty;

    if ((int)ssPerState_.size() < nStates) ssPerState_.resize(nStates);

    auto& cell = ssPerState_[stateIdx];
    if (!cell.empty()) return cell;

    // Compute. For multi-state files we run DSSP against the stored
    // state's atoms; for single-state we use atoms_ directly.
    const std::vector<AtomData>* src = &atoms_;
    if (!states_.empty() && stateIdx < (int)states_.size()) {
        src = &states_[stateIdx];
    }
    cell = molterm::dssp::compute(*src);
    return cell;
}

void MolObject::invalidateSSCache() {
    ssPerState_.clear();
}

void MolObject::seedSSCacheFromAtoms(int stateIdx) const {
    int nStates = std::max<int>(1, static_cast<int>(states_.size()));
    if (stateIdx < 0 || stateIdx >= nStates) return;
    if ((int)ssPerState_.size() < nStates) ssPerState_.resize(nStates);
    auto& cell = ssPerState_[stateIdx];
    cell.resize(atoms_.size());
    for (size_t i = 0; i < atoms_.size(); ++i) cell[i] = atoms_[i].ssType;
}

const std::vector<float>& MolObject::sasaAtState(int stateIdx) const {
    static const std::vector<float> kEmpty;

    int nStates = std::max<int>(1, static_cast<int>(states_.size()));
    if (stateIdx < 0 || stateIdx >= nStates) return kEmpty;

    if ((int)sasaPerState_.size() < nStates) sasaPerState_.resize(nStates);

    auto& cell = sasaPerState_[stateIdx];
    if (!cell.empty()) return cell;

    const std::vector<AtomData>* src = &atoms_;
    if (!states_.empty() && stateIdx < (int)states_.size()) {
        src = &states_[stateIdx];
    }
    cell = molterm::sasa::compute(*src);
    return cell;
}

void MolObject::invalidateSASACache() {
    sasaPerState_.clear();
    sasaRelCache_.clear();
}

const std::vector<float>& MolObject::sasaRelFractions() const {
    if (sasaRelCache_.size() == atoms_.size()) return sasaRelCache_;
    const auto& abs = sasaAtState(activeState_);
    if (abs.size() != atoms_.size()) {
        sasaRelCache_.assign(atoms_.size(), 0.0f);
        return sasaRelCache_;
    }
    sasaRelCache_ = molterm::sasa::relativePerAtom(atoms_, abs);
    return sasaRelCache_;
}

std::vector<bool> MolObject::atomVisMask(ReprType r) const {
    if (!hasPerAtomRepr()) return {};
    std::vector<bool> mask(atoms_.size());
    for (size_t i = 0; i < atoms_.size(); ++i)
        mask[i] = reprVisibleForAtom(r, static_cast<int>(i));
    return mask;
}

void MolObject::applySmartDefaults() {
    bool hasProtein = false;
    bool hasNA = false;
    bool hasLigand = false;
    std::vector<int> ligandAtoms;

    for (int i = 0; i < static_cast<int>(atoms_.size()); ++i) {
        const auto& a = atoms_[i];
        if (isStandardAA(a.resName)) {
            hasProtein = true;
        } else if (isStandardNA(a.resName)) {
            hasNA = true;
        } else if (a.isHet) {
            hasLigand = true;
            ligandAtoms.push_back(i);
        }
    }

    bool isSmallMol = (!hasProtein && !hasNA && atoms_.size() < 100);

    if (isSmallMol) {
        // Small molecule: ball-and-stick for everything
        hideAllRepr();
        showRepr(ReprType::BallStick);
        return;
    }

    // Macromolecule: cartoon for protein/NA, ballstick for ligands
    hideAllRepr();

    if (hasProtein || hasNA) {
        if (atoms_.size() > Representation::backboneCutoff) {
            // Very large: backbone only
            showRepr(ReprType::Backbone);
        } else {
            showRepr(ReprType::Cartoon);
        }
        setColorScheme(ColorScheme::Chain);
    }

    if (hasLigand && !ligandAtoms.empty()) {
        showReprForAtoms(ReprType::Wireframe, ligandAtoms);
    }
}

} // namespace molterm
