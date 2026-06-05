#include "molterm/core/SASA.h"
#include "molterm/core/SpatialHash.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace molterm::sasa {

namespace {

// ── dssp-specific radii (Å) ─────────────────────────────────────────
// Backbone atoms have dedicated radii; every side-chain atom shares one.
// These are NOT element-wise Bondi radii — they are baked into the
// PDB-REDO/dssp accessibility model and must be reproduced verbatim.
constexpr float kRadiusN        = 1.65f;
constexpr float kRadiusCA       = 1.87f;
constexpr float kRadiusC        = 1.76f;
constexpr float kRadiusO        = 1.40f;
constexpr float kRadiusSideAtom = 1.80f;
constexpr float kRadiusWater    = 1.40f;

const double kPI = 4.0 * std::atan(1.0);

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

struct Vec3 { float x = 0, y = 0, z = 0; };

inline float dist2(const Vec3& a, const Vec3& b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx*dx + dy*dy + dz*dz;
}

// A residue's surface atom: position + dssp radius.
struct SurfAtom {
    int   atomIdx;
    Vec3  pos;
    float radius;
};

struct Res {
    int  firstAtom = -1, lastAtom = -1;  // inclusive atom range
    std::string chainId, resName;
    int  resSeq = 0;
    char insCode = ' ';
    bool isAA = false;                    // standard amino acid, non-HET
    std::vector<SurfAtom> surface;        // N/CA/C/O + side-chain (no H)
    Vec3  center;                         // bounding-box midpoint
    float radius = 0;                     // bounding-box max dimension
};

inline float radiusForName(const std::string& name) {
    if (name == "N")  return kRadiusN;
    if (name == "CA") return kRadiusCA;
    if (name == "C")  return kRadiusC;
    if (name == "O")  return kRadiusO;
    return kRadiusSideAtom;
}

// Group atoms by (chainId, resSeq, insCode), preserving order. For standard
// amino-acid residues, gather the surface atoms (skipping hydrogens) and the
// dssp-style bounding box (AABB over atoms each padded by radius + 2·water).
std::vector<Res> collectResidues(const std::vector<AtomData>& atoms) {
    std::vector<Res> residues;
    int n = static_cast<int>(atoms.size());
    int i = 0;
    while (i < n) {
        int rs = i;
        while (i < n &&
               atoms[i].chainId == atoms[rs].chainId &&
               atoms[i].resSeq  == atoms[rs].resSeq &&
               atoms[i].insCode == atoms[rs].insCode) {
            ++i;
        }
        Res r;
        r.firstAtom = rs;
        r.lastAtom  = i - 1;
        r.chainId   = atoms[rs].chainId;
        r.resName   = atoms[rs].resName;
        r.resSeq    = atoms[rs].resSeq;
        r.insCode   = atoms[rs].insCode;
        r.isAA      = isStandardAA(atoms[rs].resName) && !atoms[rs].isHet;

        if (r.isAA) {
            float lo[3] = { 1e30f, 1e30f, 1e30f };
            float hi[3] = { -1e30f, -1e30f, -1e30f };
            for (int j = rs; j < i; ++j) {
                const auto& a = atoms[j];
                if (a.element == "H") continue;   // models lack H in the surface set
                float rad = radiusForName(a.name);
                r.surface.push_back({ j, { a.x, a.y, a.z }, rad });
                float pad = rad + 2.0f * kRadiusWater;
                float c[3] = { a.x, a.y, a.z };
                for (int k = 0; k < 3; ++k) {
                    lo[k] = std::min(lo[k], c[k] - pad);
                    hi[k] = std::max(hi[k], c[k] + pad);
                }
            }
            if (!r.surface.empty()) {
                r.center = { (lo[0]+hi[0])*0.5f, (lo[1]+hi[1])*0.5f, (lo[2]+hi[2])*0.5f };
                r.radius = std::max({ hi[0]-lo[0], hi[1]-lo[1], hi[2]-lo[2] });
            } else {
                r.isAA = false;   // nothing to measure (e.g. CA-only trace)
            }
        }
        residues.push_back(std::move(r));
    }
    return residues;
}

// One neighbouring atom that may occlude the dot sphere of the measured atom.
struct Candidate {
    Vec3   location;   // neighbour position relative to the measured atom
    float  radiusSq;   // (neighbour_radius + water)²
    float  distSq;     // squared distance to the measured atom (sort key)
};

// Fibonacci / golden-section sphere of unit dots, built once. N=200 ⇒ 401
// dots, each carrying weight 4π/401. Verbatim from PDB-REDO MSurfaceDots
// (φ and π are hand-defined here — molterm targets C++17, no std::numbers).
struct SurfaceDots {
    std::vector<Vec3> points;
    double weight = 0;
};

const SurfaceDots& surfaceDots() {
    static const SurfaceDots dots = [] {
        const int    N   = 200;
        const int    P   = 2 * N + 1;
        const double phi = (1.0 + std::sqrt(5.0)) / 2.0;
        SurfaceDots d;
        d.weight = (4.0 * kPI) / P;
        d.points.reserve(P);
        for (int i = -N; i <= N; ++i) {
            double lat = std::asin((2.0 * i) / P);
            double lon = std::fmod(static_cast<double>(i), phi) * 2.0 * kPI / phi;
            d.points.push_back({
                static_cast<float>(std::sin(lon) * std::cos(lat)),
                static_cast<float>(std::cos(lon) * std::cos(lat)),
                static_cast<float>(std::sin(lat)),
            });
        }
        return d;
    }();
    return dots;
}

// Accessible surface of a single atom: fraction of free dots · 4π · radius².
// `cands` is a caller-owned scratch buffer, reused across atoms to avoid a
// heap allocation per atom in this hot path.
float calculateSurface(const Vec3& atom, float inRadius,
                       const std::vector<const Res*>& neighbours,
                       std::vector<Candidate>& cands) {
    cands.clear();
    const float reach0 = inRadius + kRadiusWater;

    for (const Res* r : neighbours) {
        for (const SurfAtom& s : r->surface) {
            float d2 = dist2(atom, s.pos);
            if (d2 <= 0.0001f) continue;            // self / coincident
            float reach = reach0 + (s.radius + kRadiusWater);
            if (d2 >= reach * reach) continue;      // too far to occlude
            float rw = s.radius + kRadiusWater;
            cands.push_back({ { s.pos.x - atom.x, s.pos.y - atom.y, s.pos.z - atom.z },
                              rw * rw, d2 });
        }
    }

    // Closest occluders first — most dots are killed by a near neighbour,
    // so the early-exit below fires sooner.
    std::sort(cands.begin(), cands.end(),
              [](const Candidate& a, const Candidate& b) { return a.distSq < b.distSq; });

    const SurfaceDots& dots = surfaceDots();
    const float radius = reach0;
    double surface = 0.0;

    for (const Vec3& dot : dots.points) {
        Vec3 xx { dot.x * radius, dot.y * radius, dot.z * radius };
        bool free = true;
        for (const Candidate& c : cands) {
            if (!(c.radiusSq < dist2(xx, c.location))) { free = false; break; }
        }
        if (free) surface += dots.weight;
    }

    return static_cast<float>(surface) * radius * radius;
}

}  // anonymous namespace

