#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "molterm/app/CommandScope.h"
#include "molterm/app/TabManager.h"
#include "molterm/app/TabViewState.h"
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

// PNG / pixel-canvas background. `Transparent` writes alpha=0 for untouched
// pixels (deterministic, via canvas-tracked clear color + colorIds_ mask —
// not the legacy "RGB==(0,0,0)" heuristic that flipped between canvas sizes).
// Background color modes for screenshots and the live canvas.
//   Transparent : touched-pixel mask → PNG alpha channel
//   White       : flat 255,255,255 (publication light theme)
//   Black       : flat 0,0,0       (publication dark theme)
//   Custom      : user-supplied RGB triple stored in bgCustomR/G/B_ on
//                 Application — set via `:set bg "#RRGGBB"` or
//                 `:set bg "rgb(R,G,B)"`. Always opaque; for transparency
//                 use BgMode::Transparent.
enum class BgMode { Transparent, White, Black, Custom };
// OutlineMode lives in molterm/render/OutlineMode.h — included via Canvas.h
// transitively. App-layer code uses the unqualified name `OutlineMode`.

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

private:
    // Block-aware script dispatcher (issue #68). Buffers the script
    // into a line vector then walks it with control-flow tracking —
    // `:if / :elseif / :else / :endif` skip inactive branches;
    // `:foreach VAR in LO..HI / :end` iterates a numeric range and
    // re-dispatches the body N times. Mutually recursive (foreach
    // body re-enters via the same dispatcher) but bounded by script
    // size, so stack depth = number of nested :foreach loops.
    bool dispatchScriptLines(const std::vector<std::string>& lines,
                             size_t lo, size_t hi,
                             ScriptRunResult& result, bool strict);
