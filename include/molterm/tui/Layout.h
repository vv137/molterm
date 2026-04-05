#pragma once

#include <cstdint>
#include <memory>

#include "molterm/tui/Window.h"

namespace molterm {

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
    bool seqBarWrap() const { return seqBarWrap_; }
    void toggleSeqBarWrap();
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

    void refreshAll();

private:
    std::unique_ptr<Window> tabBar_;
    std::unique_ptr<Window> viewport_;
    std::unique_ptr<Window> objectPanel_;
    std::unique_ptr<Window> analysisPanel_;
    std::unique_ptr<Window> seqBar_;
    std::unique_ptr<Window> statusBar_;
    std::unique_ptr<Window> commandLine_;

    int screenH_ = 0, screenW_ = 0;
    bool panelVisible_ = false;
    bool analysisPanelVisible_ = false;
    int panelWidth_ = 22;
    bool seqBarVisible_ = true;
    bool seqBarWrap_ = false;
    int seqBarHeight_ = 1;

    uint8_t dirtyFlags_ = 0xFF; // all dirty initially

    void rebuildWindows();
};

} // namespace molterm
