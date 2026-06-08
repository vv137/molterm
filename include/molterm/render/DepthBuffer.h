#pragma once

#include <vector>
#include <limits>

namespace molterm {

// Painter's depth buffer. Convention: SMALLER depth = nearer the viewer
// (testAndSet keeps the min). Camera projection feeds into-screen depth
// (= -rz, see Camera::project); depth fog (PixelCanvas::applyDepthFog) and
// sphere self-occlusion (PixelCanvas::drawCircle zOff) rely on this sign.
class DepthBuffer {
public:
    DepthBuffer() = default;

    void resize(int w, int h) {
        width_ = w;
        height_ = h;
        buffer_.assign(static_cast<size_t>(w) * h,
                       std::numeric_limits<float>::max());
    }

    void clear() {
        std::fill(buffer_.begin(), buffer_.end(),
                  std::numeric_limits<float>::max());
    }

    bool testAndSet(int x, int y, float depth) {
        if (x < 0 || x >= width_ || y < 0 || y >= height_) return false;
        auto idx = static_cast<size_t>(y) * width_ + x;
        if (depth < buffer_[idx]) {
            buffer_[idx] = depth;
            return true;
        }
        return false;
    }

    // Read-only depth test (no write). Used by alpha-blend draws so a
    // transparent atom doesn't occlude content drawn later.
    bool test(int x, int y, float depth) const {
        if (x < 0 || x >= width_ || y < 0 || y >= height_) return false;
        auto idx = static_cast<size_t>(y) * width_ + x;
        return depth < buffer_[idx];
    }

    float get(int x, int y) const {
        if (x < 0 || x >= width_ || y < 0 || y >= height_)
            return std::numeric_limits<float>::max();
        return buffer_[static_cast<size_t>(y) * width_ + x];
    }

    int width() const { return width_; }
    int height() const { return height_; }

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<float> buffer_;
};

} // namespace molterm
