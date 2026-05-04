#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "molterm/core/MolObject.h"

namespace molterm {

// Classification of an inter-chain atomic contact, picked per residue
// pair by priority (SaltBridge > HBond > Hydrophobic > Other) and
// rendered with a distinct color in the interface overlay.
enum class InteractionType : std::uint8_t {
    HBond = 0,        // N/O ↔ N/O   ≤ 3.5 Å
    SaltBridge,       // ±charged    ≤ 4.0 Å
    Hydrophobic,      // C–C between hydrophobic residues ≤ 4.5 Å
    Other,            // any heavy-atom pair below the search cutoff
};

struct InterfaceContact {
    int atom1;
    int atom2;
    float distance;
    InteractionType type;
};

struct ResidueInfo {
    int resSeq;
    std::string chainId;
    std::string resName;
    int caAtomIdx;      // index of CA atom in MolObject (-1 if none)
    int firstAtomIdx;   // first atom index of this residue
    int lastAtomIdx;    // last atom index of this residue (inclusive)
};

struct ContactPair {
    int residueIdx1;  // index into residues()
    int residueIdx2;
    float distance;   // distance in Angstroms (CA-CA or closest heavy atom)
    int atomIdx1;     // atom index in MolObject (CA for contact map, closest atom for interface)
    int atomIdx2;
};

class ContactMap {
public:
    // Compute full contact map from a MolObject. cutoff in Angstroms.
    void compute(const MolObject& mol, float cutoff = 8.0f);

    // Compute inter-chain contacts using closest heavy atom distance.
    // Uses spatial hash for O(N) performance. Does not destroy contact map state.
    void computeInterface(const MolObject& mol, float cutoff = 4.5f);

    // Access
    const std::vector<ResidueInfo>& residues() const { return residues_; }
    const std::vector<ContactPair>& contacts() const { return contacts_; }
    float cutoff() const { return cutoff_; }
    int residueCount() const { return static_cast<int>(residues_.size()); }

    // Get distance between two residue indices (NaN if not computed)
    float distance(int ri, int rj) const;

    // Interface contacts: classified inter-chain atom pairs for 3D overlay.
    // computeInterface() populates this with per-pair classification;
    // filterContacts() (called from compute()) populates Other-typed
    // pairs only (it has no atom-name context).
    const std::vector<InterfaceContact>& interfaceContacts() const {
        return interfaceContacts_;
    }

    bool valid() const { return !residues_.empty(); }
    void clear();

private:
    std::vector<ResidueInfo> residues_;
    std::vector<ContactPair> contacts_;
    std::vector<InterfaceContact> interfaceContacts_;
    float cutoff_ = 8.0f;

    // Dense distance matrix: distMatrix_[i * N + j] = CA-CA distance
    std::vector<float> distMatrix_;
    int matrixSize_ = 0;

    void extractResidues(const MolObject& mol);
    void filterContacts(bool interfaceOnly);
};

} // namespace molterm
