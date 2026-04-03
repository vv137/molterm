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
    void rotateZ(float degrees);
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

    // Prepare cached projection constants for a given viewport.
    // Call once per frame, then use projectCached() per vertex.
    void prepareProjection(int screenW, int screenH, float aspectYX = 1.0f) const;
    void projectCached(float wx, float wy, float wz,
                       float& sx, float& sy, float& depth) const;

    // Access internal state (for export)
    const std::array<float, 9>& rotation() const { return rot_; }
    float centerX() const { return centerX_; }
    float centerY() const { return centerY_; }
    float centerZ() const { return centerZ_; }
    float panXOffset() const { return panX_; }
    float panYOffset() const { return panY_; }

    float zoom() const { return zoom_; }
    void setZoom(float z) { zoom_ = z; }
    float rotationSpeed() const { return rotSpeed_; }
    void setRotationSpeed(float s) { rotSpeed_ = s; }
    float panSpeed() const { return panSpeed_; }
    void setPanSpeed(float s) { panSpeed_ = s; }

    // Dirty tracking — check and clear in one call
    bool isDirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

private:
    void markDirty() { dirty_ = true; }
    // 3x3 rotation matrix (row-major)
    std::array<float, 9> rot_;
    float centerX_, centerY_, centerZ_;
    float panX_, panY_;
    float zoom_;
    float rotSpeed_;
    float panSpeed_ = 5.0f;
    bool dirty_ = true;

    void multiplyRotX(float rad);
    void multiplyRotY(float rad);
    void multiplyRotZ(float rad);

    // Cached projection constants (set by prepareProjection)
    mutable float projScale_ = 0, projScaleY_ = 0;
    mutable float projOffX_ = 0, projOffY_ = 0;
};

} // namespace molterm
