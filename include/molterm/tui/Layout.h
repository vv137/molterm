#pragma once

#include <cstdint>
#include <memory>

#include "molterm/tui/Window.h"

namespace molterm {

struct TabViewState;  // forward decl

// Axis-aligned rectangle for layout computation
struct Rect {
    int y = 0, x = 0, h = 0, w = 0;
    bool operator==(const Rect& o) const { return y == o.y && x == o.x && h == o.h && w == o.w; }
    bool operator!=(const Rect& o) const { return !(*this == o); }
};

// Manages the main screen layout:
//   [TabBar]
//   [Viewport          | ObjectPanel  ]
//   [                  | AnalysisPanel]
//   [SeqBar]
//   [StatusBar]
//   [CommandLine]
class Layout {
public:
    Layout();

    void init(int screenH, int screenW);
    void resize(int screenH, int screenW);

    // Apply per-tab view state (panel/seqbar visibility).
    // Only rebuilds windows if geometry actually changed.
    void applyViewState(const TabViewState& vs);

    Window& tabBar() { return *tabBar_; }
    Window& viewport() { return *viewport_; }
    Window& objectPanel() { return *objectPanel_; }
    Window& analysisPanel() { return *analysisPanel_; }
    Window& seqBar() { return *seqBar_; }
    Window& statusBar() { return *statusBar_; }
    Window& commandLine() { return *commandLine_; }

    bool panelVisible() const { return panelVisible_; }
    void togglePanel();
    void setPanel(bool visible);

    bool analysisPanelVisible() const { return analysisPanelVisible_; }
    void toggleAnalysisPanel();
    void setAnalysisPanel(bool visible);

    bool seqBarVisible() const { return seqBarVisible_; }
    void toggleSeqBar();
    void setSeqBar(bool visible);
    bool seqBarWrap() const { return seqBarWrap_; }
    void setSeqBarWrap(bool wrap);
    void setSeqBarHeight(int h);

    int viewportWidth() const;
    int viewportHeight() const;

    // Per-component dirty flags
    enum class Component : uint8_t {
        TabBar         = 0,
        Viewport       = 1,
        ObjectPanel    = 2,
        AnalysisPanel  = 3,
        SeqBar         = 4,
        StatusBar      = 5,
        CommandLine    = 6,
    };

    void markDirty(Component c);
    void markAllDirty();
    bool isDirty(Component c) const;
    uint8_t dirtyFlags() const { return dirtyFlags_; }

    void refreshAll();

private:
    static constexpr int kNumComponents = 7;

    std::unique_ptr<Window> tabBar_;
    std::unique_ptr<Window> viewport_;
    std::unique_ptr<Window> objectPanel_;
    std::unique_ptr<Window> analysisPanel_;
    std::unique_ptr<Window> seqBar_;
    std::unique_ptr<Window> statusBar_;
    std::unique_ptr<Window> commandLine_;

    // Cached rects for incremental rebuild (Phase 3)
    Rect rects_[kNumComponents] = {};

    int screenH_ = 0, screenW_ = 0;
    bool panelVisible_ = false;
    bool analysisPanelVisible_ = false;
    int panelWidth_ = 22;
    bool seqBarVisible_ = true;
    bool seqBarWrap_ = true;
    int seqBarHeight_ = 1;

    uint8_t dirtyFlags_ = 0xFF; // all dirty initially

    // Compute rects for all 7 components based on current flags/dimensions.
    // Returns true if any rect changed vs cached.
    bool computeRects(Rect out[kNumComponents]) const;

    // Apply computed rects: resize only changed windows, mark them dirty.
    void applyRects(const Rect newRects[kNumComponents]);

    void updateLayout();
};

} // namespace molterm
