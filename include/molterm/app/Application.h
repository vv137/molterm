#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "molterm/app/CommandScope.h"
#include "molterm/app/FocusState.h"
#include "molterm/app/InterfaceOverlay.h"
#include "molterm/app/TabManager.h"
#include "molterm/app/TabViewState.h"
#include "molterm/app/ViewSettings.h"
#include "molterm/cmd/CommandRegistry.h"
#include "molterm/cmd/Register.h"
#include "molterm/cmd/UndoStack.h"
#include "molterm/core/ObjectStore.h"
#include "molterm/core/Selection.h"
#include "molterm/input/InputHandler.h"
#include "molterm/input/KeymapManager.h"
#include "molterm/render/Canvas.h"
#include "molterm/render/ColorMapper.h"
#include "molterm/render/OutlineMode.h"
#include "molterm/render/ProtocolPicker.h"
#include "molterm/render/StereoMode.h"
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

// Name of the transient last-result selection populated by `:sele`,
// click-picks, and similar one-off operations. Persisted in
// namedSelections_ so the user can refer back to it as `$sele`.
inline constexpr const char* kSele = "sele";

enum class InspectLevel { Atom, Residue, Chain, Object };
enum class PickMode { Inspect, SelectAtom, SelectResidue, SelectChain, Focus };

// Short display names for the status bar / inspect HUD. Free + inline so the
// input and render translation units (Application_Input/Render.cpp) share one
// definition.
inline const char* inspectLevelName(InspectLevel lvl) {
    switch (lvl) {
        case InspectLevel::Atom:    return "ATOM";
        case InspectLevel::Residue: return "RESIDUE";
        case InspectLevel::Chain:   return "CHAIN";
        case InspectLevel::Object:  return "OBJECT";
    }
    return "?";
}
inline const char* pickModeName(PickMode pm) {
    switch (pm) {
        case PickMode::Inspect:       return "INSPECT";
        case PickMode::SelectAtom:    return "SEL:ATOM";
        case PickMode::SelectResidue: return "SEL:RES";
        case PickMode::SelectChain:   return "SEL:CHAIN";
        case PickMode::Focus:         return "FOCUS";
    }
    return "?";
}

// FocusGranularity + the focus-mode state cluster live in
// molterm/app/FocusState.h (reached via Application::focus()).

enum class RendererType {
    Ascii,
    Braille,
    Block,
    Pixel,    // PixelCanvas + auto-detected protocol (Sixel/Kitty/iTerm2)
};

// BgMode (PNG / pixel-canvas background) and the visual/overlay appearance
// knobs live on ViewSettings (molterm/app/ViewSettings.h), reached via
// Application::view(). OutlineMode lives in molterm/render/OutlineMode.h —
// included transitively; app-layer code uses the unqualified name.

// The script execution engine (molterm/app/ScriptRunner.h). Held by
// unique_ptr so Application only needs this forward declaration.
class ScriptRunner;

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

    // Centered modal overlay for `?`, `:help`, `:help <cmd>`, and
    // `:interface legend`. Title + lines feed the renderer in
    // renderViewport(); scroll/dismiss keys are handled in processInput().
    struct InfoOverlay {
        std::string title;
        std::vector<std::string> lines;
        // Optional per-line color override (legend swatches). When
        // lineColors[i] >= 0 that line renders in that color; otherwise
        // the default status-bar color is used. May be shorter than lines.
        std::vector<int> lineColors;
        bool active = false;
        int scrollOffset = 0;     // first visible line in *wrapped* line space
        int lastVisibleRows = 0;  // last frame's content height
        int lastTotalLines = 0;   // last frame's wrapped-line count (paging math)
    };
    InfoOverlay& infoOverlay() { return infoOverlay_; }
    const InfoOverlay& infoOverlay() const { return infoOverlay_; }
    void showKeybindingHelp();
    void showCommandIndex();
    void showCommandHelp(const CommandInfo& info);
    void showInterfaceLegend();
    // Open the command transcript (interactive input + output) overlay.
    void showCommandTranscript();
    // Record one interactive command line and its result message in the
    // transcript (input lines are stored ':'-prefixed). Multi-line output is
    // split; the buffer is capped.
    void recordTranscript(const std::string& input, const std::string& output);

private:
    // Activate the centered overlay with the given content. In headless
    // mode (no TUI), `headlessTitle` is non-empty and the lines are
    // printed to stdout instead — set it empty to skip the headless
    // branch (e.g. `?` cheat sheet, which is interactive-only).
    void activateOverlay(std::string title,
                         std::vector<std::string> lines,
                         std::vector<int> colors = {},
                         std::string headlessTitle = "");

