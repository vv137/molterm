#pragma once

#include <string>

namespace molterm {

class MolObject;

// Write a MolObject to a PDB file. Returns true on success. Output
// follows the canonical PDB v3.3 ATOM/HETATM/TER column layout so
// external tools (PyMOL, ChimeraX, USalign, ANARCI, …) round-trip
// cleanly.
//
// Current limitations:
//   * single-model only (active state's atoms; multi-model NMR
//     ensembles are exported as just the active frame)
//   * no CONECT records (bonds are recomputed by the consumer)
//   * no HEADER/TITLE/REMARK preamble
//   * mmCIF multi-char chain ids (e.g. "AA", "AB") are truncated to
//     the first char; writePdb logs a stderr warning when truncation
//     would cause a chain-id collision so the user knows to switch
//     to mmCIF export.
bool writePdb(const MolObject& obj, const std::string& path);

}  // namespace molterm
