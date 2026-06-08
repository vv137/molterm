#pragma once

#include <array>
#include <cmath>
#include <vector>

namespace molterm::geom {

// Principal component analysis result for a set of 3D points.
//
//   axis1 / axis2 / axis3 : eigenvectors sorted by descending eigenvalue.
//                           axis1 = longest axis, axis3 = shortest. All
//                           unit-length, right-handed (axis3 == axis1×axis2).
//   eigvals               : matching eigenvalues, sorted descending.
//   center                : centroid of the input points.
//
// Used by `:orient` to align the camera and by `:let G = pca(<sel>)`
// to expose the PCA frame to scripts (issue #33).
struct PcaResult {
    std::array<double, 3> axis1{};
    std::array<double, 3> axis2{};
    std::array<double, 3> axis3{};
    std::array<double, 3> eigvals{};
    std::array<double, 3> center{};
    // Rotation angle (degrees) and residual RMSD of the optimal superposition
    // that produced this result — only meaningful for superposeAxisOf()
    // (issues #110/#115); pcaOf/helixAxisOf leave both 0 and keep eigvals as
    // genuine eigenvalues. Exposed to scripts as `$reg.angle` / `$reg.rmsd`.
    double angle = 0.0;
    double rmsd = 0.0;
    bool valid = false;  // false if input had < 2 points
};

// Compute PCA of N parallel-array 3D points (xs[i], ys[i], zs[i]).
// Returns PcaResult with `valid=false` when n < 2 (degenerate).
// Implementation: covariance matrix → Jacobi eigen-decomposition →
// sort eigenvalues descending → enforce right-handed frame.
PcaResult pcaOf(const std::vector<float>& xs,
                const std::vector<float>& ys,
                const std::vector<float>& zs);

// Helix axis from an *ordered* point trace (Cα backbone, or a nucleic
// phosphate backbone) via the consecutive-chord cross-product method:
// for a regular helix the cross products of successive chord vectors,
// Σ (v_i × v_{i+1}), all point along the helix axis. Unlike PCA, this
// stays reliable for short, curved, or single-strand segments (PCA only
// recovers the axis once the cloud is clearly elongated along it). Needs
// ≥ 3 points; near-linear traces fall back to the first→last chord.
// Returned as a PcaResult so it drops into the same `:let`/`.axis1`/`:axis`
// flow as pca(): axis1 = helix axis (oriented first→last), center =
// centroid, eigvals[0] = variance of points projected onto the axis.
PcaResult helixAxisOf(const std::vector<float>& xs,
                      const std::vector<float>& ys,
                      const std::vector<float>& zs);

// Screw axis of the optimal rigid superposition mapping ordered point
// set A onto B (equal counts; i-th of A ↔ i-th of B), via Horn's (1987)
// unit-quaternion method — reflection-free by construction. Returned as a
// PcaResult with: axis1 = rotation axis (unit), center = midpoint of the
// two centroids (an anchor for drawing), angle = rotation in degrees, rmsd =
// post-fit residual. valid=false when either set has < 3 points or the
// counts differ.
PcaResult superposeAxisOf(const std::vector<float>& ax,
                          const std::vector<float>& ay,
                          const std::vector<float>& az,
                          const std::vector<float>& bx,
                          const std::vector<float>& by,
                          const std::vector<float>& bz);

// Result of a non-destructive RMSD query between two ordered point sets.
struct RmsdResult {
    double rmsd = 0.0;
    int n = 0;            // number of paired points used
    bool valid = false;   // false when counts differ or are 0
};

// RMSD of the *optimal* rigid superposition mapping ordered point set A
// onto B (equal counts; i-th of A ↔ i-th of B), computed via Horn's
// quaternion method WITHOUT moving either set (issue #115). Shares the
// closed-form residual λmax path with superposeAxisOf, so it is cheap and
// reflection-free. valid=false when the counts differ or are 0; unlike the
// screw-axis query this accepts as few as 1 point (the RMSD is still the
// minimum achievable even when the rotation is under-determined).
RmsdResult rmsdOf(const std::vector<float>& ax,
                  const std::vector<float>& ay,
                  const std::vector<float>& az,
                  const std::vector<float>& bx,
                  const std::vector<float>& by,
                  const std::vector<float>& bz);


// Signed dihedral angle in degrees, [-180, +180], for four 3D points.
// Uses the standard atan2 convention (no IUPAC sign flip): callers that
// need IUPAC φ/ψ (right-handed α-helix at ~(-57°, -47°)) should negate
// the return value, or equivalently swap the first two points.
inline float dihedralDeg(float ax, float ay, float az,
                         float bx, float by, float bz,
                         float cx, float cy, float cz,
                         float dx, float dy, float dz) {
    float b1x = bx - ax, b1y = by - ay, b1z = bz - az;
    float b2x = cx - bx, b2y = cy - by, b2z = cz - bz;
    float b3x = dx - cx, b3y = dy - cy, b3z = dz - cz;
    float b2len = std::sqrt(b2x*b2x + b2y*b2y + b2z*b2z);
    if (b2len < 1e-6f) return 0.0f;
    float n1x = b1y*b2z - b1z*b2y;
    float n1y = b1z*b2x - b1x*b2z;
    float n1z = b1x*b2y - b1y*b2x;
    float n2x = b2y*b3z - b2z*b3y;
    float n2y = b2z*b3x - b2x*b3z;
    float n2z = b2x*b3y - b2y*b3x;
    float bnx = b2x / b2len, bny = b2y / b2len, bnz = b2z / b2len;
    float m1x = n1y*bnz - n1z*bny;
    float m1y = n1z*bnx - n1x*bnz;
    float m1z = n1x*bny - n1y*bnx;
    float x = n1x*n2x + n1y*n2y + n1z*n2z;
    float y = m1x*n2x + m1y*n2y + m1z*n2z;
    return std::atan2(y, x) * 180.0f / 3.14159265f;
}

} // namespace molterm::geom
