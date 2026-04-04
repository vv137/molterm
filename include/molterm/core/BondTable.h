#pragma once

#include <string>
#include <vector>

namespace molterm {

struct BondEntry {
    const char* atom1;
    const char* atom2;
    int order;  // 1=single, 2=double, 3=triple
};

// Standard intra-residue bond topology for 20 amino acids + 8 nucleotides.
// Returns nullptr if residue name is not in the table.
const std::vector<BondEntry>* standardBonds(const std::string& resName);

// Check if a residue name is a standard amino acid
bool isStandardAA(const std::string& resName);

// Check if a residue name is a standard nucleotide
bool isStandardNA(const std::string& resName);

} // namespace molterm
