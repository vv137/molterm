#include "molterm/render/PixelCanvas.h"
#include "molterm/render/ColorMapper.h"

#include "stb_truetype.h"
#include "font_data.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <thread>
#include <vector>
#include <ncurses.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace molterm {

PixelCanvas::PixelCanvas(std::unique_ptr<GraphicsEncoder> encoder)
    : encoder_(std::move(encoder)) {
    queryCellSize();
}

PixelCanvas::~PixelCanvas() = default;

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
    if (!atomIds_.empty()) atomIds_.resize(nPix);
    prevRgb_.clear();
    zbuf_.resize(pixW_, pixH_);
    clear();
}

void PixelCanvas::resizePixels(int pixW, int pixH) {
    if (pixW < 1) pixW = 1;
    if (pixH < 1) pixH = 1;
    if (pixW == pixW_ && pixH == pixH_) return;

    pixW_ = pixW;
    pixH_ = pixH;
    // Keep termW_/termH_ as a coarse cell-grid view of the framebuffer
    // (cellPixW_/cellPixH_ are unchanged so aspectYX() still reports
    // square pixels for export).
    termW_ = std::max(1, pixW_ / std::max(1, cellPixW_));
    termH_ = std::max(1, pixH_ / std::max(1, cellPixH_));

    size_t nPix = static_cast<size_t>(pixW_) * pixH_;
    rgb_.resize(nPix * 3);
    colorIds_.resize(nPix);
    if (!atomIds_.empty()) atomIds_.resize(nPix);
    prevRgb_.clear();
    zbuf_.resize(pixW_, pixH_);
    clear();
}

void PixelCanvas::clear() {
    std::memset(rgb_.data(), 0, rgb_.size());
    std::memset(colorIds_.data(), 0xFF, colorIds_.size());
    if (!atomIds_.empty()) std::fill(atomIds_.begin(), atomIds_.end(), -1);
    zbuf_.clear();
    zMin_ = std::numeric_limits<float>::max();
    zMax_ = std::numeric_limits<float>::lowest();
}

void PixelCanvas::setActiveAtomIndex(int idx) {
    activeAtomIdx_ = idx;
    if (idx >= 0 && atomIds_.empty() && pixW_ > 0 && pixH_ > 0) {
        // Lazy alloc on first stamping caller
        atomIds_.assign(static_cast<size_t>(pixW_) * pixH_, -1);
    }
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
    size_t pIdx = static_cast<size_t>(sy) * pixW_ + sx;
    colorIds_[pIdx] = static_cast<int8_t>(colorPair);
    if (!atomIds_.empty()) atomIds_[pIdx] = activeAtomIdx_;
}

