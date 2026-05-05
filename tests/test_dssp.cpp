// tests/test_dssp.cpp — DSSP validation against mkdssp ground truth
// Build:  c++ -std=c++17 -O2 -I include -I build/_deps/gemmi-src/include \
//             tests/test_dssp.cpp build/CMakeFiles/molterm.dir/src/core/DSSP.cpp.o \
//             build/_deps/gemmi-build/libgemmi_cpp.a -lz -o build/test_dssp
// Run:    ./build/test_dssp <input.cif|pdb> <reference.dssp>
//
// Reads the structure via gemmi, runs molterm::dssp::compute, parses
// mkdssp's classic-format output (column 16 of each residue record is
// the 8-class SS letter: H/G/I/E/B/T/S/space), collapses both to
// {Helix, Sheet, Loop}, and prints H/E/overall agreement.

#include "molterm/core/AtomData.h"
#include "molterm/core/DSSP.h"

#include <gemmi/cif.hpp>
#include <gemmi/mmcif.hpp>
#include <gemmi/pdb.hpp>
#include <gemmi/mmread.hpp>
#include <gemmi/util.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

using molterm::AtomData;
using molterm::SSType;

static char threeClass(char c) {
    switch (c) {
        case 'H': case 'G': case 'I': return 'H';
        case 'E': case 'B':           return 'E';
        default:                       return 'L';
    }
}

static char ssToChar(SSType s) {
    switch (s) {
        case SSType::Helix: return 'H';
        case SSType::Sheet: return 'E';
        default:             return 'L';
    }
}

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

static std::map<ResKey, char> parseDssp(const std::string& path) {
    std::map<ResKey, char> out;
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return out;
    }
    std::string line;
    bool inData = false;
    while (std::getline(in, line)) {
        if (!inData) {
            if (line.find("  #  RESIDUE AA STRUCTURE") == 0) inData = true;
            continue;
        }
        if (line.size() < 20) continue;
        // chain-break sentinels — skip (resseq is empty / "!").
        if (line.size() > 13 && line[13] == '!') continue;
        // Columns (0-indexed):
        //   5..9  resnum, 11 chain, 13 AA, 16 SS letter (or space).
        std::string seqStr = line.substr(5, 5);
        std::string chain  = line.substr(11, 1);
        char ss = line.size() > 16 ? line[16] : ' ';
        char icode = line.size() > 10 ? line[10] : ' ';
        seqStr.erase(std::remove(seqStr.begin(), seqStr.end(), ' '), seqStr.end());
        if (seqStr.empty()) continue;
        ResKey k{chain, std::stoi(seqStr), icode};
        out[k] = ss;
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <input.cif|pdb> <reference.dssp>\n", argv[0]);
        return 2;
    }
    auto atoms = loadStructure(argv[1]);
    auto ref   = parseDssp(argv[2]);
    if (atoms.empty() || ref.empty()) {
        std::fprintf(stderr, "empty input — atoms=%zu ref=%zu\n", atoms.size(), ref.size());
        return 1;
    }

    auto ss = molterm::dssp::compute(atoms);

    int nCompared = 0, nMatch = 0;
    int helixRef = 0, helixMatch = 0, helixOverpredict = 0;
    int sheetRef = 0, sheetMatch = 0, sheetOverpredict = 0;
    int loopRef  = 0, loopMatch  = 0;

    char prevChain = 0; int prevSeq = -9999; char prevIcode = 0;
    std::string seqMine, seqRef;
    std::string chainMarks;

    for (size_t i = 0; i < atoms.size(); ++i) {
        const auto& a = atoms[i];
        if (a.name != "CA") continue;
        ResKey k{a.chainId, a.resSeq, a.insCode};
        auto it = ref.find(k);
        if (it == ref.end()) continue;
        char m = ssToChar(ss[i]);
        char r = threeClass(it->second);

        seqMine.push_back(m);
        seqRef.push_back(r);
        chainMarks.push_back(a.chainId.empty() ? '?' : a.chainId[0]);

        ++nCompared;
        if (m == r) ++nMatch;
        if (r == 'H') { ++helixRef; if (m == 'H') ++helixMatch; }
        if (r == 'E') { ++sheetRef; if (m == 'E') ++sheetMatch; }
        if (r == 'L') { ++loopRef;  if (m == 'L') ++loopMatch;  }
        if (m == 'H' && r != 'H') ++helixOverpredict;
        if (m == 'E' && r != 'E') ++sheetOverpredict;
        (void)prevChain; (void)prevSeq; (void)prevIcode;
    }

    std::printf("Compared %d residues across %s\n", nCompared, argv[1]);
    std::printf("Overall match : %d / %d (%.2f%%)\n",
                nMatch, nCompared, 100.0 * nMatch / std::max(1, nCompared));
    std::printf("Helix recall  : %d / %d (%.2f%%)  overpredict=%d\n",
                helixMatch, helixRef,
                helixRef ? 100.0 * helixMatch / helixRef : 0.0,
                helixOverpredict);
    std::printf("Sheet recall  : %d / %d (%.2f%%)  overpredict=%d\n",
                sheetMatch, sheetRef,
                sheetRef ? 100.0 * sheetMatch / sheetRef : 0.0,
                sheetOverpredict);
    std::printf("Loop  recall  : %d / %d (%.2f%%)\n",
                loopMatch, loopRef,
                loopRef ? 100.0 * loopMatch / loopRef : 0.0);

    // Per-mismatch dump for inspection.
    if (nMatch != nCompared) {
        std::printf("\nMismatches (chain:resnum  mine vs ref):\n");
        size_t k = 0;
        for (size_t i = 0; i < atoms.size() && k < seqMine.size(); ++i) {
            if (atoms[i].name != "CA") continue;
            ResKey rk{atoms[i].chainId, atoms[i].resSeq, atoms[i].insCode};
            if (!ref.count(rk)) continue;
            if (seqMine[k] != seqRef[k]) {
                std::printf("  %s:%d  %c vs %c\n",
                            atoms[i].chainId.c_str(), atoms[i].resSeq,
                            seqMine[k], seqRef[k]);
            }
            ++k;
        }
    }
    return 0;
}
