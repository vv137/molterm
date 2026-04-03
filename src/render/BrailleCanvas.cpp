#include "molterm/render/BrailleCanvas.h"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace molterm {

static constexpr char32_t BRAILLE_BASE = 0x2800;

uint8_t BrailleCanvas::dotBit(int dx, int dy) {
    // dx: 0 or 1, dy: 0..3
    // Layout:
    //   (0,0)=bit0  (1,0)=bit3
    //   (0,1)=bit1  (1,1)=bit4
    //   (0,2)=bit2  (1,2)=bit5
    //   (0,3)=bit6  (1,3)=bit7
    static const uint8_t table[2][4] = {
        {0x01, 0x02, 0x04, 0x40},  // dx=0
        {0x08, 0x10, 0x20, 0x80},  // dx=1
    };
    return table[dx][dy];
}

void BrailleCanvas::resize(int termW, int termH) {
    termW_ = termW;
    termH_ = termH;
    cells_.resize(static_cast<size_t>(termW) * termH);
    zbuf_.resize(termW * 2, termH * 4);
    clear();
}

void BrailleCanvas::clear() {
    for (auto& c : cells_) { c.dots = 0; c.color = 0; }
    zbuf_.clear();
}

void BrailleCanvas::flush(Window& win) {
    for (int ty = 0; ty < termH_; ++ty) {
        for (int tx = 0; tx < termW_; ++tx) {
            auto& c = cell(tx, ty);
            if (c.dots == 0) continue;

            char32_t cp = BRAILLE_BASE | c.dots;
            win.addWideChar(ty, tx, cp, c.color);
        }
    }
}

void BrailleCanvas::drawDot(int sx, int sy, float depth, int colorPair) {
    if (!inBounds(sx, sy)) return;
    if (!zbuf_.testAndSet(sx, sy, depth)) return;

    int tx = sx / 2, ty = sy / 4;
    int dx = sx % 2, dy = sy % 4;

    auto& c = cell(tx, ty);
    c.dots |= dotBit(dx, dy);
    c.color = colorPair;
}

void BrailleCanvas::drawChar(int termX, int termY, float depth,
                              char ch, int colorPair) {
    // For labels: just write the character directly at the terminal cell
    // This overwrites any braille dots in that cell
    if (termX < 0 || termX >= termW_ || termY < 0 || termY >= termH_) return;
    // Check depth at center of cell
    int sx = termX * 2 + 1, sy = termY * 4 + 2;
    if (zbuf_.testAndSet(sx, sy, depth)) {
        auto& c = cell(termX, termY);
        // Mark cell as char-based by setting dots to 0xFF (sentinel)
        // flush() will need to check for this
        // Actually, simpler: just clear dots and let flush handle chars separately
        // For now, skip char rendering in braille mode
        (void)ch;
        (void)colorPair;
        (void)c;
    }
}

} // namespace molterm