void PixelCanvas::drawLine(int x0, int y0, float d0,
                            int x1, int y1, float d1, int colorPair) {
    // Shaded line: interpolate depth along line, apply subtle intensity variation.
    // Closer segments are brighter, farther are dimmer (complements depth fog).
    auto base = colorPairToRGB(colorPair);

    int32_t* aid = atomIds_.empty() ? nullptr : atomIds_.data();
    int32_t aIdx = activeAtomIdx_;
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
            size_t pIdx = static_cast<size_t>(y) * pixW_ + x;
            colorIds_[pIdx] = static_cast<int8_t>(colorPair);
            if (aid) aid[pIdx] = aIdx;
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
            size_t pIdx = static_cast<size_t>(py) * pixW_ + px;
            colorIds_[pIdx] = static_cast<int8_t>(colorPair);
            if (!atomIds_.empty()) atomIds_[pIdx] = activeAtomIdx_;
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

    // Barycentric denominator (= twice the signed area). If the triangle is
    // CW we swap two vertices so the inside-test below can use a single
    // sign convention (u, v, w >= 0). Shading is two-sided so flipping the
    // implied normal direction is harmless.
    float denom = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (std::abs(denom) < 1e-6f) return;  // degenerate
    if (denom < 0.0f) {
        std::swap(x1, x2); std::swap(y1, y2); std::swap(z1, z2);
        denom = -denom;
    }
    float invDenom = 1.0f / denom;

    // Triangle normal for shading (flat per-triangle, computed after the
    // optional swap above so the magnitude is unaffected).
    float ax = x1 - x0, ay = y1 - y0, az = z1 - z0;
    float bx = x2 - x0, by = y2 - y0, bz = z2 - z0;
    float nx = ay * bz - az * by;
    float ny = az * bx - ax * bz;
    float nz = ax * by - ay * bx;
    float nLen = std::sqrt(nx*nx + ny*ny + nz*nz);
    if (nLen > 1e-6f) { nx /= nLen; ny /= nLen; nz /= nLen; }

    // Two-sided Lambert: light from upper-left-front, strong contrast
    float dot = -0.4f * nx - 0.4f * ny + 0.82f * nz;
    float halfLambert = std::abs(dot) * 0.5f + 0.5f;
    float intensity = 0.2f + 0.8f * halfLambert;

    auto base = colorPairToRGB(colorPair);
    uint8_t cr = static_cast<uint8_t>(std::min(255.0f, base.r * intensity));
    uint8_t cg = static_cast<uint8_t>(std::min(255.0f, base.g * intensity));
    uint8_t cb = static_cast<uint8_t>(std::min(255.0f, base.b * intensity));

    // Hoisted barycentric.  For every pixel inside the bounding box we
    // need
    //   u = ((y1 - y2)*(px - x2) + (x2 - x1)*(py - y2)) / denom
    //   v = ((y2 - y0)*(px - x2) + (x0 - x2)*(py - y2)) / denom
    //   w = 1 - u - v
    // Splitting each into x- and y-dependent halves lets the per-pixel
    // inner loop do 2 muls + 4 adds for u and v (vs. 8 muls + 4 adds
    // for the naive recomputation) on top of the depth interpolation.
    float u_x_step  = (y1 - y2) * invDenom;
    float v_x_step  = (y2 - y0) * invDenom;
    float u_y_coeff = (x2 - x1) * invDenom;
    float v_y_coeff = (x0 - x2) * invDenom;

    constexpr float kEdgeEps = -1e-4f;

    for (int py = minY; py <= maxY; ++py) {
        float fpy = static_cast<float>(py) + 0.5f;
        float dy  = fpy - y2;
        float u_y = u_y_coeff * dy;
        float v_y = v_y_coeff * dy;

        for (int px = minX; px <= maxX; ++px) {
            float fpx = static_cast<float>(px) + 0.5f;
            float dx  = fpx - x2;

            float u = u_x_step * dx + u_y;
            float v = v_x_step * dx + v_y;
            float w = 1.0f - u - v;

            if (u < kEdgeEps || v < kEdgeEps || w < kEdgeEps) continue;

            float depth = u * z0 + v * z1 + w * z2;

            if (!zbuf_.testAndSet(px, py, depth)) continue;
            if (depth < zMin_) zMin_ = depth;
            if (depth > zMax_) zMax_ = depth;

            setPixel(px, py, cr, cg, cb);
            size_t pIdx = static_cast<size_t>(py) * pixW_ + px;
            colorIds_[pIdx] = static_cast<int8_t>(colorPair);
            if (!atomIds_.empty()) atomIds_[pIdx] = activeAtomIdx_;
        }
    }
}

