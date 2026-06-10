#include "molterm/analysis/ContactMap.h"
#include "molterm/core/BondTable.h"
#include "molterm/core/Geometry.h"
#include "molterm/core/SpatialHash.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace molterm {

const char* interactionName(InteractionType t) {
    switch (t) {
        case InteractionType::HBond:       return "hbond";
        case InteractionType::SaltBridge:  return "salt";
        case InteractionType::Hydrophobic: return "hydrophobic";
        case InteractionType::Other:       return "other";
    }
    return "other";
}

int parseInterfaceShowSpec(const std::string& spec) {
    auto trim = [](std::string s) {
        auto issp = [](unsigned char c) { return std::isspace(c) != 0; };
        while (!s.empty() && issp(s.front())) s.erase(s.begin());
        while (!s.empty() && issp(s.back()))  s.pop_back();
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::string lower = trim(spec);
    if (lower.empty())         return -1;
    if (lower == "all")        return kInterfaceShowAll;
    if (lower == "specific")   return kInterfaceShowSpecific;
    if (lower == "none")       return kInterfaceShowNone;

    int mask = 0;
    size_t i = 0;
    while (i < lower.size()) {
        // Accept ',' or '+' as separators — '+' matches the selection-language
        // convention (chain A+B) some users reach for first.
        size_t sep = lower.find_first_of(",+", i);
        std::string tok = trim(lower.substr(i, sep - i));
        if (tok == "hbond" || tok == "h")
            mask |= interactionBit(InteractionType::HBond);
        else if (tok == "salt" || tok == "saltbridge")
            mask |= interactionBit(InteractionType::SaltBridge);
        else if (tok == "hydrophobic" || tok == "hydro")
            mask |= interactionBit(InteractionType::Hydrophobic);
        else if (tok == "other")
            mask |= interactionBit(InteractionType::Other);
        else
            return -1;
        if (sep == std::string::npos) break;
        i = sep + 1;
    }
    return mask;
}

std::string formatInterfaceShowSpec(std::uint8_t mask) {
    if (mask == kInterfaceShowAll)      return "all";
    if (mask == kInterfaceShowSpecific) return "specific";
    if (mask == kInterfaceShowNone)     return "none";
    std::string out;
    auto add = [&](InteractionType t) {
        if (!(mask & interactionBit(t))) return;
        if (!out.empty()) out += ',';
        out += interactionName(t);
    };
    add(InteractionType::HBond);
    add(InteractionType::SaltBridge);
    add(InteractionType::Hydrophobic);
    add(InteractionType::Other);
    return out;
}

namespace {

// Side-chain atoms we treat as carrying a formal positive charge at
// physiological pH — guanidinium of Arg, ε-ammonium of Lys, and the
// imidazolium ring nitrogens of His (partially protonated near pH 7).
bool isPositive(const std::string& name, const std::string& resName) {
    if (resName == "LYS" && name == "NZ") return true;
    if (resName == "ARG" &&
        (name == "NE" || name == "NH1" || name == "NH2")) return true;
    if (resName == "HIS" && (name == "ND1" || name == "NE2")) return true;
    return false;
}

// Heavy-atom hydrogen-bond donor/acceptor heuristic (no hydrogens needed):
//   donor    = any nitrogen (amide/amine/indole/nucleobase NH) or a hydroxyl
//              oxygen (Ser/Thr/Tyr OH, RNA 2'-OH)
//   acceptor = any oxygen, plus the lone-pair ring nitrogens of His and the
//              nucleobases
// An H-bond is a donor opposite an acceptor — this rejects the
// acceptor–acceptor (O···O) and donor–donor (Nbb···Nbb) pairs a bare
// N/O-proximity test wrongly reports.
bool isHBondDonor(const std::string& name, const std::string& element) {
    if (element == "N") return true;
    if (element == "O")
        return name == "OG" || name == "OG1" || name == "OH" ||
               name == "O2'" || name == "O2*";
    return false;
}
bool isHBondAcceptor(const std::string& name, const std::string& element,
                     const std::string& resName) {
    if (element == "O") return true;
    if (element == "N") {
        if (resName == "HIS" && (name == "ND1" || name == "NE2")) return true;
        if (isStandardNA(resName) &&
            (name == "N1" || name == "N3" || name == "N7")) return true;
    }
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
    if (dist <= kSaltBridgeDistCutoff) {
        bool aPos = isPositive(a.name, a.resName);
        bool aNeg = isNegative(a.name, a.resName);
        bool bPos = isPositive(b.name, b.resName);
        bool bNeg = isNegative(b.name, b.resName);
        if ((aPos && bNeg) || (aNeg && bPos))
            return InteractionType::SaltBridge;
    }
    if (dist <= kHBondDistCutoff) {
        bool ab = isHBondDonor(a.name, a.element) &&
                  isHBondAcceptor(b.name, b.element, b.resName);
        bool ba = isHBondAcceptor(a.name, a.element, a.resName) &&
                  isHBondDonor(b.name, b.element);
        if (ab || ba) return InteractionType::HBond;
    }
    if (dist <= kHydrophobicDistCutoff &&
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

    // Group atoms into residues and find CA atoms. The boundary test must
    // include the insertion code: residues sharing chain+resSeq but differing
    // in insCode (antibody CDR loops 100/100A/100B, crystal insertions) are
    // distinct and must not be folded into one — otherwise the second
    // residue's atoms inherit the first's identity and corrupt the contact
    // map. The rest of the codebase keys residues by insCode too.
    std::string prevChain;
    int prevResSeq = std::numeric_limits<int>::min();
    char prevInsCode = '\0';

    for (int i = 0; i < nAtoms; ++i) {
        const auto& a = atoms[i];
        bool newResidue = (a.chainId != prevChain || a.resSeq != prevResSeq ||
                           a.insCode != prevInsCode);

        if (newResidue && !residues_.empty()) {
            residues_.back().lastAtomIdx = i - 1;
        }

        if (newResidue) {
            residues_.push_back({a.resSeq, a.chainId, a.resName, -1, i, i});
            prevChain = a.chainId;
            prevResSeq = a.resSeq;
            prevInsCode = a.insCode;
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
            float d = geom::distance(xi, yi, zi, atoms[aj].x, atoms[aj].y, atoms[aj].z);
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

std::vector<InterfaceContact> ContactMap::detectInteractions(
        const MolObject& mol, float cutoff, bool interChainOnly,
        int minSameChainResGap, const std::vector<int>& scope) {
    std::vector<InterfaceContact> out;
    const auto& atoms = mol.atoms();
    int nAtoms = static_cast<int>(atoms.size());
    if (nAtoms == 0 || cutoff <= 0.0f) return out;

    // Scope mask — which atoms may participate (empty scope = all atoms).
    std::vector<char> inScope(nAtoms, scope.empty() ? 1 : 0);
    if (!scope.empty())
        for (int i : scope)
            if (i >= 0 && i < nAtoms) inScope[i] = 1;

    // Order-independent residue identity, used to keep one best contact per
    // residue pair.
    auto resKey = [](const AtomData& a) {
        return a.chainId + "|" + std::to_string(a.resSeq) + a.insCode;
    };
    // Solvent never participates in a drawn interaction. (CIF loading drops
    // HOH/WAT/DOD already, but PDB-format and non-standard solvent names
    // survive, so filter here too — via the shared BondTable predicate.)
    auto skipAtom = [&](int i) {
        return !inScope[i] || atoms[i].element == "H" || isSolvent(atoms[i].resName);
    };

    SpatialHash grid(cutoff, nAtoms);
    for (int i = 0; i < nAtoms; ++i) {
        if (skipAtom(i)) continue;
        grid.insert(i, atoms[i].x, atoms[i].y, atoms[i].z);
    }

    struct Best { float dist; int a1, a2; InteractionType type; int prio; };
    std::unordered_map<std::string, Best> bestByPair;

    for (int i = 0; i < nAtoms; ++i) {
        if (skipAtom(i)) continue;
        const auto& ai = atoms[i];
        grid.forEachNeighbor(ai.x, ai.y, ai.z, cutoff, [&](int j) {
            if (j <= i) return;
            if (skipAtom(j)) return;
            const auto& aj = atoms[j];
            bool sameChain = (ai.chainId == aj.chainId);
            // Skip the same residue, the inter-chain-only exclusion, and
            // trivial sequential neighbors within the same chain.
            if (sameChain && ai.resSeq == aj.resSeq && ai.insCode == aj.insCode) return;
            if (interChainOnly && sameChain) return;
            if (sameChain && std::abs(ai.resSeq - aj.resSeq) < minSameChainResGap) return;

            float dx = ai.x - aj.x, dy = ai.y - aj.y, dz = ai.z - aj.z;
            float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 > cutoff * cutoff) return;
            float d = std::sqrt(d2);

            InteractionType t = classifyContact(ai, aj, d);
            int prio = interactionPriority(t);

            std::string ka = resKey(ai), kb = resKey(aj);
            std::string key = (ka < kb) ? ka + "~" + kb : kb + "~" + ka;
            auto it = bestByPair.find(key);
            if (it == bestByPair.end() || prio > it->second.prio ||
                (prio == it->second.prio && d < it->second.dist)) {
                bestByPair[key] = {d, i, j, t, prio};
            }
        });
    }

    out.reserve(bestByPair.size());
    for (const auto& [key, b] : bestByPair)
        out.push_back({b.a1, b.a2, b.dist, b.type});
    return out;
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
