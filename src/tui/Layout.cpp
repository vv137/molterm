#include "molterm/tui/Layout.h"

namespace molterm {

Layout::Layout() = default;

void Layout::init(int screenH, int screenW) {
    screenH_ = screenH;
    screenW_ = screenW;
    rebuildWindows();
}

void Layout::resize(int screenH, int screenW) {
    screenH_ = screenH;
    screenW_ = screenW;
    rebuildWindows();
}

void Layout::togglePanel() {
    panelVisible_ = !panelVisible_;
    rebuildWindows();
}

void Layout::setPanel(bool visible) {
    if (panelVisible_ != visible) {
        panelVisible_ = visible;
        rebuildWindows();
    }
}

int Layout::viewportWidth() const {
    if (!viewport_) return 0;
    return viewport_->width();
}

int Layout::viewportHeight() const {
    if (!viewport_) return 0;
    return viewport_->height();
}

void Layout::refreshAll() {
    if (tabBar_) tabBar_->refresh();
    if (viewport_) viewport_->refresh();
    if (panelVisible_ && objectPanel_) objectPanel_->refresh();
    if (statusBar_) statusBar_->refresh();
    if (commandLine_) commandLine_->refresh();
}

void Layout::rebuildWindows() {
    // Layout:
    // Row 0:          TabBar (1 line)
    // Row 1..H-3:     Viewport [| ObjectPanel]
    // Row H-2:        StatusBar (1 line)
    // Row H-1:        CommandLine (1 line)

    int tabH = 1;
    int statusH = 1;
    int cmdH = 1;
    int vpH = screenH_ - tabH - statusH - cmdH;
    if (vpH < 1) vpH = 1;

    int vpW = screenW_;
    int panelW = 0;
    if (panelVisible_) {
        panelW = panelWidth_;
        if (panelW > screenW_ / 2) panelW = screenW_ / 2;
        vpW = screenW_ - panelW;
    }

    tabBar_ = std::make_unique<Window>(tabH, screenW_, 0, 0);
    viewport_ = std::make_unique<Window>(vpH, vpW, tabH, 0);
    if (panelVisible_) {
        objectPanel_ = std::make_unique<Window>(vpH, panelW, tabH, vpW);
    } else {
        objectPanel_ = std::make_unique<Window>(vpH, 0, tabH, screenW_);
    }
    statusBar_ = std::make_unique<Window>(statusH, screenW_, tabH + vpH, 0);
    commandLine_ = std::make_unique<Window>(cmdH, screenW_, tabH + vpH + statusH, 0);
}

} // namespace molterm
