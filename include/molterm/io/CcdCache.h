#pragma once

#include <string>

namespace gemmi { struct ChemComp; }

namespace molterm::ccd {

// Resolves wwPDB Chemical Component Dictionary (CCD) definitions for ligands and
// other non-standard residues, so their connectivity can be taken from the
// canonical bond list instead of a distance heuristic.
//
// Lookup is lazy and per-component: only residue names actually queried are
// resolved, each at most once per session. Resolution order:
//   1. in-memory cache (this session)
//   2. on-disk cache   ~/.molterm/ccd/<NAME>.cif
//   3. local CCD dir   $MOLTERM_CCD_DIR/<NAME>.cif        (if set)
//   4. network fetch   https://files.rcsb.org/ligands/download/<NAME>.cif
//                      (skipped if $MOLTERM_CCD_FETCH=0), cached into (2)
//
// Returns nullptr if the component cannot be resolved (offline, unknown ligand,
// parse failure); callers should then fall back to distance-based bonding. The
// returned pointer is owned by the internal session cache and stays valid for
// the lifetime of the program.
const gemmi::ChemComp* lookup(const std::string& resName);

} // namespace molterm::ccd
