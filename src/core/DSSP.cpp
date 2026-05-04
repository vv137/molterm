#include "molterm/core/DSSP.h"
#include "molterm/core/SpatialHash.h"

#include <cmath>
#include <vector>

namespace molterm::dssp {

namespace {

// ── Kabsch-Sander parameters ────────────────────────────────────────
// E = q1·q2·(1/r_ON + 1/r_CH − 1/r_OH − 1/r_CN) · 332 kcal·Å/mol
// H-bond accepted when E < kHBondCutoff.
constexpr float kQ1 = 0.42f;
constexpr float kQ2 = 0.20f;
constexpr float kCoulomb = 332.0f;          // f kcal·Å/(mol·e²)
constexpr float kHBondCutoff = -0.5f;       // kcal/mol
constexpr float kHBondMaxDist = 5.2f;       // Å — N…O distance gating
constexpr float kNHBondLength = 1.01f;      // Å — virtual N-H distance

inline float dist3(float ax, float ay, float az,
                   float bx, float by, float bz) {
    float dx = ax - bx, dy = ay - by, dz = az - bz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Per-residue backbone slot. -1 means "atom missing" (e.g. terminus).
struct Res {
    int firstAtom = -1, lastAtom = -1;   // [first, last] inclusive in atoms_
    std::string chainId;
    int resSeq = 0;
    char insCode = ' ';
    int N = -1, CA = -1, C = -1, O = -1;
    float Hx = 0, Hy = 0, Hz = 0;
    bool hasH = false;
    bool isAA = false;                    // amino-acid backbone present
};

// Residues sharing the same chain are linearly indexed; H-bond pattern
// matching uses (i, i+n) offsets in this linear order. Chain breaks
// are detected by chainId mismatch — bonds across chain boundaries are
// possible (sheets between chains) but turns are intra-chain only.

bool isStandardAA(const std::string& resName) {
    // Cheap whitelist; anything else is treated as non-protein (loop).
    // (We don't need a perfect set — DSSP just degrades gracefully on
    // unrecognized residues by leaving them as Loop.)
    static const char* kAA[] = {
        "ALA","ARG","ASN","ASP","CYS","GLN","GLU","GLY","HIS","ILE",
        "LEU","LYS","MET","PHE","PRO","SER","THR","TRP","TYR","VAL",
        "MSE","SEC","PYL",   // selenomet, selenocys, pyrrolysine
    };
    for (const char* s : kAA) {
        if (resName == s) return true;
    }
    return false;
}

// Group atoms by (chainId, resSeq, insCode) preserving original order.
std::vector<Res> collectResidues(const std::vector<AtomData>& atoms) {
    std::vector<Res> residues;
    int n = static_cast<int>(atoms.size());
    int i = 0;
    while (i < n) {
        int rs = i;
        while (i < n &&
               atoms[i].chainId == atoms[rs].chainId &&
               atoms[i].resSeq == atoms[rs].resSeq &&
               atoms[i].insCode == atoms[rs].insCode) {
            ++i;
        }
        Res r;
        r.firstAtom = rs;
        r.lastAtom  = i - 1;
        r.chainId   = atoms[rs].chainId;
        r.resSeq    = atoms[rs].resSeq;
        r.insCode   = atoms[rs].insCode;
        r.isAA      = isStandardAA(atoms[rs].resName) && !atoms[rs].isHet;
        for (int j = rs; j < i; ++j) {
            const auto& a = atoms[j];
            if (a.name == "N")  r.N  = j;
            if (a.name == "CA") r.CA = j;
            if (a.name == "C")  r.C  = j;
            if (a.name == "O")  r.O  = j;
        }
        residues.push_back(std::move(r));
    }
    return residues;
}

// Place virtual amide H 1.01 Å from N along the previous residue's
// carbonyl direction. Standard DSSP heuristic — first residue of each
// chain gets no H (no donor).
void placeHydrogens(std::vector<Res>& residues,
                    const std::vector<AtomData>& atoms) {
    for (size_t i = 1; i < residues.size(); ++i) {
        Res& cur  = residues[i];
        Res& prev = residues[i - 1];
        if (cur.chainId != prev.chainId) continue;     // chain break — skip
        if (!cur.isAA || cur.N < 0) continue;
        if (prev.C < 0 || prev.O < 0) continue;
        if (atoms[cur.N].resName == "PRO") continue;   // proline has no NH

        const auto& C = atoms[prev.C];
        const auto& O = atoms[prev.O];
        const auto& N = atoms[cur.N];
        float dx = C.x - O.x, dy = C.y - O.y, dz = C.z - O.z;
        float len = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (len < 1e-6f) continue;
        cur.Hx = N.x + kNHBondLength * dx / len;
        cur.Hy = N.y + kNHBondLength * dy / len;
        cur.Hz = N.z + kNHBondLength * dz / len;
        cur.hasH = true;
    }
}

// Kabsch-Sander electrostatic H-bond energy (donor i = N-H of res i,
// acceptor j = C=O of res j).
float hbondEnergy(const Res& donor, const Res& acceptor,
                  const std::vector<AtomData>& atoms) {
    if (!donor.hasH || acceptor.O < 0 || acceptor.C < 0 || donor.N < 0) {
        return 0.0f;
    }
    const auto& O = atoms[acceptor.O];
    const auto& C = atoms[acceptor.C];
    const auto& N = atoms[donor.N];

    float r_ON = dist3(O.x, O.y, O.z, N.x, N.y, N.z);
    float r_CH = dist3(C.x, C.y, C.z, donor.Hx, donor.Hy, donor.Hz);
    float r_OH = dist3(O.x, O.y, O.z, donor.Hx, donor.Hy, donor.Hz);
    float r_CN = dist3(C.x, C.y, C.z, N.x, N.y, N.z);

    if (r_ON < 0.5f || r_CH < 0.5f || r_OH < 0.5f || r_CN < 0.5f) {
        return 0.0f;     // collapse — atoms coincident, skip
    }
    return kQ1 * kQ2 * (1.0f/r_ON + 1.0f/r_CH - 1.0f/r_OH - 1.0f/r_CN) * kCoulomb;
}

// Build the donor→acceptor H-bond table. hbondPartner[i] = j means
// residue i donates H to residue j's carbonyl (or -1 if none qualifies).
// We keep only the strongest partner per donor — sufficient for the
// patterns we recognise (α-helix, β-bridges).
std::vector<int> findHBondPartners(const std::vector<Res>& residues,
                                   const std::vector<AtomData>& atoms) {
    const int R = static_cast<int>(residues.size());
    std::vector<int> partner(R, -1);
    std::vector<float> bestE(R, 0.0f);

    // Spatial hash over carbonyl O atoms to limit O(N²) → O(N) average.
    SpatialHash hash(kHBondMaxDist, R);
    for (int j = 0; j < R; ++j) {
        if (residues[j].O < 0 || !residues[j].isAA) continue;
        const auto& O = atoms[residues[j].O];
        hash.insert(j, O.x, O.y, O.z);
    }

    for (int i = 0; i < R; ++i) {
        const Res& donor = residues[i];
        if (!donor.hasH || donor.N < 0 || !donor.isAA) continue;
        const auto& N = atoms[donor.N];
        hash.forEachNeighbor(N.x, N.y, N.z, kHBondMaxDist,
            [&](int j) {
                if (j == i) return;                     // self
                const Res& acc = residues[j];
                if (!acc.isAA || acc.O < 0) return;
                // Skip immediate neighbours (no DSSP H-bond between
                // i and i±1 — not chemically meaningful).
                if (donor.chainId == acc.chainId &&
                    std::abs(j - i) <= 1) return;
                float E = hbondEnergy(donor, acc, atoms);
                if (E < kHBondCutoff && E < bestE[i]) {
                    bestE[i] = E;
                    partner[i] = j;
                }
            });
    }
    return partner;
}

// Helper: is residue i H-bonded to residue j?
inline bool hb(const std::vector<int>& p, int i, int j) {
    if (i < 0 || i >= (int)p.size()) return false;
    return p[i] == j;
}

// Helix patterns (Kabsch-Sander):
//   "n-turn at residue i" = the NH of (i+n) hydrogen-bonds to the CO of i,
//   i.e. partner[i+n] == i (donor i+n → acceptor i).
//   An n-helix spanning residues (i, ..., i+n-1) requires two consecutive
//   n-turns at (i-1) and (i). All n-helix residues are mapped to molterm's
//   single Helix class (we don't distinguish α / 3-10 / π).
//
//   Priority: α (n=4) > 3-10 (n=3) > π (n=5). The α pass runs first so
//   later passes only fill in residues still left as Loop — matches DSSP's
//   convention that overlapping turns prefer α.
void assignHelices(const std::vector<int>& partner,
                   const std::vector<Res>& residues,
                   std::vector<SSType>& ssRes) {
    const int R = static_cast<int>(residues.size());

    auto turnN = [&](int i, int n) -> bool {
        if (i < 0 || i + n >= R) return false;
        const std::string& cid = residues[i].chainId;
        for (int k = 1; k <= n; ++k) {
            if (residues[i + k].chainId != cid) return false;
        }
        return partner[i + n] == i;
    };

    auto markRange = [&](int i, int n, bool fillOnlyLoop) {
        for (int k = 0; k < n; ++k) {
            int idx = i + k;
            if (idx >= R) break;
            if (residues[idx].chainId != residues[i].chainId) continue;
            if (fillOnlyLoop && ssRes[idx] != SSType::Loop) continue;
            ssRes[idx] = SSType::Helix;
        }
    };

    // α-helix (n=4): primary pass, may overwrite any prior label.
    for (int i = 1; i + 4 <= R; ++i) {
        if (turnN(i - 1, 4) && turnN(i, 4)) markRange(i, 4, /*fillOnlyLoop=*/false);
    }
    // 3-10 helix (n=3): only fills Loop residues.
    for (int i = 1; i + 3 <= R; ++i) {
        if (turnN(i - 1, 3) && turnN(i, 3)) markRange(i, 3, /*fillOnlyLoop=*/true);
    }
    // π-helix (n=5): only fills Loop residues.
    for (int i = 1; i + 5 <= R; ++i) {
        if (turnN(i - 1, 5) && turnN(i, 5)) markRange(i, 5, /*fillOnlyLoop=*/true);
    }
}

// β-sheet via Kabsch-Sander bridges. Two residues i, j (|i-j|≥3)
// form a bridge if either of the following H-bond patterns holds:
//
//   Parallel:
//      HB(i-1, j) AND HB(j, i+1)    OR    HB(j-1, i) AND HB(i, j+1)
//
//   Antiparallel:
//      HB(i, j) AND HB(j, i)        OR    HB(i-1, j+1) AND HB(j-1, i+1)
//
// Any residue participating in at least one bridge is marked Sheet.
void assignSheets(const std::vector<int>& partner,
                  const std::vector<Res>& residues,
                  std::vector<SSType>& ssRes) {
    const int R = static_cast<int>(residues.size());
    auto sameChain = [&](int a, int b) {
        return a >= 0 && b >= 0 && a < R && b < R &&
               residues[a].chainId == residues[b].chainId;
    };

    for (int i = 1; i + 1 < R; ++i) {
        if (!residues[i].isAA) continue;
        if (!sameChain(i - 1, i) || !sameChain(i, i + 1)) continue;

        for (int j = 1; j + 1 < R; ++j) {
            if (j == i || !residues[j].isAA) continue;
            if (residues[i].chainId == residues[j].chainId &&
                std::abs(i - j) < 3) continue;       // too close on same chain
            if (!sameChain(j - 1, j) || !sameChain(j, j + 1)) continue;

            // Parallel
            bool par1 = hb(partner, i - 1, j) && hb(partner, j, i + 1);
            bool par2 = hb(partner, j - 1, i) && hb(partner, i, j + 1);
            // Antiparallel
            bool anti1 = hb(partner, i, j) && hb(partner, j, i);
            bool anti2 = hb(partner, i - 1, j + 1) && hb(partner, j - 1, i + 1);

            if (par1 || par2 || anti1 || anti2) {
                if (ssRes[i] == SSType::Loop) ssRes[i] = SSType::Sheet;
                if (ssRes[j] == SSType::Loop) ssRes[j] = SSType::Sheet;
            }
        }
    }
}

}  // anonymous namespace

std::vector<SSType> compute(const std::vector<AtomData>& atoms) {
    std::vector<SSType> result(atoms.size(), SSType::Loop);

    auto residues = collectResidues(atoms);
    if (residues.empty()) return result;

    placeHydrogens(residues, atoms);
    auto partner = findHBondPartners(residues, atoms);

    // Per-residue SS labels, then projected back onto every atom of
    // each residue (the existing AtomData.ssType field is per-atom).
    std::vector<SSType> ssRes(residues.size(), SSType::Loop);

    // Priority: helix wins over sheet wins over loop. assignHelices
    // overwrites; assignSheets only fills in Loop slots so a residue
    // that's both 4-turn and bridge stays Helix.
    assignHelices(partner, residues, ssRes);
    assignSheets(partner, residues, ssRes);

    for (size_t r = 0; r < residues.size(); ++r) {
        SSType ss = ssRes[r];
        for (int j = residues[r].firstAtom; j <= residues[r].lastAtom; ++j) {
            result[j] = ss;
        }
    }
    return result;
}

} // namespace molterm::dssp
