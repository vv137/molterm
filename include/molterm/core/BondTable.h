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

// Check if a residue name is a solvent/water molecule (HOH/WAT/DOD/H2O/SOL/
// TIP3). Canonical home so loaders and analyses don't each keep their own list.
bool isSolvent(const std::string& resName);

// Standard atomic weight (u) for an element symbol (case-insensitive).
// Falls back to carbon (12.011) for unrecognized symbols. Shared home for
// element metadata so mass-weighted geometry (com()) and any future physics
// consumer read one table instead of scattering copies.
float atomicMass(const std::string& element);

} // namespace molterm
