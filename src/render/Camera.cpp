#include "molterm/render/Camera.h"
#include <cstring>

namespace molterm {

Camera::Camera()
    : centerX_(0), centerY_(0), centerZ_(0)
    , panX_(0), panY_(0)
    , zoom_(1.0f)
    , rotSpeed_(3.0f) {
    // Identity matrix
    rot_.fill(0.0f);
    rot_[0] = rot_[4] = rot_[8] = 1.0f;
}

void Camera::rotateX(float degrees) {
    multiplyRotX(degrees * 3.14159265f / 180.0f);
    markDirty();
}

void Camera::rotateY(float degrees) {
    multiplyRotY(degrees * 3.14159265f / 180.0f);
    markDirty();
}

void Camera::rotateZ(float degrees) {
    multiplyRotZ(degrees * 3.14159265f / 180.0f);
    markDirty();
}

void Camera::pan(float dx, float dy) {
    // Pan in Angstrom units (independent of canvas resolution)
    panX_ += dx;
    panY_ += dy;
    markDirty();
}

void Camera::zoomBy(float factor) {
    zoom_ *= factor;
    if (zoom_ < 0.01f) zoom_ = 0.01f;
    if (zoom_ > 100.0f) zoom_ = 100.0f;
    markDirty();
}

void Camera::reset() {
    rot_.fill(0.0f);
    rot_[0] = rot_[4] = rot_[8] = 1.0f;
    panX_ = panY_ = 0.0f;
    zoom_ = 1.0f;
    markDirty();
}

void Camera::setCenter(float cx, float cy, float cz) {
    centerX_ = cx; centerY_ = cy; centerZ_ = cz;
    markDirty();
}

bool Camera::project(float wx, float wy, float wz,
                     int screenW, int screenH,
                     int& sx, int& sy, float& depth) const {
    // Translate to center
    float x = wx - centerX_;
    float y = wy - centerY_;
    float z = wz - centerZ_;

    // Apply rotation
    float rx = rot_[0]*x + rot_[1]*y + rot_[2]*z;
    float ry = rot_[3]*x + rot_[4]*y + rot_[5]*z;
    float rz = rot_[6]*x + rot_[7]*y + rot_[8]*z;

    // Orthographic projection with zoom
    // Scale: 1 Angstrom = zoom_ * (screenH/50) pixels roughly
    float scale = zoom_ * static_cast<float>(std::min(screenW, screenH)) / 50.0f;

    sx = static_cast<int>((rx + panX_) * scale) + screenW / 2;
    sy = static_cast<int>(-(ry + panY_) * scale / 2.0f) + screenH / 2;
    depth = rz;

    return true;
}

bool Camera::projectf(float wx, float wy, float wz,
                      int screenW, int screenH,
                      float& sx, float& sy, float& depth,
                      float aspectYX) const {
    float x = wx - centerX_;
    float y = wy - centerY_;
    float z = wz - centerZ_;

    float rx = rot_[0]*x + rot_[1]*y + rot_[2]*z;
    float ry = rot_[3]*x + rot_[4]*y + rot_[5]*z;
    float rz = rot_[6]*x + rot_[7]*y + rot_[8]*z;

    float scale = zoom_ * static_cast<float>(std::min(screenW, screenH)) / 50.0f;

    // Pan is in Angstrom units — multiply by scale to get screen offset
    sx = (rx + panX_) * scale + static_cast<float>(screenW) / 2.0f;
    sy = -(ry + panY_) * scale / aspectYX + static_cast<float>(screenH) / 2.0f;
    depth = rz;

    return true;
}

void Camera::prepareProjection(int screenW, int screenH, float aspectYX) const {
    projScale_ = zoom_ * static_cast<float>(std::min(screenW, screenH)) / 50.0f;
    projScaleY_ = projScale_ / aspectYX;
    projOffX_ = static_cast<float>(screenW) / 2.0f;
    projOffY_ = static_cast<float>(screenH) / 2.0f;
}

void Camera::projectCached(float wx, float wy, float wz,
                            float& sx, float& sy, float& depth) const {
    float x = wx - centerX_;
    float y = wy - centerY_;
    float z = wz - centerZ_;
    float rx = rot_[0]*x + rot_[1]*y + rot_[2]*z;
    float ry = rot_[3]*x + rot_[4]*y + rot_[5]*z;
    sx = (rx + panX_) * projScale_ + projOffX_;
    sy = -(ry + panY_) * projScaleY_ + projOffY_;
    depth = rot_[6]*x + rot_[7]*y + rot_[8]*z;
}

void Camera::multiplyRotX(float rad) {
    float c = std::cos(rad), s = std::sin(rad);
    // Rotation matrix around X: [[1,0,0],[0,c,-s],[0,s,c]]
    // Multiply: new = Rx * old
    std::array<float, 9> m;
    m[0] = rot_[0];         m[1] = rot_[1];         m[2] = rot_[2];
    m[3] = c*rot_[3] - s*rot_[6]; m[4] = c*rot_[4] - s*rot_[7]; m[5] = c*rot_[5] - s*rot_[8];
    m[6] = s*rot_[3] + c*rot_[6]; m[7] = s*rot_[4] + c*rot_[7]; m[8] = s*rot_[5] + c*rot_[8];
    rot_ = m;
}

void Camera::multiplyRotY(float rad) {
    float c = std::cos(rad), s = std::sin(rad);
    std::array<float, 9> m;
    m[0] = c*rot_[0] + s*rot_[6]; m[1] = c*rot_[1] + s*rot_[7]; m[2] = c*rot_[2] + s*rot_[8];
    m[3] = rot_[3];               m[4] = rot_[4];               m[5] = rot_[5];
    m[6] = -s*rot_[0] + c*rot_[6]; m[7] = -s*rot_[1] + c*rot_[7]; m[8] = -s*rot_[2] + c*rot_[8];
    rot_ = m;
}

void Camera::multiplyRotZ(float rad) {
    float c = std::cos(rad), s = std::sin(rad);
    // Rotation matrix around Z: [[c,-s,0],[s,c,0],[0,0,1]]
    std::array<float, 9> m;
    m[0] = c*rot_[0] - s*rot_[3]; m[1] = c*rot_[1] - s*rot_[4]; m[2] = c*rot_[2] - s*rot_[5];
    m[3] = s*rot_[0] + c*rot_[3]; m[4] = s*rot_[1] + c*rot_[4]; m[5] = s*rot_[2] + c*rot_[5];
    m[6] = rot_[6];               m[7] = rot_[7];               m[8] = rot_[8];
    rot_ = m;
}

} // namespace molterm
