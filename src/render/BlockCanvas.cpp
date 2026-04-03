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
    static constexpr char32_t FULL_BLOCK  = 0x2588; // █
    static constexpr char32_t UPPER_HALF  = 0x2580; // ▀
    static constexpr char32_t LOWER_HALF  = 0x2584; // ▄

    for (int ty = 0; ty < termH_; ++ty) {
        for (int tx = 0; tx < termW_; ++tx) {
            auto& c = cell(tx, ty);
            if (!c.topFilled && !c.botFilled) continue;

            if (c.topFilled && c.botFilled) {
                if (c.topColor == c.botColor) {
                    win.addWideChar(ty, tx, FULL_BLOCK, c.topColor);
                } else {
                    // ncurses can't easily do fg+bg per char with arbitrary colors
                    win.addWideChar(ty, tx, UPPER_HALF, c.topColor);
                }
            } else if (c.topFilled) {
                win.addWideChar(ty, tx, UPPER_HALF, c.topColor);
            } else {
                win.addWideChar(ty, tx, LOWER_HALF, c.botColor);
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