public:

    // Expand ${VAR} references against the in-process scriptEnv_ map (set by
    // :setenv) with a fall-through to the OS environment. Unset → empty.
    // Backslash-escapes the dollar: `\$X` → literal `$X`.
    std::string expandScriptVars(const std::string& line) const;
    // Active script env (top of envStack_). `:setenv` reads/writes this.
    std::unordered_map<std::string, std::string>& scriptEnv() { return envStack_.back(); }
    const std::unordered_map<std::string, std::string>& scriptEnv() const { return envStack_.back(); }

    BgMode bgMode() const { return bgMode_; }
    void setBgMode(BgMode m) { bgMode_ = m; }
    // Custom bg RGB used when bgMode_ == BgMode::Custom. Setter also
    // flips the mode so callers don't have to remember to.
    void setBgCustomRGB(uint8_t r, uint8_t g, uint8_t b) {
        bgCustomR_ = r; bgCustomG_ = g; bgCustomB_ = b;
        bgMode_ = BgMode::Custom;
    }
    uint8_t bgCustomR() const { return bgCustomR_; }
    uint8_t bgCustomG() const { return bgCustomG_; }
    uint8_t bgCustomB() const { return bgCustomB_; }
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

    // Settings
    float fogStrength() const { return fogStrength_; }
    void setFogStrength(float s) { fogStrength_ = s; }
    bool outlineEnabled() const { return outlineEnabled_; }
    void setOutlineEnabled(bool v) { outlineEnabled_ = v; }
    bool screenshotOverlay() const { return screenshotOverlay_; }
    void setScreenshotOverlay(bool v) { screenshotOverlay_ = v; }
    float outlineThreshold() const { return outlineThreshold_; }
    void setOutlineThreshold(float t) { outlineThreshold_ = t; }
    float outlineDarken() const { return outlineDarken_; }
    void setOutlineDarken(float d) { outlineDarken_ = d; }
    bool autoCenter() const { return autoCenter_; }
    void setForcedProtocol(GraphicsProtocol p) { forcedProtocol_ = p; }
    void setAutoCenter(bool v) { autoCenter_ = v; }

    StereoMode stereoMode() const { return stereoMode_; }
    void setStereoMode(StereoMode m) { stereoMode_ = m; }
    float stereoAngle() const { return stereoAngle_; }
    void setStereoAngle(float deg) { stereoAngle_ = deg; }

    // Overlay sizing — labels, measurement dashes/captions, selection rings.
    // The cell-derived defaults render fine on small canvases but vanish at
    // hi-DPI screenshot sizes (issue #25). Each knob has its own setting;
    // overlayScale_ is a global multiplier applied on top.
    int labelFontSize() const { return labelFontSize_; }
    void setLabelFontSize(int px) { labelFontSize_ = px; }
    int annotationFontSize() const { return annotationFontSize_; }
    void setAnnotationFontSize(int px) { annotationFontSize_ = px; }
    int annotationLineWidth() const { return annotationLineWidth_; }
    void setAnnotationLineWidth(int px) { annotationLineWidth_ = px; }
    float overlayScale() const { return overlayScale_; }
    void setOverlayScale(float s) { overlayScale_ = s; }

    // Per-render auto-scale model (issue #48). With Pixels (default), the
    // *_font_size knobs are interpreted as raw screen pixels — what they
    // were before #48 — so existing scripts still render identically.
    // Physical treats them as point sizes (1 pt = liveDpi_/72 px live)
    // and rescales by the screenshot's DPI so a `:screenshot W H 300`
    // looks like the same figure printed at 300 dpi. Relative interprets
    // them as "pixels at referenceCanvasHeight_ tall canvas" and rescales
    // by canvasH/refH so a rough 1280x960 render and a final 2400x1800
    // render show labels at the same fraction of the canvas.
    enum class OverlaySizeMode { Pixels, Physical, Relative };
    OverlaySizeMode overlaySizeMode() const { return overlaySizeMode_; }
    void setOverlaySizeMode(OverlaySizeMode m) { overlaySizeMode_ = m; }
    int referenceCanvasHeight() const { return referenceCanvasHeight_; }
    void setReferenceCanvasHeight(int h) { referenceCanvasHeight_ = h; }
    int liveDpi() const { return liveDpi_; }
    void setLiveDpi(int dpi) { liveDpi_ = dpi; }
    // Compute the auto-scale multiplier for the given render context.
    // Returns 1.0 in Pixels mode (back-compat) or when canvas/dpi look
    // bogus, so callers don't have to special-case those.
    float computeRenderScale(int canvasHeight, int dpi) const;

    // Text outline / halo for label and annotation legibility (issue #49).
    // When enabled, drawTextOutlinedRGB paints a `*OutlineThickness_`-wide
    // halo around each glyph in `*OutlineColor_` before the body color, so
    // text reads cleanly against any background. Auto color (nullopt)
    // picks white-on-dark / black-on-light against the body color.
    bool labelOutline() const { return labelOutline_; }
    void setLabelOutline(bool on) { labelOutline_ = on; }
    int labelOutlineThickness() const { return labelOutlineThickness_; }
    void setLabelOutlineThickness(int t) { labelOutlineThickness_ = t; }
    bool annotationOutline() const { return annotationOutline_; }
    void setAnnotationOutline(bool on) { annotationOutline_ = on; }
    int annotationOutlineThickness() const { return annotationOutlineThickness_; }
    void setAnnotationOutlineThickness(int t) { annotationOutlineThickness_ = t; }

    // Custom overlay colors (issue #30, #31). Each is std::optional —
    // unset means "use the legacy default color constant" (white for
    // labels, yellow for annotation captions / measurement lines, plain
    // darken for outlines). Set via :set <kind>_color <named|#hex|rgb()>.
    using ColorRGB = std::array<uint8_t, 3>;
    const std::optional<ColorRGB>& labelColor()                 const { return labelColor_; }
    const std::optional<ColorRGB>& annotationColor()            const { return annotationColor_; }
    const std::optional<ColorRGB>& measurementLineColor()       const { return measurementLineColor_; }
    const std::optional<ColorRGB>& outlineColor()               const { return outlineColor_; }
    const std::optional<ColorRGB>& labelOutlineColor()          const { return labelOutlineColor_; }
    const std::optional<ColorRGB>& annotationOutlineColor()     const { return annotationOutlineColor_; }
    void setLabelColor(std::optional<ColorRGB> c)             { labelColor_             = c; }
    void setAnnotationColor(std::optional<ColorRGB> c)        { annotationColor_        = c; }
    void setMeasurementLineColor(std::optional<ColorRGB> c)   { measurementLineColor_   = c; }
    void setOutlineColor(std::optional<ColorRGB> c)           { outlineColor_           = c; }
    void setLabelOutlineColor(std::optional<ColorRGB> c)      { labelOutlineColor_      = c; }
    void setAnnotationOutlineColor(std::optional<ColorRGB> c) { annotationOutlineColor_ = c; }
    // Pick the auto outline color from a body color: white on dark
    // bodies, black on light bodies. Used when *OutlineColor_ is unset.
    static ColorRGB autoOutlineColor(uint8_t r, uint8_t g, uint8_t b) {
        int luma = (r * 299 + g * 587 + b * 114) / 1000;
        return luma < 128 ? ColorRGB{255, 255, 255} : ColorRGB{0, 0, 0};
    }
    OutlineMode outlineMode() const { return outlineMode_; }
    void setOutlineMode(OutlineMode m) { outlineMode_ = m; }
    // Pre-multiplied effective values used by the overlay renderer. The
    // renderScaleHint_ factor is set transiently by the render entry
    // points (live and screenshot) based on overlaySizeMode_ + the
    // current canvas size + DPI — see computeRenderScale().
    int effectiveLabelFontSize() const {
        return std::max(4, static_cast<int>(std::lround(labelFontSize_ * overlayScale_ * renderScaleHint_)));
    }
    int effectiveAnnotationFontSize() const {
        return std::max(4, static_cast<int>(std::lround(annotationFontSize_ * overlayScale_ * renderScaleHint_)));
    }
    int effectiveAnnotationLineWidth() const {
        return std::max(1, static_cast<int>(std::lround(annotationLineWidth_ * overlayScale_ * renderScaleHint_)));
    }
    int effectiveArrowThickness() const {
        return std::max(1, static_cast<int>(std::lround(arrowThickness_ * overlayScale_ * renderScaleHint_)));
    }
    int effectiveArrowHeadSize() const {
        return std::max(2, static_cast<int>(std::lround(arrowHeadSize_ * overlayScale_ * renderScaleHint_)));
    }
    // Set/clear the transient hint. Use the RAII helper below instead of
    // calling these directly so the previous value is always restored.
    float renderScaleHint() const { return renderScaleHint_; }
    void setRenderScaleHint(float s) { renderScaleHint_ = s; }
    struct RenderScaleScope {
        Application& app;
        float saved;
        RenderScaleScope(Application& a, int canvasH, int dpi)
            : app(a), saved(a.renderScaleHint_) {
            a.renderScaleHint_ = a.computeRenderScale(canvasH, dpi);
        }
        ~RenderScaleScope() { app.renderScaleHint_ = saved; }
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
    // Last cutoff (Å) used by `:interface on`. Persisted so that switching
    // the current object via `:object` / NextObject / PrevObject can
    // recompute the overlay against the new mol with the same setting.
    float interfaceCutoff_ = 4.5f;
    // Bitmask of InteractionType bits — only contacts with their bit set
    // get a dashed line drawn. Default hides Hydrophobic + Other (the
    // dense, low-information categories) so a typical complex is
    // readable without losing the legend's full per-type breakdown.
    std::uint8_t interfaceShowMask_ = kInterfaceShowSpecific;

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

    BgMode bgMode_ = BgMode::Transparent;
    uint8_t bgCustomR_ = 0, bgCustomG_ = 0, bgCustomB_ = 0;
    bool verbose_ = false;

    // Labels: atom indices to render text labels for
    std::vector<int> labelAtoms_;
    // Per-atom override text. If absent for an atom in `labelAtoms_`,
    // labelFormat_ (or the built-in default) supplies the text.
    std::unordered_map<int, std::string> labelText_;
    // Template applied when no per-atom override exists. Empty = built-in
    // default ("{resname}{resseq}"). Tokens: {resname}, {resseq}/{seqid},
    // {chain}, {name}, {element}, {restype} (1-letter AA code).
    std::string labelFormat_;

    // Persistent measurements: pairs/triples/quads of atom indices + label.
    // `label` is the auto-formatted value ("5.85A" / "127.3°"); `caption` is
    // the optional user string from `:measure ... = "..."` for figure prep.
    struct Measurement {
        std::vector<int> atoms;
        std::string label;
        std::string caption;
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
    };
private:
    std::vector<ArrowOverlay> arrows_;
    std::optional<std::array<uint8_t, 3>> arrowColor_;
    int arrowThickness_ = 2;
    int arrowHeadSize_  = 8;

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
    std::vector<int>& labelAtoms() { return labelAtoms_; }
    std::unordered_map<int, std::string>& labelText() { return labelText_; }
    const std::string& labelFormat() const { return labelFormat_; }
    void setLabelFormat(std::string fmt) { labelFormat_ = std::move(fmt); }
    // Resolve the displayed text for the label at `atomIdx` against the
    // currently-loaded object: per-atom override → labelFormat_ → default.
    std::string resolveLabel(int atomIdx) const;
    std::vector<Measurement>& measurements() { return measurements_; }
    // Free-label storage — type defined above near `freeLabels_` so the
    // member declaration order satisfies "use after declaration" inside
    // the class body.
    std::vector<FreeLabel>& freeLabels() { return freeLabels_; }
    const std::vector<FreeLabel>& freeLabels() const { return freeLabels_; }
    std::vector<ArrowOverlay>& arrows() { return arrows_; }
    const std::vector<ArrowOverlay>& arrows() const { return arrows_; }
    const std::optional<ColorRGB>& arrowColor() const { return arrowColor_; }
    void setArrowColor(std::optional<ColorRGB> c) { arrowColor_ = c; }
    int arrowThickness() const { return arrowThickness_; }
    void setArrowThickness(int t) { arrowThickness_ = t; }
    int arrowHeadSize() const { return arrowHeadSize_; }
    void setArrowHeadSize(int s) { arrowHeadSize_ = s; }
    // Drop every per-atom label + every measurement/angle/dihedral entry.
    // Shared by `:overlay clear` and `:run --fresh`; keeping them in lockstep
    // here means a future overlay-list addition (e.g. arrows, axes) is wired
    // into both call sites by editing one method instead of two.
    void clearOverlayAnnotations() {
        measurements_.clear();
        labelAtoms_.clear();
        labelText_.clear();
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
    float fogStrength_ = 0.35f;
    bool outlineEnabled_ = true;
    // Include the $sele / pk halo in :screenshot PNGs. Default off so
    // editor HUD state (transient selection cue) doesn't bake into
    // figure exports. Live render and stereo render always show the
    // halo. Issue #96.
    bool screenshotOverlay_ = false;
    float outlineThreshold_ = 0.3f;
    StereoMode stereoMode_ = StereoMode::Off;
    float stereoAngle_ = 6.0f;  // total parallax in degrees (eyes ±half)
    float outlineDarken_ = 0.15f;
    int labelFontSize_ = 14;
    int annotationFontSize_ = 14;
    int annotationLineWidth_ = 2;
    float overlayScale_ = 1.0f;
    OverlaySizeMode overlaySizeMode_ = OverlaySizeMode::Pixels;
    int referenceCanvasHeight_ = 1080;
    int liveDpi_ = 96;
    // Set transiently by RenderScaleScope at the top of each render pass.
    // Lives outside overlayScale_ so the user-visible scale knob isn't
    // mutated by screenshot dispatch.
    float renderScaleHint_ = 1.0f;
    std::optional<std::array<uint8_t, 3>> labelColor_;
    std::optional<std::array<uint8_t, 3>> annotationColor_;
    std::optional<std::array<uint8_t, 3>> measurementLineColor_;
    std::optional<std::array<uint8_t, 3>> outlineColor_;
    std::optional<std::array<uint8_t, 3>> labelOutlineColor_;
    std::optional<std::array<uint8_t, 3>> annotationOutlineColor_;
    bool labelOutline_ = false;
    int  labelOutlineThickness_ = 2;
    bool annotationOutline_ = false;
    int  annotationOutlineThickness_ = 2;
    OutlineMode outlineMode_ = OutlineMode::Edge;
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
        return stereoMode_ == StereoMode::Off ? 1 : 2;
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
