#include "molterm/core/BondTable.h"

#include <unordered_map>

namespace molterm {

// Backbone bonds shared by all amino acids (heavy atoms only)
static const std::vector<BondEntry> kBackboneBonds = {
    {"N",  "CA", 1},
    {"CA", "C",  1},
    {"C",  "O",  2},
};

// Amino acid sidechain bonds (heavy atoms only, no H)
// clang-format off
static const std::unordered_map<std::string, std::vector<BondEntry>> kAASidechains = {
    {"GLY", {}},
    {"ALA", {{"CA","CB",1}}},
    {"VAL", {{"CA","CB",1},{"CB","CG1",1},{"CB","CG2",1}}},
    {"LEU", {{"CA","CB",1},{"CB","CG",1},{"CG","CD1",1},{"CG","CD2",1}}},
    {"ILE", {{"CA","CB",1},{"CB","CG1",1},{"CB","CG2",1},{"CG1","CD1",1}}},
    {"PRO", {{"CA","CB",1},{"CB","CG",1},{"CG","CD",1},{"CD","N",1}}},
    {"PHE", {{"CA","CB",1},{"CB","CG",1},{"CG","CD1",2},{"CG","CD2",1},{"CD1","CE1",1},{"CD2","CE2",2},{"CE1","CZ",2},{"CE2","CZ",1}}},
    {"TYR", {{"CA","CB",1},{"CB","CG",1},{"CG","CD1",2},{"CG","CD2",1},{"CD1","CE1",1},{"CD2","CE2",2},{"CE1","CZ",2},{"CE2","CZ",1},{"CZ","OH",1}}},
    {"TRP", {{"CA","CB",1},{"CB","CG",1},{"CG","CD1",2},{"CG","CD2",1},{"CD1","NE1",1},{"NE1","CE2",1},{"CD2","CE2",2},{"CD2","CE3",1},{"CE2","CZ2",1},{"CE3","CZ3",2},{"CZ2","CH2",2},{"CZ3","CH2",1}}},
    {"SER", {{"CA","CB",1},{"CB","OG",1}}},
    {"THR", {{"CA","CB",1},{"CB","OG1",1},{"CB","CG2",1}}},
    {"CYS", {{"CA","CB",1},{"CB","SG",1}}},
    {"MET", {{"CA","CB",1},{"CB","CG",1},{"CG","SD",1},{"SD","CE",1}}},
    {"ASP", {{"CA","CB",1},{"CB","CG",1},{"CG","OD1",2},{"CG","OD2",1}}},
    {"GLU", {{"CA","CB",1},{"CB","CG",1},{"CG","CD",1},{"CD","OE1",2},{"CD","OE2",1}}},
    {"ASN", {{"CA","CB",1},{"CB","CG",1},{"CG","OD1",2},{"CG","ND2",1}}},
    {"GLN", {{"CA","CB",1},{"CB","CG",1},{"CG","CD",1},{"CD","OE1",2},{"CD","NE2",1}}},
    {"LYS", {{"CA","CB",1},{"CB","CG",1},{"CG","CD",1},{"CD","CE",1},{"CE","NZ",1}}},
    {"ARG", {{"CA","CB",1},{"CB","CG",1},{"CG","CD",1},{"CD","NE",1},{"NE","CZ",1},{"CZ","NH1",2},{"CZ","NH2",1}}},
    {"HIS", {{"CA","CB",1},{"CB","CG",1},{"CG","ND1",1},{"CG","CD2",2},{"ND1","CE1",2},{"CD2","NE2",1},{"CE1","NE2",1}}},
};
// clang-format on

// Nucleotide bonds (heavy atoms only, common to all bases)
static const std::vector<BondEntry> kNASugarPhosphate = {
    {"P",   "OP1", 2}, {"P",   "OP2", 1},
    {"P",   "O5'", 1}, {"O5'", "C5'", 1},
    {"C5'", "C4'", 1}, {"C4'", "O4'", 1},
    {"C4'", "C3'", 1}, {"C3'", "O3'", 1},
    {"C3'", "C2'", 1}, {"C2'", "C1'", 1},
    {"C1'", "O4'", 1},
};

// RNA has 2'-OH
static const std::vector<BondEntry> kRNA2OH = {
    {"C2'", "O2'", 1},
};

// clang-format off
static const std::unordered_map<std::string, std::vector<BondEntry>> kNABases = {
    // Purines (A, G, DA, DG)
    {"A",  {{"C1'","N9",1},{"N9","C8",1},{"C8","N7",2},{"N7","C5",1},{"C5","C6",1},{"C6","N6",1},{"C6","N1",2},{"N1","C2",1},{"C2","N3",2},{"N3","C4",1},{"C4","C5",2},{"C4","N9",1}}},
    {"DA", {{"C1'","N9",1},{"N9","C8",1},{"C8","N7",2},{"N7","C5",1},{"C5","C6",1},{"C6","N6",1},{"C6","N1",2},{"N1","C2",1},{"C2","N3",2},{"N3","C4",1},{"C4","C5",2},{"C4","N9",1}}},
    {"G",  {{"C1'","N9",1},{"N9","C8",1},{"C8","N7",2},{"N7","C5",1},{"C5","C6",1},{"C6","O6",2},{"C6","N1",1},{"N1","C2",1},{"C2","N2",1},{"C2","N3",2},{"N3","C4",1},{"C4","C5",2},{"C4","N9",1}}},
    {"DG", {{"C1'","N9",1},{"N9","C8",1},{"C8","N7",2},{"N7","C5",1},{"C5","C6",1},{"C6","O6",2},{"C6","N1",1},{"N1","C2",1},{"C2","N2",1},{"C2","N3",2},{"N3","C4",1},{"C4","C5",2},{"C4","N9",1}}},
    // Pyrimidines (C, U, T, DC, DT)
    {"C",  {{"C1'","N1",1},{"N1","C2",1},{"C2","O2",2},{"C2","N3",1},{"N3","C4",2},{"C4","N4",1},{"C4","C5",1},{"C5","C6",2},{"C6","N1",1}}},
    {"DC", {{"C1'","N1",1},{"N1","C2",1},{"C2","O2",2},{"C2","N3",1},{"N3","C4",2},{"C4","N4",1},{"C4","C5",1},{"C5","C6",2},{"C6","N1",1}}},
    {"U",  {{"C1'","N1",1},{"N1","C2",1},{"C2","O2",2},{"C2","N3",1},{"N3","C4",1},{"C4","O4",2},{"C4","C5",1},{"C5","C6",2},{"C6","N1",1}}},
    {"DT", {{"C1'","N1",1},{"N1","C2",1},{"C2","O2",2},{"C2","N3",1},{"N3","C4",1},{"C4","O4",2},{"C4","C5",1},{"C5","C7",1},{"C5","C6",2},{"C6","N1",1}}},
};
// clang-format on

// Thread-safe cache of combined residue bond lists
static std::unordered_map<std::string, std::vector<BondEntry>>& bondCache() {
    static std::unordered_map<std::string, std::vector<BondEntry>> cache;
    return cache;
}

const std::vector<BondEntry>* standardBonds(const std::string& resName) {
    auto& cache = bondCache();
    auto it = cache.find(resName);
    if (it != cache.end()) return &it->second;

    // Try amino acid
    auto aaIt = kAASidechains.find(resName);
    if (aaIt != kAASidechains.end()) {
        auto& entry = cache[resName];
        entry = kBackboneBonds;
        entry.insert(entry.end(), aaIt->second.begin(), aaIt->second.end());
        return &entry;
    }

    // Try nucleotide
    auto naIt = kNABases.find(resName);
    if (naIt != kNABases.end()) {
        auto& entry = cache[resName];
        entry = kNASugarPhosphate;
        // RNA bases (no "D" prefix) get 2'-OH
        if (resName.size() <= 2 && resName[0] != 'D') {
            entry.insert(entry.end(), kRNA2OH.begin(), kRNA2OH.end());
        }
        entry.insert(entry.end(), naIt->second.begin(), naIt->second.end());
        return &entry;
    }

    return nullptr;
}

bool isStandardAA(const std::string& resName) {
    return kAASidechains.count(resName) > 0;
}

bool isStandardNA(const std::string& resName) {
    return kNABases.count(resName) > 0;
}

} // namespace molterm
