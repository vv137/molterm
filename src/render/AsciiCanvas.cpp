#include "molterm/render/AsciiCanvas.h"
#include <cmath>
#include <algorithm>

namespace molterm {

void AsciiCanvas::resize(int termW, int termH) {
    termW_ = termW;
    termH_ = termH;
    cells_.resize(static_cast<size_t>(termW) * termH);
    zbuf_.resize(termW, termH);
    clear();
}

void AsciiCanvas::clear() {
    for (auto& c : cells_) { c.ch = ' '; c.color = 0; }
    zbuf_.clear();
}

void AsciiCanvas::flush(Window& win) {
    for (int y = 0; y < termH_; ++y) {
        for (int x = 0; x < termW_; ++x) {
            auto& c = cell(x, y);
            if (c.ch != ' ') {
                win.addCharColored(y, x, c.ch, c.color);
            }
        }
    }
}

void AsciiCanvas::drawDot(int sx, int sy, float depth, int colorPair) {
    if (!inBounds(sx, sy)) return;
    if (zbuf_.testAndSet(sx, sy, depth)) {
        auto& c = cell(sx, sy);
        c.ch = '*';
        c.color = colorPair;
    }
}

void AsciiCanvas::drawLine(int x0, int y0, float d0,
                            int x1, int y1, float d1,
                            int colorPair) {
    int dx = std::abs(x1 - x0);
    int dy = std::abs(y1 - y0);
    int signX = (x0 < x1) ? 1 : -1;
    int signY = (y0 < y1) ? 1 : -1;

    bresenham(x0, y0, d0, x1, y1, d1,
        [&](int x, int y, float depth) {
            if (!inBounds(x, y)) return;
            if (zbuf_.testAndSet(x, y, depth)) {
                char ch;
                if (dx > dy * 2)      ch = '-';
                else if (dy > dx * 2) ch = '|';
                else                  ch = (signX == signY) ? '\\' : '/';
                auto& c = cell(x, y);
                c.ch = ch;
                c.color = colorPair;
            }
        });
}

void AsciiCanvas::drawCircle(int cx, int cy, float depth,
                              int radius, int colorPair, bool filled) {
    if (filled) {
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (dx*dx + dy*dy <= radius*radius) {
                    int x = cx + dx, y = cy + dy;
                    if (inBounds(x, y) && zbuf_.testAndSet(x, y, depth)) {
                        auto& c = cell(x, y);
                        c.ch = '@';
                        c.color = colorPair;
                    }
                }
            }
        }
    } else {
        // Draw circle outline using midpoint algorithm
        int x = radius, y = 0;
        int err = 1 - radius;
        while (x >= y) {
            int pts[][2] = {
                {cx+x, cy+y}, {cx-x, cy+y}, {cx+x, cy-y}, {cx-x, cy-y},
                {cx+y, cy+x}, {cx-y, cy+x}, {cx+y, cy-x}, {cx-y, cy-x},
            };
            for (auto& p : pts) {
                if (inBounds(p[0], p[1]) && zbuf_.testAndSet(p[0], p[1], depth)) {
                    auto& c = cell(p[0], p[1]);
                    c.ch = 'o';
                    c.color = colorPair;
                }
            }
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

void AsciiCanvas::drawChar(int termX, int termY, float depth,
                            char ch, int colorPair) {
    if (!inBounds(termX, termY)) return;
    if (zbuf_.testAndSet(termX, termY, depth)) {
        auto& c = cell(termX, termY);
        c.ch = ch;
        c.color = colorPair;
    }
}

} // namespace molterm
