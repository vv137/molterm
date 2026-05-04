#pragma once

#include <array>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "molterm/app/TabManager.h"
#include "molterm/app/TabViewState.h"
#include "molterm/cmd/CommandRegistry.h"
#include "molterm/cmd/UndoStack.h"
#include "molterm/core/ObjectStore.h"
#include "molterm/core/Selection.h"
#include "molterm/input/InputHandler.h"
#include "molterm/input/KeymapManager.h"
#include "molterm/render/Canvas.h"
#include "molterm/render/ColorMapper.h"
#include "molterm/render/ProtocolPicker.h"
#include "molterm/render/ZoomGate.h"
#include "molterm/repr/InterfaceRepr.h"
#include "molterm/repr/Representation.h"
#include "molterm/tui/CommandLine.h"
#include "molterm/tui/ContactMapPanel.h"
#include "molterm/tui/Layout.h"
#include "molterm/tui/ObjectPanel.h"
#include "molterm/tui/Screen.h"
#include "molterm/tui/SeqBar.h"
#include "molterm/tui/StatusBar.h"
#include "molterm/tui/TabBar.h"

namespace molterm {

enum class InspectLevel { Atom, Residue, Chain, Object };
enum class PickMode { Inspect, SelectAtom, SelectResidue, SelectChain, Focus };

// What to expand a clicked atom into when entering focus.
//   Residue   — atoms sharing chainId+resSeq+insCode (default; matches prior F behavior)
//   Chain     — every atom in the same chainId
//   Sidechain — same residue minus backbone (N/CA/C/O); falls back to Residue
//               when sidechain is empty (Gly).
enum class FocusGranularity { Residue, Chain, Sidechain };

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

    // Result of running commands from a script stream.
    struct ScriptRunResult {
        int count = 0;        // commands attempted (non-blank, non-comment)
        int failures = 0;     // failed commands
        std::string firstFail; // first failure message (if any)
        std::string failLine;  // the offending line for the first failure
        std::string lastMsg;   // last non-empty result message
        bool stopped = false;  // true if strict mode aborted early
    };

    // Run commands from a stream. Skips blank lines and `#` comments.
    // If strict is true, stops on the first failure.
    ScriptRunResult runScriptStream(std::istream& in, bool strict = false);

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
    bool outlineEnabled() const { return outlineEnabled_; }
    void setOutlineEnabled(bool v) { outlineEnabled_ = v; }
    float outlineThreshold() const { return outlineThreshold_; }
    void setOutlineThreshold(float t) { outlineThreshold_ = t; }
    float outlineDarken() const { return outlineDarken_; }
    void setOutlineDarken(float d) { outlineDarken_ = d; }
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
    // SeqBar is per-tab via TabViewState (accessed through activeSeqBar())
    SeqBar& activeSeqBar() { return tabMgr_.currentTab().viewState().seqBar; }
    TabViewState& activeViewState() { return tabMgr_.currentTab().viewState(); }
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

