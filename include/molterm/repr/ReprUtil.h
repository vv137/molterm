#pragma once

#include <cmath>

namespace molterm {

// Shared math utilities for spline-based representations (Cartoon, Ribbon).

inline float len3(float x, float y, float z) {
    return std::sqrt(x * x + y * y + z * z);
}

// Normalize in place; returns original length.
inline float normalize3(float& x, float& y, float& z) {
    float l = len3(x, y, z);
    if (l > 1e-8f) { x /= l; y /= l; z /= l; }
    return l;
}

// Cross product: out = a x b
inline void cross3(float ax, float ay, float az,
                   float bx, float by, float bz,
                   float& ox, float& oy, float& oz) {
    ox = ay * bz - az * by;
    oy = az * bx - ax * bz;
    oz = ax * by - ay * bx;
}

// Catmull-Rom cubic interpolation between p1 and p2.
inline float catmullRom(float p0, float p1, float p2, float p3, float t) {
    float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * p1) +
                   (-p0 + p2) * t +
                   (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

} // namespace molterm
