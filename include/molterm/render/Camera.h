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

    // Override the projection X origin after prepareProjection(). Used by
    // stereoscopic rendering to place each eye's view in its half of the
    // canvas without changing the projection scale.
    void setProjOffsetX(float ox) const { projOffX_ = ox; }
    float projOffsetX() const { return projOffX_; }

    // Access internal state (for export)
    const std::array<float, 9>& rotation() const { return rot_; }
    float centerX() const { return centerX_; }
    float centerY() const { return centerY_; }
    float centerZ() const { return centerZ_; }
    float panXOffset() const { return panX_; }
    float panYOffset() const { return panY_; }

    float zoom() const { return zoom_; }
    void setZoom(float z) { zoom_ = z; markDirty(); }
    // Sub-pixels per Å under the current viewport — i.e. the same factor
    // projectCached() applies to world coordinates. Reprs that draw shapes
    // sized in Å (vdW spheres, cartoon ribbon half-widths) should multiply
    // by this so they track positions consistently across canvas sizes.
    // Stale until prepareProjection() runs for the frame.
    float projScale() const { return projScale_; }
    void setRotation(const std::array<float, 9>& r) { rot_ = r; markDirty(); }
    void setPan(float x, float y) { panX_ = x; panY_ = y; markDirty(); }
    float rotationSpeed() const { return rotSpeed_; }
    void setRotationSpeed(float s) { rotSpeed_ = s; }
    float panSpeed() const { return panSpeed_; }
    void setPanSpeed(float s) { panSpeed_ = s; }

    // Snap the camera to focus on (cx,cy,cz) at `targetZoom`. Resets pan
    // so the focus subject sits at screen center under the current
    // rotation. Used by Focus Selection (Mol*-style click-to-focus).
    void focusOn(float cx, float cy, float cz, float targetZoom) {
        centerX_ = cx;
        centerY_ = cy;
        centerZ_ = cz;
        panX_ = 0.0f;
        panY_ = 0.0f;
        zoom_ = targetZoom;
        markDirty();
    }

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
    float panSpeed_ = 1.0f;  // Angstroms per keypress
    bool dirty_ = true;

    void multiplyRotX(float rad);
    void multiplyRotY(float rad);
    void multiplyRotZ(float rad);

    // Cached projection constants (set by prepareProjection)
    mutable float projScale_ = 0, projScaleY_ = 0;
    mutable float projOffX_ = 0, projOffY_ = 0;
};

} // namespace molterm
