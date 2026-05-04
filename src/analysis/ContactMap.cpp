#include "molterm/analysis/ContactMap.h"
#include "molterm/core/SpatialHash.h"

#include <cmath>
#include <limits>
#include <unordered_map>

namespace molterm {

namespace {

// Side-chain atoms we treat as carrying a formal positive charge at
// physiological pH — guanidinium of Arg and ε-ammonium of Lys.
bool isPositive(const std::string& name, const std::string& resName) {
    if (resName == "LYS" && name == "NZ") return true;
    if (resName == "ARG" &&
        (name == "NE" || name == "NH1" || name == "NH2")) return true;
    return false;
}

// Side-chain carboxylate oxygens — Asp Oδ, Glu Oε. C-terminal OXT
// would also qualify but the renderer only chases inter-chain pairs.
bool isNegative(const std::string& name, const std::string& resName) {
    if (resName == "ASP" && (name == "OD1" || name == "OD2")) return true;
    if (resName == "GLU" && (name == "OE1" || name == "OE2")) return true;
    return false;
}

bool isHydrophobicResidue(const std::string& resName) {
    return resName == "ALA" || resName == "VAL" || resName == "LEU" ||
           resName == "ILE" || resName == "MET" || resName == "PHE" ||
           resName == "PRO" || resName == "TRP" || resName == "CYS";
}

InteractionType classifyContact(const AtomData& a, const AtomData& b,
                                float dist) {
    if (dist <= 4.0f) {
        bool aPos = isPositive(a.name, a.resName);
        bool aNeg = isNegative(a.name, a.resName);
        bool bPos = isPositive(b.name, b.resName);
        bool bNeg = isNegative(b.name, b.resName);
        if ((aPos && bNeg) || (aNeg && bPos))
            return InteractionType::SaltBridge;
    }
    if (dist <= 3.5f) {
        bool aHB = (a.element == "N" || a.element == "O");
        bool bHB = (b.element == "N" || b.element == "O");
        if (aHB && bHB) return InteractionType::HBond;
    }
    if (dist <= 4.5f &&
        a.element == "C" && b.element == "C" &&
        isHydrophobicResidue(a.resName) &&
        isHydrophobicResidue(b.resName)) {
        return InteractionType::Hydrophobic;
    }
    return InteractionType::Other;
}

int interactionPriority(InteractionType t) {
    switch (t) {
        case InteractionType::SaltBridge:  return 3;
        case InteractionType::HBond:       return 2;
        case InteractionType::Hydrophobic: return 1;
        case InteractionType::Other:       return 0;
    }
    return 0;
}

}  // namespace

void ContactMap::clear() {
    residues_.clear();
    contacts_.clear();
    interfaceContacts_.clear();
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
    // Compute inter-chain contacts using a spatial-hash neighbor walk.
    // Each residue pair keeps the highest-priority atomic interaction
    // we can find within the search cutoff: salt bridge wins over a
    // hydrogen bond, which wins over a hydrophobic contact, which wins
    // over a generic "other" contact. Within the same priority class
    // the closer atom pair wins.
    //
    // Does NOT destroy the CA-CA contact map / distance matrix.
    interfaceContacts_.clear();

    const auto& atoms = mol.atoms();
    int nAtoms = static_cast<int>(atoms.size());
    if (nAtoms == 0) return;

    if (residues_.empty()) extractResidues(mol);
    int nRes = static_cast<int>(residues_.size());
    if (nRes == 0) return;

    std::vector<int> atomToRes(nAtoms, -1);
    for (int ri = 0; ri < nRes; ++ri) {
        for (int ai = residues_[ri].firstAtomIdx;
             ai <= residues_[ri].lastAtomIdx; ++ai) {
            atomToRes[ai] = ri;
        }
    }

    SpatialHash grid(cutoff, nAtoms);
    for (int i = 0; i < nAtoms; ++i) {
        if (atoms[i].element == "H") continue;
        grid.insert(i, atoms[i].x, atoms[i].y, atoms[i].z);
    }

    struct Best {
        float dist;
        int atom1, atom2;
        InteractionType type;
        int priority;
    };
    std::unordered_map<int64_t, Best> bestByPair;

    for (int i = 0; i < nAtoms; ++i) {
        if (atoms[i].element == "H") continue;
        int ri = atomToRes[i];
        if (ri < 0) continue;

        const auto& ai = atoms[i];
        grid.forEachNeighbor(ai.x, ai.y, ai.z, cutoff, [&](int j) {
            if (j <= i) return;
            if (atoms[j].element == "H") return;
            int rj = atomToRes[j];
            if (rj < 0 || rj == ri) return;
            if (residues_[ri].chainId == residues_[rj].chainId) return;

            float dx = ai.x - atoms[j].x;
            float dy = ai.y - atoms[j].y;
            float dz = ai.z - atoms[j].z;
            float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > cutoff * cutoff) return;
            float d = std::sqrt(d2);

            InteractionType t = classifyContact(ai, atoms[j], d);
            int prio = interactionPriority(t);

            int rLo = std::min(ri, rj), rHi = std::max(ri, rj);
            int64_t key = static_cast<int64_t>(rLo) * nRes + rHi;
            int a1 = (ri < rj) ? i : j;
            int a2 = (ri < rj) ? j : i;

            auto it = bestByPair.find(key);
            if (it == bestByPair.end() ||
                prio > it->second.priority ||
                (prio == it->second.priority && d < it->second.dist)) {
                bestByPair[key] = {d, a1, a2, t, prio};
            }
        });
    }

    interfaceContacts_.reserve(bestByPair.size());
    for (const auto& [key, b] : bestByPair) {
        interfaceContacts_.push_back({b.atom1, b.atom2, b.dist, b.type});
    }
}

float ContactMap::distance(int ri, int rj) const {
    if (ri < 0 || ri >= matrixSize_ || rj < 0 || rj >= matrixSize_)
        return std::numeric_limits<float>::quiet_NaN();
    return distMatrix_[static_cast<size_t>(ri) * matrixSize_ + rj];
}

void ContactMap::filterContacts(bool interfaceOnly) {
    contacts_.clear();
    interfaceContacts_.clear();

    int n = matrixSize_;
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            float d = distMatrix_[static_cast<size_t>(i) * n + j];
            if (std::isnan(d) || d > cutoff_) continue;

            bool interChain = (residues_[i].chainId != residues_[j].chainId);

            if (interfaceOnly && !interChain) continue;

            contacts_.push_back({i, j, d, residues_[i].caAtomIdx, residues_[j].caAtomIdx});

            if (interChain) {
                // CA-CA pairs from the contact map have no atom-name
                // context to classify with; tag them as Other so the
                // overlay can still render them if explicitly enabled.
                interfaceContacts_.push_back({
                    residues_[i].caAtomIdx,
                    residues_[j].caAtomIdx,
                    d,
                    InteractionType::Other,
                });
            }
        }
    }
}

} // namespace molterm
