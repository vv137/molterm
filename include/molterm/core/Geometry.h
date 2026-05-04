#pragma once

#include <cmath>

namespace molterm::geom {

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
