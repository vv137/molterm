#include "molterm/tui/Layout.h"
#include "molterm/tui/Constraint.h"
#include "molterm/app/TabViewState.h"

namespace molterm {

Layout::Layout() = default;

void Layout::init(int screenH, int screenW) {
    screenH_ = screenH;
    screenW_ = screenW;
    updateLayout();
}

void Layout::resize(int screenH, int screenW) {
    screenH_ = screenH;
    screenW_ = screenW;
    updateLayout();
}

void Layout::applyViewState(const TabViewState& vs) {
    panelVisible_ = vs.panelVisible;
    analysisPanelVisible_ = vs.analysisPanelVisible;
    seqBarVisible_ = vs.seqBarVisible;
    seqBarWrap_ = vs.seqBarWrap;
    seqBarHeight_ = vs.seqBarHeight;
    updateLayout();
}

void Layout::togglePanel() {
    panelVisible_ = !panelVisible_;
    updateLayout();
}

void Layout::setPanel(bool visible) {
    if (panelVisible_ != visible) {
        panelVisible_ = visible;
        updateLayout();
    }
}

void Layout::toggleAnalysisPanel() {
    analysisPanelVisible_ = !analysisPanelVisible_;
    updateLayout();
}

void Layout::setAnalysisPanel(bool visible) {
    if (analysisPanelVisible_ != visible) {
        analysisPanelVisible_ = visible;
        updateLayout();
    }
}

void Layout::toggleSeqBar() {
    seqBarVisible_ = !seqBarVisible_;
    updateLayout();
}

void Layout::setSeqBar(bool visible) {
    if (seqBarVisible_ != visible) {
        seqBarVisible_ = visible;
        updateLayout();
    }
}

void Layout::setSeqBarWrap(bool wrap) {
    if (seqBarWrap_ != wrap) {
        seqBarWrap_ = wrap;
        updateLayout();
    }
}

void Layout::setSeqBarHeight(int h) {
    seqBarHeight_ = std::max(1, h);
    updateLayout();
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

// --- Constraint-based layout computation ---

bool Layout::computeRects(Rect out[kNumComponents]) const {
    // Cap seqbar height
    int seqH = seqBarVisible_ ? seqBarHeight_ : 0;
    int maxSeqH = screenH_ / 4;
    if (seqH > maxSeqH) seqH = maxSeqH;

    // Right column width
    bool rightCol = panelVisible_ || analysisPanelVisible_;
    int panelW = 0;
    if (rightCol) {
        panelW = panelWidth_;
        if (panelW > screenW_ / 2) panelW = screenW_ / 2;
    }

    // ── Vertical strip: TabBar | MiddleRow | SeqBar | StatusBar | CommandLine ──
    std::vector<LayoutSlot> vSlots = {
        {SizePolicy::Fixed, 1,    true,  0, 0},                     // 0: TabBar
        {SizePolicy::Fill,  1,    true,  1, 0},                     // 1: Middle row (viewport + panels)
        {SizePolicy::Fixed, std::max(1, seqH), seqBarVisible_, 0, 0}, // 2: SeqBar
        {SizePolicy::Fixed, 1,    true,  0, 0},                     // 3: StatusBar
        {SizePolicy::Fixed, 1,    true,  0, 0},                     // 4: CommandLine
    };
    Rect screenRect = {0, 0, screenH_, screenW_};
    auto vRects = solveLayout(screenRect, SplitDir::Vertical, vSlots);

    // ── Horizontal strip for middle row: Viewport | RightColumn ──
    std::vector<LayoutSlot> hSlots = {
        {SizePolicy::Fill,  1,    true, 1, 0},              // 0: Viewport
        {SizePolicy::Fixed, panelW, rightCol, 0, 0},        // 1: Right column
    };
    auto hRects = solveLayout(vRects[1], SplitDir::Horizontal, hSlots);

    // ── Vertical split of right column: ObjectPanel | AnalysisPanel ──
    Rect objRect, anlRect;
    if (rightCol) {
        if (panelVisible_ && analysisPanelVisible_) {
            int vpH = hRects[1].h;
            int objH = std::min(12, vpH / 3);
            if (objH < 3) objH = 3;
            int anlH = vpH - objH;
            if (anlH < 1) { anlH = 1; objH = vpH - 1; }
            std::vector<LayoutSlot> pSlots = {
                {SizePolicy::Fixed, objH, true, 3, 0},
                {SizePolicy::Fill,  1,    true, 1, 0},
            };
            auto pRects = solveLayout(hRects[1], SplitDir::Vertical, pSlots);
            objRect = pRects[0];
            anlRect = pRects[1];
        } else if (panelVisible_) {
            objRect = hRects[1];
            anlRect = {hRects[1].y + hRects[1].h, hRects[1].x, 0, 0};
        } else {
            objRect = {hRects[1].y, screenW_, 0, 0};
            anlRect = hRects[1];
        }
    } else {
        int tabH = vRects[0].h;
        int vpH = hRects[0].h;
        objRect = {tabH, screenW_, vpH, 0};
        anlRect = {tabH, screenW_, 0, 0};
    }

    // Assemble output: [TabBar, Viewport, ObjectPanel, AnalysisPanel, SeqBar, StatusBar, CommandLine]
    out[0] = vRects[0];       // TabBar
    out[1] = hRects[0];       // Viewport
    out[2] = objRect;         // ObjectPanel
    out[3] = anlRect;         // AnalysisPanel
    out[4] = vRects[2];       // SeqBar
    out[5] = vRects[3];       // StatusBar
    out[6] = vRects[4];       // CommandLine

    // Check if anything changed
    bool changed = false;
    for (int i = 0; i < kNumComponents; ++i) {
        if (out[i] != rects_[i]) { changed = true; break; }
    }
    return changed;
}

void Layout::applyRects(const Rect newRects[kNumComponents]) {
    std::unique_ptr<Window>* windows[kNumComponents] = {
        &tabBar_, &viewport_, &objectPanel_, &analysisPanel_,
        &seqBar_, &statusBar_, &commandLine_
    };

    for (int i = 0; i < kNumComponents; ++i) {
        const auto& r = newRects[i];
        if (!*windows[i]) {
            *windows[i] = std::make_unique<Window>(r.h, r.w, r.y, r.x);
            markDirty(static_cast<Component>(i));
        } else if (newRects[i] != rects_[i]) {
            (*windows[i])->resize(r.h, r.w, r.y, r.x);
            markDirty(static_cast<Component>(i));
        }
        rects_[i] = newRects[i];
    }
}

void Layout::updateLayout() {
    if (screenH_ <= 0 || screenW_ <= 0) return;

    Rect newRects[kNumComponents];
    bool changed = computeRects(newRects);

    if (changed || !tabBar_) {
        applyRects(newRects);
    }
}

} // namespace molterm
