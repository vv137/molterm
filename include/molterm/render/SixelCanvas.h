#pragma once

#include "molterm/render/Canvas.h"
#include "molterm/render/DepthBuffer.h"

#include <string>
#include <vector>

namespace molterm {

// Pixel-based rendering using the Sixel graphics protocol.
// Each terminal cell maps to kPixPerCellX × kPixPerCellY pixels,
// giving much higher resolution than character-based canvases.
// Output is written directly to stdout, bypassing ncurses for the
// viewport area while leaving the rest of the TUI intact.
class SixelCanvas : public Canvas {
public:
    static constexpr int kPixPerCellX = 10;
    static constexpr int kPixPerCellY = 20;

    void resize(int termW, int termH) override;
    void clear() override;
    void flush(Window& win) override;

    int subW() const override { return termW_ * kPixPerCellX; }
    int subH() const override { return termH_ * kPixPerCellY; }
    int scaleX() const override { return kPixPerCellX; }
    int scaleY() const override { return kPixPerCellY; }

    void drawDot(int sx, int sy, float depth, int colorPair) override;
    void drawChar(int termX, int termY, float depth,
                  char ch, int colorPair) override;

private:
    int termW_ = 0, termH_ = 0;
    int pixW_ = 0, pixH_ = 0;

    // Per-pixel color pair ID; -1 means background (empty).
    // int8_t suffices since color pair IDs max at ~64.
    std::vector<int8_t> colorBuf_;

    DepthBuffer zbuf_;

    bool inBounds(int sx, int sy) const {
        return sx >= 0 && sx < pixW_ && sy >= 0 && sy < pixH_;
    }

    int8_t& pixel(int sx, int sy) { return colorBuf_[sy * pixW_ + sx]; }

    // Reusable buffer for Sixel output (avoids per-frame allocation).
    mutable std::string sixelBuf_;

    // Build Sixel escape-sequence string from the framebuffer.
    void buildSixelData(std::string& out) const;

    // RGB triplet for Sixel palette entries.
    struct RGB { uint8_t r, g, b; };
    static RGB colorPairToRGB(int colorPair);
};

} // namespace molterm
