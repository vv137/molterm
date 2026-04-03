#include "molterm/render/SixelCanvas.h"
#include "molterm/render/ColorMapper.h"

#include <algorithm>
#include <cstdio>
#include <ncurses.h>
#include <set>

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
    pixel(sx, sy) = colorPair;
}

void SixelCanvas::drawLine(int x0, int y0, float d0,
                            int x1, int y1, float d1,
                            int colorPair) {
    bresenham(x0, y0, d0, x1, y1, d1,
        [&](int x, int y, float depth) {
            drawDot(x, y, depth, colorPair);
        });
}

void SixelCanvas::drawCircle(int cx, int cy, float depth,
                              int radius, int colorPair, bool filled) {
    if (filled) {
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (dx * dx + dy * dy <= radius * radius) {
                    drawDot(cx + dx, cy + dy, depth, colorPair);
                }
            }
        }
    } else {
        int x = radius, y = 0;
        int err = 1 - radius;
        while (x >= y) {
            drawDot(cx + x, cy + y, depth, colorPair);
            drawDot(cx - x, cy + y, depth, colorPair);
            drawDot(cx + x, cy - y, depth, colorPair);
            drawDot(cx - x, cy - y, depth, colorPair);
            drawDot(cx + y, cy + x, depth, colorPair);
            drawDot(cx - y, cy + x, depth, colorPair);
            drawDot(cx + y, cy - x, depth, colorPair);
            drawDot(cx - y, cy - x, depth, colorPair);
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
    // Collect the set of color pair IDs actually used in the framebuffer.
    std::set<int> usedColors;
    for (int c : colorBuf_) {
        if (c >= 0) usedColors.insert(c);
    }
    if (usedColors.empty()) return;

    // Assign compact Sixel palette indices.
    // Map: colorPair → sixelIndex  (0-based, dense).
    std::vector<std::pair<int, int>> palette; // (colorPair, sixelIdx)
    int nextIdx = 0;
    for (int cp : usedColors) {
        palette.push_back({cp, nextIdx++});
    }

    // Reserve a generous estimate to avoid repeated reallocation.
    out.reserve(static_cast<size_t>(pixW_) * pixH_ / 2);

    // DCS introducer: ESC P 0 ; 0 ; 0 q
    //   Params: P1=0 (normal aspect), P2=0 (no bg), P3=0 (horiz grid = default)
    out += "\033P0;0;0q";

    // Set raster attributes: "  width ; height
    // "1;1; tells the terminal the pixel aspect ratio is 1:1
    out += "\"1;1;";
    out += std::to_string(pixW_);
    out += ';';
    out += std::to_string(pixH_);

    // Define palette entries: # idx ; 2 ; R ; G ; B  (RGB in 0-100 range)
    for (auto& [cp, idx] : palette) {
        RGB rgb = colorPairToRGB(cp);
        int r100 = rgb.r * 100 / 255;
        int g100 = rgb.g * 100 / 255;
        int b100 = rgb.b * 100 / 255;
        out += '#';
        out += std::to_string(idx);
        out += ";2;";
        out += std::to_string(r100);
        out += ';';
        out += std::to_string(g100);
        out += ';';
        out += std::to_string(b100);
    }

    // Build a quick reverse map for colorPair → sixelIdx.
    // Max color pair ID we need to support is ~64.
    int maxCp = 0;
    for (auto& [cp, _] : palette) maxCp = std::max(maxCp, cp);
    std::vector<int> cpToIdx(maxCp + 1, -1);
    for (auto& [cp, idx] : palette) cpToIdx[cp] = idx;

    // Encode scanlines in 6-row bands.
    for (int bandY = 0; bandY < pixH_; bandY += 6) {
        bool firstColor = true;
        for (auto& [cp, sixIdx] : palette) {
            // Check if this color appears in this band at all (skip if not).
            bool hasPixels = false;
            int bandH = std::min(6, pixH_ - bandY);
            for (int row = 0; row < bandH && !hasPixels; ++row) {
                const int* rowPtr = &colorBuf_[(bandY + row) * pixW_];
                for (int x = 0; x < pixW_; ++x) {
                    if (rowPtr[x] == cp) { hasPixels = true; break; }
                }
            }
            if (!hasPixels) continue;

            // Graphics CR to restart the X cursor for this color
            // (not needed for the first color in a band).
            if (!firstColor) out += '$';
            firstColor = false;

            // Select this color
            out += '#';
            out += std::to_string(sixIdx);

            // Encode the 6-pixel columns with RLE.
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
                // Build the 6-bit Sixel value for this column.
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
        // Graphics newline — move to next 6-pixel band.
        if (bandY + 6 < pixH_) out += '-';
    }

    // String terminator: ESC backslash
    out += "\033\\";
}

// ── Flush to terminal ───────────────────────────────────────────────────────

void SixelCanvas::flush(Window& win) {
    if (pixW_ <= 0 || pixH_ <= 0) return;

    std::string sixel;
    buildSixelData(sixel);
    if (sixel.empty()) return;

    // Determine the window's absolute position on screen.
    int wy = 0, wx = 0;
    getbegyx(win.raw(), wy, wx);

    // Temporarily leave ncurses mode to write raw escape sequences.
    // Use the cursor-positioning escape and then emit the Sixel data.
    // Row/col are 1-based in VT sequences.
    fprintf(stdout, "\033[%d;%dH%s", wy + 1, wx + 1, sixel.c_str());
    fflush(stdout);
}

} // namespace molterm
