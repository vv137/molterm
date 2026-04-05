#pragma once

#include <string>
#include <utility>
#include <vector>

#include "molterm/core/MolObject.h"

namespace molterm {

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

    // Interface pairs: inter-chain contacts as atom index pairs for 3D overlay
    const std::vector<std::pair<int,int>>& interfacePairs() const { return interfacePairs_; }

    bool valid() const { return !residues_.empty(); }
    void clear();

private:
    std::vector<ResidueInfo> residues_;
    std::vector<ContactPair> contacts_;
    std::vector<std::pair<int,int>> interfacePairs_;
    float cutoff_ = 8.0f;

    // Dense distance matrix: distMatrix_[i * N + j] = CA-CA distance
    std::vector<float> distMatrix_;
    int matrixSize_ = 0;

    void extractResidues(const MolObject& mol);
    void filterContacts(bool interfaceOnly);
};

} // namespace molterm