std::vector<float> compute(const std::vector<AtomData>& atoms) {
    std::vector<float> perAtom(atoms.size(), 0.0f);

    auto residues = collectResidues(atoms);

    // Index of amino-acid residues; neighbour search runs over these only.
    std::vector<int> aa;
    aa.reserve(residues.size());
    float maxRadius = 0.0f;
    for (int i = 0; i < static_cast<int>(residues.size()); ++i) {
        if (residues[i].isAA) {
            aa.push_back(i);
            maxRadius = std::max(maxRadius, residues[i].radius);
        }
    }
    if (aa.empty()) return perAtom;

    // Spatial hash on residue centres — a faithful accelerator for the
    // O(R²) bounding-sphere overlap cull; the occlusion maths is unchanged.
    SpatialHash hash(std::max(1.0f, maxRadius), static_cast<int>(aa.size()));
    for (int ri : aa) {
        const Res& r = residues[ri];
        hash.insert(ri, r.center.x, r.center.y, r.center.z);
    }

    std::vector<const Res*> neighbours;   // scratch, reused per residue
    std::vector<Candidate>  cands;        // scratch, reused per atom
    for (int ri : aa) {
        const Res& r = residues[ri];
        neighbours.clear();
        hash.forEachNeighbor(r.center.x, r.center.y, r.center.z,
                             r.radius + maxRadius, [&](int rj) {
            const Res& o = residues[rj];
            float sum = r.radius + o.radius;
            if (dist2(r.center, o.center) < sum * sum) neighbours.push_back(&o);
        });

        for (const SurfAtom& s : r.surface) {
            perAtom[s.atomIdx] = calculateSurface(s.pos, s.radius, neighbours, cands);
        }
    }

    return perAtom;
}

std::vector<float> relativePerAtom(const std::vector<AtomData>& atoms,
                                   const std::vector<float>& absPerAtom) {
    std::vector<float> rel(atoms.size(), 0.0f);
    if (absPerAtom.size() != atoms.size()) return rel;

    auto residues = collectResidues(atoms);
    for (const Res& r : residues) {
        if (!r.isAA) continue;
        float m = maxAsa(r.resName);
        if (m <= 0.0f) continue;
        double sum = 0.0;
        for (int j = r.firstAtom; j <= r.lastAtom; ++j) sum += absPerAtom[j];
        float v = static_cast<float>(sum) / m;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        for (int j = r.firstAtom; j <= r.lastAtom; ++j) rel[j] = v;
    }
    return rel;
}

float maxAsa(const std::string& resName) {
    // Tien et al. (2013) "Theoretical" maximum accessible surface areas (Å²).
    // PLoS ONE 8(11): e80635. MSE/SEC/PYL fall back to chemically similar
    // residues (MET/CYS/LYS).
    struct E { const char* name; float asa; };
    static const E kTable[] = {
        {"ALA",129.0f}, {"ARG",274.0f}, {"ASN",195.0f}, {"ASP",193.0f},
        {"CYS",167.0f}, {"GLN",225.0f}, {"GLU",223.0f}, {"GLY",104.0f},
        {"HIS",224.0f}, {"ILE",197.0f}, {"LEU",201.0f}, {"LYS",236.0f},
        {"MET",224.0f}, {"PHE",240.0f}, {"PRO",159.0f}, {"SER",155.0f},
        {"THR",172.0f}, {"TRP",285.0f}, {"TYR",263.0f}, {"VAL",174.0f},
        {"MSE",224.0f}, {"SEC",167.0f}, {"PYL",236.0f},
    };
    for (const E& e : kTable) {
        if (resName == e.name) return e.asa;
    }
    return 0.0f;
}

} // namespace molterm::sasa
