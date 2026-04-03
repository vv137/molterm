#include "molterm/render/PixelCanvas.h"
#include "molterm/render/ColorMapper.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <ncurses.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace molterm {

PixelCanvas::PixelCanvas(std::unique_ptr<GraphicsEncoder> encoder)
    : encoder_(std::move(encoder)) {
    queryCellSize();
}

void PixelCanvas::queryCellSize() {
    struct winsize ws = {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_xpixel > 0 && ws.ws_ypixel > 0 &&
        ws.ws_col > 0 && ws.ws_row > 0) {
        cellPixW_ = std::max(2, static_cast<int>(ws.ws_xpixel / ws.ws_col));
        cellPixH_ = std::max(4, static_cast<int>(ws.ws_ypixel / ws.ws_row));
    }
}

void PixelCanvas::setEncoder(std::unique_ptr<GraphicsEncoder> enc) {
    encoder_ = std::move(enc);
    prevRgb_.clear();  // force full redraw
}

void PixelCanvas::resize(int termW, int termH) {
    queryCellSize();
    int newPixW = termW * cellPixW_;
    int newPixH = termH * cellPixH_;

    if (newPixW == pixW_ && newPixH == pixH_) return;

    termW_ = termW;
    termH_ = termH;
    pixW_ = newPixW;
    pixH_ = newPixH;

    size_t nPix = static_cast<size_t>(pixW_) * pixH_;
    rgb_.resize(nPix * 3);
    colorIds_.resize(nPix);
    prevRgb_.clear();
    zbuf_.resize(pixW_, pixH_);
    clear();
}

void PixelCanvas::clear() {
    std::memset(rgb_.data(), 0, rgb_.size());
    std::memset(colorIds_.data(), 0xFF, colorIds_.size());
    zbuf_.clear();
    zMin_ = std::numeric_limits<float>::max();
    zMax_ = std::numeric_limits<float>::lowest();
}

// ── Drawing ─────────────────────────────────────────────────────────────────

void PixelCanvas::setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    size_t idx = (static_cast<size_t>(y) * pixW_ + x) * 3;
    rgb_[idx] = r;
    rgb_[idx + 1] = g;
    rgb_[idx + 2] = b;
}

void PixelCanvas::drawDot(int sx, int sy, float depth, int colorPair) {
    if (!inBounds(sx, sy)) return;
    if (!zbuf_.testAndSet(sx, sy, depth)) return;
    if (depth < zMin_) zMin_ = depth;
    if (depth > zMax_) zMax_ = depth;
    auto c = colorPairToRGB(colorPair);
    setPixel(sx, sy, c.r, c.g, c.b);
    colorIds_[static_cast<size_t>(sy) * pixW_ + sx] = static_cast<int8_t>(colorPair);
}

void PixelCanvas::drawLine(int x0, int y0, float d0,
                            int x1, int y1, float d1, int colorPair) {
    // Shaded line: interpolate depth along line, apply subtle intensity variation.
    // Closer segments are brighter, farther are dimmer (complements depth fog).
    auto base = colorPairToRGB(colorPair);

    Canvas::bresenham(x0, y0, d0, x1, y1, d1,
        [&](int x, int y, float depth) {
            if (!inBounds(x, y)) return;
            if (!zbuf_.testAndSet(x, y, depth)) return;
            if (depth < zMin_) zMin_ = depth;
            if (depth > zMax_) zMax_ = depth;

            float intensity = 0.85f + 0.15f * (1.0f / (1.0f + std::abs(depth) * 0.01f));
            intensity = std::min(1.0f, intensity);

            uint8_t cr = static_cast<uint8_t>(std::min(255.0f, base.r * intensity));
            uint8_t cg = static_cast<uint8_t>(std::min(255.0f, base.g * intensity));
            uint8_t cb = static_cast<uint8_t>(std::min(255.0f, base.b * intensity));
            setPixel(x, y, cr, cg, cb);
            colorIds_[static_cast<size_t>(y) * pixW_ + x] = static_cast<int8_t>(colorPair);
        });
}

