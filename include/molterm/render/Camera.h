#pragma once

#include <array>
#include <cmath>

namespace molterm {

class Camera {
public:
    Camera();

    // View manipulation
    void rotateX(float degrees);
    void rotateY(float degrees);
    void pan(float dx, float dy);
    void zoomBy(float factor);
    void reset();

    // Set rotation center (e.g., center of mass)
    void setCenter(float cx, float cy, float cz);

    // Project 3D world coordinate to 2D screen coordinate (int output)
    bool project(float wx, float wy, float wz,
                 int screenW, int screenH,
                 int& sx, int& sy, float& depth) const;

    // Project to float sub-pixel coordinates (for high-res canvas)
    // aspectYX: Y/X sub-pixel aspect ratio (sub-pixel height / sub-pixel width)
    //   ASCII:   cellH/cellW ≈ 2.0  (1×1 sub-pixels, tall cells)
    //   Braille: (cellH/4)/(cellW/2) ≈ 1.0  (2×4 sub-pixels, roughly square)
    //   Block:   (cellH/2)/(cellW) ≈ 1.0  (1×2 sub-pixels)
    bool projectf(float wx, float wy, float wz,
                  int screenW, int screenH,
                  float& sx, float& sy, float& depth,
                  float aspectYX = 1.0f) const;

    float zoom() const { return zoom_; }
    void setZoom(float z) { zoom_ = z; }
    float rotationSpeed() const { return rotSpeed_; }
    void setRotationSpeed(float s) { rotSpeed_ = s; }

private:
    // 3x3 rotation matrix (row-major)
    std::array<float, 9> rot_;
    float centerX_, centerY_, centerZ_;
    float panX_, panY_;
    float zoom_;
    float rotSpeed_;

    void multiplyRotX(float rad);
    void multiplyRotY(float rad);
};

} // namespace molterm
