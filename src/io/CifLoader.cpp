#include "molterm/io/CifLoader.h"
#include "molterm/core/BondTable.h"
#include "molterm/core/SpatialHash.h"

#include <gemmi/mmread.hpp>
#include <gemmi/mmread_gz.hpp>
#include <gemmi/model.hpp>
#include <gemmi/assembly.hpp>
#include <gemmi/chemcomp.hpp>
#include <gemmi/logger.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace molterm {

// Covalent radius lookup (Å) — covers common biomolecule elements
static float covalentRadius(const std::string& element) {
    static const std::unordered_map<std::string, float> radii = {
        {"H",  0.31f}, {"C",  0.76f}, {"N",  0.71f}, {"O",  0.66f},
        {"S",  1.05f}, {"P",  1.07f}, {"F",  0.57f}, {"Cl", 1.02f},
        {"Br", 1.20f}, {"I",  1.39f}, {"Se", 1.20f}, {"Fe", 1.32f},
        {"Zn", 1.22f}, {"Mg", 1.41f}, {"Ca", 1.76f}, {"Mn", 1.39f},
        {"Cu", 1.32f}, {"Co", 1.26f}, {"Na", 1.66f}, {"K",  2.03f},
    };
    auto it = radii.find(element);
    return (it != radii.end()) ? it->second : 1.5f;
}

std::unique_ptr<MolObject> CifLoader::load(const std::string& filepath) {
    return loadAuto(filepath);
}

std::unique_ptr<MolObject> CifLoader::loadAuto(const std::string& filepath) {
    std::string fmt = detectFormat(filepath);
    if (fmt == "cif") return loadCif(filepath);
    if (fmt == "pdb") return loadPdb(filepath);
    return loadCif(filepath);
}

// ── Shared helpers ──────────────────────────────────────────────────────────

static std::vector<AtomData> convertModel(const gemmi::Model& model) {
    std::vector<AtomData> result;
    for (const auto& chain : model.chains) {
        for (const auto& res : chain.residues) {
            if (res.name == "HOH" || res.name == "WAT" || res.name == "DOD") continue;
            for (const auto& atom : res.atoms) {
                AtomData ad;
                ad.x = static_cast<float>(atom.pos.x);
                ad.y = static_cast<float>(atom.pos.y);
                ad.z = static_cast<float>(atom.pos.z);
                ad.name = atom.name;
                ad.element = atom.element.name();
                ad.resName = res.name;
                ad.chainId = chain.name;
                ad.resSeq = res.seqid.num.value;
                ad.insCode = res.seqid.icode ? res.seqid.icode : ' ';
                ad.bFactor = static_cast<float>(atom.b_iso);
                ad.occupancy = static_cast<float>(atom.occ);
                ad.serial = atom.serial;
                ad.formalCharge = static_cast<int8_t>(atom.charge);
                ad.isHet = (res.het_flag == 'H');
                result.push_back(std::move(ad));
            }
        }
    }
    return result;
}

// Dihedral angle (deg) of four 3D points, signed in [-180, +180].
static float dihedralDeg(float ax, float ay, float az,
                         float bx, float by, float bz,
                         float cx, float cy, float cz,
                         float dx, float dy, float dz) {
    float b1x = bx - ax, b1y = by - ay, b1z = bz - az;
    float b2x = cx - bx, b2y = cy - by, b2z = cz - bz;
    float b3x = dx - cx, b3y = dy - cy, b3z = dz - cz;
    float b2len = std::sqrt(b2x*b2x + b2y*b2y + b2z*b2z);
    if (b2len < 1e-6f) return 0.0f;
    float n1x = b1y*b2z - b1z*b2y;
    float n1y = b1z*b2x - b1x*b2z;
    float n1z = b1x*b2y - b1y*b2x;
    float n2x = b2y*b3z - b2z*b3y;
    float n2y = b2z*b3x - b2x*b3z;
    float n2z = b2x*b3y - b2y*b3x;
    float bnx = b2x / b2len, bny = b2y / b2len, bnz = b2z / b2len;
    float m1x = n1y*bnz - n1z*bny;
    float m1y = n1z*bnx - n1x*bnz;
    float m1z = n1x*bny - n1y*bnx;
    float x = n1x*n2x + n1y*n2y + n1z*n2z;
    float y = m1x*n2x + m1y*n2y + m1z*n2z;
    // IUPAC convention: φ/ψ for right-handed α-helix should be ≈ (-57°, -47°),
    // so flip the sign of the standard atan2(y, x) result. (Equivalent to
    // using b1 = a - b instead of b - a in the Praxeolitic formula.)
    return -std::atan2(y, x) * 180.0f / 3.14159265f;
}

