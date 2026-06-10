#include "molterm/io/PdbWriter.h"

#include "molterm/core/MolObject.h"

#include <cstdio>
#include <fstream>
#include <set>
#include <string>

namespace molterm {

namespace {

// PDB ATOM record requires the atom-name field (cols 13-16) to align
// to a specific column based on element width. The simplest reliable
// rule: single-char element → name starts at col 14 (one leading
// space); two-char element → name starts at col 13. Four-char names
// (e.g. "1HG2") are pinned at col 13 regardless. This matches what
// gemmi/PyMOL produce on round-trip.
std::string formatAtomName(const std::string& name, const std::string& element) {
    char buf[5] = {' ', ' ', ' ', ' ', '\0'};
    if (name.empty()) return std::string(buf);
    bool elementIs2Char = (element.size() == 2);
    bool nameIs4Char    = (name.size() >= 4);
    int start = (elementIs2Char || nameIs4Char) ? 0 : 1;
    for (size_t i = 0; i < name.size() && start + i < 4; ++i) {
        buf[start + i] = name[i];
    }
    return std::string(buf);
}

// Residue name padded/truncated to the 3-char PDB field (cols 18-20).
std::string formatResName(const std::string& s) {
    char out[4] = {' ', ' ', ' ', '\0'};
    for (size_t k = 0; k < s.size() && k < 3; ++k) out[k] = s[k];
    return std::string(out);
}

}  // namespace

void formatPdbAtomRecord(char* buf, std::size_t bufSize, const char* record,
                         int serial, const AtomData& a, bool alignName) {
    const char insCode = (a.insCode == 0) ? ' ' : a.insCode;
    const char chainCh = a.chainId.empty() ? ' ' : a.chainId[0];
    // altLoc is always blank, so the aligned form's `%4s %3s` (name, space,
    // resName) reproduces the historic `%4s%c%3s` exactly. The element field's
    // `%2s` of a 0-2 char element right-justifies identically to the old
    // hand-padded array. Only the name column differs between the two forms.
    if (alignName) {
        const std::string nameField = formatAtomName(a.name, a.element);
        const std::string resName   = formatResName(a.resName);
        std::snprintf(buf, bufSize,
            "%-6s%5d %4s %3s %c%4d%c   %8.3f%8.3f%8.3f%6.2f%6.2f          %2s",
            record, serial % 100000, nameField.c_str(), resName.c_str(),
            chainCh, a.resSeq % 10000, insCode,
            static_cast<double>(a.x), static_cast<double>(a.y),
            static_cast<double>(a.z), static_cast<double>(a.occupancy),
            static_cast<double>(a.bFactor), a.element.c_str());
    } else {
        std::snprintf(buf, bufSize,
            "%-6s%5d %-4s %3s %c%4d%c   %8.3f%8.3f%8.3f%6.2f%6.2f          %2s",
            record, serial % 100000, a.name.c_str(), a.resName.c_str(),
            chainCh, a.resSeq % 10000, insCode,
            static_cast<double>(a.x), static_cast<double>(a.y),
            static_cast<double>(a.z), static_cast<double>(a.occupancy),
            static_cast<double>(a.bFactor), a.element.c_str());
    }
}

bool writePdb(const MolObject& obj, const std::string& path) {
    std::ofstream out(path);
    if (!out) return false;

    const auto& atoms = obj.atoms();

    // First pass: detect mmCIF multi-char chain ids that would collide
    // when truncated to PDB's single-column chain field. Warn once on
    // stderr so a silent mmCIF→PDB→USalign data-loss isn't hidden.
    {
        std::set<char> seenTrunc;
        std::set<std::string> warnedFull;
        for (const auto& a : atoms) {
            if (a.chainId.size() <= 1) continue;
            char trunc = a.chainId[0];
            if (seenTrunc.count(trunc) && !warnedFull.count(a.chainId)) {
                std::fprintf(stderr,
                    "[warn] writePdb: chain id \"%s\" truncated to '%c' — collides "
                    "with another chain. Export as .cif for round-trip safety.\n",
                    a.chainId.c_str(), trunc);
                warnedFull.insert(a.chainId);
            }
            seenTrunc.insert(trunc);
        }
    }

    // Walk atoms in source order, emitting TER between chain boundaries
    // so downstream parsers (USalign, ANARCI) see chain breaks the way
    // the input file had them. Renumber serial 1..N — molterm's stored
    // serial can have gaps after :extract / :split, which trips parsers
    // that assume monotone numbering.
    // formatResName / formatAtomName are file-scope helpers (also used by
    // formatPdbAtomRecord). chainChar is local to the TER path below.
    auto chainChar = [](const std::string& s) {
        return s.empty() ? ' ' : s[0];
    };

    // PDB v3.3 TER record has its own column layout:
    //   "TER   %5d      %3s %c%4d%c" — closes the chain with the last
    // residue's identifier so strict parsers (gemmi, some BioPython
    // versions) accept it. Plain "TER\n" is technically invalid even
    // though tolerant parsers swallow it.
    auto writeTer = [&](int serial, const std::string& resName,
                        const std::string& chainId, int resSeq, char insCode) {
        char line[81];
        // Wrap serial/resSeq into their fixed-width fields (cols 7-11 / 23-26)
        // so >99,999 atoms or >9,999 residue numbers (common from mmCIF author
        // numbering) don't overflow the field and shift every later column.
        // Matches SessionExporter::writePDB and Aligner::writeTempPDB.
        std::snprintf(line, sizeof(line),
            "TER   %5d      %3s %c%4d%c",
            serial % 100000, formatResName(resName).c_str(),
            chainChar(chainId), resSeq % 10000,
            insCode == 0 ? ' ' : insCode);
        out << line << "\n";
    };

    int serial = 1;
    int lastAtomIdxInChain = -1;
    for (size_t i = 0; i < atoms.size(); ++i) {
        const auto& a = atoms[i];
        if (lastAtomIdxInChain >= 0 &&
            a.chainId != atoms[lastAtomIdxInChain].chainId) {
            const auto& last = atoms[lastAtomIdxInChain];
            writeTer(serial, last.resName, last.chainId, last.resSeq, last.insCode);
            ++serial;
        }

        char line[81];
        formatPdbAtomRecord(line, sizeof(line), a.isHet ? "HETATM" : "ATOM",
                            serial, a, /*alignName=*/true);
        out << line << "\n";
        ++serial;
        lastAtomIdxInChain = static_cast<int>(i);
    }
    if (!atoms.empty()) {
        const auto& last = atoms[lastAtomIdxInChain];
        writeTer(serial, last.resName, last.chainId, last.resSeq, last.insCode);
    }
    out << "END\n";
    return out.good();
}

}  // namespace molterm
