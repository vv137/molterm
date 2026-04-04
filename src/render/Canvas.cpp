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

} // namespace molterm
