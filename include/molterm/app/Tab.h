#pragma once

#include <memory>
#include <string>
#include <vector>

#include "molterm/app/TabViewState.h"
#include "molterm/core/MolObject.h"
#include "molterm/render/Camera.h"

namespace molterm {

class Tab {
public:
    explicit Tab(std::string name = "untitled");

    const std::string& name() const { return name_; }
    void setName(const std::string& n) { name_ = n; }

    // Object management
    void addObject(std::shared_ptr<MolObject> obj);
    void removeObject(int idx);
    std::shared_ptr<MolObject> currentObject() const;
    const std::vector<std::shared_ptr<MolObject>>& objects() const { return objects_; }

    int selectedObjectIdx() const { return selectedIdx_; }
    void selectObject(int idx);
    void selectNextObject();
    void selectPrevObject();

    // Camera
    Camera& camera() { return camera_; }
    const Camera& camera() const { return camera_; }

    // Per-tab view state (SeqBar, panel visibility, etc.)
    TabViewState& viewState() { return viewState_; }
    const TabViewState& viewState() const { return viewState_; }

    // Center camera on all loaded objects
    void centerView();

private:
    std::string name_;
    std::vector<std::shared_ptr<MolObject>> objects_;
    int selectedIdx_ = -1;
    Camera camera_;
    TabViewState viewState_;
};

} // namespace molterm