void PixelCanvas::drawCircle(int cx, int cy, float depth,
                              int radius, int colorPair, bool filled) {
    if (!filled) {
        Canvas::drawCircle(cx, cy, depth, radius, colorPair, false);
        return;
    }

    // Sphere shading with precomputed intensity per (dx,dy) offset.
    auto base = colorPairToRGB(colorPair);
    float invR = 1.0f / static_cast<float>(std::max(1, radius));
    float r2 = static_cast<float>(radius * radius);

    for (int dy = -radius; dy <= radius; ++dy) {
        int dy2 = dy * dy;
        for (int dx = -radius; dx <= radius; ++dx) {
            float dist2 = static_cast<float>(dx * dx + dy2);
            if (dist2 > r2) continue;

            int px = cx + dx, py = cy + dy;
            if (!inBounds(px, py)) continue;

            // Fast approximation: nz ≈ 1 - dist2/(2*r2) instead of sqrt
            float normDist2 = dist2 / r2;
            float nz = 1.0f - normDist2 * 0.5f;
            float nx = static_cast<float>(dx) * invR;
            float ny = static_cast<float>(dy) * invR;

            float dot = -0.5f * nx - 0.5f * ny + 0.707f * nz;
            float halfLambert = std::abs(dot) * 0.4f + 0.6f;
            float intensity = 0.45f + 0.55f * halfLambert;

            float zOff = depth - nz * static_cast<float>(radius) * 0.01f;
            if (!zbuf_.testAndSet(px, py, zOff)) continue;

            if (depth < zMin_) zMin_ = depth;
            if (depth > zMax_) zMax_ = depth;

            uint8_t cr = static_cast<uint8_t>(std::min(255.0f, base.r * intensity));
            uint8_t cg = static_cast<uint8_t>(std::min(255.0f, base.g * intensity));
            uint8_t cb = static_cast<uint8_t>(std::min(255.0f, base.b * intensity));
            setPixel(px, py, cr, cg, cb);
            colorIds_[static_cast<size_t>(py) * pixW_ + px] = static_cast<int8_t>(colorPair);
        }
    }
}

void PixelCanvas::drawTriangle(float x0, float y0, float z0,
                                float x1, float y1, float z1,
                                float x2, float y2, float z2,
                                int colorPair) {
    // Bounding box (clamped to canvas)
    int minX = std::max(0, static_cast<int>(std::min({x0, x1, x2})));
    int maxX = std::min(pixW_ - 1, static_cast<int>(std::max({x0, x1, x2})) + 1);
    int minY = std::max(0, static_cast<int>(std::min({y0, y1, y2})));
    int maxY = std::min(pixH_ - 1, static_cast<int>(std::max({y0, y1, y2})) + 1);

    // Edge vectors
    float ex0 = x1 - x0, ey0 = y1 - y0;
    float ex1 = x2 - x1, ey1 = y2 - y1;
    float ex2 = x0 - x2, ey2 = y0 - y2;

    // Triangle normal for shading (flat per-triangle)
    float ax = x1 - x0, ay = y1 - y0, az = z1 - z0;
    float bx = x2 - x0, by = y2 - y0, bz = z2 - z0;
    float nx = ay * bz - az * by;
    float ny = az * bx - ax * bz;
    float nz = ax * by - ay * bx;
    float nLen = std::sqrt(nx*nx + ny*ny + nz*nz);
    if (nLen > 1e-6f) { nx /= nLen; ny /= nLen; nz /= nLen; }

    // Two-sided half-Lambert: light from upper-left-front
    float dot = -0.4f * nx - 0.4f * ny + 0.82f * nz;
    float halfLambert = std::abs(dot) * 0.4f + 0.6f;
    float intensity = 0.55f + 0.45f * halfLambert;

    auto base = colorPairToRGB(colorPair);
    uint8_t cr = static_cast<uint8_t>(std::min(255.0f, base.r * intensity));
    uint8_t cg = static_cast<uint8_t>(std::min(255.0f, base.g * intensity));
    uint8_t cb = static_cast<uint8_t>(std::min(255.0f, base.b * intensity));

    // Barycentric denominator
    float denom = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (std::abs(denom) < 1e-6f) return;  // degenerate
    float invDenom = 1.0f / denom;

    for (int py = minY; py <= maxY; ++py) {
        for (int px = minX; px <= maxX; ++px) {
            float fpx = static_cast<float>(px) + 0.5f;
            float fpy = static_cast<float>(py) + 0.5f;

            // Edge tests (half-plane)
            float e0 = ex0 * (fpy - y0) - ey0 * (fpx - x0);
            float e1 = ex1 * (fpy - y1) - ey1 * (fpx - x1);
            float e2 = ex2 * (fpy - y2) - ey2 * (fpx - x2);

            // All same sign → inside (with small epsilon for edge)
            if ((e0 >= -0.5f && e1 >= -0.5f && e2 >= -0.5f) ||
                (e0 <= 0.5f && e1 <= 0.5f && e2 <= 0.5f)) {

                // Barycentric interpolation for depth
                float w0 = ((y1 - y2) * (fpx - x2) + (x2 - x1) * (fpy - y2)) * invDenom;
                float w1 = ((y2 - y0) * (fpx - x2) + (x0 - x2) * (fpy - y2)) * invDenom;
                float w2 = 1.0f - w0 - w1;
                float depth = w0 * z0 + w1 * z1 + w2 * z2;

                if (!zbuf_.testAndSet(px, py, depth)) continue;
                if (depth < zMin_) zMin_ = depth;
                if (depth > zMax_) zMax_ = depth;

                setPixel(px, py, cr, cg, cb);
                colorIds_[static_cast<size_t>(py) * pixW_ + px] = static_cast<int8_t>(colorPair);
            }
        }
    }
}