// Tile-binned, thread-parallel batch rasterizer.
//
// 1. Project (already in screen space) + flat-shade + AABB-clip every
//    triangle once. Triangles whose bounding box falls outside the
//    framebuffer are dropped here.
// 2. Bin each surviving triangle into the 64×64 tiles its bounding box
//    overlaps.
// 3. Walk tiles in parallel; tiles are disjoint so workers writing to
//    the shared rgb_ / zbuf_ / colorIds_ buffers never touch the same
//    pixel. Each tile clamps the per-triangle inner loop to its own
//    rectangle, so a triangle straddling several tiles is only
//    rasterized N times where N is the number of tiles it covers.
// 4. After workers finish, fold the per-tile zMin / zMax into the
//    canvas-wide min/max used by the depth-fog post-pass.
void PixelCanvas::drawTriangleBatch(const TriangleSpan* tris,
                                    std::size_t count) {
    if (count == 0 || pixW_ <= 0 || pixH_ <= 0) return;

    constexpr int TILE = 64;
    constexpr float kEdgeEps = -1e-4f;

    const int numTilesX = (pixW_ + TILE - 1) / TILE;
    const int numTilesY = (pixH_ + TILE - 1) / TILE;
    const int numTiles = numTilesX * numTilesY;

    struct ProjTri {
        float x[3], y[3], z[3];
        uint8_t cr, cg, cb;
        int8_t colorPair;
        int minX, maxX, minY, maxY;
    };

    std::vector<ProjTri> projected;
    projected.reserve(count);
    std::vector<std::vector<uint32_t>> bins(numTiles);

    for (std::size_t i = 0; i < count; ++i) {
        const auto& t = tris[i];

        // Force CCW winding so the inside-test in the inner loop can use
        // a single sign convention. The shader is two-sided, so flipping
        // an implicit normal direction has no visible effect.
        ProjTri p{};
        float denom = (t.y[1] - t.y[2]) * (t.x[0] - t.x[2])
                    + (t.x[2] - t.x[1]) * (t.y[0] - t.y[2]);
        if (std::abs(denom) < 1e-6f) continue;
        if (denom < 0.0f) {
            p.x[0] = t.x[0]; p.y[0] = t.y[0]; p.z[0] = t.z[0];
            p.x[1] = t.x[2]; p.y[1] = t.y[2]; p.z[1] = t.z[2];
            p.x[2] = t.x[1]; p.y[2] = t.y[1]; p.z[2] = t.z[1];
        } else {
            for (int k = 0; k < 3; ++k) {
                p.x[k] = t.x[k]; p.y[k] = t.y[k]; p.z[k] = t.z[k];
            }
        }

        // Flat-shade once (matches the per-triangle path's lighting).
        float ax = p.x[1] - p.x[0], ay = p.y[1] - p.y[0], az = p.z[1] - p.z[0];
        float bx = p.x[2] - p.x[0], by = p.y[2] - p.y[0], bz = p.z[2] - p.z[0];
        float nx = ay * bz - az * by;
        float ny = az * bx - ax * bz;
        float nz = ax * by - ay * bx;
        float nLen = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (nLen > 1e-6f) { nx /= nLen; ny /= nLen; nz /= nLen; }

        float dot = -0.4f * nx - 0.4f * ny + 0.82f * nz;
        float halfLambert = std::abs(dot) * 0.5f + 0.5f;
        float intensity = 0.2f + 0.8f * halfLambert;

        auto base = colorPairToRGB(t.colorPair);
        p.cr = static_cast<uint8_t>(std::min(255.0f, base.r * intensity));
        p.cg = static_cast<uint8_t>(std::min(255.0f, base.g * intensity));
        p.cb = static_cast<uint8_t>(std::min(255.0f, base.b * intensity));
        p.colorPair = static_cast<int8_t>(t.colorPair);

        int aabMinX = static_cast<int>(std::min({p.x[0], p.x[1], p.x[2]}));
        int aabMaxX = static_cast<int>(std::max({p.x[0], p.x[1], p.x[2]})) + 1;
        int aabMinY = static_cast<int>(std::min({p.y[0], p.y[1], p.y[2]}));
        int aabMaxY = static_cast<int>(std::max({p.y[0], p.y[1], p.y[2]})) + 1;

        p.minX = std::max(0, aabMinX);
        p.maxX = std::min(pixW_ - 1, aabMaxX);
        p.minY = std::max(0, aabMinY);
        p.maxY = std::min(pixH_ - 1, aabMaxY);
        if (p.minX > p.maxX || p.minY > p.maxY) continue;

        uint32_t triIdx = static_cast<uint32_t>(projected.size());
        projected.push_back(p);

        int t0x = p.minX / TILE;
        int t1x = p.maxX / TILE;
        int t0y = p.minY / TILE;
        int t1y = p.maxY / TILE;
        for (int ty = t0y; ty <= t1y; ++ty) {
            for (int tx = t0x; tx <= t1x; ++tx) {
                bins[ty * numTilesX + tx].push_back(triIdx);
            }
        }
    }

    if (projected.empty()) return;

    std::vector<float> tileZMin(numTiles, std::numeric_limits<float>::max());
    std::vector<float> tileZMax(numTiles, std::numeric_limits<float>::lowest());

    // All triangles in the batch share whatever atom index was set when
    // the call started (per-triangle atom IDs would require extending
    // TriangleSpan). Capture once so workers don't race on the field.
    int32_t* aidBuf = atomIds_.empty() ? nullptr : atomIds_.data();
    const int32_t batchAtomIdx = activeAtomIdx_;

    auto rasterizeTile = [&](int tileIdx) {
        const auto& bin = bins[tileIdx];
        if (bin.empty()) return;

        int tx = tileIdx % numTilesX;
        int ty = tileIdx / numTilesX;
        int xs = tx * TILE;
        int ys = ty * TILE;
        int xe = std::min(xs + TILE - 1, pixW_ - 1);
        int ye = std::min(ys + TILE - 1, pixH_ - 1);

        float lZMin = std::numeric_limits<float>::max();
        float lZMax = std::numeric_limits<float>::lowest();

        for (uint32_t idx : bin) {
            const auto& p = projected[idx];

            int minX = std::max(xs, p.minX);
            int maxX = std::min(xe, p.maxX);
            int minY = std::max(ys, p.minY);
            int maxY = std::min(ye, p.maxY);
            if (minX > maxX || minY > maxY) continue;

            float denom = (p.y[1] - p.y[2]) * (p.x[0] - p.x[2])
                        + (p.x[2] - p.x[1]) * (p.y[0] - p.y[2]);
            if (std::abs(denom) < 1e-6f) continue;
            float invDenom = 1.0f / denom;

            float u_x_step  = (p.y[1] - p.y[2]) * invDenom;
            float v_x_step  = (p.y[2] - p.y[0]) * invDenom;
            float u_y_coeff = (p.x[2] - p.x[1]) * invDenom;
            float v_y_coeff = (p.x[0] - p.x[2]) * invDenom;

            for (int py = minY; py <= maxY; ++py) {
                float fpy = static_cast<float>(py) + 0.5f;
                float dy  = fpy - p.y[2];
                float u_y = u_y_coeff * dy;
                float v_y = v_y_coeff * dy;

                for (int px = minX; px <= maxX; ++px) {
                    float fpx = static_cast<float>(px) + 0.5f;
                    float dx  = fpx - p.x[2];

                    float u = u_x_step * dx + u_y;
                    float v = v_x_step * dx + v_y;
                    float w = 1.0f - u - v;
                    if (u < kEdgeEps || v < kEdgeEps || w < kEdgeEps) continue;

                    float depth = u * p.z[0] + v * p.z[1] + w * p.z[2];
                    if (!zbuf_.testAndSet(px, py, depth)) continue;
                    if (depth < lZMin) lZMin = depth;
                    if (depth > lZMax) lZMax = depth;

                    setPixel(px, py, p.cr, p.cg, p.cb);
                    size_t pIdx = static_cast<size_t>(py) * pixW_ + px;
                    colorIds_[pIdx] = p.colorPair;
                    if (aidBuf) aidBuf[pIdx] = batchAtomIdx;
                }
            }
        }

        tileZMin[tileIdx] = lZMin;
        tileZMax[tileIdx] = lZMax;
    };

    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 1;
    int numThreads = static_cast<int>(std::min(hw, 8u));

    // Below this many tiles or triangles, threading overhead dominates.
    bool serial = (numThreads < 2 || numTiles < 4 || projected.size() < 200);

    if (serial) {
        for (int t = 0; t < numTiles; ++t) rasterizeTile(t);
    } else {
        std::atomic<int> nextTile{0};
        auto worker = [&]() {
            while (true) {
                int t = nextTile.fetch_add(1, std::memory_order_relaxed);
                if (t >= numTiles) break;
                rasterizeTile(t);
            }
        };
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        for (int i = 0; i < numThreads; ++i) threads.emplace_back(worker);
        for (auto& th : threads) th.join();
    }

    for (int i = 0; i < numTiles; ++i) {
        if (tileZMin[i] < zMin_) zMin_ = tileZMin[i];
        if (tileZMax[i] > zMax_) zMax_ = tileZMax[i];
    }
}

