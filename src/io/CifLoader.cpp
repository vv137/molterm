#include "molterm/io/CifLoader.h"

#include <gemmi/mmread.hpp>
#include <gemmi/mmread_gz.hpp>
#include <gemmi/model.hpp>
#include <gemmi/assembly.hpp>
#include <gemmi/logger.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace molterm {

// Spatial hash for fast neighbor lookup in Cartesian space
class SpatialHash {
public:
    SpatialHash(float cellSize, int nAtoms)
        : cellSize_(cellSize), invCell_(1.0f / cellSize) {
        buckets_.reserve(nAtoms);
    }

    void insert(int idx, float x, float y, float z) {
        auto key = makeKey(x, y, z);
        buckets_[key].push_back(idx);
    }

    // Call func(int neighborIdx) for all atoms within radius of (x,y,z)
    template<typename Func>
    void forEachNeighbor(float x, float y, float z, float radius, const Func& func) const {
        int cx = toCell(x), cy = toCell(y), cz = toCell(z);
        int span = static_cast<int>(std::ceil(radius * invCell_));
        for (int dz = -span; dz <= span; ++dz) {
            for (int dy = -span; dy <= span; ++dy) {
                for (int dx = -span; dx <= span; ++dx) {
                    auto key = packKey(cx + dx, cy + dy, cz + dz);
                    auto it = buckets_.find(key);
                    if (it != buckets_.end()) {
                        for (int idx : it->second) {
                            func(idx);
                        }
                    }
                }
            }
        }
    }

private:
    float cellSize_, invCell_;
    std::unordered_map<uint64_t, std::vector<int>> buckets_;

    int toCell(float v) const { return static_cast<int>(std::floor(v * invCell_)); }

    uint64_t makeKey(float x, float y, float z) const {
        return packKey(toCell(x), toCell(y), toCell(z));
    }

    static uint64_t packKey(int x, int y, int z) {
        // Offset to handle negative coords, then pack into 64 bits
        auto u = [](int v) -> uint64_t { return static_cast<uint64_t>(v + 10000); };
        return (u(x) * 20001ULL + u(y)) * 20001ULL + u(z);
    }
};

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

static void assignSS(std::vector<AtomData>& atoms, const gemmi::Structure& st) {
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

static void buildBonds(MolObject& obj) {
    const auto& atoms = obj.atoms();
    int n = static_cast<int>(atoms.size());
    SpatialHash grid(2.5f, n);
    for (int i = 0; i < n; ++i)
        grid.insert(i, atoms[i].x, atoms[i].y, atoms[i].z);

    constexpr float tolerance = 0.5f;
    constexpr float minDist = 0.4f;
    constexpr float maxSearchDist = 2.5f;

    for (int i = 0; i < n; ++i) {
        float r1 = covalentRadius(atoms[i].element);
        grid.forEachNeighbor(atoms[i].x, atoms[i].y, atoms[i].z, maxSearchDist,
            [&](int j) {
                if (j <= i) return;
                float dx = atoms[i].x - atoms[j].x;
                float dy = atoms[i].y - atoms[j].y;
                float dz = atoms[i].z - atoms[j].z;
                float dist2 = dx*dx + dy*dy + dz*dz;
                float r2 = covalentRadius(atoms[j].element);
                float maxBondDist = r1 + r2 + tolerance;
                if (dist2 < minDist * minDist || dist2 > maxBondDist * maxBondDist) return;
                BondData bd;
                bd.atom1 = i; bd.atom2 = j; bd.order = 1;
                obj.bonds().push_back(bd);
            });
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
