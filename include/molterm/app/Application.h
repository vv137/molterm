#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "molterm/app/TabManager.h"
#include "molterm/cmd/CommandRegistry.h"
#include "molterm/cmd/UndoStack.h"
#include "molterm/core/ObjectStore.h"
#include "molterm/core/Selection.h"
#include "molterm/input/InputHandler.h"
#include "molterm/input/KeymapManager.h"
#include "molterm/render/Canvas.h"
#include "molterm/repr/Representation.h"
#include "molterm/tui/CommandLine.h"
#include "molterm/tui/Layout.h"
#include "molterm/tui/ObjectPanel.h"
#include "molterm/tui/Screen.h"
#include "molterm/tui/StatusBar.h"
#include "molterm/tui/TabBar.h"

namespace molterm {

enum class RendererType {
    Ascii,
    Braille,
    Block,
    Sixel,
};

class Application {
public:
    Application();
    ~Application();

    // Initialize and run
    void init(int argc, char* argv[]);
    int run();
    void quit(bool force = false);

    // Public accessors for command handlers
    TabManager& tabs() { return tabMgr_; }
    ObjectStore& store() { return store_; }
    CommandLine& cmdLine() { return cmdLine_; }
    Layout& layout() { return layout_; }
    InputHandler& input() { return *inputHandler_; }
    UndoStack& undoStack() { return undoStack_; }

    // Load a file into the current tab
    std::string loadFile(const std::string& path);

    // Renderer switching
    void setRenderer(RendererType type);
    RendererType rendererType() const { return rendererType_; }

    // Representation access (for :set commands)
    Representation* getRepr(ReprType type);

    // Search
    const std::string& lastSearch() const { return lastSearch_; }
    std::vector<int>& searchMatches() { return searchMatches_; }
    int searchIdx() const { return searchIdx_; }
    void setSearchIdx(int i) { searchIdx_ = i; }

    // Named selections
    std::unordered_map<std::string, Selection>& namedSelections() { return namedSelections_; }

    bool running() const { return running_; }

private:
    // Subsystems
    Screen screen_;
    Layout layout_;
    TabBar tabBar_;
    StatusBar statusBar_;
    CommandLine cmdLine_;
    ObjectPanel objectPanel_;
    KeymapManager keymapMgr_;
    std::unique_ptr<InputHandler> inputHandler_;
    CommandRegistry cmdRegistry_;
    TabManager tabMgr_;
    ObjectStore store_;
    UndoStack undoStack_;

    // Rendering
    RendererType rendererType_ = RendererType::Braille;
    std::unique_ptr<Canvas> canvas_;
    std::unordered_map<ReprType, std::unique_ptr<Representation>> representations_;

    // Search state
    std::string lastSearch_;
    std::vector<int> searchMatches_;  // atom indices
    int searchIdx_ = -1;

    // Named selections
    std::unordered_map<std::string, Selection> namedSelections_;

    // Inspect mode state
    bool inspectMode_ = false;
    int cursorX_ = -1, cursorY_ = -1;  // viewport terminal coords
    int pickedAtomIdx_ = -1;           // nearest atom index

    // Projected atom cache for picking (populated each frame)
    struct ProjAtom { int idx; int sx, sy; float depth; };
    std::vector<ProjAtom> projCache_;

    bool running_ = false;
    bool needsRedraw_ = true;

    // Main loop internals
    void processInput();
    void handleAction(Action action);
    void handleCommandInput(int key);
    void handleSearchInput(int key);
    void handleMouse(int key);
    void renderFrame();
    void renderViewport();
    void updateStatusBar();

    void registerCommands();
    Selection parseSelection(const std::string& expr, const MolObject& mol);
    void buildProjCache();
    int findNearestAtom(int termX, int termY) const;
    std::string atomInfoString(const MolObject& mol, int atomIdx) const;
    void initRepresentations();
    void executeSearch(const std::string& query);
    void onResize();
};

} // namespace molterm