// ── Font rasterization ──────────────────────────────────────────────────────

struct PixelCanvas::FontState {
    stbtt_fontinfo info{};
    bool ready = false;
};

void PixelCanvas::initFont() {
    if (font_ && font_->ready) return;
    font_ = std::make_unique<FontState>();
    if (stbtt_InitFont(&font_->info, kEmbeddedFont,
                       stbtt_GetFontOffsetForIndex(kEmbeddedFont, 0))) {
        font_->ready = true;
    }
}

void PixelCanvas::drawChar(int termX, int termY, float depth,
                           char ch, int colorPair) {
    // Render a single character at terminal cell coordinates
    int sx = termX * cellPixW_;
    int sy = termY * cellPixH_;
    char buf[2] = {ch, '\0'};
    drawText(sx, sy, depth, buf, colorPair);
}

void PixelCanvas::drawText(int sx, int sy, float /* depth */,
                           const std::string& text, int colorPair) {
    initFont();
    if (!font_ || !font_->ready) return;
    if (text.empty()) return;

    auto c = colorPairToRGB(colorPair);

    // Scale font to match cell height (use ~60% of cell for ascender)
    float fontHeight = static_cast<float>(cellPixH_) * 0.7f;
    float scale = stbtt_ScaleForPixelHeight(&font_->info, fontHeight);

    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&font_->info, &ascent, &descent, &lineGap);
    int baseline = sy + static_cast<int>(static_cast<float>(ascent) * scale);

    float xpos = static_cast<float>(sx);

    for (size_t i = 0; i < text.size(); ++i) {
        int ch = static_cast<unsigned char>(text[i]);

        int advance = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&font_->info, ch, &advance, &lsb);

        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointBitmapBox(&font_->info, ch, scale, scale, &x0, &y0, &x1, &y1);

        int gw = x1 - x0, gh = y1 - y0;
        if (gw <= 0 || gh <= 0) {
            xpos += static_cast<float>(advance) * scale;
            continue;
        }

        // Rasterize glyph to temp bitmap
        std::vector<unsigned char> bitmap(gw * gh);
        stbtt_MakeCodepointBitmap(&font_->info, bitmap.data(), gw, gh, gw,
                                   scale, scale, ch);

        // Blit to framebuffer with alpha blending
        int gx = static_cast<int>(xpos) + x0;
        int gy = baseline + y0;
        for (int row = 0; row < gh; ++row) {
            for (int col = 0; col < gw; ++col) {
                int px = gx + col;
                int py = gy + row;
                if (!inBounds(px, py)) continue;

                unsigned char alpha = bitmap[row * gw + col];
                if (alpha == 0) continue;

                size_t idx = (static_cast<size_t>(py) * pixW_ + px) * 3;
                if (alpha >= 240) {
                    // Opaque — write directly
                    rgb_[idx]     = c.r;
                    rgb_[idx + 1] = c.g;
                    rgb_[idx + 2] = c.b;
                } else {
                    // Alpha blend
                    float a = static_cast<float>(alpha) / 255.0f;
                    float inv = 1.0f - a;
                    rgb_[idx]     = static_cast<uint8_t>(c.r * a + rgb_[idx]     * inv);
                    rgb_[idx + 1] = static_cast<uint8_t>(c.g * a + rgb_[idx + 1] * inv);
                    rgb_[idx + 2] = static_cast<uint8_t>(c.b * a + rgb_[idx + 2] * inv);
                }
            }
        }

        xpos += static_cast<float>(advance) * scale;

        // Kerning
        if (i + 1 < text.size()) {
            int kern = stbtt_GetCodepointKernAdvance(&font_->info, ch,
                static_cast<unsigned char>(text[i + 1]));
            xpos += static_cast<float>(kern) * scale;
        }
    }
}

