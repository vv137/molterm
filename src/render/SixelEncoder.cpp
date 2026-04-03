#include "molterm/render/SixelEncoder.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace molterm {

// Quantize an RGB pixel to a palette index (simple 6-bit color cube)
static int quantize(uint8_t r, uint8_t g, uint8_t b) {
    // 6×6×6 color cube = 216 colors
    int ri = r * 5 / 255;
    int gi = g * 5 / 255;
    int bi = b * 5 / 255;
    return ri * 36 + gi * 6 + bi;
}

std::string SixelEncoder::encode(const uint8_t* rgb, int width, int height) {
    buf_.clear();
    if (width <= 0 || height <= 0) return buf_;

    // Build palette from actually used colors
    static constexpr int kPaletteSize = 216;  // 6×6×6 cube
    bool used[kPaletteSize] = {};

    // Quantize all pixels and find used palette entries
    std::vector<uint8_t> indexed(static_cast<size_t>(width) * height);
    for (int i = 0; i < width * height; ++i) {
        uint8_t r = rgb[i * 3], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
        // Treat near-black as background (skip)
        if (r < 5 && g < 5 && b < 5) {
            indexed[i] = 255;  // sentinel: background
            continue;
        }
        uint8_t idx = static_cast<uint8_t>(quantize(r, g, b));
        indexed[i] = idx;
        used[idx] = true;
    }

    // Build dense palette mapping: sparse_idx → dense_idx
    int denseMap[kPaletteSize];
    std::memset(denseMap, -1, sizeof(denseMap));
    struct PEntry { int sparse; int dense; };
    std::vector<PEntry> palette;
    int nextDense = 0;
    for (int i = 0; i < kPaletteSize; ++i) {
        if (used[i]) {
            denseMap[i] = nextDense;
            palette.push_back({i, nextDense});
            ++nextDense;
        }
    }
    if (palette.empty()) return buf_;

    buf_.reserve(static_cast<size_t>(width) * height / 2);

    // DCS introducer (transparent background)
    buf_ += "\033P0;1;0q";

    // Raster attributes
    buf_ += "\"1;1;";
    buf_ += std::to_string(width);
    buf_ += ';';
    buf_ += std::to_string(height);

    // Palette definitions
    for (auto& pe : palette) {
        int ri = (pe.sparse / 36) * 100 / 5;
        int gi = ((pe.sparse / 6) % 6) * 100 / 5;
        int bi = (pe.sparse % 6) * 100 / 5;
        buf_ += '#';
        buf_ += std::to_string(pe.dense);
        buf_ += ";2;";
        buf_ += std::to_string(ri);
        buf_ += ';';
        buf_ += std::to_string(gi);
        buf_ += ';';
        buf_ += std::to_string(bi);
    }

    // RLE helper
    auto flushRun = [this](int ch, int len) {
        if (len <= 0) return;
        if (len <= 3) {
            for (int i = 0; i < len; ++i)
                buf_ += static_cast<char>(ch);
        } else {
            buf_ += '!';
            buf_ += std::to_string(len);
            buf_ += static_cast<char>(ch);
        }
    };

    // Precompute sixel grid per band (all colors at once)
    int numColors = nextDense;
    std::vector<uint8_t> sixelGrid(static_cast<size_t>(numColors) * width, 0);

    for (int bandY = 0; bandY < height; bandY += 6) {
        int bandH = std::min(6, height - bandY);

        std::memset(sixelGrid.data(), 0, static_cast<size_t>(numColors) * width);
        bool bandHasColor[kPaletteSize] = {};

        // Single pass: build all sixel bytes
        for (int row = 0; row < bandH; ++row) {
            const uint8_t* rowPtr = &indexed[(bandY + row) * width];
            uint8_t bit = 1 << row;
            for (int x = 0; x < width; ++x) {
                uint8_t ci = rowPtr[x];
                if (ci == 255) continue;  // background
                int di = denseMap[ci];
                if (di >= 0) {
                    sixelGrid[di * width + x] |= bit;
                    bandHasColor[ci] = true;
                }
            }
        }

        // Emit each active color
        bool firstColor = true;
        for (auto& pe : palette) {
            if (!bandHasColor[pe.sparse]) continue;

            if (!firstColor) buf_ += '$';
            firstColor = false;

            buf_ += '#';
            buf_ += std::to_string(pe.dense);

            const uint8_t* row = &sixelGrid[pe.dense * width];
            int runChar = -1, runLen = 0;

            for (int x = 0; x < width; ++x) {
                int ch = 0x3F + row[x];
                if (ch == runChar) {
                    ++runLen;
                } else {
                    flushRun(runChar, runLen);
                    runChar = ch;
                    runLen = 1;
                }
            }
            flushRun(runChar, runLen);
        }

        if (bandY + 6 < height) buf_ += '-';
    }

    buf_ += "\033\\";
    return buf_;
}

} // namespace molterm
