#pragma once

#include "molterm/render/Canvas.h"
#include "molterm/render/DepthBuffer.h"

namespace molterm {

// 1:1 sub-pixel to terminal cell mapping
// Draws using ASCII characters: ., *, -, |, /, \.
class AsciiCanvas : public Canvas {
public:
    void resize(int termW, int termH) override;
    void clear() override;
    void flush(Window& win) override;

    int subW() const override { return termW_; }
    int subH() const override { return termH_; }
    int scaleX() const override { return 1; }
    int scaleY() const override { return 1; }

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
    DepthBuffer zbuf_;

    struct Cell {
        char ch = ' ';
        int color = 0;
    };
    std::vector<Cell> cells_;

    Cell& cell(int x, int y) { return cells_[y * termW_ + x]; }
    bool inBounds(int x, int y) const {
        return x >= 0 && x < termW_ && y >= 0 && y < termH_;
    }
};

} // namespace molterm