// Geometric fallback for files without HELIX/SHEET records (CASP/AlphaFold
// predictions, raw coordinate dumps). Classifies each residue by its φ/ψ
// dihedrals — Ramachandran α-region → Helix, β-region → Sheet, else Loop —
// then smooths out single-residue noise. Cheaper and simpler than full DSSP
// (no H-bond energy pass), accurate enough for cartoon visualization.
static void assignSSGeometric(std::vector<AtomData>& atoms) {
    struct Res {
        std::string chain;
        int n = -1, ca = -1, c = -1;
        SSType ss = SSType::Loop;
        int firstAtom = -1, lastAtom = -1;
    };

    auto sameRes = [&](int a, int b) {
        return atoms[a].chainId == atoms[b].chainId &&
               atoms[a].resSeq == atoms[b].resSeq &&
               atoms[a].insCode == atoms[b].insCode;
    };

    std::vector<Res> residues;
    int n = static_cast<int>(atoms.size());
    int i = 0;
    while (i < n) {
        int rs = i;
        while (i < n && sameRes(rs, i)) ++i;
        Res r;
        r.chain = atoms[rs].chainId;
        r.firstAtom = rs;
        r.lastAtom = i - 1;
        for (int j = rs; j < i; ++j) {
            const auto& a = atoms[j];
            if (a.name == "N")  r.n  = j;
            if (a.name == "CA") r.ca = j;
            if (a.name == "C")  r.c  = j;
        }
        residues.push_back(r);
    }

    // φ/ψ classification per residue (skips chain ends and incomplete backbones).
    for (std::size_t k = 0; k < residues.size(); ++k) {
        auto& r = residues[k];
        if (r.n < 0 || r.ca < 0 || r.c < 0) continue;
        if (k == 0 || k + 1 >= residues.size()) continue;
        const auto& prev = residues[k - 1];
        const auto& next = residues[k + 1];
        if (prev.c < 0 || next.n < 0) continue;
        if (prev.chain != r.chain || next.chain != r.chain) continue;

        float phi = dihedralDeg(
            atoms[prev.c].x, atoms[prev.c].y, atoms[prev.c].z,
            atoms[r.n].x,    atoms[r.n].y,    atoms[r.n].z,
            atoms[r.ca].x,   atoms[r.ca].y,   atoms[r.ca].z,
            atoms[r.c].x,    atoms[r.c].y,    atoms[r.c].z);
        float psi = dihedralDeg(
            atoms[r.n].x,    atoms[r.n].y,    atoms[r.n].z,
            atoms[r.ca].x,   atoms[r.ca].y,   atoms[r.ca].z,
            atoms[r.c].x,    atoms[r.c].y,    atoms[r.c].z,
            atoms[next.n].x, atoms[next.n].y, atoms[next.n].z);

        // Right-handed α-helix region centered on (-57°, -47°), allow ±~30°.
        bool helix = (phi >= -100.0f && phi <= -30.0f &&
                      psi >= -80.0f  && psi <=  10.0f);
        // β-strand region: φ very negative, ψ near +130° (with wrap-around tail).
        bool sheet = (phi >= -180.0f && phi <= -90.0f &&
                      ((psi >= 80.0f  && psi <= 180.0f) ||
                       (psi >= -180.0f && psi <= -160.0f)));

        if (helix)      r.ss = SSType::Helix;
        else if (sheet) r.ss = SSType::Sheet;
        else            r.ss = SSType::Loop;
    }

    // Smoothing: require ≥4 consecutive helix residues (one full α-turn) and
    // ≥3 consecutive sheet residues to keep the assignment; isolated hits get
    // dropped back to Loop. Run boundaries also stop at chain breaks.
    if (!residues.empty()) {
        std::vector<SSType> raw(residues.size());
        for (std::size_t k = 0; k < residues.size(); ++k) raw[k] = residues[k].ss;
        auto out = raw;
        std::size_t a = 0;
        while (a < raw.size()) {
            std::size_t b = a + 1;
            while (b < raw.size() && raw[b] == raw[a] &&
                   residues[b].chain == residues[a].chain) ++b;
            int runLen = static_cast<int>(b - a);
            if ((raw[a] == SSType::Helix && runLen < 4) ||
                (raw[a] == SSType::Sheet && runLen < 3)) {
                for (std::size_t k = a; k < b; ++k) out[k] = SSType::Loop;
            }
            a = b;
        }
        for (std::size_t k = 0; k < residues.size(); ++k) residues[k].ss = out[k];
    }

    for (const auto& r : residues) {
        for (int j = r.firstAtom; j <= r.lastAtom; ++j) atoms[j].ssType = r.ss;
    }
}

