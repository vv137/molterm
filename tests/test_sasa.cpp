// tests/test_sasa.cpp — SASA validation against mkdssp accessibility
// Build:  c++ -std=c++17 -O2 -I include -I build/_deps/gemmi-src/include \
//             tests/test_sasa.cpp build/CMakeFiles/molterm.dir/src/core/SASA.cpp.o \
//             build/_deps/gemmi-build/libgemmi_cpp.a -lz -o build/test_sasa
// Reference: mkdssp --output-format dssp <input> ref.dssp
// Run:    ./build/test_sasa <input.cif|pdb> <reference.dssp>
//
// Reads the structure via gemmi, runs molterm::sasa::compute, aggregates the
// per-atom SASA to per-residue accessibility, and compares against mkdssp's
// ACC column (classic format, columns 35-38). Since both implement the same
// 401-dot PDB-REDO algorithm, agreement should be tight.

#include "molterm/core/AtomData.h"
#include "molterm/core/SASA.h"

#include <gemmi/mmread.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using molterm::AtomData;

static std::vector<AtomData> loadStructure(const std::string& path) {
    auto st = gemmi::read_structure_file(path);
    std::vector<AtomData> atoms;
    if (st.models.empty()) return atoms;
    int serial = 0;
    for (const auto& chain : st.models[0].chains) {
        for (const auto& res : chain.residues) {
            for (const auto& atom : res.atoms) {
                AtomData a;
                a.x = static_cast<float>(atom.pos.x);
                a.y = static_cast<float>(atom.pos.y);
                a.z = static_cast<float>(atom.pos.z);
                a.name    = atom.name;
                a.element = atom.element.name();
                a.resName = res.name;
                a.chainId = chain.name;
                a.resSeq  = res.seqid.num.value;
                a.insCode = res.seqid.icode == ' ' ? ' ' : res.seqid.icode;
                a.serial  = ++serial;
                a.isHet   = res.het_flag == 'H';
                atoms.push_back(std::move(a));
            }
        }
    }
    return atoms;
}

struct ResKey { std::string chain; int seq; char icode; };
static bool operator<(const ResKey& a, const ResKey& b) {
    if (a.chain != b.chain) return a.chain < b.chain;
    if (a.seq != b.seq) return a.seq < b.seq;
    return a.icode < b.icode;
}

static std::map<ResKey, double> parseDsspAcc(const std::string& path) {
    std::map<ResKey, double> out;
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return out; }
    std::string line;
    bool inData = false;
    while (std::getline(in, line)) {
        if (!inData) {
            if (line.find("  #  RESIDUE AA STRUCTURE") == 0) inData = true;
            continue;
        }
        if (line.size() < 38) continue;
        if (line[13] == '!') continue;                 // chain-break sentinel
        std::string seqStr = line.substr(5, 5);
        std::string chain  = line.substr(11, 1);
        char icode = line[10];
        std::string accStr = line.substr(34, 4);       // ACC, cols 35-38
        seqStr.erase(std::remove(seqStr.begin(), seqStr.end(), ' '), seqStr.end());
        if (seqStr.empty()) continue;
        ResKey k{chain, std::stoi(seqStr), icode == ' ' ? ' ' : icode};
        out[k] = std::stod(accStr);
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <input.cif|pdb> <reference.dssp>\n", argv[0]);
        return 2;
    }
    auto atoms = loadStructure(argv[1]);
    auto ref   = parseDsspAcc(argv[2]);
    if (atoms.empty() || ref.empty()) {
        std::fprintf(stderr, "empty input — atoms=%zu ref=%zu\n", atoms.size(), ref.size());
        return 1;
    }

    auto perAtom = molterm::sasa::compute(atoms);

    // Invariant: all SASA values non-negative.
    int negatives = 0;
    for (float v : perAtom) if (v < 0.0f) ++negatives;

    // Aggregate per residue (group by chain/resSeq/insCode).
    std::map<ResKey, double> mine;
    for (size_t i = 0; i < atoms.size();) {
        size_t j = i;
        while (j < atoms.size() &&
               atoms[j].chainId == atoms[i].chainId &&
               atoms[j].resSeq  == atoms[i].resSeq &&
               atoms[j].insCode == atoms[i].insCode) ++j;
        double sum = 0.0;
        for (size_t k = i; k < j; ++k) sum += perAtom[k];
        ResKey key{atoms[i].chainId, atoms[i].resSeq, atoms[i].insCode};
        mine[key] = sum;
        i = j;
    }

    int n = 0;
    double sumAbsDiff = 0.0, maxDiff = 0.0, totMine = 0.0, totRef = 0.0;
    ResKey worst{};
    for (const auto& [k, refAcc] : ref) {
        auto it = mine.find(k);
        if (it == mine.end()) continue;
        double d = std::fabs(it->second - refAcc);
        sumAbsDiff += d;
        if (d > maxDiff) { maxDiff = d; worst = k; }
        totMine += it->second;
        totRef  += refAcc;
        ++n;
    }
    if (n == 0) { std::fprintf(stderr, "no residues matched\n"); return 1; }

    double meanDiff = sumAbsDiff / n;
    double totErr = 100.0 * std::fabs(totMine - totRef) / std::max(1.0, totRef);

    std::printf("Compared %d residues across %s\n", n, argv[1]);
    std::printf("Total SASA    : mine %.1f Å²  vs ref %.1f Å²  (%.2f%% diff)\n",
                totMine, totRef, totErr);
    std::printf("Per-residue   : mean |Δ| %.2f Å²,  max |Δ| %.2f Å² at %s:%d\n",
                meanDiff, maxDiff, worst.chain.c_str(), worst.seq);
    std::printf("Negatives     : %d\n", negatives);

    // Thresholds: same algorithm ⇒ tight agreement expected. mkdssp rounds
    // ACC to integers, so allow a few Å² mean / a modest per-residue max.
    bool ok = (negatives == 0) && (meanDiff <= 2.0) && (totErr <= 2.0) && (maxDiff <= 12.0);
    std::printf("\n%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