public:

    // Load a file into the current tab
    std::string loadFile(const std::string& path);

    // Result of running commands from a script stream.
    struct ScriptRunResult {
        int count = 0;        // commands attempted (non-blank, non-comment)
        int failures = 0;     // failed commands
        // Per-failure detail for `path:line: cmd: reason` reporting
        // (issue #80). firstFail/failLine are kept for strict-abort
        // back-compat and ergonomic single-fail messages.
        struct Failure {
            int lineNum;          // 1-indexed line in the source file
            std::string srcLine;  // original (pre-expansion) text
            std::string reason;
        };
        std::vector<Failure> failureList;
        std::string sourcePath;   // script file path (or "<stdin>", "init.mt", …)
        int srcLineOffset = 1;    // buffer-idx 0 → file line `srcLineOffset`
        std::string firstFail;    // first failure message (if any)
        std::string failLine;     // the offending line for the first failure
        std::string lastMsg;      // last non-empty result message
        bool stopped = false;     // true if strict mode aborted early

        int firstFailureLine() const {
            return failureList.empty() ? 0 : failureList.front().lineNum;
        }
        int lastFailureLine() const {
            return failureList.empty() ? 0 : failureList.back().lineNum;
        }
        // `<path>:<line>: `<cmd>`: <reason>` — used by the in-band
        // stderr stream (issue #90) and the post-run failure dump
        // for TUI mode. Single source so the two stay in lockstep.
        std::string formatFailure(const Failure& f) const {
            return sourcePath + ":" + std::to_string(f.lineNum) +
                   ": `" + f.srcLine + "`: " + f.reason;
        }
    };

    // Run commands from a stream. Skips blank lines and `#` comments.
    // If strict is true, stops on the first failure. `sourcePath` is the
    // human-readable label used in failure reports (file path, "<stdin>", …).
    ScriptRunResult runScriptStream(std::istream& in, bool strict = false,
                                    const std::string& sourcePath = "");
    // Variant with call-site KEY=VALUE args (issue #67). When `args` is
    // non-empty OR the script declares `#!molterm scope=local`, the
    // runner pushes a fresh register/env frame for the script body so
    // its `:let` writes don't leak into the caller's namespace. Names
    // explicitly :export'd (or listed in the shebang `export=` field)
    // flow back into the caller frame on script exit; everything else
    // is discarded with the frame.
    ScriptRunResult runScriptStream(std::istream& in, bool strict,
                                    const std::unordered_map<std::string, std::string>& args,
                                    const std::string& sourcePath = "");

    // Expand ${VAR} references against the in-process scriptEnv_ map (set by
    // :setenv) with a fall-through to the OS environment. Unset → empty.
    // Backslash-escapes the dollar: `\$X` → literal `$X`.
    std::string expandScriptVars(const std::string& line) const;
    // Active script env (top of envStack_). `:setenv` reads/writes this.
    std::unordered_map<std::string, std::string>& scriptEnv() { return envStack_.back(); }
    const std::unordered_map<std::string, std::string>& scriptEnv() const { return envStack_.back(); }

    BgMode bgMode() const { return view_.bgMode(); }
    void setBgMode(BgMode m) { view_.setBgMode(m); }
    void setBgCustomRGB(uint8_t r, uint8_t g, uint8_t b) { view_.setBgCustomRGB(r, g, b); }
    uint8_t bgCustomR() const { return view_.bgCustomR(); }
    uint8_t bgCustomG() const { return view_.bgCustomG(); }
    uint8_t bgCustomB() const { return view_.bgCustomB(); }
    // Push the current bgMode onto a PixelCanvas before clear()/savePNG().
    void applyBgMode(class PixelCanvas& pc) const;

    bool verbose() const { return verbose_; }
    void setVerbose(bool v) { verbose_ = v; }
    // Helps diagnose empty-canvas screenshots from headless scripts.
    // basisAtoms ≥ 0 → that count went into the camera-basis bbox (the
    // selection's atoms across all in-scope objects). basisAtoms < 0 →
    // the call didn't have a bbox basis (e.g. `:focus off`); fall back
    // to the current object's total atom count so the log line stays
    // structurally identical.
    void logViewState(const struct ParsedCommand& cmd, int basisAtoms = -1) const;
    void logSelectionInfo(const std::string& name,
                          const std::string& expr,
                          const class Selection& sel,
                          const class MolObject& obj) const;
    void logAlignPair(const std::string& mobile,
                      const std::string& target,
                      bool complex,
                      const struct AlignResult& r) const;
    void logRenderStats(int pixW, int pixH, int dpi,
                        int visibleAtoms,
                        double elapsedSec) const;
    // Atoms that would actually be rasterized: sum over visible objects of
    // atoms in any visible repr (approximated by atom count when no
    // per-atom mask, otherwise atoms that pass the mask).
    int countVisibleAtoms() const;

    // Renderer switching
    void setRenderer(RendererType type);
    RendererType rendererType() const { return rendererType_; }

    // Canvas access
    Canvas* canvas() { return canvas_.get(); }
    // Parse a selection expression against a MolObject, recording the result
    // as $sele. Public so command helpers (e.g. :measure/:bond selection
    // endpoints) can resolve selections outside Application's own members.
    Selection parseSelection(const std::string& expr, const MolObject& mol);

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

    // Command scope: whether per-object commands (color/show/hide/...)
    // fan out across every object in the tab or stay on the current one.
    // commandScope_ is the persistent setting (`:set scope ...`);
    // scopeOverride_ is a transient one-off used by the `:!` bang-prefix.
    ScopeMode commandScope() const { return commandScope_; }
    void setCommandScope(ScopeMode m) { commandScope_ = m; }
    ScopeMode effectiveCommandScope() const {
        return scopeOverride_.has_value() ? *scopeOverride_ : commandScope_;
    }
    void setScopeOverride(ScopeMode m) { scopeOverride_ = m; }
    void clearScopeOverride() { scopeOverride_.reset(); }
    bool hasScopeOverride() const { return scopeOverride_.has_value(); }

    bool running() const { return running_; }

    // Settings. The visual/overlay appearance knobs (fog, outlines, stereo,
    // labels, annotations, arrows, overlay sizing, custom colors) live on
    // view() — a cohesive ViewSettings. The accessors below delegate to it for
    // back-compat; new code may go through view() directly.
    using ColorRGB = ViewSettings::ColorRGB;
    using OverlaySizeMode = ViewSettings::OverlaySizeMode;
    ViewSettings& view() { return view_; }
    const ViewSettings& view() const { return view_; }

    // Focus-mode state cluster (Mol*-style click-to-focus). Command handlers
    // read/write the tunables through here (`:set focus_*`); enterFocus/
    // exitFocus drive the snapshot/restore.
    FocusState& focus() { return focus_; }
    const FocusState& focus() const { return focus_; }

    // Inter-chain interface overlay cluster (`:interface`, `:set interface_*`).
    // recomputeInterface() recomputes it against the current object.
    InterfaceOverlay& interface() { return interface_; }
    const InterfaceOverlay& interface() const { return interface_; }

    float fogStrength() const { return view_.fogStrength(); }
    void setFogStrength(float s) { view_.setFogStrength(s); }
    bool outlineEnabled() const { return view_.outlineEnabled(); }
    void setOutlineEnabled(bool v) { view_.setOutlineEnabled(v); }
    bool screenshotOverlay() const { return view_.screenshotOverlay(); }
    void setScreenshotOverlay(bool v) { view_.setScreenshotOverlay(v); }
    float outlineThreshold() const { return view_.outlineThreshold(); }
    void setOutlineThreshold(float t) { view_.setOutlineThreshold(t); }
    float outlineDarken() const { return view_.outlineDarken(); }
    void setOutlineDarken(float d) { view_.setOutlineDarken(d); }
    bool autoCenter() const { return autoCenter_; }
    void setForcedProtocol(GraphicsProtocol p) { forcedProtocol_ = p; }
    void setAutoCenter(bool v) { autoCenter_ = v; }

    StereoMode stereoMode() const { return view_.stereoMode(); }
    void setStereoMode(StereoMode m) { view_.setStereoMode(m); }
    float stereoAngle() const { return view_.stereoAngle(); }
    void setStereoAngle(float deg) { view_.setStereoAngle(deg); }

    int labelFontSize() const { return view_.labelFontSize(); }
    void setLabelFontSize(int px) { view_.setLabelFontSize(px); }
    // Max rows of the live transcript shown above the command line
    // (`:set transcript_lines <n>`). Range-checked by the :set handler.
    int transcriptHintLines() const { return transcriptHintLines_; }
    void setTranscriptHintLines(int n) { transcriptHintLines_ = n; }
    int annotationFontSize() const { return view_.annotationFontSize(); }
    void setAnnotationFontSize(int px) { view_.setAnnotationFontSize(px); }
    int annotationLineWidth() const { return view_.annotationLineWidth(); }
    void setAnnotationLineWidth(int px) { view_.setAnnotationLineWidth(px); }
    float overlayScale() const { return view_.overlayScale(); }
    void setOverlayScale(float s) { view_.setOverlayScale(s); }

    OverlaySizeMode overlaySizeMode() const { return view_.overlaySizeMode(); }
    void setOverlaySizeMode(OverlaySizeMode m) { view_.setOverlaySizeMode(m); }
    int referenceCanvasHeight() const { return view_.referenceCanvasHeight(); }
    void setReferenceCanvasHeight(int h) { view_.setReferenceCanvasHeight(h); }
    int liveDpi() const { return view_.liveDpi(); }
    void setLiveDpi(int dpi) { view_.setLiveDpi(dpi); }
    float computeRenderScale(int canvasHeight, int dpi) const {
        return view_.computeRenderScale(canvasHeight, dpi);
    }

    bool labelOutline() const { return view_.labelOutline(); }
    void setLabelOutline(bool on) { view_.setLabelOutline(on); }
    int labelOutlineThickness() const { return view_.labelOutlineThickness(); }
    void setLabelOutlineThickness(int t) { view_.setLabelOutlineThickness(t); }
    bool annotationOutline() const { return view_.annotationOutline(); }
    void setAnnotationOutline(bool on) { view_.setAnnotationOutline(on); }
    int annotationOutlineThickness() const { return view_.annotationOutlineThickness(); }
    void setAnnotationOutlineThickness(int t) { view_.setAnnotationOutlineThickness(t); }

    const std::optional<ColorRGB>& labelColor()             const { return view_.labelColor(); }
    const std::optional<ColorRGB>& annotationColor()        const { return view_.annotationColor(); }
    const std::optional<ColorRGB>& measurementLineColor()   const { return view_.measurementLineColor(); }
    const std::optional<ColorRGB>& outlineColor()           const { return view_.outlineColor(); }
    const std::optional<ColorRGB>& labelOutlineColor()      const { return view_.labelOutlineColor(); }
    const std::optional<ColorRGB>& annotationOutlineColor() const { return view_.annotationOutlineColor(); }
    void setLabelColor(std::optional<ColorRGB> c)             { view_.setLabelColor(c); }
    void setAnnotationColor(std::optional<ColorRGB> c)        { view_.setAnnotationColor(c); }
    void setMeasurementLineColor(std::optional<ColorRGB> c)   { view_.setMeasurementLineColor(c); }
    void setOutlineColor(std::optional<ColorRGB> c)           { view_.setOutlineColor(c); }
    void setLabelOutlineColor(std::optional<ColorRGB> c)      { view_.setLabelOutlineColor(c); }
    void setAnnotationOutlineColor(std::optional<ColorRGB> c) { view_.setAnnotationOutlineColor(c); }
    static ColorRGB autoOutlineColor(uint8_t r, uint8_t g, uint8_t b) {
        return ViewSettings::autoOutlineColor(r, g, b);
    }
    OutlineMode outlineMode() const { return view_.outlineMode(); }
    void setOutlineMode(OutlineMode m) { view_.setOutlineMode(m); }
    int effectiveLabelFontSize() const { return view_.effectiveLabelFontSize(); }
    int effectiveAnnotationFontSize() const { return view_.effectiveAnnotationFontSize(); }
    int effectiveAnnotationLineWidth() const { return view_.effectiveAnnotationLineWidth(); }
    int effectiveArrowThickness() const { return view_.effectiveArrowThickness(); }
    int effectiveArrowHeadSize() const { return view_.effectiveArrowHeadSize(); }
    float renderScaleHint() const { return view_.renderScaleHint(); }
    void setRenderScaleHint(float s) { view_.setRenderScaleHint(s); }
    // Transient per-render scale hint, restored on scope exit. Use this RAII
    // helper instead of poking setRenderScaleHint directly.
    struct RenderScaleScope {
        Application& app;
        float saved;
        RenderScaleScope(Application& a, int canvasH, int dpi)
            : app(a), saved(a.view_.renderScaleHint()) {
            a.view_.setRenderScaleHint(a.view_.computeRenderScale(canvasH, dpi));
        }
        ~RenderScaleScope() { app.view_.setRenderScaleHint(saved); }
        RenderScaleScope(const RenderScaleScope&)            = delete;
        RenderScaleScope& operator=(const RenderScaleScope&) = delete;
        RenderScaleScope(RenderScaleScope&&)                 = delete;
        RenderScaleScope& operator=(RenderScaleScope&&)      = delete;
    };

    // Recompute the inter-chain interface overlay against the current
    // object using the last-used cutoff. Returns false (and clears the
    // overlay) when the new current object has no inter-chain contacts.
    // Idempotent — safe to call repeatedly.
    bool recomputeInterface();

    // Hook called whenever the current object changes (via :object,
    // NextObject/PrevObject hotkeys, etc.). Refreshes per-object overlays
    // that would otherwise show stale state from a different mol.
    void onCurrentObjectChanged();

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
    // Typed registers populated by `:let <name> = <expr>` (#32, #33, #35).
    // Stack of frames — frame[0] is the interactive/top-level frame and
    // is always present. `:run` with `#!molterm scope=local` pushes a
    // new frame on entry and pops it on exit (issue #67). Names starting
    // with `_` never escape a popped frame; explicit `:export` lists
    // the survivors. Without a shebang, scripts inherit frame[0] —
    // back-compat for pre-#67 scripts.
    std::vector<std::unordered_map<std::string, Register>> regStack_{1};
    // The script execution engine. Drives runScriptStream / interactive
    // command lines; owns the :def user-function table + recursion guard.
    std::unique_ptr<ScriptRunner> script_;