static void assignSS(std::vector<AtomData>& atoms, const gemmi::Structure& st) {
    if (st.helices.empty() && st.sheets.empty()) {
        // No HELIX/SHEET records (CASP TS, AlphaFold output, bare coords) —
        // fall back to a φ/ψ geometric classifier.
        assignSSGeometric(atoms);
        return;
    }
    for (auto& ad : atoms) {
        for (const auto& helix : st.helices) {
            if (ad.chainId == helix.start.chain_name) {
                int s = helix.start.res_id.seqid.num.value;
                int e = helix.end.res_id.seqid.num.value;
                if (ad.resSeq >= s && ad.resSeq <= e) { ad.ssType = SSType::Helix; break; }
            }
        }
        if (ad.ssType != SSType::Loop) continue;
        for (const auto& sheet : st.sheets) {
            for (const auto& strand : sheet.strands) {
                if (ad.chainId == strand.start.chain_name) {
                    int s = strand.start.res_id.seqid.num.value;
                    int e = strand.end.res_id.seqid.num.value;
                    if (ad.resSeq >= s && ad.resSeq <= e) { ad.ssType = SSType::Sheet; break; }
                }
            }
            if (ad.ssType != SSType::Loop) break;
        }
    }
}

// Build atom index by (chain, resSeq, insCode, atomName) for fast lookup within a residue
struct ResidueKey {
    std::string chainId;
    int resSeq;
    char insCode;
    bool operator==(const ResidueKey& o) const {
        return chainId == o.chainId && resSeq == o.resSeq && insCode == o.insCode;
    }
};

struct ResKeyHash {
    size_t operator()(const ResidueKey& k) const {
        return std::hash<std::string>()(k.chainId) ^ (std::hash<int>()(k.resSeq) << 16) ^ k.insCode;
    }
};

