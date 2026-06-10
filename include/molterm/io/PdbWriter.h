#pragma once

#include <cstddef>
#include <string>

namespace molterm {

class MolObject;
struct AtomData;

// Format a single PDB ATOM/HETATM record (v3.3 column layout) into `buf`.
// The caller owns serial numbering — it differs by use: writePdb renumbers and
// bumps on every TER; SessionExporter needs serial == atomIndex+1 for its .pml
// id selections; the aligner numbers 1..N over a selection — so the chosen
// `serial` is passed in. serial and resSeq are wrapped into their fixed-width
// fields so a >99,999-atom / >9,999-residue structure can't shift the columns.
// `record` is "ATOM" or "HETATM". When `alignName` is true the atom-name column
// follows the strict PDB element-width rule (formatAtomName); when false the
// name is written raw left-justified — sufficient for tolerant consumers
// (PyMOL, USalign). Single source for the ATOM-record format the three writers
// used to each spell out.
void formatPdbAtomRecord(char* buf, std::size_t bufSize, const char* record,
                         int serial, const AtomData& a, bool alignName);

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