public:
    std::unordered_map<std::string, Register>& registers() { return regStack_.back(); }
    const std::unordered_map<std::string, Register>& registers() const { return regStack_.back(); }
    // Push a new register + env + exports frame for a `scope=local`
    // script. The caller passes `seedEnv` (typically the call-site
    // KEY=VALUE args) which is laid over a fresh copy of the parent env
    // so the child can read inherited vars but can't mutate the caller's
    // env on assignment. `seedExports` is the comma-list parsed from
    // `#!molterm export=…`; the running script can grow it via :export.
    // Returns the depth after push (always >= 2).
    int pushScriptFrame(const std::unordered_map<std::string, std::string>& seedEnv,
                        const std::vector<std::string>& seedExports);
    // Pop the topmost frame. Names in this frame's export list are
    // copied into the caller frame; `_`-prefixed names are rejected
    // silently (private). Frame[0] is never popped. Returns the
    // number of exported registers actually copied.
    int popScriptFrame();
    // Add a name to the current frame's export list. Called by
    // `:expose <name>` inside a running script. The same name may be
    // added multiple times; popScriptFrame is the single chokepoint
    // that filters `_`-prefixed (private) entries.
    enum class ExportResult { Ok, NoFrame, Empty, Private };
    ExportResult markExport(const std::string& name);

    // RAII wrapper around push/popScriptFrame so all early-return paths
    // in runScriptStream (errors, strict abort, exceptions) pop the
    // frame exactly once. Construct with `active = false` for the
    // script-inherits-caller path; the destructor is a no-op then.
    struct ScriptFrameGuard {
        Application& app;
        bool active;
        ScriptFrameGuard(Application& a, bool on,
                         const std::unordered_map<std::string, std::string>& seedEnv,
                         const std::vector<std::string>& seedExports)
            : app(a), active(on) {
            if (active) app.pushScriptFrame(seedEnv, seedExports);
        }
        ~ScriptFrameGuard() { if (active) app.popScriptFrame(); }
        ScriptFrameGuard(const ScriptFrameGuard&)            = delete;
        ScriptFrameGuard& operator=(const ScriptFrameGuard&) = delete;
    };
