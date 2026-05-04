#pragma once

#include <vector>

#include "molterm/core/AtomData.h"

namespace molterm::dssp {

// Stateless DSSP-style secondary structure assignment.
//
// Implements a simplified Kabsch-Sander hydrogen-bond model:
//   1. Estimate amide-H position from previous residue's carbonyl
//      direction (PDB usually has no H atoms).
//   2. For every (donor-N, acceptor-O) pair within ~5.2 Å, compute the
//      KS electrostatic energy. Pairs with E < -0.5 kcal/mol are
//      considered hydrogen bonded.
//   3. Pattern matching:
//        α-helix  : two overlapping i→i+4 turns ⇒ residues marked Helix.
//        β-sheet  : parallel or antiparallel bridges between strands ⇒
//                   residues marked Sheet.
//
// Output is molterm's 3-class SSType (Helix / Sheet / Loop) — the
// canonical 8-class DSSP output (H/G/I/E/B/T/S/C) is collapsed:
//   {H, G, I}  → Helix
//   {E, B}     → Sheet
//   {T, S, C}  → Loop
//
// The function is stateless: it operates only on the supplied atom
// vector. Trajectory viewers compute a fresh assignment per frame by
// passing the per-frame atoms_ snapshot.

// Returns SS labels indexed by atom index, length == atoms.size().
// Atoms not part of an amino-acid residue (HETATM, nucleic, water)
// receive SSType::Loop.
std::vector<SSType> compute(const std::vector<AtomData>& atoms);

} // namespace molterm::dssp
