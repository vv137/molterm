#include "molterm/analysis/ContactMap.h"
#include "molterm/core/SpatialHash.h"

#include <cmath>
#include <limits>

namespace molterm {

void ContactMap::clear() {
    residues_.clear();
    contacts_.clear();
    interfacePairs_.clear();
    distMatrix_.clear();
    matrixSize_ = 0;
}

void ContactMap::extractResidues(const MolObject& mol) {
    residues_.clear();
    const auto& atoms = mol.atoms();
    int nAtoms = static_cast<int>(atoms.size());

    // Group atoms into residues and find CA atoms
    std::string prevChain;
    int prevResSeq = std::numeric_limits<int>::min();

    for (int i = 0; i < nAtoms; ++i) {
        const auto& a = atoms[i];
        bool newResidue = (a.chainId != prevChain || a.resSeq != prevResSeq);

        if (newResidue && !residues_.empty()) {
            residues_.back().lastAtomIdx = i - 1;
        }

        if (newResidue) {
            residues_.push_back({a.resSeq, a.chainId, a.resName, -1, i, i});
            prevChain = a.chainId;
            prevResSeq = a.resSeq;
        }

        // Track CA atom
        if (a.name == "CA" && a.element != "CA") {
            // "CA" atom name with element != "CA" (calcium) — this is alpha carbon
            residues_.back().caAtomIdx = i;
        } else if (a.name == "CA" && residues_.back().caAtomIdx < 0) {
            // Fallback: accept any CA
            residues_.back().caAtomIdx = i;
        }

        residues_.back().lastAtomIdx = i;
    }
}

void ContactMap::compute(const MolObject& mol, float cutoff) {
    cutoff_ = cutoff;
    clear();
    extractResidues(mol);

    int n = static_cast<int>(residues_.size());
    matrixSize_ = n;
    distMatrix_.assign(static_cast<size_t>(n) * n, std::numeric_limits<float>::quiet_NaN());

    if (n == 0) return;

    const auto& atoms = mol.atoms();

    // Build NxN distance matrix from CA positions
    for (int i = 0; i < n; ++i) {
        int ai = residues_[i].caAtomIdx;
        if (ai < 0) continue;
        float xi = atoms[ai].x, yi = atoms[ai].y, zi = atoms[ai].z;

        distMatrix_[static_cast<size_t>(i) * n + i] = 0.0f;

        for (int j = i + 1; j < n; ++j) {
            int aj = residues_[j].caAtomIdx;
            if (aj < 0) continue;
            float dx = xi - atoms[aj].x;
            float dy = yi - atoms[aj].y;
            float dz = zi - atoms[aj].z;
            float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            distMatrix_[static_cast<size_t>(i) * n + j] = d;
            distMatrix_[static_cast<size_t>(j) * n + i] = d;
        }
    }

    filterContacts(false);
}

void ContactMap::computeInterface(const MolObject& mol, float cutoff) {
    // Compute inter-chain contacts using closest heavy atom distance.
    // This does NOT destroy the CA-CA contact map / distance matrix.
    // It only populates interfacePairs_.
    interfacePairs_.clear();

    const auto& atoms = mol.atoms();
    int nAtoms = static_cast<int>(atoms.size());
    if (nAtoms == 0) return;

    // If residues not yet extracted, do it now
    if (residues_.empty()) extractResidues(mol);
    int nRes = static_cast<int>(residues_.size());
    if (nRes == 0) return;

    // Build atom → residue index map
    std::vector<int> atomToRes(nAtoms, -1);
    for (int ri = 0; ri < nRes; ++ri) {
        for (int ai = residues_[ri].firstAtomIdx; ai <= residues_[ri].lastAtomIdx; ++ai) {
            atomToRes[ai] = ri;
        }
    }

    // Build spatial hash of all heavy atoms (skip H)
    SpatialHash grid(cutoff, nAtoms);
    for (int i = 0; i < nAtoms; ++i) {
        if (atoms[i].element == "H") continue;
        grid.insert(i, atoms[i].x, atoms[i].y, atoms[i].z);
    }

    // For each inter-chain residue pair, find minimum heavy-atom distance.
    // Track: minDist per (ri, rj) pair, and the corresponding atom pair.
    // Use a flat map keyed by (ri * nRes + rj) to avoid O(nRes^2) memory.
    struct InterfaceContact {
        float dist;
        int atom1, atom2;
    };
    std::unordered_map<int64_t, InterfaceContact> bestContact;

    for (int i = 0; i < nAtoms; ++i) {
        if (atoms[i].element == "H") continue;
        int ri = atomToRes[i];
        if (ri < 0) continue;

        const auto& ai = atoms[i];
        grid.forEachNeighbor(ai.x, ai.y, ai.z, cutoff, [&](int j) {
            if (j <= i) return;  // avoid duplicates
            if (atoms[j].element == "H") return;
            int rj = atomToRes[j];
            if (rj < 0 || rj == ri) return;

            // Inter-chain only
            if (residues_[ri].chainId == residues_[rj].chainId) return;

            float dx = ai.x - atoms[j].x;
            float dy = ai.y - atoms[j].y;
            float dz = ai.z - atoms[j].z;
            float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > cutoff * cutoff) return;

            float d = std::sqrt(d2);

            // Canonical order: smaller residue index first
            int rLo = std::min(ri, rj), rHi = std::max(ri, rj);
            int64_t key = static_cast<int64_t>(rLo) * nRes + rHi;

            auto it = bestContact.find(key);
            if (it == bestContact.end() || d < it->second.dist) {
                int a1 = (ri < rj) ? i : j;
                int a2 = (ri < rj) ? j : i;
                bestContact[key] = {d, a1, a2};
            }
        });
    }

    // Collect interface pairs (closest atom pairs per residue pair)
    interfacePairs_.reserve(bestContact.size());
    for (const auto& [key, contact] : bestContact) {
        interfacePairs_.emplace_back(contact.atom1, contact.atom2);
    }
}

float ContactMap::distance(int ri, int rj) const {
    if (ri < 0 || ri >= matrixSize_ || rj < 0 || rj >= matrixSize_)
        return std::numeric_limits<float>::quiet_NaN();
    return distMatrix_[static_cast<size_t>(ri) * matrixSize_ + rj];
}

void ContactMap::filterContacts(bool interfaceOnly) {
    contacts_.clear();
    interfacePairs_.clear();

    int n = matrixSize_;
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            float d = distMatrix_[static_cast<size_t>(i) * n + j];
            if (std::isnan(d) || d > cutoff_) continue;

            bool interChain = (residues_[i].chainId != residues_[j].chainId);

            if (interfaceOnly && !interChain) continue;

            contacts_.push_back({i, j, d, residues_[i].caAtomIdx, residues_[j].caAtomIdx});

            if (interChain) {
                interfacePairs_.emplace_back(residues_[i].caAtomIdx, residues_[j].caAtomIdx);
            }
        }
    }
}

} // namespace molterm
