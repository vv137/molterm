#pragma once

#include <cstddef>
#include <functional>

#include "molterm/render/DepthBuffer.h"
#include "molterm/tui/Window.h"

namespace molterm {

// Bundled triangle for batched submission. Storing the three vertices
// inline lets the rasterizer bin and dispatch them to tiles without
// touching the originating data structure.
struct TriangleSpan {
    float x[3];
    float y[3];
    float z[3];
    int colorPair;
};

// Abstract canvas for sub-pixel rendering.
// All coordinates are in sub-pixel space.
// Implementations map sub-pixels to terminal cells differently:
//   Ascii:   1×1 sub-pixels per cell
//   Braille: 2×4 sub-pixels per cell
//   Block:   1×2 sub-pixels per cell
class Canvas {
public:
    virtual ~Canvas() = default;

    virtual void resize(int termW, int termH) = 0;
    virtual void clear() = 0;
    virtual void flush(Window& win) = 0;
    virtual void invalidate() {}  // force full flush on next frame

    // Sub-pixel resolution
    virtual int subW() const = 0;  // total sub-pixel width
    virtual int subH() const = 0;  // total sub-pixel height
    virtual int scaleX() const = 0; // sub-pixels per cell horizontally
    virtual int scaleY() const = 0; // sub-pixels per cell vertically

    // Y/X aspect ratio of a single sub-pixel (height/width in real screen space)
    // Terminal cells are ~2:1 (height:width).
    //   ASCII (1×1):   sub-pixel = full cell → aspect = 2.0
    //   Braille (2×4): sub-pixel = cellW/2 × cellH/4 → aspect = (cellH/4)/(cellW/2) = 2*cellH/(4*cellW) ≈ 1.0
    //   Block (1×2):   sub-pixel = cellW × cellH/2 → aspect = (cellH/2)/cellW ≈ 1.0
    virtual float aspectYX() const {
        // Default: assume terminal cell aspect ~2:1
        float cellAspect = 2.0f;  // cellH / cellW
        return cellAspect * static_cast<float>(scaleX()) / static_cast<float>(scaleY());
    }

    // Drawing primitives (sub-pixel coords)
    virtual void drawDot(int sx, int sy, float depth, int colorPair) = 0;
    virtual void drawLine(int x0, int y0, float d0,
                          int x1, int y1, float d1,
                          int colorPair);
    virtual void drawCircle(int cx, int cy, float depth,
                            int radius, int colorPair, bool filled);

    // Z-buffered triangle rasterization (sub-pixel coords, barycentric scanline)
    virtual void drawTriangle(float x0, float y0, float z0,
                              float x1, float y1, float z1,
                              float x2, float y2, float z2,
                              int colorPair);

    // Batched submission of many triangles in one call. Default
    // implementation forwards to drawTriangle in a loop; PixelCanvas
    // overrides this to bin triangles into tiles and rasterize the
    // tiles in parallel. Caller must keep `tris` alive until the
    // function returns.
    virtual void drawTriangleBatch(const TriangleSpan* tris, std::size_t count);

    // Convenience: draw char at terminal cell coords (for labels)
    virtual void drawChar(int termX, int termY, float depth,
                          char ch, int colorPair) = 0;

    // Bresenham line helper (public so representations can use for thick lines)
    static void bresenham(int x0, int y0, float d0,
                          int x1, int y1, float d1,
                          const std::function<void(int, int, float)>& plot);
};

} // namespace molterm
