#pragma once

#include <algorithm>
#include <cmath>

namespace molterm {

// sqrt(zoom) clamped to [0.75, 1.8] — applied to thickness/marker sizes
// in Wireframe, Backbone, Interface, and the selection-highlight overlay
// so they grow gently with camera zoom (4× zoom → ~2× thickness) without
// bloating at close-up. Negative/zero zoom collapses to the lower clamp.
inline float cameraZoomScale(float zoom) {
    return std::clamp(std::sqrt(std::max(zoom, 0.0f)), 0.75f, 1.8f);
}

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

// Catmull-Rom cubic interpolation between p1 and p2 with tunable
// tension (Mol* convention — `tension` is the multiplier on the
// derivative estimate at p1 and p2). tension=0.5 gives the classic
// Catmull-Rom spline; tension=0.9 yields tighter helices, matching
// Mol*'s `HelixTension`.
inline float catmullRomT(float p0, float p1, float p2, float p3,
                         float t, float tension) {
    const float v0 = (p2 - p0) * tension;
    const float v1 = (p3 - p1) * tension;
    const float t2 = t * t, t3 = t2 * t;
    return (2.0f * p1 - 2.0f * p2 + v0 + v1) * t3
         + (-3.0f * p1 + 3.0f * p2 - 2.0f * v0 - v1) * t2
         + v0 * t
         + p1;
}

// Backwards-compatible alias — same numerical result as the original
// 0.5-tension form.
inline float catmullRom(float p0, float p1, float p2, float p3, float t) {
    return catmullRomT(p0, p1, p2, p3, t, 0.5f);
}

// Spherical linear interpolation of two unit-length 3-vectors. Falls
// back to lerp + renormalise when the angle is small (numerical
// stability) or close to π (axis ambiguous).
inline void slerp3(float ax, float ay, float az,
                   float bx, float by, float bz, float t,
                   float& ox, float& oy, float& oz) {
    float d = ax * bx + ay * by + az * bz;
    if (d > 1.0f)  d = 1.0f;
    if (d < -1.0f) d = -1.0f;
    if (d > 0.9995f) {
        ox = ax + t * (bx - ax);
        oy = ay + t * (by - ay);
        oz = az + t * (bz - az);
        normalize3(ox, oy, oz);
        return;
    }
    float theta = std::acos(d);
    float sinTheta = std::sin(theta);
    float wa = std::sin((1.0f - t) * theta) / sinTheta;
    float wb = std::sin(t * theta) / sinTheta;
    ox = wa * ax + wb * bx;
    oy = wa * ay + wb * by;
    oz = wa * az + wb * bz;
    normalize3(ox, oy, oz);
}

} // namespace molterm
