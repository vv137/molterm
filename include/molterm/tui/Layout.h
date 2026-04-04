#pragma once

#include <memory>

#include "molterm/tui/Window.h"

namespace molterm {

// Manages the main screen layout:
//   [TabBar]
//   [Viewport          | ObjectPanel]
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
    Window& seqBar() { return *seqBar_; }
    Window& statusBar() { return *statusBar_; }
    Window& commandLine() { return *commandLine_; }

    bool panelVisible() const { return panelVisible_; }
    void togglePanel();
    void setPanel(bool visible);

    bool seqBarVisible() const { return seqBarVisible_; }
    void toggleSeqBar();
    bool seqBarWrap() const { return seqBarWrap_; }
    void toggleSeqBarWrap();
    void setSeqBarHeight(int h);

    int viewportWidth() const;
    int viewportHeight() const;

    void refreshAll();

private:
    std::unique_ptr<Window> tabBar_;
    std::unique_ptr<Window> viewport_;
    std::unique_ptr<Window> objectPanel_;
    std::unique_ptr<Window> seqBar_;
    std::unique_ptr<Window> statusBar_;
    std::unique_ptr<Window> commandLine_;

    int screenH_ = 0, screenW_ = 0;
    bool panelVisible_ = false;
    int panelWidth_ = 22;
    bool seqBarVisible_ = true;
    bool seqBarWrap_ = false;
    int seqBarHeight_ = 1;

    void rebuildWindows();
};

} // namespace molterm
