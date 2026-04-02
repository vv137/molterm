#pragma once

#include "molterm/render/Canvas.h"
#include "molterm/render/DepthBuffer.h"

#include <vector>

namespace molterm {

// 2×4 sub-pixel per terminal cell using Unicode Braille (U+2800–U+28FF)
// Provides 8× resolution compared to ASCII.
//
// Braille dot layout per cell:
//   col 0  col 1
//   bit0   bit3    row 0
//   bit1   bit4    row 1
//   bit2   bit5    row 2
//   bit6   bit7    row 3
class BrailleCanvas : public Canvas {
public:
    void resize(int termW, int termH) override;
    void clear() override;
    void flush(Window& win) override;

    int subW() const override { return termW_ * 2; }
    int subH() const override { return termH_ * 4; }
    int scaleX() const override { return 2; }
    int scaleY() const override { return 4; }

    void drawDot(int sx, int sy, float depth, int colorPair) override;
    void drawLine(int x0, int y0, float d0,
                  int x1, int y1, float d1,
                  int colorPair) override;
    void drawCircle(int cx, int cy, float depth,
                    int radius, int colorPair, bool filled) override;
    void drawChar(int termX, int termY, float depth,
                  char ch, int colorPair) override;

private:
    int termW_ = 0, termH_ = 0;

    struct Cell {
        uint8_t dots = 0;   // braille bitmask
        int color = 0;      // dominant color
    };
    std::vector<Cell> cells_;

    // Depth buffer at sub-pixel resolution
    DepthBuffer zbuf_;

    Cell& cell(int tx, int ty) { return cells_[ty * termW_ + tx]; }
    bool inBounds(int sx, int sy) const {
        return sx >= 0 && sx < termW_ * 2 && sy >= 0 && sy < termH_ * 4;
    }

    static uint8_t dotBit(int dx, int dy);
};

} // namespace molterm
