#include "molterm/app/Tab.h"

#include <limits>

namespace molterm {

Tab::Tab(std::string name) : name_(std::move(name)) {}

void Tab::addObject(std::shared_ptr<MolObject> obj) {
    objects_.push_back(std::move(obj));
    if (selectedIdx_ < 0) selectedIdx_ = 0;
}

void Tab::removeObject(int idx) {
    if (idx < 0 || idx >= static_cast<int>(objects_.size())) return;
    objects_.erase(objects_.begin() + idx);
    if (objects_.empty()) {
        selectedIdx_ = -1;
    } else if (selectedIdx_ >= static_cast<int>(objects_.size())) {
        selectedIdx_ = static_cast<int>(objects_.size()) - 1;
    }
}

std::shared_ptr<MolObject> Tab::currentObject() const {
    if (selectedIdx_ >= 0 && selectedIdx_ < static_cast<int>(objects_.size()))
        return objects_[selectedIdx_];
    return nullptr;
}

void Tab::selectObject(int idx) {
    if (idx >= 0 && idx < static_cast<int>(objects_.size()))
        selectedIdx_ = idx;
}

void Tab::selectNextObject() {
    if (objects_.empty()) return;
    selectedIdx_ = (selectedIdx_ + 1) % static_cast<int>(objects_.size());
}

void Tab::selectPrevObject() {
    if (objects_.empty()) return;
    selectedIdx_--;
    if (selectedIdx_ < 0) selectedIdx_ = static_cast<int>(objects_.size()) - 1;
}

void Tab::centerView() {
    if (objects_.empty()) return;

    float cx = 0, cy = 0, cz = 0;
    int count = 0;
    for (const auto& obj : objects_) {
        float ox, oy, oz;
        obj->computeCenter(ox, oy, oz);
        if (!obj->atoms().empty()) {
            cx += ox; cy += oy; cz += oz;
            ++count;
        }
    }
    if (count > 0) {
        cx /= count; cy /= count; cz /= count;
    }
    camera_.setCenter(cx, cy, cz);

    // Compute appropriate zoom
    float minX = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float minY = minX, maxY = maxX;
    for (const auto& obj : objects_) {
        float bx0, by0, bz0, bx1, by1, bz1;
        obj->computeBoundingBox(bx0, by0, bz0, bx1, by1, bz1);
        if (bx0 < minX) minX = bx0;
        if (bx1 > maxX) maxX = bx1;
        if (by0 < minY) minY = by0;
        if (by1 > maxY) maxY = by1;
    }
    float span = std::max(maxX - minX, maxY - minY);
    if (span > 0.0f) {
        camera_.setZoom(40.0f / span);
    }
}

} // namespace molterm
