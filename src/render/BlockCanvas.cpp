#include "molterm/render/BlockCanvas.h"
#include <cmath>
#include <algorithm>

namespace molterm {

void BlockCanvas::resize(int termW, int termH) {
    termW_ = termW;
    termH_ = termH;
    cells_.resize(static_cast<size_t>(termW) * termH);
    zbuf_.resize(termW, termH * 2);
    clear();
}

void BlockCanvas::clear() {
    for (auto& c : cells_) {
        c.topFilled = false;
        c.botFilled = false;
        c.topColor = 0;
        c.botColor = 0;
    }
    zbuf_.clear();
}

void BlockCanvas::flush(Window& win) {
    for (int ty = 0; ty < termH_; ++ty) {
        for (int tx = 0; tx < termW_; ++tx) {
            auto& c = cell(tx, ty);
            if (!c.topFilled && !c.botFilled) continue;

            if (c.topFilled && c.botFilled) {
                if (c.topColor == c.botColor) {
                    // Full block, same color
                    wattron(win.raw(), COLOR_PAIR(c.topColor));
                    mvwprintw(win.raw(), ty, tx, "\xe2\x96\x88"); // █
                    wattroff(win.raw(), COLOR_PAIR(c.topColor));
                } else {
                    // Upper half block with top color, background would be bottom
                    // ncurses can't easily do fg+bg per char with arbitrary colors
                    // Use upper half with top color
                    wattron(win.raw(), COLOR_PAIR(c.topColor));
                    mvwprintw(win.raw(), ty, tx, "\xe2\x96\x80"); // ▀
                    wattroff(win.raw(), COLOR_PAIR(c.topColor));
                }
            } else if (c.topFilled) {
                wattron(win.raw(), COLOR_PAIR(c.topColor));
                mvwprintw(win.raw(), ty, tx, "\xe2\x96\x80"); // ▀
                wattroff(win.raw(), COLOR_PAIR(c.topColor));
            } else {
                wattron(win.raw(), COLOR_PAIR(c.botColor));
                mvwprintw(win.raw(), ty, tx, "\xe2\x96\x84"); // ▄
                wattroff(win.raw(), COLOR_PAIR(c.botColor));
            }
        }
    }
}

void BlockCanvas::drawDot(int sx, int sy, float depth, int colorPair) {
    if (!inBounds(sx, sy)) return;
    if (!zbuf_.testAndSet(sx, sy, depth)) return;

    int tx = sx, ty = sy / 2;
    int half = sy % 2;  // 0 = top, 1 = bottom

    auto& c = cell(tx, ty);
    if (half == 0) {
        c.topFilled = true;
        c.topColor = colorPair;
    } else {
        c.botFilled = true;
        c.botColor = colorPair;
    }
}

void BlockCanvas::drawLine(int x0, int y0, float d0,
                            int x1, int y1, float d1,
                            int colorPair) {
    bresenham(x0, y0, d0, x1, y1, d1,
        [&](int x, int y, float depth) {
            drawDot(x, y, depth, colorPair);
        });
}

void BlockCanvas::drawCircle(int cx, int cy, float depth,
                              int radius, int colorPair, bool filled) {
    if (filled) {
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (dx*dx + dy*dy <= radius*radius) {
                    drawDot(cx + dx, cy + dy, depth, colorPair);
                }
            }
        }
    } else {
        int x = radius, y = 0;
        int err = 1 - radius;
        while (x >= y) {
            drawDot(cx+x, cy+y, depth, colorPair);
            drawDot(cx-x, cy+y, depth, colorPair);
            drawDot(cx+x, cy-y, depth, colorPair);
            drawDot(cx-x, cy-y, depth, colorPair);
            drawDot(cx+y, cy+x, depth, colorPair);
            drawDot(cx-y, cy+x, depth, colorPair);
            drawDot(cx+y, cy-x, depth, colorPair);
            drawDot(cx-y, cy-x, depth, colorPair);
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

void BlockCanvas::drawChar(int termX, int termY, float depth,
                            char ch, int colorPair) {
    if (termX < 0 || termX >= termW_ || termY < 0 || termY >= termH_) return;
    int sx = termX, sy = termY * 2;
    if (zbuf_.testAndSet(sx, sy, depth)) {
        (void)ch;
        (void)colorPair;
    }
}

} // namespace molterm