static void buildBonds(MolObject& obj) {
    const auto& atoms = obj.atoms();
    int n = static_cast<int>(atoms.size());

    // Index: (chain, resSeq, insCode) → {atomName → atomIdx}
    std::unordered_map<ResidueKey,
        std::unordered_map<std::string, int>, ResKeyHash> resAtomMap;
    for (int i = 0; i < n; ++i) {
        ResidueKey rk{atoms[i].chainId, atoms[i].resSeq, atoms[i].insCode};
        resAtomMap[rk][atoms[i].name] = i;
    }

    std::set<std::pair<int,int>> bondSet;  // dedup across tiers

    auto addBond = [&](int a1, int a2, int order) {
        auto key = std::make_pair(std::min(a1, a2), std::max(a1, a2));
        if (bondSet.insert(key).second) {
            BondData bd;
            bd.atom1 = key.first; bd.atom2 = key.second; bd.order = order;
            obj.bonds().push_back(bd);
        }
    };

    // ── Tier 1: Standard residue bond table ────────────────────────────────
    for (auto& [rk, atomMap] : resAtomMap) {
        const auto* table = standardBonds(atoms[atomMap.begin()->second].resName);
        if (!table) continue;
        for (const auto& be : *table) {
            auto it1 = atomMap.find(be.atom1);
            auto it2 = atomMap.find(be.atom2);
            if (it1 != atomMap.end() && it2 != atomMap.end())
                addBond(it1->second, it2->second, be.order);
        }
    }

    // ── Tier 1b: Inter-residue bonds (peptide C-N, phosphodiester O3'-P) ──
    // Group residues by chain, sorted by resSeq
    std::unordered_map<std::string, std::vector<ResidueKey>> chainResidues;
    for (const auto& [rk, _] : resAtomMap)
        chainResidues[rk.chainId].push_back(rk);
    for (auto& [chain, residues] : chainResidues) {
        std::sort(residues.begin(), residues.end(),
            [](const ResidueKey& a, const ResidueKey& b) {
                return a.resSeq < b.resSeq || (a.resSeq == b.resSeq && a.insCode < b.insCode);
            });
        for (size_t ri = 0; ri + 1 < residues.size(); ++ri) {
            auto& map1 = resAtomMap[residues[ri]];
            auto& map2 = resAtomMap[residues[ri + 1]];
            // Peptide bond: C(i) → N(i+1)
            auto cIt = map1.find("C");
            auto nIt = map2.find("N");
            if (cIt != map1.end() && nIt != map2.end()) {
                float dx = atoms[cIt->second].x - atoms[nIt->second].x;
                float dy = atoms[cIt->second].y - atoms[nIt->second].y;
                float dz = atoms[cIt->second].z - atoms[nIt->second].z;
                float d2 = dx*dx + dy*dy + dz*dz;
                if (d2 < 1.8f * 1.8f)  // peptide bond ~1.33 Å
                    addBond(cIt->second, nIt->second, 1);
            }
            // Phosphodiester: O3'(i) → P(i+1)
            auto o3It = map1.find("O3'");
            auto pIt = map2.find("P");
            if (o3It != map1.end() && pIt != map2.end()) {
                float dx = atoms[o3It->second].x - atoms[pIt->second].x;
                float dy = atoms[o3It->second].y - atoms[pIt->second].y;
                float dz = atoms[o3It->second].z - atoms[pIt->second].z;
                float d2 = dx*dx + dy*dy + dz*dz;
                if (d2 < 2.0f * 2.0f)  // P-O ~1.6 Å
                    addBond(o3It->second, pIt->second, 1);
            }
        }
    }

    // ── Tier 2: Distance-based fallback for non-standard residues ──────────
    // Only apply to atoms NOT already bonded by the table
    std::vector<bool> hasBonds(n, false);
    for (const auto& b : obj.bonds()) {
        hasBonds[b.atom1] = true;
        hasBonds[b.atom2] = true;
    }

    // Collect un-bonded atoms (ligands, modified residues)
    std::vector<int> unbonded;
    for (int i = 0; i < n; ++i) {
        if (!hasBonds[i]) unbonded.push_back(i);
    }

    if (!unbonded.empty()) {
        SpatialHash grid(2.5f, static_cast<int>(unbonded.size()));
        for (int idx = 0; idx < static_cast<int>(unbonded.size()); ++idx)
            grid.insert(unbonded[idx], atoms[unbonded[idx]].x,
                        atoms[unbonded[idx]].y, atoms[unbonded[idx]].z);

        constexpr float tolerance = 0.5f;
        constexpr float minDist = 0.4f;
        constexpr float maxSearchDist = 2.5f;

        for (int ai : unbonded) {
            float r1 = covalentRadius(atoms[ai].element);
            grid.forEachNeighbor(atoms[ai].x, atoms[ai].y, atoms[ai].z, maxSearchDist,
                [&](int aj) {
                    if (aj <= ai) return;
                    float dx = atoms[ai].x - atoms[aj].x;
                    float dy = atoms[ai].y - atoms[aj].y;
                    float dz = atoms[ai].z - atoms[aj].z;
                    float dist2 = dx*dx + dy*dy + dz*dz;
                    float r2 = covalentRadius(atoms[aj].element);
                    float maxBondDist = r1 + r2 + tolerance;
                    if (dist2 < minDist * minDist || dist2 > maxBondDist * maxBondDist) return;
                    addBond(ai, aj, 1);
                });
        }
    }
}

// ── Loaders ────────────────────────────────────────────────────────────────