// ── Post-processing ─────────────────────────────────────────────────────────

void PixelCanvas::applyOutline(float threshold, float darken) {
    if (pixW_ <= 0 || pixH_ <= 0) return;
    if (zMax_ <= zMin_) return;

    float range = zMax_ - zMin_;
    // Two thresholds: strong edges (silhouette) and subtle edges (nearby overlap)
    float threshStrong = threshold * range;
    float threshSubtle = threshold * range * 0.15f;  // much more sensitive
    constexpr int kRadius = 2;

    // Detect edge pixels: 2 = strong (silhouette/bg boundary), 1 = subtle (nearby overlap)
    std::vector<uint8_t> edge(static_cast<size_t>(pixW_) * pixH_, 0);

    for (int y = 1; y < pixH_ - 1; ++y) {
        for (int x = 1; x < pixW_ - 1; ++x) {
            size_t idx = static_cast<size_t>(y) * pixW_ + x;
            if (colorIds_[idx] < 0) continue;

            float z = zbuf_.get(x, y);
            uint8_t level = 0;

            auto check = [&](int nx, int ny) {
                size_t ni = static_cast<size_t>(ny) * pixW_ + nx;
                if (colorIds_[ni] < 0) { level = 2; return; }  // bg boundary = strong
                float dz = std::abs(z - zbuf_.get(nx, ny));
                if (dz > threshStrong) level = std::max(level, uint8_t(2));
                else if (dz > threshSubtle) level = std::max(level, uint8_t(1));
            };

            check(x-1, y); check(x+1, y); check(x, y-1); check(x, y+1);
            edge[idx] = level;
        }
    }

    // Dilate and darken
    for (int y = 0; y < pixH_; ++y) {
        for (int x = 0; x < pixW_; ++x) {
            size_t idx = static_cast<size_t>(y) * pixW_ + x;
            if (colorIds_[idx] < 0) continue;

            uint8_t maxLevel = 0;
            bool onEdge = (edge[idx] > 0);
            for (int dy = -kRadius; dy <= kRadius && maxLevel < 2; ++dy) {
                for (int dx = -kRadius; dx <= kRadius && maxLevel < 2; ++dx) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || nx >= pixW_ || ny < 0 || ny >= pixH_) continue;
                    if (dx*dx + dy*dy > kRadius*kRadius) continue;
                    uint8_t e = edge[static_cast<size_t>(ny) * pixW_ + nx];
                    if (e > maxLevel) maxLevel = e;
                }
            }

            if (maxLevel > 0) {
                size_t pi = idx * 3;
                float d;
                if (onEdge && maxLevel == 2)
                    d = darken;                          // strong edge center: darkest
                else if (maxLevel == 2)
                    d = darken + (1.0f - darken) * 0.4f; // strong edge fringe
                else if (onEdge)
                    d = darken + (1.0f - darken) * 0.55f; // subtle edge center
                else
                    d = darken + (1.0f - darken) * 0.7f;  // subtle edge fringe
                rgb_[pi]     = static_cast<uint8_t>(rgb_[pi]     * d);
                rgb_[pi + 1] = static_cast<uint8_t>(rgb_[pi + 1] * d);
                rgb_[pi + 2] = static_cast<uint8_t>(rgb_[pi + 2] * d);
            }
        }
    }
}

