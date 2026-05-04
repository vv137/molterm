#pragma once

#include "molterm/render/Canvas.h"
#include "molterm/render/DepthBuffer.h"
#include "molterm/render/GraphicsEncoder.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace molterm {

// Pixel-based canvas that renders into an RGB framebuffer,
// then encodes via a pluggable GraphicsEncoder (Sixel/Kitty/iTerm2).
class PixelCanvas : public Canvas {
public:
    explicit PixelCanvas(std::unique_ptr<GraphicsEncoder> encoder);
    ~PixelCanvas() override;

    void resize(int termW, int termH) override;
    void clear() override;
    void flush(Window& win) override;
    void invalidate() override { prevRgb_.clear(); }

    int subW() const override { return pixW_; }
    int subH() const override { return pixH_; }
    int scaleX() const override { return cellPixW_; }
    int scaleY() const override { return cellPixH_; }

    void drawDot(int sx, int sy, float depth, int colorPair) override;
    void drawLine(int x0, int y0, float d0,
                  int x1, int y1, float d1, int colorPair) override;
    void drawCircle(int cx, int cy, float depth,
                    int radius, int colorPair, bool filled) override;

    void drawTriangle(float x0, float y0, float z0,
                      float x1, float y1, float z1,
                      float x2, float y2, float z2,
                      int colorPair) override;
    void drawTriangleBatch(const TriangleSpan* tris,
                           std::size_t count) override;
    void drawChar(int termX, int termY, float depth,
                  char ch, int colorPair) override;

    // Render a text string at sub-pixel coordinates with depth testing.
    void drawText(int sx, int sy, float depth,
                  const std::string& text, int colorPair);

    // Access framebuffer (for post-processing like depth fog)
    uint8_t* rgbData() { return rgb_.data(); }
    const uint8_t* rgbData() const { return rgb_.data(); }
    int pixelWidth() const { return pixW_; }
    int pixelHeight() const { return pixH_; }

    // Depth buffer access (for post-processing)
    const DepthBuffer& depthBuffer() const { return zbuf_; }

    // Apply silhouette outline: darken pixels at depth discontinuities
    void applyOutline(float threshold = 0.3f, float darken = 0.3f);

    // Apply depth fog: blend pixels toward fogColor based on depth.
    void applyDepthFog(float strength = 0.35f,
                       uint8_t fogR = 30, uint8_t fogG = 35, uint8_t fogB = 50);

    // Save current framebuffer as PNG (captures pre-fog on next frame).
    // If dpi > 0, embed a pHYs chunk so LaTeX / Word / image viewers know
    // the intended physical size. Pixel count is unchanged; this is
    // metadata only.
    bool savePNG(const std::string& path, int dpi = 0) const;

    // Swap encoder at runtime
    void setEncoder(std::unique_ptr<GraphicsEncoder> enc);
    const GraphicsEncoder* encoder() const { return encoder_.get(); }

private:
    std::unique_ptr<GraphicsEncoder> encoder_;

    int termW_ = 0, termH_ = 0;
    int pixW_ = 0, pixH_ = 0;
    int cellPixW_ = 10, cellPixH_ = 20;

    // RGB framebuffer (3 bytes per pixel, row-major)
    std::vector<uint8_t> rgb_;
    // Per-pixel color pair ID for dirty detection (-1 = bg)
    std::vector<int8_t> colorIds_;
    // Previous frame for diff
    std::vector<uint8_t> prevRgb_;

    DepthBuffer zbuf_;
    float zMin_ = 0, zMax_ = 0;
    std::vector<uint8_t> preFogRgb_;  // snapshot before fog for PNG export

    void queryCellSize();
    void initFont();

    // stb_truetype font state (lazy-initialized on first drawText call)
    struct FontState;
    std::unique_ptr<FontState> font_;

    bool inBounds(int sx, int sy) const {
        return sx >= 0 && sx < pixW_ && sy >= 0 && sy < pixH_;
    }

    // Set a pixel in the RGB buffer
    void setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b);

    // Color pair → RGB conversion
    struct RGB { uint8_t r, g, b; };
    static RGB colorPairToRGB(int colorPair);
};

} // namespace molterm