void PixelCanvas::drawChar(int, int, float, char, int) {
    // No glyph rasterizer — labels not rendered in pixel mode
}

// ── Post-processing ─────────────────────────────────────────────────────────

void PixelCanvas::applyDepthFog(float strength, uint8_t fogR, uint8_t fogG, uint8_t fogB) {
    preFogRgb_ = rgb_;

    if (pixW_ <= 0 || pixH_ <= 0 || strength <= 0.0f) return;
    if (zMax_ <= zMin_) return;

    float invRange = strength / (zMax_ - zMin_);

    // Single pass: blend each rendered pixel toward fog color
    size_t nPix = static_cast<size_t>(pixW_) * pixH_;
    for (size_t i = 0; i < nPix; ++i) {
        if (colorIds_[i] < 0) continue;  // background — skip

        float z = zbuf_.get(static_cast<int>(i % pixW_), static_cast<int>(i / pixW_));
        float t = (z - zMin_) * invRange;

        size_t idx = i * 3;
        rgb_[idx]     = static_cast<uint8_t>(rgb_[idx]     + (fogR - rgb_[idx])     * t);
        rgb_[idx + 1] = static_cast<uint8_t>(rgb_[idx + 1] + (fogG - rgb_[idx + 1]) * t);
        rgb_[idx + 2] = static_cast<uint8_t>(rgb_[idx + 2] + (fogB - rgb_[idx + 2]) * t);
    }
}

// ── PNG export ──────────────────────────────────────────────────────────────

// Minimal PNG writer using zlib (no libpng needed).
#include <zlib.h>

static void pngPut32(std::vector<uint8_t>& v, uint32_t val) {
    v.push_back((val >> 24) & 0xFF);
    v.push_back((val >> 16) & 0xFF);
    v.push_back((val >> 8) & 0xFF);
    v.push_back(val & 0xFF);
}

static void pngWriteChunk(std::ofstream& f, const char* type,
                           const uint8_t* data, size_t len) {
    uint32_t crc;
    std::vector<uint8_t> buf;
    pngPut32(buf, static_cast<uint32_t>(len));
    buf.insert(buf.end(), type, type + 4);
    if (data && len > 0) buf.insert(buf.end(), data, data + len);
    crc = crc32(0L, buf.data() + 4, static_cast<uInt>(4 + len));
    pngPut32(buf, crc);
    f.write(reinterpret_cast<const char*>(buf.data()), buf.size());
}

