#pragma once

#include <string>
#include <vector>

#include "molterm/core/AtomData.h"

namespace molterm::sasa {

// Solvent Accessible Surface Area — a faithful port of the accessibility
// algorithm from PDB-REDO/dssp (libdssp/src/dssp.cpp).
//
// The method is the classic numerical surface integration (Lee & Richards):
//   • Each atom is a sphere of radius (atom_radius + water_probe).
//   • A fixed Fibonacci/golden-section set of 401 dots is distributed over
//     that sphere; a dot is "accessible" if it lies outside every
//     neighbouring atom's (radius + water) sphere.
//   • The atom's accessible area = (free dots / total) · 4π · radius².
//   • A residue's accessibility is the sum over its atoms.
//
// Radii are dssp-specific (NOT element-wise Bondi radii): backbone N/CA/C/O
// have dedicated values and every side-chain atom uses a single radius.
//
// Only standard amino-acid residues participate (mirrors molterm::dssp's
// standard-AA set). HETATM / nucleic / water atoms receive 0.

// Per-atom absolute SASA in Å², indexed by atom (length == atoms.size()).
// Stateless: operates only on the supplied atom vector, like dssp::compute.
std::vector<float> compute(const std::vector<AtomData>& atoms);

// Relative accessibility = residue absolute SASA / residue maximum ASA
// (Tien et al. 2013 theoretical values), expanded back to a per-atom array
// so renderers can colour per atom. Each atom carries its residue's value,
// clamped to [0, 1]. Non-amino-acid atoms get 0.
std::vector<float> relativePerAtom(const std::vector<AtomData>& atoms,
                                   const std::vector<float>& absPerAtom);

// Maximum solvent accessibility for a residue type (Tien et al. 2013
// theoretical, Å²). Returns 0 for unknown residue names.
float maxAsa(const std::string& resName);

} // namespace molterm::sasa
