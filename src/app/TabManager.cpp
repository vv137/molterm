#include "molterm/app/TabManager.h"

namespace molterm {

TabManager::TabManager() {
    addTab("tab1");
}

Tab& TabManager::currentTab() {
    return *tabs_[activeIdx_];
}

const Tab& TabManager::currentTab() const {
    return *tabs_[activeIdx_];
}

int TabManager::addTab(const std::string& name) {
    std::string tabName = name.empty()
        ? "tab" + std::to_string(nextId_)
        : name;
    tabs_.push_back(std::make_unique<Tab>(tabName));
    ++nextId_;
    return static_cast<int>(tabs_.size()) - 1;
}

void TabManager::closeTab(int idx) {
    if (tabs_.size() <= 1) return;  // keep at least one tab
    if (idx < 0 || idx >= static_cast<int>(tabs_.size())) return;
    tabs_.erase(tabs_.begin() + idx);
    if (activeIdx_ >= static_cast<int>(tabs_.size()))
        activeIdx_ = static_cast<int>(tabs_.size()) - 1;
}

void TabManager::closeCurrentTab() {
    closeTab(activeIdx_);
}

void TabManager::nextTab() {
    if (tabs_.size() <= 1) return;
    activeIdx_ = (activeIdx_ + 1) % static_cast<int>(tabs_.size());
}

void TabManager::prevTab() {
    if (tabs_.size() <= 1) return;
    activeIdx_--;
    if (activeIdx_ < 0) activeIdx_ = static_cast<int>(tabs_.size()) - 1;
}

void TabManager::goToTab(int idx) {
    if (idx >= 0 && idx < static_cast<int>(tabs_.size()))
        activeIdx_ = idx;
}

std::vector<std::string> TabManager::tabNames() const {
    std::vector<std::string> names;
    for (const auto& tab : tabs_) names.push_back(tab->name());
    return names;
}

void TabManager::moveObjectToTab(int objIdx, int targetTab) {
    if (targetTab < 0 || targetTab >= static_cast<int>(tabs_.size())) return;
    if (targetTab == activeIdx_) return;

    auto& src = *tabs_[activeIdx_];
    const auto& objs = src.objects();
    if (objIdx < 0 || objIdx >= static_cast<int>(objs.size())) return;

    auto obj = objs[objIdx];
    tabs_[targetTab]->addObject(obj);
    src.removeObject(objIdx);
}

void TabManager::copyObjectToTab(int objIdx, int targetTab) {
    if (targetTab < 0 || targetTab >= static_cast<int>(tabs_.size())) return;

    auto& src = *tabs_[activeIdx_];
    const auto& objs = src.objects();
    if (objIdx < 0 || objIdx >= static_cast<int>(objs.size())) return;

    // Shallow copy - shares the same MolObject
    tabs_[targetTab]->addObject(objs[objIdx]);
}

} // namespace molterm