bool PixelCanvas::savePNG(const std::string& path) const {
    if (pixW_ <= 0 || pixH_ <= 0) return false;

    // Use pre-fog image for brighter PNG export.
    // Fall back to prevRgb_ (post-fog but pre-swap), then rgb_.
    const std::vector<uint8_t>& src =
        (!preFogRgb_.empty() && preFogRgb_.size() == static_cast<size_t>(pixW_ * pixH_ * 3)) ? preFogRgb_ :
        (!prevRgb_.empty() && prevRgb_.size() == rgb_.size()) ? prevRgb_ : rgb_;

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;

    // PNG signature
    const uint8_t sig[] = {137, 80, 78, 71, 13, 10, 26, 10};
    f.write(reinterpret_cast<const char*>(sig), 8);

    // IHDR
    std::vector<uint8_t> ihdr;
    pngPut32(ihdr, static_cast<uint32_t>(pixW_));
    pngPut32(ihdr, static_cast<uint32_t>(pixH_));
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(6);  // color type: RGBA (transparent background)
    ihdr.push_back(0);  // compression
    ihdr.push_back(0);  // filter
    ihdr.push_back(0);  // interlace
    pngWriteChunk(f, "IHDR", ihdr.data(), ihdr.size());

    // IDAT: filter byte (0=None) + RGBA row data, zlib compressed
    size_t rawSize = static_cast<size_t>(pixH_) * (1 + pixW_ * 4);
    std::vector<uint8_t> raw(rawSize);
    size_t offset = 0;
    for (int y = 0; y < pixH_; ++y) {
        raw[offset++] = 0;  // filter: None
        for (int x = 0; x < pixW_; ++x) {
            size_t si = (static_cast<size_t>(y) * pixW_ + x) * 3;
            uint8_t r = src[si], g = src[si + 1], b = src[si + 2];
            bool isBg = (r == 0 && g == 0 && b == 0);
            raw[offset++] = r;
            raw[offset++] = g;
            raw[offset++] = b;
            raw[offset++] = isBg ? 0 : 255;  // alpha: 0=transparent, 255=opaque
        }
    }

    uLongf compLen = compressBound(static_cast<uLong>(rawSize));
    std::vector<uint8_t> comp(compLen);
    if (compress2(comp.data(), &compLen, raw.data(), static_cast<uLong>(rawSize), 6) != Z_OK)
        return false;
    comp.resize(compLen);

    pngWriteChunk(f, "IDAT", comp.data(), comp.size());

    // IEND
    pngWriteChunk(f, "IEND", nullptr, 0);

    return f.good();
}

// ── Flush ───────────────────────────────────────────────────────────────────

void PixelCanvas::flush(Window& win) {
    if (pixW_ <= 0 || pixH_ <= 0 || !encoder_) return;

    // Skip if identical to previous frame
    if (prevRgb_.size() == rgb_.size() &&
        std::memcmp(prevRgb_.data(), rgb_.data(), rgb_.size()) == 0) {
        return;
    }

    std::string encoded = encoder_->encode(rgb_.data(), pixW_, pixH_, termW_, termH_);
    if (!encoded.empty()) {
        int wy = 0, wx = 0;
        getbegyx(win.raw(), wy, wx);
        // Save cursor, move to viewport origin, emit image, restore cursor
        fprintf(stdout, "\0337\033[%d;%dH%s\0338", wy + 1, wx + 1, encoded.c_str());
        fflush(stdout);
    }

    std::swap(prevRgb_, rgb_);
    rgb_.resize(prevRgb_.size());  // re-allocate for next frame's clear()
}

// ── Color pair → RGB ────────────────────────────────────────────────────────

PixelCanvas::RGB PixelCanvas::colorPairToRGB(int colorPair) {
    switch (colorPair) {
        case kColorCarbon:      return {34, 200, 34};
        case kColorNitrogen:    return {50, 80, 255};
        case kColorOxygen:      return {255, 40, 40};
        case kColorSulfur:      return {255, 220, 30};
        case kColorPhosphorus:  return {200, 50, 200};
        case kColorHydrogen:    return {220, 220, 220};
        case kColorIron:        return {80, 220, 220};
        case kColorOther:       return {200, 200, 200};
        case kColorChainA:      return {34, 200, 34};
        case kColorChainB:      return {80, 220, 220};
        case kColorChainC:      return {200, 50, 200};
        case kColorChainD:      return {255, 220, 30};
        case kColorChainE:      return {255, 40, 40};
        case kColorChainF:      return {50, 80, 255};
        case kColorHelix:       return {255, 60, 60};
        case kColorSheet:       return {255, 220, 30};
        case kColorLoop:        return {34, 200, 34};
        case kColorBFactorLow:  return {50, 80, 255};
        case kColorBFactorMid:  return {34, 200, 34};
        case kColorBFactorHigh: return {255, 40, 40};
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
        case kColorPLDDTVeryHigh: return {0, 83, 214};
        case kColorPLDDTHigh:     return {101, 203, 243};
        case kColorPLDDTLow:      return {255, 219, 19};
        case kColorPLDDTVeryLow:  return {255, 125, 69};
        case kColorRainbow0:    return {0, 60, 255};
        case kColorRainbow1:    return {0, 220, 255};
        case kColorRainbow2:    return {0, 220, 0};
        case kColorRainbow3:    return {255, 255, 0};
        case kColorRainbow4:    return {255, 0, 0};
        case kColorResNonpolar: return {190, 190, 190};   // light gray
        case kColorResPolar:    return {50, 200, 80};     // green
        case kColorResAcidic:   return {230, 50, 50};     // red
        case kColorResBasic:    return {60, 100, 255};    // blue
        default:                return {200, 200, 200};
    }
}

} // namespace molterm