private:

    // Command scope (`:set scope all|current`, default all). The optional
    // override slot is set by the `:!` bang-prefix dispatcher around a
    // single command and cleared right after.
    ScopeMode commandScope_ = ScopeMode::All;
    std::optional<ScopeMode> scopeOverride_;

    InfoOverlay infoOverlay_;

    // Rolling transcript of interactive command lines + their result output,
    // shown by :messages. Input lines are stored ':'-prefixed; output lines
    // indented. Capped to the most recent kTranscriptMax lines.
    std::vector<std::string> cmdTranscript_;
    // Scroll offset for the live transcript shown above the active command line
    // (0 = most recent; PgUp/PgDn page back/forward). Reset when `:` opens.
    int transcriptHintScroll_ = 0;
    // Max rows the transcript hint renders (`:set transcript_lines`).
    int transcriptHintLines_ = 8;

    // Contact map panel (`:contactmap`) — shared with the interface overlay's
    // computation, so it stays on Application rather than inside interface_.
    ContactMapPanel contactMapPanel_;
    // Inter-chain interface overlay state cluster (`:interface`): cached
    // contacts/mask, the overlay renderer + zoom gate, and the `:set interface_*`
    // tunables (InterfaceOverlay.h). recomputeInterface() drives it; reached
    // publicly via interface().
    InterfaceOverlay interface_;
    // Focus mode (Mol*-style click-to-focus): the saved view + visibility
    // state plus the user tunables, as one cohesive cluster (FocusState.h).
    // enterFocus/exitFocus below snapshot into / restore from focus_; the
    // focus().atomMask drives applyFocusDim. Reached publicly via focus().
    FocusState        focus_;
    // issue #98 — see hasViewFit()/setViewFit() for the rationale.
    struct ViewFit {
        bool active = false;
        std::vector<float> xs, ys, zs;  // subject atom coords (world Å)
        float fill = 0.9f;              // fraction of frame to fill
        float pad = 1.0f;               // Å padding added to the extent
        float minExtent = 1.0f;         // Å floor (avoids over-zoom on a point)
    };
    ViewFit           viewFit_;

    // Inspect / pick state (mouse-only)
    InspectLevel inspectLevel_ = InspectLevel::Atom;
    PickMode pickMode_ = PickMode::Inspect;
    int pickedAtomIdx_ = -1;           // nearest atom index from last click

    // Pick registers: pk1→pk4, rotates on each inspect click (like PyMOL)
    int pickRegs_[4] = {-1, -1, -1, -1};
    int pickNext_ = 0;

    std::vector<std::unordered_map<std::string, std::string>> envStack_{1};
    // Parallel to regStack_/envStack_: names the current script frame
    // wants to export into its caller on pop. Frame[0]'s slot is
    // unused (top level has no caller).
    std::vector<std::vector<std::string>> exportStack_{ {} };

    bool verbose_ = false;

