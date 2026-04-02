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
}

void Camera::rotateY(float degrees) {
    multiplyRotY(degrees * 3.14159265f / 180.0f);
}

void Camera::pan(float dx, float dy) {
    panX_ += dx;
    panY_ += dy;
}

void Camera::zoomBy(float factor) {
    zoom_ *= factor;
    if (zoom_ < 0.01f) zoom_ = 0.01f;
    if (zoom_ > 100.0f) zoom_ = 100.0f;
}

void Camera::reset() {
    rot_.fill(0.0f);
    rot_[0] = rot_[4] = rot_[8] = 1.0f;
    panX_ = panY_ = 0.0f;
    zoom_ = 1.0f;
}

void Camera::setCenter(float cx, float cy, float cz) {
    centerX_ = cx; centerY_ = cy; centerZ_ = cz;
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

    sx = static_cast<int>(rx * scale + panX_) + screenW / 2;
    // Terminal cells are ~2:1 tall:wide, so halve Y for ASCII (1:1 sub-pixel)
    sy = static_cast<int>(-ry * scale / 2.0f + panY_) + screenH / 2;
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

    sx = rx * scale + panX_ + static_cast<float>(screenW) / 2.0f;
    // Correct for non-square sub-pixels: divide by aspectYX
    // If sub-pixels are tall (aspectYX > 1), compress Y; if square (1.0), no correction
    sy = -ry * scale / aspectYX + panY_ + static_cast<float>(screenH) / 2.0f;
    depth = rz;

    return true;
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

} // namespace molterm
