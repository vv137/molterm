#include "molterm/render/Canvas.h"
#include <cmath>
#include <algorithm>

namespace molterm {

void Canvas::drawLine(int x0, int y0, float d0,
                      int x1, int y1, float d1,
                      int colorPair) {
    bresenham(x0, y0, d0, x1, y1, d1,
        [&](int x, int y, float depth) {
            drawDot(x, y, depth, colorPair);
        });
}

void Canvas::drawCircle(int cx, int cy, float depth,
                        int radius, int colorPair, bool filled) {
    if (filled) {
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (dx * dx + dy * dy <= radius * radius) {
                    drawDot(cx + dx, cy + dy, depth, colorPair);
                }
            }
        }
    } else {
        int x = radius, y = 0;
        int err = 1 - radius;
        while (x >= y) {
            drawDot(cx + x, cy + y, depth, colorPair);
            drawDot(cx - x, cy + y, depth, colorPair);
            drawDot(cx + x, cy - y, depth, colorPair);
            drawDot(cx - x, cy - y, depth, colorPair);
            drawDot(cx + y, cy + x, depth, colorPair);
            drawDot(cx - y, cy + x, depth, colorPair);
            drawDot(cx + y, cy - x, depth, colorPair);
            drawDot(cx - y, cy - x, depth, colorPair);
            ++y;
            if (err < 0) {
                err += 2 * y + 1;
            } else {
                --x;
                err += 2 * (y - x) + 1;
            }
        }
    }
}

void Canvas::bresenham(int x0, int y0, float d0,
                       int x1, int y1, float d1,
                       const std::function<void(int, int, float)>& plot) {
    int dx = std::abs(x1 - x0);
    int dy = std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    int steps = std::max(dx, dy);
    float depth = d0;
    float depthStep = (steps > 0) ? (d1 - d0) / static_cast<float>(steps) : 0.0f;

    while (true) {
        plot(x0, y0, depth);

        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
        depth += depthStep;
    }
}

void Canvas::drawTriangle(float x0, float y0, float z0,
                          float x1, float y1, float z1,
                          float x2, float y2, float z2,
                          int colorPair) {
    // Sort vertices by Y (top to bottom)
    if (y0 > y1) { std::swap(x0, x1); std::swap(y0, y1); std::swap(z0, z1); }
    if (y0 > y2) { std::swap(x0, x2); std::swap(y0, y2); std::swap(z0, z2); }
    if (y1 > y2) { std::swap(x1, x2); std::swap(y1, y2); std::swap(z1, z2); }

    int iy0 = static_cast<int>(y0), iy2 = static_cast<int>(y2);
    if (iy0 == iy2) return;  // degenerate

    // Compute triangle bounding box from actual vertices (not from edge extrapolation)
    float triMinX = std::min({x0, x1, x2});
    float triMaxX = std::max({x0, x1, x2});
    int clampMinX = std::max(0, static_cast<int>(triMinX));
    int clampMaxX = std::min(subW() - 1, static_cast<int>(triMaxX) + 1);

    for (int y = std::max(0, iy0); y <= std::min(subH() - 1, iy2); ++y) {
        float fy = static_cast<float>(y) + 0.5f;

        float xa, xb, za, zb;

        float tLong = (fy - y0) / (y2 - y0);
        xa = x0 + (x2 - x0) * tLong;
        za = z0 + (z2 - z0) * tLong;

        if (fy < y1) {
            float denom = y1 - y0;
            if (std::abs(denom) < 0.5f) {
                // Near-horizontal top edge: interpolate x directly
                float t01 = (y2 - y0 > 0.5f) ? (fy - y0) / (y2 - y0) : 0.5f;
                xb = x0 + (x1 - x0) * t01;
                zb = z0 + (z1 - z0) * t01;
            } else {
                float tShort = (fy - y0) / denom;
                xb = x0 + (x1 - x0) * tShort;
                zb = z0 + (z1 - z0) * tShort;
            }
        } else {
            float denom = y2 - y1;
            if (std::abs(denom) < 0.5f) {
                float t12 = 0.5f;
                xb = x1 + (x2 - x1) * t12;
                zb = z1 + (z2 - z1) * t12;
            } else {
                float tShort = (fy - y1) / denom;
                xb = x1 + (x2 - x1) * tShort;
                zb = z1 + (z2 - z1) * tShort;
            }
        }

        if (xa > xb) { std::swap(xa, xb); std::swap(za, zb); }

        // Clamp to triangle bounding box (prevents runaway scanlines)
        int ixa = std::max(clampMinX, static_cast<int>(xa));
        int ixb = std::min(clampMaxX, static_cast<int>(xb));
        float spanLen = xb - xa;
        if (spanLen < 0.5f) spanLen = 0.5f;

        for (int x = ixa; x <= ixb; ++x) {
            float t = (static_cast<float>(x) - xa) / spanLen;
            float z = za + (zb - za) * t;
            drawDot(x, y, z, colorPair);
        }
    }
}

} // namespace molterm