public:
    // Atom labels, scoped per object. Atom indices are per-MolObject, so a
    // flat list silently aliases atom #idx across objects — labels would
    // bleed onto the wrong atoms after an object swap and `:label
    // <obj>/(...)` could not target a non-current object. Keying by object
    // name fixes both and lets every object's labels render in a multi-
    // object overlay (issue #101).
    struct ObjectLabels {
        std::vector<int> atoms;                       // atom indices in that object
        std::unordered_map<int, std::string> text;    // per-atom override text
    };
private:
    std::map<std::string, ObjectLabels> labelsByObject_;  // MolObject::name() → labels
    // Template applied when no per-atom override exists. Empty = built-in
    // default ("{resname}{resseq}"). Tokens: {resname}, {resseq}/{seqid},
    // {chain}, {name}, {element}, {restype} (1-letter AA code).
    std::string labelFormat_;

    // Persistent measurements: pairs/triples/quads of atom indices + label.
    // `label` is the auto-formatted value ("5.85A" / "127.3°"); `caption` is
    // the optional user string from `:measure ... = "..."` for figure prep.
    // `obj` names the owning object — atom indices are per-object, so the
    // measurement renders against and computes from that object's atoms,
    // even when it isn't the current one (issue #101).
    struct Measurement {
        std::vector<int> atoms;
        std::string label;
        std::string caption;
        std::string obj;
        std::string displayLabel() const {
            return caption.empty() ? label : label + " " + caption;
        }
    };
    std::vector<Measurement> measurements_;

    // Free-position labels — text not anchored to an atom (issue #34).
    // Three anchor modes:
    //   Corner : pinned to a viewport corner with a small inset.
    //   Screen : normalised viewport coords (fx, fy) ∈ [0, 1].
    //   World  : explicit 3D position projected through the camera each
    //            frame so the label tracks rotation/zoom.
