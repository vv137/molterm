#include "molterm/core/DSSP.h"
#include "molterm/core/Geometry.h"
#include "molterm/core/SpatialHash.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace molterm::dssp {

namespace {

// ── Kabsch-Sander parameters ────────────────────────────────────────
//   E = -332·q1·q2·(1/r_OH − 1/r_CH + 1/r_CN − 1/r_ON) kcal/mol
//   q1=0.42 (CO), q2=0.20 (NH) → Q = -27.888.
constexpr double kHBondCutoff = -0.5;       // accept H-bond if E < cutoff
constexpr double kHBondMin    = -9.9;       // floor (clamp pathological geometry)
constexpr float  kCAGateDist  = 9.0f;       // Cα–Cα cull radius (Mol*-aligned)
constexpr float  kPeptideMaxSq= 6.25f;      // (2.5 Å)² peptide-bond C(i)…N(i+1)
constexpr float  kNHBondLength= 1.0f;       // virtual N-H length

// ── Per-residue DSSP flags ─────────────────────────────────────────
// Subset of Mol*'s 8-class DSSPType layout — we only keep the bits we
// actually consume. Helices read T(n)S start markers; sheets read E/B
// and helix flags. Mol*'s T/T3/T4/T5/S are dropped because the 3-class
// collapse maps them all to Loop and nothing else inspects them.
namespace F {
    constexpr uint32_t H   = 0x0001;
    constexpr uint32_t B   = 0x0002;
    constexpr uint32_t E   = 0x0004;
    constexpr uint32_t G   = 0x0008;
    constexpr uint32_t I   = 0x0010;
    constexpr uint32_t T3S = 0x0400;
    constexpr uint32_t T4S = 0x0800;
    constexpr uint32_t T5S = 0x1000;
}

struct Res {
    int firstAtom = -1, lastAtom = -1;   // [first, last] inclusive in atoms
    std::string chainId;
    int resSeq = 0;
    char insCode = ' ';
    int N = -1, CA = -1, C = -1, O = -1;
    float Hx = 0, Hy = 0, Hz = 0;
    bool hasH = false;
    bool isAA = false;
};

bool isStandardAA(const std::string& resName) {
    static const char* kAA[] = {
        "ALA","ARG","ASN","ASP","CYS","GLN","GLU","GLY","HIS","ILE",
        "LEU","LYS","MET","PHE","PRO","SER","THR","TRP","TYR","VAL",
        "MSE","SEC","PYL",
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

// True iff residues[i] and residues[i+1] are consecutive in the polypeptide
// (same chain AND C(i)…N(i+1) within peptide-bond range). Lets DSSP detect
// chain breaks even when chainId is shared (e.g. HETATM gap inside a chain).
bool isPeptideBonded(const Res& a, const Res& b, const std::vector<AtomData>& atoms) {
    if (!a.isAA || !b.isAA) return false;
    if (a.chainId != b.chainId) return false;
    if (a.C < 0 || b.N < 0) return false;
    const auto& C = atoms[a.C];
    const auto& N = atoms[b.N];
    float dx = C.x - N.x, dy = C.y - N.y, dz = C.z - N.z;
    return (dx*dx + dy*dy + dz*dz) <= kPeptideMaxSq;
}

// peptideFwd[k] = isPeptideBonded(residues[k], residues[k+1]); the last
// slot is unused. Computed once per DSSP run and reused by every
// "are i..i+span connected?" check downstream — turns this from
// O(R²) string compares into O(R²) bit reads.
std::vector<char> computePeptideForward(const std::vector<Res>& residues,
                                        const std::vector<AtomData>& atoms) {
    int R = static_cast<int>(residues.size());
    std::vector<char> fwd(R, 0);
    for (int k = 0; k + 1 < R; ++k) {
        fwd[k] = isPeptideBonded(residues[k], residues[k + 1], atoms) ? 1 : 0;
    }
    return fwd;
}

// True iff residues [i..i+span] are a peptide-bonded run.
bool runConnected(const std::vector<char>& peptideFwd, int i, int span) {
    int R = static_cast<int>(peptideFwd.size());
    if (i < 0 || i + span >= R) return false;
    for (int k = 0; k < span; ++k) {
        if (!peptideFwd[i + k]) return false;
    }
    return true;
}

// Place virtual amide H 1.0 Å from N along the previous residue's C→O
// direction (Kabsch & Sander heuristic). First residue of each polypeptide
// run gets no H.
void placeHydrogens(std::vector<Res>& residues,
                    const std::vector<AtomData>& atoms) {
    for (size_t i = 1; i < residues.size(); ++i) {
        Res& cur  = residues[i];
        Res& prev = residues[i - 1];
        if (!cur.isAA || cur.N < 0) continue;
        if (atoms[cur.N].resName == "PRO") continue;   // proline: no NH donor
        if (!isPeptideBonded(prev, cur, atoms)) continue;
        if (prev.C < 0 || prev.O < 0) continue;

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

// Energy in double precision: the final value is compared against the
// −0.5 kcal/mol cutoff and we see real PDB structures with a hand-full
// of bonds within ~0.01 kcal/mol of the cutoff. Float would push them
// either side of the boundary unpredictably.
double hbondEnergy(const Res& donor, const Res& acceptor,
                   const std::vector<AtomData>& atoms) {
    if (!donor.hasH || acceptor.O < 0 || acceptor.C < 0 || donor.N < 0) return 0.0;
    const auto& O = atoms[acceptor.O];
    const auto& C = atoms[acceptor.C];
    const auto& N = atoms[donor.N];

    auto d = [](double ax, double ay, double az,
                double bx, double by, double bz) {
        double dx = ax - bx, dy = ay - by, dz = az - bz;
        return std::sqrt(dx*dx + dy*dy + dz*dz);
    };

    double r_OH = d(O.x, O.y, O.z, donor.Hx, donor.Hy, donor.Hz);
    double r_CH = d(C.x, C.y, C.z, donor.Hx, donor.Hy, donor.Hz);
    double r_CN = d(C.x, C.y, C.z, N.x, N.y, N.z);
    double r_ON = d(O.x, O.y, O.z, N.x, N.y, N.z);

    if (r_OH < 0.5 || r_CH < 0.5 || r_CN < 0.5 || r_ON < 0.5) return 0.0;

    double e = -27.888 * (1.0/r_OH - 1.0/r_CH + 1.0/r_CN - 1.0/r_ON);
    if (e < kHBondMin) e = kHBondMin;
    return e;
}

// H-bond table: hash set of (acceptor*R + donor). hb(a, d) returns true iff
// donor d's NH is hydrogen-bonded to acceptor a's CO. Mirrors Mol*'s
// directed H-bond graph: edge (a → d) means CO(a)···HN(d).
//
// Crucially we record EVERY pair with E < cutoff — not just the strongest
// per donor. Bridges/ladders need bidirectional and offset H-bond patterns
// where multiple acceptors share a donor or vice-versa, so dropping
// secondary partners broke ~14% of sheet residues on 4HHB.
struct HBondSet {
    std::unordered_set<int64_t> entries;
    int R = 0;
    void add(int acceptor, int donor) {
        entries.insert(static_cast<int64_t>(acceptor) * R + donor);
    }
    bool has(int acceptor, int donor) const {
        if (acceptor < 0 || donor < 0 || acceptor >= R || donor >= R) return false;
        return entries.count(static_cast<int64_t>(acceptor) * R + donor) > 0;
    }
};

HBondSet findHBonds(const std::vector<Res>& residues,
                    const std::vector<AtomData>& atoms) {
    const int R = static_cast<int>(residues.size());
    HBondSet set;
    set.R = R;
    set.entries.reserve(R * 2);

    SpatialHash hash(kCAGateDist, R);
    for (int i = 0; i < R; ++i) {
        if (residues[i].CA < 0 || !residues[i].isAA) continue;
        const auto& CA = atoms[residues[i].CA];
        hash.insert(i, CA.x, CA.y, CA.z);
    }

    for (int oPI = 0; oPI < R; ++oPI) {
        const Res& acc = residues[oPI];
        if (!acc.isAA || acc.O < 0 || acc.C < 0 || acc.CA < 0) continue;
        const auto& CA = atoms[acc.CA];
        hash.forEachNeighbor(CA.x, CA.y, CA.z, kCAGateDist,
            [&](int nPI) {
                if (nPI == oPI) return;
                if (residues[nPI].chainId == acc.chainId &&
                    std::abs(nPI - oPI) <= 1) return;
                const Res& donor = residues[nPI];
                if (!donor.isAA || !donor.hasH) return;
                float e = hbondEnergy(donor, acc, atoms);
                if (e < kHBondCutoff) set.add(oPI, nPI);
            });
    }
    return set;
}

// n-turn-start at residue i ⇔ Hbond from CO(i) to NH(i+n), n ∈ {3,4,5}.
// Sets only the T(n)S start marker — Mol* also stamps T(n) onto residues
// i..i+n and a generic T on intermediates, but downstream code only
// inspects start markers, so those writes were dead.
void assignTurns(const HBondSet& hb,
                 const std::vector<char>& peptideFwd,
                 std::vector<uint32_t>& flags) {
    const int R = static_cast<int>(flags.size());
    constexpr uint32_t startFlag[6] = {0,0,0, F::T3S, F::T4S, F::T5S};

    for (int n = 3; n <= 5; ++n) {
        for (int i = 0; i + n < R; ++i) {
            if (!runConnected(peptideFwd, i, n)) continue;
            if (!hb.has(i, i + n)) continue;
            flags[i] |= startFlag[n];
        }
    }
}

// Helix(n, i, i+n−1) ⇔ n-turn-start at both i−1 and i.
// Order: α (n=4) ▶ 3₁₀ (n=3) ▶ π (n=5). 3₁₀ yields to α (per Mol* /
// classic DSSP); π does NOT yield, since mkdssp 4.5+ marks π-helices
// that extend α-helices and our 3-class collapse folds {H,G,I} →
// Helix anyway.
void assignHelices(const std::vector<char>& peptideFwd,
                   std::vector<uint32_t>& flags) {
    const int R = static_cast<int>(flags.size());
    constexpr uint32_t startFlag[6] = {0,0,0, F::T3S, F::T4S, F::T5S};
    constexpr uint32_t helixFlag[6] = {0,0,0, F::G,   F::H,   F::I  };
    constexpr int order[3] = {4, 3, 5};

    for (int oi = 0; oi < 3; ++oi) {
        int n = order[oi];
        for (int i = 1; i + n < R; ++i) {
            uint32_t fI  = flags[i];
            uint32_t fI2 = flags[i + 1];
            if (n == 3) {
                if ((fI & F::H) || (fI2 & F::H)) continue;
            }
            uint32_t fI1 = flags[i - 1];
            if (!(fI  & startFlag[n])) continue;
            if (!(fI1 & startFlag[n])) continue;
            if (!runConnected(peptideFwd, i - 1, n + 1)) continue;
            for (int k = 0; k < n; ++k) flags[i + k] |= helixFlag[n];
        }
    }
}

// β-bridge (Kabsch & Sander):
//   Parallel(i,j)      = [Hb(i−1,j) ∧ Hb(j,i+1)] ∨ [Hb(j−1,i) ∧ Hb(i,j+1)]
//   Antiparallel(i,j)  = [Hb(i,j) ∧ Hb(j,i)]     ∨ [Hb(i−1,j+1) ∧ Hb(j−1,i+1)]
// where Hb(a,b) ≡ "CO(a) accepts from NH(b)".
enum class BridgeType { Parallel, Antiparallel };
struct Bridge {
    int p1 = 0, p2 = 0;
    BridgeType type = BridgeType::Parallel;
};

void assignBridges(const HBondSet& hb,
                   const std::vector<Res>& residues,
                   const std::vector<char>& peptideFwd,
                   std::vector<uint32_t>& flags,
                   std::vector<Bridge>& bridges) {
    const int R = static_cast<int>(flags.size());
    auto sameChain = [&](int a, int b) {
        return a >= 0 && b >= 0 && a < R && b < R &&
               residues[a].chainId == residues[b].chainId;
    };

    for (int i = 1; i + 1 < R; ++i) {
        if (!residues[i].isAA) continue;
        if (!peptideFwd[i - 1] || !peptideFwd[i]) continue;
        for (int j = i + 1; j + 1 < R; ++j) {
            if (!residues[j].isAA) continue;
            if (sameChain(i, j) && j - i < 3) continue;
            if (!peptideFwd[j - 1] || !peptideFwd[j]) continue;

            bool par1  = hb.has(i - 1, j) && hb.has(j, i + 1);
            bool par2  = hb.has(j - 1, i) && hb.has(i, j + 1);
            bool anti1 = hb.has(i, j)     && hb.has(j, i);
            bool anti2 = hb.has(i - 1, j + 1) && hb.has(j - 1, i + 1);

            if (par1 || par2) {
                flags[i] |= F::B;
                flags[j] |= F::B;
                bridges.push_back({i, j, BridgeType::Parallel});
            }
            if (anti1 || anti2) {
                flags[i] |= F::B;
                flags[j] |= F::B;
                bridges.push_back({i, j, BridgeType::Antiparallel});
            }
        }
    }
    std::sort(bridges.begin(), bridges.end(),
              [](const Bridge& a, const Bridge& b) {
                  if (a.p1 != b.p1) return a.p1 < b.p1;
                  return a.p2 < b.p2;
              });
}

// Ladder = consecutive bridges of the same type. We extend an existing
// ladder when the next bridge's first partner is the immediate sequence
// neighbor (firstEnd+1) and the second partner aligns with the appropriate
// end (secondEnd+1 for parallel, secondStart−1 for antiparallel).
struct Ladder {
    int firstStart = 0, firstEnd = 0;
    int secondStart = 0, secondEnd = 0;
    BridgeType type = BridgeType::Parallel;
    int nextLadder = -1;   // index of bulge-connected successor, -1 if none
};

bool extendLadder(Ladder& L, const Bridge& b) {
    if (b.type != L.type) return false;
    if (b.p1 != L.firstEnd + 1) return false;
    if (L.type == BridgeType::Parallel) {
        if (b.p2 != L.secondEnd + 1) return false;
        ++L.firstEnd;
        ++L.secondEnd;
    } else {
        if (b.p2 != L.secondStart - 1) return false;
        ++L.firstEnd;
        --L.secondStart;
    }
    return true;
}

// Bulge-linked ladders: same type, next ladder starts within 5 sequence
// positions of L1's firstEnd, and the second-strand gap meets the bulge
// criterion (≤5 residues, with the tighter inner condition). Mirrors
// Mol*'s `resemblesBulge` exactly.
bool bulgeCriterion2(const Ladder& l1, const Ladder& l2) {
    int d2 = l2.secondStart - l1.secondEnd;
    int d1 = l2.firstStart  - l1.firstEnd;
    if (d2 <= 0) return false;
    return (d2 < 6 && d1 < 3) || d2 < 3;
}

bool resemblesBulge(const Ladder& l1, const Ladder& l2) {
    if (l1.type != l2.type) return false;
    if (l2.firstStart - l1.firstEnd >= 6) return false;
    if (l1.firstStart >= l2.firstStart) return false;
    if (l2.nextLadder != -1) return false;
    return l1.type == BridgeType::Parallel
        ? bulgeCriterion2(l1, l2)
        : bulgeCriterion2(l2, l1);
}

void assignLadders(const std::vector<Bridge>& bridges,
                   std::vector<Ladder>& ladders) {
    for (const Bridge& b : bridges) {
        bool extended = false;
        for (Ladder& L : ladders) {
            if (extendLadder(L, b)) { extended = true; break; }
        }
        if (!extended) {
            Ladder L;
            L.firstStart  = b.p1; L.firstEnd  = b.p1;
            L.secondStart = b.p2; L.secondEnd = b.p2;
            L.type = b.type;
            ladders.push_back(L);
        }
    }
    for (size_t i = 0; i < ladders.size(); ++i) {
        for (size_t j = i; j < ladders.size(); ++j) {
            if (resemblesBulge(ladders[i], ladders[j])) {
                ladders[i].nextLadder = static_cast<int>(j);
            }
        }
    }
}

inline bool isHelixFlag(uint32_t f) {
    return (f & (F::H | F::G | F::I)) != 0;
}

// Sheet = set of one or more ladders connected by bulge bridges. Mark E
// for every residue in the ladder's two strands. Singleton ladders (a
// single bridge) get B on both partners. Bulge-connected ladders fill
// their gap with E so the strand renders contiguously.
void assignSheets(const std::vector<Ladder>& ladders,
                  std::vector<uint32_t>& flags) {
    const int R = static_cast<int>(flags.size());
    auto markE = [&](int idx) {
        if (idx >= 0 && idx < R) flags[idx] |= F::E;
    };
    auto markB = [&](int idx) {
        if (idx < 0 || idx >= R) return;
        if (!isHelixFlag(flags[idx]) && !(flags[idx] & F::E)) flags[idx] |= F::B;
    };

    for (const Ladder& L : ladders) {
        for (int lc = L.firstStart; lc <= L.firstEnd; ++lc) {
            int diff = L.firstStart - lc;
            int l2c  = L.secondStart - diff;
            if (L.firstStart != L.firstEnd) {
                markE(lc);
                markE(l2c);
            } else {
                markB(lc);
                markB(l2c);
            }
        }
        if (L.nextLadder < 0) continue;
        const Ladder& next = ladders[L.nextLadder];
        for (int lc = L.firstStart; lc <= next.firstEnd; ++lc) markE(lc);
        if (L.type == BridgeType::Parallel) {
            for (int lc = L.secondStart; lc <= next.secondEnd; ++lc) markE(lc);
        } else {
            for (int lc = next.secondEnd; lc <= L.secondStart; ++lc) markE(lc);
        }
    }
}

// Collapse 8-class DSSP flags to molterm's 3-class SSType using DSSP's
// canonical priority H ▶ E ▶ B ▶ G ▶ I (turns/bends fall through to Loop).
inline SSType collapse(uint32_t f) {
    if (f & F::H) return SSType::Helix;
    if (f & F::E) return SSType::Sheet;
    if (f & F::B) return SSType::Sheet;
    if (f & F::G) return SSType::Helix;
    if (f & F::I) return SSType::Helix;
    return SSType::Loop;
}

}  // anonymous namespace

std::vector<SSType> compute(const std::vector<AtomData>& atoms) {
    std::vector<SSType> result(atoms.size(), SSType::Loop);

    auto residues = collectResidues(atoms);
    if (residues.empty()) return result;

    placeHydrogens(residues, atoms);
    auto peptideFwd = computePeptideForward(residues, atoms);
    HBondSet hb = findHBonds(residues, atoms);

    std::vector<uint32_t> flags(residues.size(), 0);
    std::vector<Bridge>   bridges;
    std::vector<Ladder>   ladders;

    assignTurns(hb, peptideFwd, flags);
    assignHelices(peptideFwd, flags);
    assignBridges(hb, residues, peptideFwd, flags, bridges);
    assignLadders(bridges, ladders);
    assignSheets(ladders, flags);

    for (size_t r = 0; r < residues.size(); ++r) {
        SSType ss = collapse(flags[r]);
        for (int j = residues[r].firstAtom; j <= residues[r].lastAtom; ++j) {
            result[j] = ss;
        }
    }
    return result;
}

namespace {

// Ramachandran box bounds (degrees). The α-helix box is centered on
// (-57°, -47°) and the β-strand box covers φ ∈ [-180, -90], ψ ∈ [+80,
// +180] with a wrap-around tail at ψ ∈ [-180, -160].
struct RamaBox { float phiLo, phiHi, psiLo, psiHi; };
constexpr RamaBox kHelixBox  = {-100.0f, -30.0f, -80.0f, 10.0f};
constexpr RamaBox kSheetBox1 = {-180.0f, -90.0f,  80.0f, 180.0f};
constexpr RamaBox kSheetBox2 = {-180.0f, -90.0f, -180.0f, -160.0f};

constexpr int kMinHelixRun = 4;   // one full α-turn
constexpr int kMinSheetRun = 3;

inline bool inBox(float phi, float psi, const RamaBox& b) {
    return phi >= b.phiLo && phi <= b.phiHi &&
           psi >= b.psiLo && psi <= b.psiHi;
}

// φ/ψ in IUPAC convention (right-handed α-helix at ≈ -57°, -47°) is the
// negation of the standard signed-dihedral atan2 result.
inline float phiPsi(float ax, float ay, float az,
                    float bx, float by, float bz,
                    float cx, float cy, float cz,
                    float dx, float dy, float dz) {
    return -geom::dihedralDeg(ax, ay, az, bx, by, bz,
                              cx, cy, cz, dx, dy, dz);
}

void classifyByRamachandran(const std::vector<AtomData>& atoms,
                            const std::vector<Res>& residues,
                            std::vector<SSType>& ssRes) {
    for (std::size_t k = 0; k < residues.size(); ++k) {
        const auto& r = residues[k];
        if (r.N < 0 || r.CA < 0 || r.C < 0) continue;
        if (k == 0 || k + 1 >= residues.size()) continue;
        const auto& prev = residues[k - 1];
        const auto& next = residues[k + 1];
        if (prev.C < 0 || next.N < 0) continue;
        if (prev.chainId != r.chainId || next.chainId != r.chainId) continue;

        float phi = phiPsi(
            atoms[prev.C].x, atoms[prev.C].y, atoms[prev.C].z,
            atoms[r.N].x,    atoms[r.N].y,    atoms[r.N].z,
            atoms[r.CA].x,   atoms[r.CA].y,   atoms[r.CA].z,
            atoms[r.C].x,    atoms[r.C].y,    atoms[r.C].z);
        float psi = phiPsi(
            atoms[r.N].x,    atoms[r.N].y,    atoms[r.N].z,
            atoms[r.CA].x,   atoms[r.CA].y,   atoms[r.CA].z,
            atoms[r.C].x,    atoms[r.C].y,    atoms[r.C].z,
            atoms[next.N].x, atoms[next.N].y, atoms[next.N].z);

        if (inBox(phi, psi, kHelixBox))                    ssRes[k] = SSType::Helix;
        else if (inBox(phi, psi, kSheetBox1) ||
                 inBox(phi, psi, kSheetBox2))              ssRes[k] = SSType::Sheet;
        else                                                ssRes[k] = SSType::Loop;
    }
}

// Drop runs shorter than kMinHelixRun / kMinSheetRun back to Loop.
// Run boundaries also stop at chain breaks.
void smoothRuns(const std::vector<Res>& residues,
                std::vector<SSType>& ssRes) {
    if (residues.empty()) return;
    auto raw = ssRes;
    std::size_t a = 0;
    while (a < raw.size()) {
        std::size_t b = a + 1;
        while (b < raw.size() && raw[b] == raw[a] &&
               residues[b].chainId == residues[a].chainId) ++b;
        int runLen = static_cast<int>(b - a);
        if ((raw[a] == SSType::Helix && runLen < kMinHelixRun) ||
            (raw[a] == SSType::Sheet && runLen < kMinSheetRun)) {
            for (std::size_t k = a; k < b; ++k) ssRes[k] = SSType::Loop;
        }
        a = b;
    }
}

} // namespace

std::vector<SSType> computeGeometric(const std::vector<AtomData>& atoms) {
    std::vector<SSType> result(atoms.size(), SSType::Loop);
    auto residues = collectResidues(atoms);
    if (residues.empty()) return result;

    std::vector<SSType> ssRes(residues.size(), SSType::Loop);
    classifyByRamachandran(atoms, residues, ssRes);
    smoothRuns(residues, ssRes);

    for (std::size_t k = 0; k < residues.size(); ++k) {
        for (int j = residues[k].firstAtom; j <= residues[k].lastAtom; ++j) {
            result[j] = ssRes[k];
        }
    }
    return result;
}

} // namespace molterm::dssp
