#include "molterm/core/MolObject.h"
#include <algorithm>
#include <limits>

namespace molterm {

MolObject::MolObject(std::string name) : name_(std::move(name)) {}

bool MolObject::reprVisible(ReprType r) const {
    auto it = reprVisible_.find(r);
    return it != reprVisible_.end() && it->second;
}

void MolObject::showRepr(ReprType r) { reprVisible_[r] = true; }
void MolObject::hideRepr(ReprType r) { reprVisible_[r] = false; }
void MolObject::hideAllRepr() {
    for (auto& [k, v] : reprVisible_) v = false;
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

} // namespace molterm