    // Contact map + interface overlay state
    ContactMapPanel contactMapPanel_;
    bool interfaceOverlay_ = false;
    // Cached classified inter-chain contacts; one entry per residue
    // pair, color-coded at render time by interaction type.
    std::vector<InterfaceContact> interfaceContacts_;
    // Per-atom mask: true for atoms in interface residues. Built from
    // `interfaceContacts_` by expanding to whole residues.
    std::vector<bool> interfaceAtomMask_;
    // Overlay renderer (sidechain bonds + dashed interaction lines).
    InterfaceRepr interfaceRepr_;
    // Auto-engage gate: when zoom > threshold, simulate :interface on.
    ZoomGate interfaceZoomGate_;
    // True iff the overlay is currently engaged via the zoom gate
    // (so we can auto-disengage cleanly without clobbering a manual toggle).
    bool interfaceFromZoom_ = false;
    // Focus mode (Mol*-style click-to-focus). Tracks the live "focus
    // subject" (a Selection) and the saved view + visibility state to
    // restore on exit. focusActive_ false → no focus mode; the
    // focusAtomMask_ drives `applyFocusDim` only.
    struct FocusSavedRepr {
        ReprType  type;
        bool      objectLevel;            // pre-focus mol.reprVisible(type)
        std::vector<bool> atomMask;       // pre-focus per-atom mask (empty = all)
    };
    struct FocusSnapshot {
        bool                 active = false;
        // Camera
        std::array<float, 9> rot{};
        float cx = 0, cy = 0, cz = 0;
        float panX = 0, panY = 0;
        float zoom = 1.0f;
        // Per-repr visibility, captured on entry, restored on exit.
        std::vector<FocusSavedRepr> reprs;
        // Object-level (no per-atom mask) visibility for the spline
        // reprs we toggle off during focus — restore to original on exit.
        bool cartoonVisible  = false;
        bool ribbonVisible   = false;
        bool backboneVisible = false;
        // Original wireframe thickness — temporarily bumped during focus.
        float wireframeThickness = 0.3f;
    };
    FocusSnapshot     focusSnapshot_;
    std::vector<bool> focusAtomMask_;       // focus subject (kept vivid)
    std::vector<bool> focusNbhdMask_;       // neighborhood (visible during focus)
    std::string       focusExpr_;
    float             focusDimStrength_ = 0.55f;
    float             focusRadius_      = 5.0f;   // Å around the subject
    float             focusZoom_        = 4.0f;   // fallback zoom (used only when
                                                  // subject bounding sphere can't
                                                  // be computed)
    // Subject-size aware zoom (Mol*-style). The effective zoom for a focus
    // subject of enclosing radius R is:
    //   zoom = focusFillFraction_ * 20.0 / max(R + focusExtraRadius_, focusMinRadius_)
    // Calibration: at fillFraction=1.0 the formula matches the existing
    // `:zoom` heuristic (40 / span ≈ 20 / R) so a default of 0.6 leaves
    // generous headroom around the subject.
    float             focusFillFraction_ = 0.6f;
    float             focusExtraRadius_  = 4.0f;  // Å padding (matches Mol*)
    float             focusMinRadius_    = 2.0f;  // Å clamp (single-atom subject)
    FocusGranularity  focusGranularity_  = FocusGranularity::Residue;
    bool              focusComputedInterface_ = false;   // true if enterFocus
                                                          // ran computeInterface
                                                          // itself (so exitFocus
                                                          // clears the cache).
    // Fallback color used when classification is disabled
    // (`:set interface_classify off`).
    int interfaceColor_ = kColorYellow;
    int interfaceThickness_ = 4; // pixel-mode line thickness (1-6) —
                                  // bumped from 2 so dashed contact lines
                                  // read against cartoon at 1080p+. Tunable
                                  // live via :set interface_thickness.
    bool interfaceClassify_ = true;
    // Toggle: draw element-colored sidechain bonds for interface residues.
    bool interfaceSidechains_ = true;

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
    bool overlayVisible_ = true;
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
    bool quitRequested_ = false;
    bool needsRedraw_ = true;
    int64_t lastFrameMs_ = 0;
    int framesToSkip_ = 0;
    int frameCounter_ = 0;
    float fogStrength_ = 0.35f;
    bool outlineEnabled_ = true;
    float outlineThreshold_ = 0.3f;
    float outlineDarken_ = 0.3f;
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
    void handleLineEdit(int key);
    void handleMouse(int key);
    void renderFrame();
    void renderViewport();
    void renderAnalysisPanel();
    void updateStatusBar();

    void registerCommands();
    Selection parseSelection(const std::string& expr, const MolObject& mol);
    void buildProjCache();

    // Focus Selection mode (Mol*-style click-to-focus).
    // `subjectIndices` are the atoms forming the focus subject (e.g. a
    // residue's atoms). enterFocus saves current camera + repr-visibility
    // state, snaps the camera to the centroid at focus_zoom, hides non-
    // neighborhood atoms for atom-direct reprs, and ensures ball-stick is
    // visible on the neighborhood. exitFocus restores everything.
    void enterFocus(MolObject& mol,
                    const std::vector<int>& subjectIndices,
                    const std::string& exprDesc);
    void exitFocus();
    bool focusActive() const { return focusSnapshot_.active; }
    // Expand a single clicked atom into the focus subject according to
    // `focusGranularity_`. Used by both Action::FocusPick (F key) and
    // PickMode::Focus (gf + click) so they stay in lockstep.
    std::vector<int> expandByFocusGranularity(const MolObject& mol,
                                              int atomIdx) const;
    int findNearestAtom(int termX, int termY) const;
    std::string atomInfoString(const MolObject& mol, int atomIdx) const;
    // "{resName} {resSeq}{insCode} (chain {chainId})" — used as enterFocus's
    // description for residue-anchored focus calls.
    std::string residueInfoString(const AtomData& a) const;
    void initRepresentations();
    void executeSearch(const std::string& query);
public:
    void onResize();
private:
};

} // namespace molterm
