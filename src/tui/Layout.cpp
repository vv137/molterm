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

void Layout::toggleAnalysisPanel() {
    analysisPanelVisible_ = !analysisPanelVisible_;
    rebuildWindows();
}

void Layout::setAnalysisPanel(bool visible) {
    if (analysisPanelVisible_ != visible) {
        analysisPanelVisible_ = visible;
        rebuildWindows();
    }
}

void Layout::toggleSeqBar() {
    seqBarVisible_ = !seqBarVisible_;
    rebuildWindows();
}

void Layout::toggleSeqBarWrap() {
    seqBarWrap_ = !seqBarWrap_;
    rebuildWindows();
}

void Layout::setSeqBarHeight(int h) {
    seqBarHeight_ = std::max(1, h);
    rebuildWindows();
}

int Layout::viewportWidth() const {
    if (!viewport_) return 0;
    return viewport_->width();
}

int Layout::viewportHeight() const {
    if (!viewport_) return 0;
    return viewport_->height();
}

// --- Dirty flags ---

void Layout::markDirty(Component c) {
    dirtyFlags_ |= (1u << static_cast<uint8_t>(c));
}

void Layout::markAllDirty() {
    dirtyFlags_ = 0xFF;
}

bool Layout::isDirty(Component c) const {
    return (dirtyFlags_ & (1u << static_cast<uint8_t>(c))) != 0;
}

void Layout::refreshAll() {
    auto refresh = [&](Component c, Window* win, bool visible = true) {
        if (win && visible && isDirty(c)) {
            win->refresh();
            dirtyFlags_ &= ~(1u << static_cast<uint8_t>(c));
        }
    };

    refresh(Component::TabBar,        tabBar_.get());
    refresh(Component::Viewport,      viewport_.get());
    refresh(Component::ObjectPanel,   objectPanel_.get(),  panelVisible_);
    refresh(Component::AnalysisPanel, analysisPanel_.get(), analysisPanelVisible_);
    refresh(Component::SeqBar,        seqBar_.get(),       seqBarVisible_);
    refresh(Component::StatusBar,     statusBar_.get());
    refresh(Component::CommandLine,   commandLine_.get());
}

void Layout::rebuildWindows() {
    // Layout:
    // Row 0:              TabBar (1 line)
    // Row 1..H-N:         Viewport [| ObjectPanel / AnalysisPanel]
    // Row H-N..H-N+seqH:  SeqBar (optional, 1+ lines)
    // Row H-2:            StatusBar (1 line)
    // Row H-1:            CommandLine (1 line)

    int tabH = 1;
    int seqH = seqBarVisible_ ? seqBarHeight_ : 0;
    int statusH = 1;
    int cmdH = 1;
    int vpH = screenH_ - tabH - seqH - statusH - cmdH;
    if (vpH < 1) vpH = 1;

    // Right column: visible when either panel is active
    bool rightCol = panelVisible_ || analysisPanelVisible_;
    int vpW = screenW_;
    int panelW = 0;
    if (rightCol) {
        panelW = panelWidth_;
        if (panelW > screenW_ / 2) panelW = screenW_ / 2;
        vpW = screenW_ - panelW;
    }

    // Cap seqbar height to avoid eating entire viewport
    int maxSeqH = screenH_ / 4;
    if (seqH > maxSeqH) { seqH = maxSeqH; vpH = screenH_ - tabH - seqH - statusH - cmdH; }

    tabBar_ = std::make_unique<Window>(tabH, screenW_, 0, 0);
    viewport_ = std::make_unique<Window>(vpH, vpW, tabH, 0);

    // Right column split: ObjectPanel (top) + AnalysisPanel (bottom)
    if (rightCol) {
        if (panelVisible_ && analysisPanelVisible_) {
            // Split: ObjectPanel gets top portion, AnalysisPanel gets rest
            int objH = std::min(12, vpH / 3);
            if (objH < 3) objH = 3;
            int anlH = vpH - objH;
            if (anlH < 1) { anlH = 1; objH = vpH - 1; }
            objectPanel_ = std::make_unique<Window>(objH, panelW, tabH, vpW);
            analysisPanel_ = std::make_unique<Window>(anlH, panelW, tabH + objH, vpW);
        } else if (panelVisible_) {
            objectPanel_ = std::make_unique<Window>(vpH, panelW, tabH, vpW);
            analysisPanel_ = std::make_unique<Window>(0, 0, tabH + vpH, vpW);
        } else {
            // Only analysis panel
            objectPanel_ = std::make_unique<Window>(0, 0, tabH, screenW_);
            analysisPanel_ = std::make_unique<Window>(vpH, panelW, tabH, vpW);
        }
    } else {
        objectPanel_ = std::make_unique<Window>(vpH, 0, tabH, screenW_);
        analysisPanel_ = std::make_unique<Window>(0, 0, tabH, screenW_);
    }

    seqBar_ = std::make_unique<Window>(std::max(1, seqH), screenW_, tabH + vpH, 0);
    statusBar_ = std::make_unique<Window>(statusH, screenW_, tabH + vpH + seqH, 0);
    commandLine_ = std::make_unique<Window>(cmdH, screenW_, tabH + vpH + seqH + statusH, 0);

    markAllDirty();
}

} // namespace molterm
