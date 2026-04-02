#pragma once

#include <string>
#include <array>

#include "molterm/core/MolObject.h"

namespace molterm {

struct AlignResult {
    bool success = false;
    double tmScore1 = 0.0;  // TM-score normalized by obj1 length
    double tmScore2 = 0.0;  // TM-score normalized by obj2 length
    double rmsd = 0.0;
    int alignedLength = 0;

    // Rotation matrix (3x3) + translation (3)
    // Transforms mobile → target: y[i] = t[i] + sum_j(u[i][j] * x[j])
    std::array<double, 3> translation = {};
    std::array<std::array<double, 3>, 3> rotation = {};

    std::string message;
};

class Aligner {
public:
    // Set path to USalign binary (auto-detected from build dir)
    static void setUSalignPath(const std::string& path);
    static std::string usAlignPath();

    // Simple alignment (single chain, TM-align)
    // Optional atom index vectors: if provided, only those atoms are written
    // to the temp PDB for alignment (but transform applies to whole object)
    static AlignResult align(const MolObject& mobile, const MolObject& target,
                             const std::vector<int>& mobileAtoms = {},
                             const std::vector<int>& targetAtoms = {});

    // Complex alignment (multi-chain, MM-align)
    static AlignResult alignComplex(const MolObject& mobile, const MolObject& target,
                                     const std::vector<int>& mobileAtoms = {},
                                     const std::vector<int>& targetAtoms = {});

    // Apply transformation to mobile object's coordinates (ALL atoms)
    static void applyTransform(MolObject& obj, const AlignResult& result);

private:
    static std::string usalignPath_;

    static AlignResult runUSalign(const MolObject& mobile, const MolObject& target,
                                   bool complex,
                                   const std::vector<int>& mobileAtoms = {},
                                   const std::vector<int>& targetAtoms = {});
    static void writeTempPDB(const MolObject& obj, const std::string& path,
                             const std::vector<int>& atomIndices = {});
    static AlignResult parseOutput(const std::string& stdout, const std::string& matrixFile);
};

} // namespace molterm