void PixelCanvas::applyFocusDim(const std::vector<bool>& keepBright, float strength) {
    if (atomIds_.empty()) return;            // no consumer ever stamped
    if (strength <= 0.0f) return;
    if (pixW_ <= 0 || pixH_ <= 0) return;

    const float s     = std::min(1.0f, strength);
    const float darkF = 1.0f - 0.4f * s;     // overall darken factor
    const int   maxId = static_cast<int>(keepBright.size());

    size_t nPix = static_cast<size_t>(pixW_) * pixH_;
    for (size_t i = 0; i < nPix; ++i) {
        int id = atomIds_[i];
        if (id < 0) continue;                // untagged (background, text)
        if (id < maxId && keepBright[id]) continue;   // focus subject — leave vivid

        size_t pi = i * 3;
        // BT.601 luma → desaturate toward gray, then darken.
        float r = rgb_[pi], g = rgb_[pi + 1], b = rgb_[pi + 2];
        float y = 0.299f * r + 0.587f * g + 0.114f * b;
        r = (r + (y - r) * s) * darkF;
        g = (g + (y - g) * s) * darkF;
        b = (b + (y - b) * s) * darkF;
        rgb_[pi]     = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, r)));
        rgb_[pi + 1] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, g)));
        rgb_[pi + 2] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, b)));
    }
}

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