std::unique_ptr<MolObject> CifLoader::loadCif(const std::string& filepath) {
    gemmi::Structure st = gemmi::read_structure_gz(filepath);
    if (st.models.empty())
        throw std::runtime_error("No models found in " + filepath);

    auto obj = std::make_unique<MolObject>(baseNameFromPath(filepath));
    obj->setSourcePath(filepath);

    obj->atoms() = convertModel(st.models[0]);
    assignSS(obj->atoms(), st);
    buildBonds(*obj);

    // Add explicit connections from _struct_conn (disulfide, metal, covalent links)
    const auto& atoms = obj->atoms();
    int n = static_cast<int>(atoms.size());
    std::set<std::pair<int,int>> bondSet;
    // Populate bondSet from existing bonds to deduplicate
    for (const auto& b : obj->bonds())
        bondSet.insert(std::make_pair(std::min(b.atom1, b.atom2), std::max(b.atom1, b.atom2)));

    auto resolveAtom = [&atoms, n](const gemmi::AtomAddress& addr) -> int {
        for (int i = 0; i < n; ++i) {
            if (atoms[i].chainId == addr.chain_name &&
                atoms[i].resSeq == addr.res_id.seqid.num.value &&
                atoms[i].name == addr.atom_name)
                return i;
        }
        return -1;
    };

    // Store multi-model states (NMR ensembles / trajectories)
    if (st.models.size() > 1) {
        for (size_t mi = 0; mi < st.models.size(); ++mi) {
            obj->addState(convertModel(st.models[mi]));
        }
    }

    for (const auto& conn : st.connections) {
        // Only add covalent-type connections (Covale, Disulf, MetalC)
        if (conn.type == gemmi::Connection::Hydrog) continue;

        int i1 = resolveAtom(conn.partner1);
        int i2 = resolveAtom(conn.partner2);
        if (i1 < 0 || i2 < 0 || i1 == i2) continue;

        auto key = std::make_pair(std::min(i1, i2), std::max(i1, i2));
        if (bondSet.insert(key).second) {
            BondData bd;
            bd.atom1 = key.first;
            bd.atom2 = key.second;
            bd.order = 1;
            obj->bonds().push_back(bd);
        }
    }

    return obj;
}

std::unique_ptr<MolObject> CifLoader::loadPdb(const std::string& filepath) {
    return loadCif(filepath);
}

static std::string stripGzSuffix(const std::string& s) {
    if (s.size() > 3) {
        std::string tail = s.substr(s.size() - 3);
        std::transform(tail.begin(), tail.end(), tail.begin(), ::tolower);
        if (tail == ".gz") return s.substr(0, s.size() - 3);
    }
    return s;
}

std::string CifLoader::detectFormat(const std::string& filepath) {
    std::string path = stripGzSuffix(filepath);
    auto dot = path.rfind('.');
    if (dot != std::string::npos) {
        std::string ext = path.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == "cif" || ext == "mmcif") return "cif";
        if (ext == "pdb" || ext == "ent") return "pdb";
        if (ext == "mol2") return "mol2";
    }
    return "cif";
}

std::string CifLoader::baseNameFromPath(const std::string& filepath) {
    auto slash = filepath.rfind('/');
    std::string filename = (slash != std::string::npos)
        ? filepath.substr(slash + 1)
        : filepath;
    filename = stripGzSuffix(filename);
    auto dot = filename.rfind('.');
    if (dot != std::string::npos) filename = filename.substr(0, dot);
    return filename;
}

std::vector<AssemblyInfo> CifLoader::listAssemblies(const std::string& filepath) {
    gemmi::Structure st = gemmi::read_structure_gz(filepath);
    std::vector<AssemblyInfo> result;
    for (const auto& assembly : st.assemblies) {
        AssemblyInfo info;
        info.name = assembly.name;
        info.oligomericCount = assembly.oligomeric_count;
        info.details = assembly.oligomeric_details;
        result.push_back(std::move(info));
    }
    return result;
}

std::unique_ptr<MolObject> CifLoader::loadAssembly(const std::string& filepath,
                                                     const std::string& assemblyId) {
    gemmi::Structure st = gemmi::read_structure_gz(filepath);
    if (st.models.empty())
        throw std::runtime_error("No models found in " + filepath);

    gemmi::Assembly* assembly = nullptr;
    for (auto& a : st.assemblies) {
        if (a.name == assemblyId) { assembly = &a; break; }
    }
    if (!assembly)
        throw std::runtime_error("Assembly '" + assemblyId + "' not found");

    gemmi::Logger logger;  // silent logger (no callback set)
    gemmi::Model bioModel = gemmi::make_assembly(*assembly, st.models[0],
        gemmi::HowToNameCopiedChain::Short, logger);

    std::string baseName = baseNameFromPath(filepath) + "_asm" + assemblyId;
    auto obj = std::make_unique<MolObject>(baseName);
    obj->setSourcePath(filepath);

    obj->atoms() = convertModel(bioModel);
    assignSS(obj->atoms(), st);
    buildBonds(*obj);

    return obj;
}

} // namespace molterm