public:
    enum class FreeLabelAnchor { Corner, Screen, World };
    enum class FreeLabelCorner { TopLeft, TopRight, BottomLeft, BottomRight };
    struct FreeLabel {
        FreeLabelAnchor anchor = FreeLabelAnchor::Corner;
        FreeLabelCorner corner = FreeLabelCorner::TopLeft;
        float fx = 0.0f, fy = 0.0f;
        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
        std::string text;
    };
private:
    std::vector<FreeLabel> freeLabels_;

    // Persistent solid arrows / axes (issue #38). Distinct from :measure
    // (dashed line + numeric distance caption) — solid arrow with a
    // triangular head at endpoint B is the "this is an axis" primitive.
    // Endpoints stored in world coordinates: atom-anchored creates resolve
    // once at :arrow time and don't re-track if atoms move (re-issue if
    // you re-align). Two construction paths today:
    //   :arrow <serial1> <serial2> [= "label"]
    //   :arrow $regA $regB [= "label"]      (vec3 / point registers)
    //   :axis  $pcaReg [= "label"]          (centered, axis1, ±2σ)
public:
    struct ArrowOverlay {
        std::array<float, 3> a{};
        std::array<float, 3> b{};
        std::string caption;
        // Per-arrow color override (issue #104). Falls back to the global
        // arrowColor_ (`:set arrow_color`), then the default yellow, when unset.
        std::optional<std::array<uint8_t, 3>> color;
    };
private:
    std::vector<ArrowOverlay> arrows_;

