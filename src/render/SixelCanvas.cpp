#include "molterm/render/SixelCanvas.h"
#include "molterm/render/ColorMapper.h"

#include <algorithm>
#include <cstdio>
#include <ncurses.h>

namespace molterm {

// ── Lifecycle ───────────────────────────────────────────────────────────────

void SixelCanvas::resize(int termW, int termH) {
    termW_ = termW;
    termH_ = termH;
    pixW_ = termW * kPixPerCellX;
    pixH_ = termH * kPixPerCellY;
    colorBuf_.resize(static_cast<size_t>(pixW_) * pixH_);
    zbuf_.resize(pixW_, pixH_);
    clear();
}

void SixelCanvas::clear() {
    std::fill(colorBuf_.begin(), colorBuf_.end(), -1);
    zbuf_.clear();
}

// ── Drawing primitives ──────────────────────────────────────────────────────

void SixelCanvas::drawDot(int sx, int sy, float depth, int colorPair) {
    if (!inBounds(sx, sy)) return;
    if (!zbuf_.testAndSet(sx, sy, depth)) return;
    pixel(sx, sy) = static_cast<int8_t>(colorPair);
}

void SixelCanvas::drawChar(int termX, int termY, float depth,
                            char ch, int colorPair) {
    // Labels are not rendered in Sixel mode (no glyph rasterizer).
    (void)termX; (void)termY; (void)depth; (void)ch; (void)colorPair;
}

// ── Color pair → RGB mapping ────────────────────────────────────────────────

SixelCanvas::RGB SixelCanvas::colorPairToRGB(int colorPair) {
    switch (colorPair) {
        // Element colors (CPK-ish)
        case kColorCarbon:      return {34, 200, 34};
        case kColorNitrogen:    return {50, 80, 255};
        case kColorOxygen:      return {255, 40, 40};
        case kColorSulfur:      return {255, 220, 30};
        case kColorPhosphorus:  return {200, 50, 200};
        case kColorHydrogen:    return {220, 220, 220};
        case kColorIron:        return {80, 220, 220};
        case kColorOther:       return {200, 200, 200};
        // Chain colors
        case kColorChainA:      return {34, 200, 34};
        case kColorChainB:      return {80, 220, 220};
        case kColorChainC:      return {200, 50, 200};
        case kColorChainD:      return {255, 220, 30};
        case kColorChainE:      return {255, 40, 40};
        case kColorChainF:      return {50, 80, 255};
        // Secondary structure
        case kColorHelix:       return {255, 60, 60};
        case kColorSheet:       return {255, 220, 30};
        case kColorLoop:        return {34, 200, 34};
        // B-factor gradient
        case kColorBFactorLow:  return {50, 80, 255};
        case kColorBFactorMid:  return {34, 200, 34};
        case kColorBFactorHigh: return {255, 40, 40};
        // Named palette
        case kColorRed:         return {255, 50, 50};
        case kColorGreen:       return {50, 220, 50};
        case kColorBlue:        return {50, 80, 255};
        case kColorYellow:      return {255, 255, 50};
        case kColorMagenta:     return {255, 50, 255};
        case kColorCyan:        return {50, 255, 255};
        case kColorWhite:       return {240, 240, 240};
        case kColorOrange:      return {255, 165, 0};
        case kColorPink:        return {255, 175, 200};
        case kColorLime:        return {135, 255, 0};
        case kColorTeal:        return {0, 150, 136};
        case kColorPurple:      return {160, 80, 255};
        case kColorSalmon:      return {255, 140, 105};
        case kColorSlate:       return {100, 130, 180};
        case kColorGray:        return {158, 158, 158};
        default:                return {200, 200, 200};
    }
}

// ── Sixel encoder ───────────────────────────────────────────────────────────

void SixelCanvas::buildSixelData(std::string& out) const {
    static constexpr int kMaxColorId = 128;

    // Collect used colors with a flat bool array (O(N), no heap alloc).
    bool used[kMaxColorId] = {};
    for (auto c : colorBuf_) {
        if (c >= 0) used[c] = true;
    }

    // Build dense palette: colorPair → sixelIndex.
    std::vector<std::pair<int, int>> palette;
    int nextIdx = 0;
    for (int i = 0; i < kMaxColorId; ++i) {
        if (used[i]) palette.push_back({i, nextIdx++});
    }
    if (palette.empty()) return;

    out.reserve(static_cast<size_t>(pixW_) * pixH_ / 2);

    // DCS introducer
    out += "\033P0;0;0q";

    // Raster attributes: pixel aspect 1:1, image dimensions
    out += "\"1;1;";
    out += std::to_string(pixW_);
    out += ';';
    out += std::to_string(pixH_);

    // Palette definitions: # idx ; 2 ; R% ; G% ; B%  (0-100 range)
    for (auto& [cp, idx] : palette) {
        RGB rgb = colorPairToRGB(cp);
        out += '#';
        out += std::to_string(idx);
        out += ";2;";
        out += std::to_string(rgb.r * 100 / 255);
        out += ';';
        out += std::to_string(rgb.g * 100 / 255);
        out += ';';
        out += std::to_string(rgb.b * 100 / 255);
    }

    // Encode scanlines in 6-row bands.
    // Precompute which colors appear in each band to avoid per-color scans.
    for (int bandY = 0; bandY < pixH_; bandY += 6) {
        int bandH = std::min(6, pixH_ - bandY);

        // Single pass over band pixels to find active colors.
        bool bandColors[kMaxColorId] = {};
        for (int row = 0; row < bandH; ++row) {
            const auto* rowPtr = &colorBuf_[(bandY + row) * pixW_];
            for (int x = 0; x < pixW_; ++x) {
                if (rowPtr[x] >= 0)
                    bandColors[rowPtr[x]] = true;
            }
        }

        bool firstColor = true;
        for (auto& [cp, sixIdx] : palette) {
            if (!bandColors[cp]) continue;

            if (!firstColor) out += '$';
            firstColor = false;

            out += '#';
            out += std::to_string(sixIdx);

            // RLE-encode the 6-pixel columns for this color.
            int runChar = -1;
            int runLen = 0;

            auto flushRun = [&]() {
                if (runLen <= 0) return;
                if (runLen <= 3) {
                    for (int i = 0; i < runLen; ++i)
                        out += static_cast<char>(runChar);
                } else {
                    out += '!';
                    out += std::to_string(runLen);
                    out += static_cast<char>(runChar);
                }
            };

            for (int x = 0; x < pixW_; ++x) {
                uint8_t sixel = 0;
                for (int bit = 0; bit < 6; ++bit) {
                    int py = bandY + bit;
                    if (py < pixH_ && colorBuf_[py * pixW_ + x] == cp) {
                        sixel |= (1 << bit);
                    }
                }
                int ch = 0x3F + sixel;

                if (ch == runChar) {
                    ++runLen;
                } else {
                    flushRun();
                    runChar = ch;
                    runLen = 1;
                }
            }
            flushRun();
        }
        if (bandY + 6 < pixH_) out += '-';
    }

    out += "\033\\";
}

// ── Flush to terminal ───────────────────────────────────────────────────────

void SixelCanvas::flush(Window& win) {
    if (pixW_ <= 0 || pixH_ <= 0) return;

    sixelBuf_.clear();
    buildSixelData(sixelBuf_);
    if (sixelBuf_.empty()) return;

    // Determine the window's absolute position on screen.
    int wy = 0, wx = 0;
    getbegyx(win.raw(), wy, wx);

    // Temporarily leave ncurses mode to write raw escape sequences.
    // Use the cursor-positioning escape and then emit the Sixel data.
    // Row/col are 1-based in VT sequences.
    fprintf(stdout, "\033[%d;%dH%s", wy + 1, wx + 1, sixelBuf_.c_str());
    fflush(stdout);
}

} // namespace molterm
