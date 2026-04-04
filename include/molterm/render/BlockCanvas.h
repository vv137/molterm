#pragma once

#include "molterm/render/Canvas.h"
#include "molterm/render/DepthBuffer.h"

#include <vector>

namespace molterm {

// 1×2 sub-pixel per terminal cell using Unicode half-block characters.
// Upper half: ▀ (U+2580), Lower half: ▄ (U+2584), Full: █ (U+2588)
// Provides 2× vertical resolution.
class BlockCanvas : public Canvas {
public:
    void resize(int termW, int termH) override;
    void clear() override;
    void flush(Window& win) override;
    void invalidate() override { prevCells_.clear(); }

    int subW() const override { return termW_; }
    int subH() const override { return termH_ * 2; }
    int scaleX() const override { return 1; }
    int scaleY() const override { return 2; }

    void drawDot(int sx, int sy, float depth, int colorPair) override;
    void drawChar(int termX, int termY, float depth,
                  char ch, int colorPair) override;

private:
    int termW_ = 0, termH_ = 0;

    struct Cell {
        bool topFilled = false;
        bool botFilled = false;
        int topColor = 0;
        int botColor = 0;
    };
    std::vector<Cell> cells_;
    std::vector<Cell> prevCells_;
    DepthBuffer zbuf_;  // at sub-pixel resolution

    Cell& cell(int tx, int ty) { return cells_[ty * termW_ + tx]; }
    bool inBounds(int sx, int sy) const {
        return sx >= 0 && sx < termW_ && sy >= 0 && sy < termH_ * 2;
    }
};

} // namespace molterm