public:
    int pickReg(int n) const { return (n >= 0 && n < 4) ? pickRegs_[n] : -1; }
    // True iff drawPixelOverlay would draw a yellow ring this frame —
    // i.e. `$sele` is non-empty or any pk1..pk4 register is set.
    bool hasSelectionHighlight() const {
        auto it = namedSelections_.find(kSele);
        if (it != namedSelections_.end() && !it->second.empty()) return true;
        for (int p = 0; p < 4; ++p) if (pickRegs_[p] >= 0) return true;
        return false;
    }
    std::map<std::string, ObjectLabels>& labelsByObject() { return labelsByObject_; }
    const std::map<std::string, ObjectLabels>& labelsByObject() const { return labelsByObject_; }
    // Total labeled atoms across every object — for status messages.
    size_t labelCount() const {
        size_t n = 0;
        for (const auto& [name, ls] : labelsByObject_) n += ls.atoms.size();
        return n;
    }
    void clearAtomLabels() { labelsByObject_.clear(); }
    // Add `idx` to object `objName`'s label set (deduped). When `text` is
    // non-null it sets the per-atom override; when null it drops any prior
    // override so the atom falls back to labelFormat_/default.
    void addAtomLabel(const std::string& objName, int idx, const std::string* text);
    // Remove `idxs` from object `objName`'s labels. Returns the count removed.
    size_t removeAtomLabels(const std::string& objName, const std::set<int>& idxs);
    const std::string& labelFormat() const { return labelFormat_; }
    void setLabelFormat(std::string fmt) { labelFormat_ = std::move(fmt); }
    // Resolve the displayed text for atom `idx` of `obj`: per-atom override
    // (from `labels`) → labelFormat_ → default ("{resname}{resseq}").
    std::string resolveLabel(const MolObject& obj, const ObjectLabels& labels,
                             int idx) const;
    std::vector<Measurement>& measurements() { return measurements_; }
    // Free-label storage — type defined above near `freeLabels_` so the
    // member declaration order satisfies "use after declaration" inside
    // the class body.
    std::vector<FreeLabel>& freeLabels() { return freeLabels_; }
    const std::vector<FreeLabel>& freeLabels() const { return freeLabels_; }
    std::vector<ArrowOverlay>& arrows() { return arrows_; }
    const std::vector<ArrowOverlay>& arrows() const { return arrows_; }
    const std::optional<ColorRGB>& arrowColor() const { return view_.arrowColor(); }
    void setArrowColor(std::optional<ColorRGB> c) { view_.setArrowColor(c); }
    int arrowThickness() const { return view_.arrowThickness(); }
    void setArrowThickness(int t) { view_.setArrowThickness(t); }
    int arrowHeadSize() const { return view_.arrowHeadSize(); }
    void setArrowHeadSize(int s) { view_.setArrowHeadSize(s); }
    // Drop every per-atom label + every measurement/angle/dihedral entry.
    // Shared by `:overlay clear` and `:run --fresh`; keeping them in lockstep
    // here means a future overlay-list addition (e.g. arrows, axes) is wired
    // into both call sites by editing one method instead of two.
    void clearOverlayAnnotations() {
        measurements_.clear();
        labelsByObject_.clear();
        freeLabels_.clear();
        arrows_.clear();
    }
    bool overlayVisible_ = true;

    // Wipe both transient selection overlays at once: `$sele` and the
    // pk1-pk4 pick registers. Both render as yellow rings; clearing
    // them gives the user a clean slate. Returns the pre-clear sizes
    // (sele atoms, live pk registers) so callers can build a status
    // message without re-querying. Backs `gx` and `:select clear`.
    std::pair<std::size_t, int> clearVisualSelection();
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
    // Visual / overlay appearance knobs (fog, outlines, stereo, labels,
    // annotations, arrows, overlay sizing, custom colors). Reached via view();
    // the screenshotOverlay note below applies to view_.screenshotOverlay():
    // include the $sele / pk halo in :screenshot PNGs (default off so editor
    // HUD state doesn't bake into figure exports; live + stereo always show
    // it — issue #96).
    ViewSettings view_;
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
    // Per-area command registration, decomposed from registerCommands() into
    // src/cmd/commands/*.cpp. Member functions so handler lambdas keep full
    // access to Application's private state. CommandRegistry is forward-declared.
    void registerSessionCommands(CommandRegistry& reg);
    void registerFilesCommands(CommandRegistry& reg);
    void registerDisplayCommands(CommandRegistry& reg);
    void registerViewCommands(CommandRegistry& reg);
    void registerSettingsCommands(CommandRegistry& reg);
    void registerObjectCommands(CommandRegistry& reg);
    void registerScriptingCommands(CommandRegistry& reg);
    void registerSelectionCommands(CommandRegistry& reg);
    void registerAlignmentCommands(CommandRegistry& reg);
    void registerFetchCommands(CommandRegistry& reg);
    void registerAnnotationCommands(CommandRegistry& reg);
    void registerMeasurementCommands(CommandRegistry& reg);
    void buildProjCache();

    // Draw labels, measurement dashed lines/labels, and sele/pk highlight
    // rings into the given pixel canvas. Used by the live render path's
    // pixel branch and by `:screenshot` so offscreen exports include the
    // same overlays the user sees on screen. Camera projection must be
    // prepared for `pc`'s pixel space before calling.
    // Label/annotation text dispatch — resolves color (override or default
    // white/yellow), outline (labelOutline_ / annotationOutline_), and
    // font size in one place so the half-dozen overlay call sites don't
    // each re-implement the if-color-else-default + outline branch.
    void paintLabelText(class PixelCanvas& pc, int sx, int sy, float depth,
                        const std::string& text);
    void paintAnnotationText(class PixelCanvas& pc, int sx, int sy, float depth,
                             const std::string& text);
    // includeSeleHighlights gates the yellow $sele/pk1..pk4 ring layer
    // — labels and measurements always draw. :screenshot passes false
    // by default so figure exports don't carry the transient selection
    // halo; opt back in with `:set screenshot_overlay on` (issue #96).
    void drawPixelOverlay(class PixelCanvas& pc, bool includeSeleHighlights = true);
    // Render free-position labels (corner / screen / world anchors). Split
    // out so the object-less early-return in drawPixelOverlay can still
    // emit them, and so the screenshot path picks them up via the same
    // call. Uses labelColor_ + effectiveLabelFontSize() like atom labels.
    void drawFreeLabelsPixel(class PixelCanvas& pc, int subW, int subH,
                             class Camera& cam);
    // Render persistent solid arrows + captions (issue #38). Solid line
    // with a triangular arrowhead at endpoint B. Style follows
    // arrow_color / arrow_thickness / arrow_head_size; caption uses
    // annotation_color + effectiveAnnotationFontSize().
    void drawArrowsPixel(class PixelCanvas& pc, int subW, int subH,
                         class Camera& cam);

    // Set up the camera + projection for one eye of a stereoscopic render.
    // eyePass = 0 → left half, 1 → right half. With stereoMode_ == Off it
    // just calls prepareProjection on the full canvas. Returns the camera
    // rotation that was active on entry; pass it to restoreStereoCamera()
    // after the eye's draw calls so the next eye (or non-stereo callers)
    // see the original rotation.
    std::array<float, 9> setupStereoEye(int eyePass,
                                        int totalSubW, int subH,
                                        float aspectYX);
    void restoreStereoCamera(const std::array<float, 9>& savedRot);
    int stereoEyeCount() const {
        return view_.stereoMode() == StereoMode::Off ? 1 : 2;
    }

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
    bool focusActive() const { return focus_.active(); }

    // ── View-fit intent (issue #98) ────────────────────────────────────
    // :focus/:zoom/:orient remember the world-space points they framed and
    // the target fill fraction so the fit can be *recomputed* for whatever
    // canvas actually renders — most importantly a :screenshot at a
    // different resolution/aspect than the live viewport. Without this the
    // zoom is a single scalar calibrated to one reference viewport, so a
    // hi-DPI screenshot of the same view leaves the subject small with dead
    // margins. The fit measures the subject's *projected* extent under the
    // current rotation and sizes it to `fill` of the frame in the more
    // constrained dimension, so it fills the frame at any resolution.
    bool hasViewFit() const { return viewFit_.active; }
    // Store a fit intent (copying the coords) and apply it immediately to
    // the best-available live canvas. Reads the camera's current rotation +
    // center, so callers must set those first.
    void setViewFit(std::vector<float> xs, std::vector<float> ys,
                    std::vector<float> zs, float fill, float pad,
                    float minExtent);
    void clearViewFit();
    // Compute the zoom that fits the current intent to a W×H sub-pixel
    // canvas (aspectYX = Y/X sub-pixel aspect); returns the camera's current
    // zoom when no intent is active. Does not mutate the camera.
    float computeFitZoom(int W, int H, float aspectYX) const;
    // Apply computeFitZoom() to the camera for the given canvas.
    void applyViewFit(int W, int H, float aspectYX);
    // Expand a single clicked atom into the focus subject according to
    // `focus_.granularity`. Used by both Action::FocusPick (F key) and
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

// Free-label / bg-mode helpers shared between Application and the annotation /
// settings command modules (definitions in Application.cpp). Declared after the
// class so they can name its nested FreeLabel* types.
std::optional<Application::FreeLabelCorner> parseCornerName(const std::string& w);
std::size_t clearFreeLabelsByAnchor(std::vector<Application::FreeLabel>& fls,
                                    Application::FreeLabelAnchor anchor,
                                    std::optional<Application::FreeLabelCorner> corner);
const char* bgModeName(BgMode m);

} // namespace molterm
