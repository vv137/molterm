#pragma once

#include <memory>
#include <string>
#include <vector>

#include "molterm/app/Tab.h"

namespace molterm {

class TabManager {
public:
    TabManager();

    Tab& currentTab();
    const Tab& currentTab() const;
    int currentIndex() const { return activeIdx_; }

    int addTab(const std::string& name = "");
    void closeTab(int idx);
    void closeCurrentTab();
    void nextTab();
    void prevTab();
    void goToTab(int idx);

    size_t count() const { return tabs_.size(); }
    Tab& tab(int idx) { return *tabs_[idx]; }
    const Tab& tab(int idx) const { return *tabs_[idx]; }
    std::vector<std::string> tabNames() const;

    // Move/copy object from current tab to target tab
    void moveObjectToTab(int objIdx, int targetTab);
    void copyObjectToTab(int objIdx, int targetTab);

private:
    std::vector<std::unique_ptr<Tab>> tabs_;
    int activeIdx_ = 0;
    int nextId_ = 1;
};

} // namespace molterm
