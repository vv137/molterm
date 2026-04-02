#include "molterm/render/Canvas.h"
#include <cmath>
#include <algorithm>

namespace molterm {

void Canvas::bresenham(int x0, int y0, float d0,
                       int x1, int y1, float d1,
                       const std::function<void(int, int, float)>& plot) {
    int dx = std::abs(x1 - x0);
    int dy = std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    int steps = std::max(dx, dy);
    if (steps == 0) steps = 1;
    int step = 0;

    while (true) {
        float t = static_cast<float>(step) / static_cast<float>(steps);
        float depth = d0 + (d1 - d0) * t;
        plot(x0, y0, depth);

        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
        ++step;
    }
}

} // namespace molterm