bool PixelCanvas::savePNG(const std::string& path, int dpi) const {
    if (pixW_ <= 0 || pixH_ <= 0) return false;

    // Always export the live framebuffer so post-passes that run AFTER
    // fog (focus-dim, interface overlay) appear in the PNG. The earlier
    // "preFogRgb_ for brighter PNG" path silently dropped those layers.
    const std::vector<uint8_t>& src = rgb_;

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

    // pHYs: physical pixel dimensions. Tells LaTeX / Word / browsers the
    // intended print size; pixel count is unchanged. 1 inch = 0.0254 m,
    // so pixels-per-meter ≈ dpi × 39.3701.
    if (dpi > 0) {
        std::vector<uint8_t> phys;
        uint32_t ppm = static_cast<uint32_t>(
            static_cast<double>(dpi) / 0.0254 + 0.5);
        pngPut32(phys, ppm);
        pngPut32(phys, ppm);
        phys.push_back(1);  // unit: meter
        pngWriteChunk(f, "pHYs", phys.data(), phys.size());
    }

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
        case kColorChainA:      return {34, 200, 34};    // green
        case kColorChainB:      return {80, 220, 220};   // cyan
        case kColorChainC:      return {200, 50, 200};   // magenta
        case kColorChainD:      return {255, 220, 30};   // yellow
        case kColorChainE:      return {255, 40, 40};    // red
        case kColorChainF:      return {50, 80, 255};    // blue
        case kColorChainG:      return {255, 140, 30};   // orange
        case kColorChainH:      return {120, 240, 30};   // lime
        case kColorChainI:      return {30, 180, 180};   // teal
        case kColorChainJ:      return {150, 50, 220};   // purple
        case kColorChainK:      return {255, 130, 200};  // pink
        case kColorChainL:      return {130, 160, 200};  // slate
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
        case kColorHeatmap0:    return {20, 30, 120};     // dark blue
        case kColorHeatmap1:    return {80, 150, 230};    // light blue
        case kColorHeatmap2:    return {210, 210, 210};   // light gray
        case kColorHeatmap3:    return {255, 165, 0};     // orange
        case kColorHeatmap4:    return {220, 30, 30};     // red
        default:                return {200, 200, 200};
    }
}

} // namespace molterm
