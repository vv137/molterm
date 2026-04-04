#pragma once

#include <cstdint>
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
#include "molterm/render/ProtocolPicker.h"
#include "molterm/repr/Representation.h"
#include "molterm/tui/CommandLine.h"
#include "molterm/tui/Layout.h"
#include "molterm/tui/ObjectPanel.h"
#include "molterm/tui/Screen.h"
#include "molterm/tui/StatusBar.h"
#include "molterm/tui/TabBar.h"

namespace molterm {

enum class InspectLevel { Atom, Residue, Chain, Object };
enum class PickMode { Inspect, SelectAtom, SelectResidue, SelectChain };

enum class RendererType {
    Ascii,
    Braille,
    Block,
    Pixel,    // PixelCanvas + auto-detected protocol (Sixel/Kitty/iTerm2)
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
    const TabManager& tabs() const { return tabMgr_; }
    ObjectStore& store() { return store_; }
    CommandLine& cmdLine() { return cmdLine_; }
    Layout& layout() { return layout_; }
    InputHandler& input() { return *inputHandler_; }
    UndoStack& undoStack() { return undoStack_; }
    CommandRegistry& cmdRegistry() { return cmdRegistry_; }

    // Load a file into the current tab
    std::string loadFile(const std::string& path);

    // Renderer switching
    void setRenderer(RendererType type);
    RendererType rendererType() const { return rendererType_; }

    // Canvas access
    Canvas* canvas() { return canvas_.get(); }

    // Representation access
    Representation* getRepr(ReprType type);
    const std::unordered_map<ReprType, std::unique_ptr<Representation>>& representations() const { return representations_; }
    std::unordered_map<ReprType, std::unique_ptr<Representation>>& representations() { return representations_; }

    // Search
    const std::string& lastSearch() const { return lastSearch_; }
    std::vector<int>& searchMatches() { return searchMatches_; }
    int searchIdx() const { return searchIdx_; }
    void setSearchIdx(int i) { searchIdx_ = i; }

    // Named selections
    std::unordered_map<std::string, Selection>& namedSelections() { return namedSelections_; }

    bool running() const { return running_; }

    // Settings
    float fogStrength() const { return fogStrength_; }
    void setFogStrength(float s) { fogStrength_ = s; }
    bool autoCenter() const { return autoCenter_; }
    void setForcedProtocol(GraphicsProtocol p) { forcedProtocol_ = p; }
    void setAutoCenter(bool v) { autoCenter_ = v; }

    // Macro recording (Phase 4)
    bool isRecordingMacro() const { return macroRecording_; }
    char macroRegister() const { return macroRegister_; }
    void startMacroRecord(char reg);
    void stopMacroRecord();
    void playMacro(char reg);
    void recordAction(Action action);

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

    // Help overlay state
    bool helpOverlay_ = false;

    // Inspect / pick state (mouse-only)
    InspectLevel inspectLevel_ = InspectLevel::Atom;
    PickMode pickMode_ = PickMode::Inspect;
    int pickedAtomIdx_ = -1;           // nearest atom index from last click

    // Pick registers: pk1→pk4, rotates on each inspect click (like PyMOL)
    int pickRegs_[4] = {-1, -1, -1, -1};
    int pickNext_ = 0;

    // Labels: atom indices to render text labels for
    std::vector<int> labelAtoms_;

    // Persistent measurements: pairs/triples/quads of atom indices + label
    struct Measurement { std::vector<int> atoms; std::string label; };
    std::vector<Measurement> measurements_;

public:
    int pickReg(int n) const { return (n >= 0 && n < 4) ? pickRegs_[n] : -1; }
    std::vector<int>& labelAtoms() { return labelAtoms_; }
    std::vector<Measurement>& measurements() { return measurements_; }
private:

    // Projected atom cache for picking (populated once per frame)
    struct ProjAtom { int idx; int sx, sy; float depth; };
    std::vector<ProjAtom> projCache_;
    int projCacheFrame_ = -1;

    // 2D spatial hash for O(1) picking
    static constexpr int kPickCellSize = 20;  // sub-pixels per cell
    std::unordered_map<int, std::vector<int>> pickGrid_;  // grid key → indices into projCache_
    int pickGridKey(int sx, int sy) const { return (sy / kPickCellSize) * 10000 + (sx / kPickCellSize); }

    bool running_ = false;
    bool needsRedraw_ = true;
    int64_t lastFrameMs_ = 0;
    int framesToSkip_ = 0;
    int frameCounter_ = 0;
    float fogStrength_ = 0.35f;
    bool autoCenter_ = true;
    GraphicsProtocol forcedProtocol_ = GraphicsProtocol::None;

    // Macro recording state
    bool macroRecording_ = false;
    char macroRegister_ = '\0';
    bool macroAwaitingRegister_ = false;       // waiting for next key to select register
    bool macroPlayAwaitingRegister_ = false;    // waiting for next key for @ playback
    std::unordered_map<char, std::vector<Action>> macros_;
    std::vector<Action> currentMacro_;

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
