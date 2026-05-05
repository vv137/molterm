#include "molterm/app/Application.h"

#include "molterm/cmd/CommandParser.h"
#include "molterm/io/Aligner.h"
#include "molterm/io/CifLoader.h"
#include "molterm/render/AsciiCanvas.h"
#include "molterm/render/BrailleCanvas.h"
#include "molterm/render/BlockCanvas.h"
#include "molterm/render/PixelCanvas.h"
#include "molterm/render/ProtocolPicker.h"
#include "molterm/render/ColorMapper.h"
#include "molterm/repr/WireframeRepr.h"
#include "molterm/repr/BallStickRepr.h"
#include "molterm/repr/BackboneRepr.h"
#include "molterm/repr/SpacefillRepr.h"
#include "molterm/repr/CartoonRepr.h"
#include "molterm/repr/RibbonRepr.h"
#include "molterm/io/SessionExporter.h"
#include "molterm/config/ConfigParser.h"
#include "molterm/core/Geometry.h"
#include "molterm/core/Logger.h"
#include "molterm/io/SessionSaver.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <signal.h>
#include <glob.h>

namespace molterm {

static int applyHeteroatomColors(MolObject& obj) {
    const auto& atoms = obj.atoms();
    int count = 0;
    for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
        if (atoms[i].element != "C") {
            obj.setAtomColor(i, ColorMapper::colorForElement(atoms[i].element));
            ++count;
        }
    }
    return count;
}

static const char* inspectLevelName(InspectLevel lvl) {
    switch (lvl) {
        case InspectLevel::Atom:    return "ATOM";
        case InspectLevel::Residue: return "RESIDUE";
        case InspectLevel::Chain:   return "CHAIN";
        case InspectLevel::Object:  return "OBJECT";
    }
    return "?";
}

static const char* pickModeName(PickMode pm) {
    switch (pm) {
        case PickMode::Inspect:       return "INSPECT";
        case PickMode::SelectAtom:    return "SEL:ATOM";
        case PickMode::SelectResidue: return "SEL:RES";
        case PickMode::SelectChain:   return "SEL:CHAIN";
        case PickMode::Focus:         return "FOCUS";
    }
    return "?";
}

// Parse a bool option value for :set commands.
// Accepts on/1/true/yes (true), off/0/false/no (false). Empty / unknown returns std::nullopt.
static std::optional<bool> parseBool(const std::string& v) {
    if (v == "on" || v == "1" || v == "true" || v == "yes")  return true;
    if (v == "off" || v == "0" || v == "false" || v == "no") return false;
    return std::nullopt;
}

// Global resize flag
static volatile sig_atomic_t g_resized = 0;
static void resizeHandler(int) { g_resized = 1; }

// Clear pixel graphics artifacts and force full ncurses repaint
static void clearScreenAndRepaint() {
    fprintf(stdout, "\033[2J");
    fflush(stdout);
    clearok(curscr, TRUE);
    wrefresh(curscr);
}

Application::Application() = default;
Application::~Application() {
    Logger::instance().close();
}

void Application::init(int argc, char* argv[]) {
    Logger::instance().open();
    MLOG_INFO("MolTerm starting (argc=%d)", argc);

    ColorMapper::initColors();

    // Set USalign path (compiled alongside molterm)
#ifdef USALIGN_PATH
    Aligner::setUSalignPath(USALIGN_PATH);
#endif
    layout_.init(screen_.height(), screen_.width());

    keymapMgr_.loadDefaults();
    keymapMgr_.loadFromFile();  // Override with ~/.molterm/keymap.toml
    inputHandler_ = std::make_unique<InputHandler>(keymapMgr_.keymap());

    // Apply config from ~/.molterm/config.toml
    auto cfg = ConfigParser::loadConfig();

    // Default renderer from config (or Braille)
    RendererType rt = RendererType::Braille;
    if (cfg.defaultRenderer == "ascii")  rt = RendererType::Ascii;
    if (cfg.defaultRenderer == "block")  rt = RendererType::Block;
    if (cfg.defaultRenderer == "pixel" || cfg.defaultRenderer == "auto") {
        rt = RendererType::Pixel;
    } else if (cfg.defaultRenderer == "sixel") {
        forcedProtocol_ = GraphicsProtocol::Sixel; rt = RendererType::Pixel;
    } else if (cfg.defaultRenderer == "kitty") {
        forcedProtocol_ = GraphicsProtocol::Kitty; rt = RendererType::Pixel;
    } else if (cfg.defaultRenderer == "iterm2") {
        forcedProtocol_ = GraphicsProtocol::ITerm2; rt = RendererType::Pixel;
    }
    setRenderer(rt);
    autoCenter_ = cfg.autoCenter;
    initRepresentations();
    registerCommands();

    // Auto-load ~/.molterm/init.mt if present (after commands are registered, before file args).
    // Failures here are logged but never abort startup — even under --strict from the CLI,
    // which only applies to the user-supplied script.
    if (const char* home = std::getenv("HOME")) {
        std::string initPath = std::string(home) + "/.molterm/init.mt";
        std::ifstream initFile(initPath);
        if (initFile) {
            MLOG_INFO("Loading init script: %s", initPath.c_str());
            ScriptRunResult r = runScriptStream(initFile, /*strict=*/false);
            if (r.failures > 0) {
                MLOG_WARN("init.mt: %d of %d command(s) failed (first: %s)",
                          r.failures, r.count, r.firstFail.c_str());
                cmdLine_.setMessage("init.mt: " + std::to_string(r.failures) +
                                    " command(s) failed - see ~/.molterm/molterm.log");
            } else {
                MLOG_INFO("init.mt: ran %d command(s)", r.count);
            }
        }
    }

    // Set up SIGWINCH handler for terminal resize
    struct sigaction sa;
    sa.sa_handler = resizeHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGWINCH, &sa, nullptr);

    // Non-blocking input with timeout for responsive rendering
    screen_.setTimeout(50);

    // Enable mouse
    screen_.enableMouse();

    // Load files from command line args (skip flags)
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-') continue;  // skip flags
        std::string msg = loadFile(argv[i]);
        if (!msg.empty()) {
            cmdLine_.setMessage(msg);
        }
    }
}

Application::ScriptRunResult Application::runScriptStream(std::istream& in, bool strict) {
    ScriptRunResult result;
    auto runOne = [&](std::string cmd) -> bool {
        size_t start = cmd.find_first_not_of(" \t");
        if (start == std::string::npos || cmd[start] == '#') return true;
        cmd.erase(0, start);
        // Accept optional leading ':' so interactive-mode habits work in scripts.
        if (!cmd.empty() && cmd[0] == ':') {
            cmd.erase(0, 1);
            size_t s2 = cmd.find_first_not_of(" \t");
            if (s2 == std::string::npos) return true;
            cmd.erase(0, s2);
        }
        size_t end = cmd.find_last_not_of(" \t");
        if (end == std::string::npos) return true;
        cmd.resize(end + 1);
        ExecResult r = cmdRegistry_.execute(*this, cmd);
        ++result.count;
        if (!r.msg.empty()) result.lastMsg = r.msg;
        if (!r.ok) {
            ++result.failures;
            if (result.firstFail.empty()) {
                result.firstFail = r.msg;
                result.failLine = cmd;
            }
            if (strict) { result.stopped = true; return false; }
        }
        return true;
    };
    std::string line;
    while (std::getline(in, line)) {
        // Split on ';' so multiple commands fit on one line (shell-style).
        size_t pos = 0;
        while (pos <= line.size()) {
            size_t next = line.find(';', pos);
            if (next == std::string::npos) next = line.size();
            if (!runOne(line.substr(pos, next - pos))) return result;
            pos = next + 1;
        }
    }
    return result;
}

void Application::initRepresentations() {
    representations_[ReprType::Wireframe] = std::make_unique<WireframeRepr>();
    representations_[ReprType::BallStick] = std::make_unique<BallStickRepr>();
    representations_[ReprType::Backbone]  = std::make_unique<BackboneRepr>();
    representations_[ReprType::Spacefill] = std::make_unique<SpacefillRepr>();
    representations_[ReprType::Cartoon]   = std::make_unique<CartoonRepr>();
    representations_[ReprType::Ribbon]    = std::make_unique<RibbonRepr>();
}

Representation* Application::getRepr(ReprType type) {
    auto it = representations_.find(type);
    return (it != representations_.end()) ? it->second.get() : nullptr;
}

void Application::setRenderer(RendererType type) {
    rendererType_ = type;
    switch (type) {
        case RendererType::Ascii:
            canvas_ = std::make_unique<AsciiCanvas>();
            break;
        case RendererType::Braille:
            canvas_ = std::make_unique<BrailleCanvas>();
            break;
        case RendererType::Block:
            canvas_ = std::make_unique<BlockCanvas>();
            break;
        case RendererType::Pixel: {
            auto proto = forcedProtocol_ != GraphicsProtocol::None
                ? forcedProtocol_ : ProtocolPicker::detect();
            auto encoder = ProtocolPicker::createEncoder(proto);
            if (encoder) {
                canvas_ = std::make_unique<PixelCanvas>(std::move(encoder));
            } else {
                rendererType_ = RendererType::Braille;
                canvas_ = std::make_unique<BrailleCanvas>();
            }
            break;
        }
    }
}

int Application::run() {
    if (quitRequested_) return 0;  // script quit before the main loop ever started
    if (isHeadless()) return 0;    // no TUI: nothing to render or read input from
    running_ = true;
    layout_.markAllDirty();
    needsRedraw_ = true;
    framesToSkip_ = 0;

    while (running_) {
        if (g_resized) {
            g_resized = 0;
            onResize();
        }

        if (needsRedraw_) {
            if (framesToSkip_ > 0) {
                --framesToSkip_;
                // Don't clear needsRedraw_ — the skipped frame must still render
            } else {
                auto t0 = std::chrono::steady_clock::now();
                renderFrame();
                auto t1 = std::chrono::steady_clock::now();
                lastFrameMs_ = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

                // Only skip continuous camera motion frames, not discrete commands
                if (lastFrameMs_ > 100) {
                    framesToSkip_ = std::min(3, static_cast<int>(lastFrameMs_ / 50));
                }
                needsRedraw_ = false;
            }
        }

        processInput();
    }

    return 0;
}

void Application::quit(bool force) {
    (void)force;
    SessionSaver::saveSession(*this);
    MLOG_INFO("Quitting MolTerm");
    running_ = false;
    quitRequested_ = true;
}

std::string Application::loadFile(const std::string& path) {
    MLOG_INFO("Loading file: %s", path.c_str());

    // Show loading message immediately
    cmdLine_.setMessage("Loading " + path + "...");
    if (!isHeadless()) {
        cmdLine_.render(layout_.commandLine());
        layout_.commandLine().refresh();
        doupdate();
    }

    try {
        auto obj = CifLoader::loadAuto(path);
        std::string name = obj->name();
        int atomCount = static_cast<int>(obj->atoms().size());
        int bondCount = static_cast<int>(obj->bonds().size());
        int stateCount = obj->stateCount();

        obj->applySmartDefaults();
        auto ptr = store_.add(std::move(obj));
        tabMgr_.currentTab().addObject(ptr);
        if (autoCenter_) tabMgr_.currentTab().centerView();
        layout_.markAllDirty();
        needsRedraw_ = true;

        std::string msg = "Loaded " + name + ": " +
               std::to_string(atomCount) + " atoms, " +
               std::to_string(bondCount) + " bonds";
        if (stateCount > 1)
            msg += ", " + std::to_string(stateCount) + " states";
        MLOG_INFO("%s", msg.c_str());
        return msg;
    } catch (const std::exception& e) {
        MLOG_ERROR("Load failed: %s — %s", path.c_str(), e.what());
        return std::string("Error loading ") + path + ": " + e.what();
    }
}

void Application::processInput() {
    int key = screen_.getKey();
    if (key == ERR) return;

    // Handle mouse events
    if (key == KEY_MOUSE) {
        handleMouse(key);
        return;
    }

    // Macro register selection: awaiting a register key after 'q' or '@'
    if (macroAwaitingRegister_) {
        macroAwaitingRegister_ = false;
        if (key >= 'a' && key <= 'z') {
            if (macroRecording_) {
                stopMacroRecord();
            } else {
                startMacroRecord(static_cast<char>(key));
            }
        } else {
            cmdLine_.setMessage("Invalid macro register (use a-z)");
        }
        layout_.markDirty(Layout::Component::CommandLine);
        layout_.markDirty(Layout::Component::StatusBar);
        needsRedraw_ = true;
        return;
    }
    if (macroPlayAwaitingRegister_) {
        macroPlayAwaitingRegister_ = false;
        if (key >= 'a' && key <= 'z') {
            playMacro(static_cast<char>(key));
            layout_.markAllDirty();  // macro playback can affect anything
        } else {
            cmdLine_.setMessage("Invalid macro register (use a-z)");
            layout_.markDirty(Layout::Component::CommandLine);
        }
        needsRedraw_ = true;
        return;
    }

    // Info/help overlay: scroll keys page through, explicit close keys
    // dismiss; everything else (resize, mouse, modifier prefixes, stray
    // alpha keys) is ignored so the overlay stays put.
    // lastVisibleRows is updated each frame by the renderer; used for paging.
    if (infoOverlay_.active) {
        const int rows = std::max(1, infoOverlay_.lastVisibleRows);
        // Wrapped line count populated by the renderer — narrow terminals
        // produce more wrapped lines than `lines.size()` would suggest, so
        // paging math must use the wrapped total to land on the real bottom.
        const int total = std::max(infoOverlay_.lastTotalLines,
                                   static_cast<int>(infoOverlay_.lines.size()));
        bool dismiss = false;
        switch (key) {
            case 'j': case KEY_DOWN:
                infoOverlay_.scrollOffset += 1;
                break;
            case 'k': case KEY_UP:
                infoOverlay_.scrollOffset -= 1;
                break;
            case KEY_NPAGE: case 4: /* Ctrl-D */
                infoOverlay_.scrollOffset += rows;
                break;
            case ' ':
                // Space: page down while content remains; dismiss at bottom.
                if (infoOverlay_.scrollOffset + rows >= total) dismiss = true;
                else infoOverlay_.scrollOffset += rows;
                break;
            case KEY_PPAGE: case 21: /* Ctrl-U */
                infoOverlay_.scrollOffset -= rows;
                break;
            case 'g':
                infoOverlay_.scrollOffset = 0;
                break;
            case 'G':
                infoOverlay_.scrollOffset = std::max(0, total - rows);
                break;
            case 'q': case 27: /* Esc */ case '?': case '\n': case KEY_ENTER:
                dismiss = true;
                break;
            default:
                // Ignore unrecognised keys (KEY_RESIZE, KEY_MOUSE,
                // bare modifiers, etc.) — closing the overlay on any
                // resize would be hostile.
                return;
        }
        if (dismiss) {
            infoOverlay_.active = false;
            infoOverlay_.scrollOffset = 0;
            // Overlay text is written via ncurses on top of the viewport
            // canvas, so the pixel renderer's frame-diff cache doesn't know
            // those cells changed. Without an explicit invalidate, the next
            // flush emits no pixels for that region and the area where the
            // overlay sat reads as a black stripe. markAllDirty also covers
            // any neighbouring component the overlay may have overlapped.
            if (canvas_) canvas_->invalidate();
            layout_.markAllDirty();
        } else {
            if (infoOverlay_.scrollOffset < 0) infoOverlay_.scrollOffset = 0;
            layout_.markDirty(Layout::Component::Viewport);
        }
        needsRedraw_ = true;
        return;
    }

    Mode mode = inputHandler_->mode();

    if (mode == Mode::Command || mode == Mode::Search) {
        Action action = inputHandler_->processKey(key);
        if (action != Action::None) {
            handleAction(action);
        } else {
            if (mode == Mode::Command) {
                handleCommandInput(key);
            } else {
                handleSearchInput(key);
            }
        }
    } else {
        Action action = inputHandler_->processKey(key);
        if (action != Action::None) {
            handleAction(action);
            recordAction(action);
        }
    }
}

void Application::handleMouse(int /*key*/) {
    MEVENT event;
    if (getmouse(&event) != OK) return;

    using C = Layout::Component;
    auto& tab = tabMgr_.currentTab();
    auto& cam = tab.camera();

    needsRedraw_ = true;

    // Scroll wheel for zoom — only viewport + status bar
    if (event.bstate & BUTTON4_PRESSED) {
        cam.zoomBy(1.15f);
        layout_.markDirty(C::Viewport);
        layout_.markDirty(C::StatusBar);
        return;
    }
#ifdef BUTTON5_PRESSED
    if (event.bstate & BUTTON5_PRESSED) {
        cam.zoomBy(1.0f / 1.15f);
        layout_.markDirty(C::Viewport);
        layout_.markDirty(C::StatusBar);
        return;
    }
#endif

    // Non-scroll clicks: may affect all visible components
    layout_.markDirty(C::Viewport);
    layout_.markDirty(C::CommandLine);
    layout_.markDirty(C::StatusBar);
    layout_.markDirty(C::SeqBar);

    if (event.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED)) {
        // Check if click is in tab bar region (row 0)
        if (event.y == 0) {
            auto names = tabMgr_.tabNames();
            int x = 1;
            for (int i = 0; i < static_cast<int>(names.size()); ++i) {
                int labelLen = static_cast<int>(names[i].size()) + 2;
                if (event.x >= x && event.x < x + labelLen) {
                    tabMgr_.goToTab(i);
                    break;
                }
                x += labelLen + 1;
            }
        }
        // Click in viewport → inspect or select atom
        else if (event.y >= 1 && event.y < 1 + layout_.viewportHeight()) {
            int vpX = event.x;
            int vpY = event.y - 1;  // offset for tab bar row
            buildProjCache();
            int atomIdx = findNearestAtom(vpX, vpY);
            if (atomIdx < 0) return;

            auto obj = tabMgr_.currentTab().currentObject();
            if (!obj) return;
            const auto& atoms = obj->atoms();
            const auto& a = atoms[atomIdx];

            if (pickMode_ == PickMode::SelectAtom) {
                // Click toggles atom in $sele
                auto& sele = namedSelections_["sele"];
                if (sele.has(atomIdx)) {
                    sele.removeIndex(atomIdx);
                    cmdLine_.setMessage("sele(-1) = " + std::to_string(sele.size()) +
                        " atoms | " + a.chainId + "/" + a.resName + " " +
                        std::to_string(a.resSeq) + "/" + a.name);
                } else {
                    sele.addIndex(atomIdx);
                    cmdLine_.setMessage("sele(+1) = " + std::to_string(sele.size()) +
                        " atoms | " + a.chainId + "/" + a.resName + " " +
                        std::to_string(a.resSeq) + "/" + a.name);
                }
            } else if (pickMode_ == PickMode::SelectResidue) {
                // Click toggles entire residue in $sele
                auto& sele = namedSelections_["sele"];
                // Check if any atom of this residue is already selected
                bool alreadySelected = false;
                std::vector<int> resAtoms;
                for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
                    if (atoms[i].chainId == a.chainId &&
                        atoms[i].resSeq == a.resSeq &&
                        atoms[i].insCode == a.insCode) {
                        resAtoms.push_back(i);
                        if (sele.has(i)) alreadySelected = true;
                    }
                }
                if (alreadySelected) {
                    for (int i : resAtoms) sele.removeIndex(i);
                    cmdLine_.setMessage("sele(-" + std::to_string(resAtoms.size()) + ") = " +
                        std::to_string(sele.size()) + " atoms [" +
                        a.chainId + "/" + a.resName + " " + std::to_string(a.resSeq) + "]");
                } else {
                    sele.addIndices(resAtoms);
                    cmdLine_.setMessage("sele(+" + std::to_string(resAtoms.size()) + ") = " +
                        std::to_string(sele.size()) + " atoms [" +
                        a.chainId + "/" + a.resName + " " + std::to_string(a.resSeq) + "]");
                }
            } else if (pickMode_ == PickMode::SelectChain) {
                // Click toggles entire chain in $sele
                auto& sele = namedSelections_["sele"];
                bool alreadySelected = false;
                std::vector<int> chainAtoms;
                for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
                    if (atoms[i].chainId == a.chainId) {
                        chainAtoms.push_back(i);
                        if (sele.has(i)) alreadySelected = true;
                    }
                }
                if (alreadySelected) {
                    for (int i : chainAtoms) sele.removeIndex(i);
                    cmdLine_.setMessage("sele(-" + std::to_string(chainAtoms.size()) + ") = " +
                        std::to_string(sele.size()) + " atoms [chain " + a.chainId + "]");
                } else {
                    sele.addIndices(chainAtoms);
                    cmdLine_.setMessage("sele(+" + std::to_string(chainAtoms.size()) + ") = " +
                        std::to_string(sele.size()) + " atoms [chain " + a.chainId + "]");
                }
            } else if (pickMode_ == PickMode::Focus) {
                // Mol*-style click-to-focus. enterFocus picks up the new
                // bounding-sphere zoom automatically. Mode stays active so
                // a second click refocuses on a different residue (matches
                // gs/gS/gc convention — ESC to exit).
                std::vector<int> subject = expandByFocusGranularity(*obj, atomIdx);
                pickedAtomIdx_ = atomIdx;        // so subsequent F can refocus
                enterFocus(*obj, subject, residueInfoString(a));
            } else {
                // Inspect mode — show info at current level
                pickedAtomIdx_ = atomIdx;
                activeViewState().focusResi = a.resSeq;
                activeViewState().focusChain = a.chainId;
                // Store in pick register (pk1-pk4, rotating)
                pickRegs_[pickNext_] = atomIdx;
                int pkNum = pickNext_ + 1;  // 1-based
                pickNext_ = (pickNext_ + 1) % 4;
                // Save as named selection $pk1..$pk4
                namedSelections_["pk" + std::to_string(pkNum)] =
                    Selection({atomIdx}, "pk" + std::to_string(pkNum));

                switch (inspectLevel_) {
                    case InspectLevel::Atom:
                        cmdLine_.setMessage("pk" + std::to_string(pkNum) + ": " +
                            atomInfoString(*obj, atomIdx));
                        break;
                    case InspectLevel::Residue: {
                        int resCount = 0;
                        for (const auto& at : atoms)
                            if (at.chainId == a.chainId && at.resSeq == a.resSeq && at.insCode == a.insCode)
                                ++resCount;
                        std::string ssStr = (a.ssType == SSType::Helix) ? "helix" :
                                            (a.ssType == SSType::Sheet) ? "sheet" : "loop";
                        cmdLine_.setMessage("RES: " + a.chainId + "/" + a.resName + " " +
                            std::to_string(a.resSeq) + " (" + std::to_string(resCount) +
                            " atoms, " + ssStr + ")");
                        break;
                    }
                    case InspectLevel::Chain: {
                        int chainAtoms = 0;
                        std::set<int> residues;
                        for (const auto& at : atoms) {
                            if (at.chainId == a.chainId) {
                                ++chainAtoms;
                                residues.insert(at.resSeq);
                            }
                        }
                        cmdLine_.setMessage("CHAIN: " + a.chainId + " (" +
                            std::to_string(residues.size()) + " residues, " +
                            std::to_string(chainAtoms) + " atoms)");
                        break;
                    }
                    case InspectLevel::Object:
                        cmdLine_.setMessage("OBJ: " + obj->name() + " (" +
                            std::to_string(atoms.size()) + " atoms, " +
                            std::to_string(obj->bonds().size()) + " bonds)");
                        break;
                }
            }
        }
        // Click in seqbar → center on residue (or refocus, if focus is active
        // or the focus pick mode is engaged — then the seqbar acts as a
        // residue navigator).
        else if (layout_.seqBarVisible()) {
            int seqBarY = layout_.seqBar().posY();
            int seqBarX = layout_.seqBar().posX();
            int seqBarH = layout_.seqBar().height();
            int seqBarW = layout_.seqBar().width();
            if (event.y < seqBarY || event.y >= seqBarY + seqBarH ||
                event.x < seqBarX || event.x >= seqBarX + seqBarW) return;
            std::string clickChain;
            int hitAtom = -1;
            int resi = activeSeqBar().resSeqAtColumn(
                event.y - seqBarY, event.x - seqBarX,
                layout_.seqBarWrap(), seqBarW, &clickChain, &hitAtom);
            if (resi < 0) return;
            activeViewState().focusResi = resi;
            activeViewState().focusChain = clickChain;
            auto obj = tabMgr_.currentTab().currentObject();
            if (!obj) return;
            const auto& atoms = obj->atoms();
            if (hitAtom < 0 || hitAtom >= (int)atoms.size()) return;
            const auto& a = atoms[hitAtom];
            if (focusActive() || pickMode_ == PickMode::Focus) {
                auto subject = expandByFocusGranularity(*obj, hitAtom);
                pickedAtomIdx_ = hitAtom;
                enterFocus(*obj, subject, residueInfoString(a));
            } else {
                auto& seqCam = tabMgr_.currentTab().camera();
                seqCam.setCenter(a.x, a.y, a.z);
                if (seqCam.zoom() < 5.0f) seqCam.setZoom(8.0f);
                cmdLine_.setMessage(a.chainId + "/" +
                    a.resName + " " + std::to_string(resi));
            }
        }
    }
}

void Application::handleAction(Action action) {
    using C = Layout::Component;
    auto& tab = tabMgr_.currentTab();
    auto& cam = tab.camera();
    float rs = cam.rotationSpeed();

    // Helper: mark specific components dirty and request redraw
    auto dirty = [&](std::initializer_list<C> components) {
        for (auto c : components) layout_.markDirty(c);
        needsRedraw_ = true;
    };

    switch (action) {
        // Navigation — only viewport + status bar
        case Action::RotateLeft:   cam.rotateY(-rs);  dirty({C::Viewport, C::StatusBar}); break;
        case Action::RotateRight:  cam.rotateY(rs);   dirty({C::Viewport, C::StatusBar}); break;
        case Action::RotateUp:     cam.rotateX(-rs);  dirty({C::Viewport, C::StatusBar}); break;
        case Action::RotateDown:   cam.rotateX(rs);   dirty({C::Viewport, C::StatusBar}); break;
        case Action::RotateCW:    cam.rotateZ(rs);    dirty({C::Viewport, C::StatusBar}); break;
        case Action::RotateCCW:   cam.rotateZ(-rs);   dirty({C::Viewport, C::StatusBar}); break;
        case Action::PanLeft:     cam.pan(-cam.panSpeed(), 0);  dirty({C::Viewport, C::StatusBar}); break;
        case Action::PanRight:    cam.pan(cam.panSpeed(), 0);   dirty({C::Viewport, C::StatusBar}); break;
        case Action::PanUp:       cam.pan(0, -cam.panSpeed());  dirty({C::Viewport, C::StatusBar}); break;
        case Action::PanDown:     cam.pan(0, cam.panSpeed());   dirty({C::Viewport, C::StatusBar}); break;
        case Action::ZoomIn:      cam.zoomBy(1.2f);  dirty({C::Viewport, C::StatusBar}); break;
        case Action::ZoomOut:     cam.zoomBy(1.0f/1.2f); dirty({C::Viewport, C::StatusBar}); break;
        case Action::ResetView:   cam.reset(); tab.centerView(); dirty({C::Viewport, C::StatusBar}); break;
        case Action::CenterSelection: tab.centerView(); dirty({C::Viewport, C::StatusBar}); break;
        case Action::Redraw:
            clearScreenAndRepaint();
            layout_.markAllDirty(); needsRedraw_ = true;
            framesToSkip_ = 0;
            break;

        // Objects — viewport + panels + seqbar + status
        case Action::NextObject:
            tab.selectNextObject();
            dirty({C::Viewport, C::ObjectPanel, C::SeqBar, C::StatusBar});
            break;
        case Action::PrevObject:
            tab.selectPrevObject();
            dirty({C::Viewport, C::ObjectPanel, C::SeqBar, C::StatusBar});
            break;
        case Action::ToggleVisible: {
            auto obj = tab.currentObject();
            if (obj) obj->toggleVisible();
            dirty({C::Viewport, C::ObjectPanel, C::StatusBar});
            break;
        }
        case Action::DeleteObject: {
            int idx = tab.selectedObjectIdx();
            if (idx >= 0) {
                auto obj = tab.currentObject();
                if (obj) store_.remove(obj->name());
                tab.removeObject(idx);
            }
            dirty({C::Viewport, C::ObjectPanel, C::SeqBar, C::StatusBar});
            break;
        }

        // Representations — viewport only
        case Action::ShowWireframe: { auto obj = tab.currentObject(); if (obj) obj->showRepr(ReprType::Wireframe); dirty({C::Viewport}); break; }
        case Action::ShowBallStick: { auto obj = tab.currentObject(); if (obj) obj->showRepr(ReprType::BallStick); dirty({C::Viewport}); break; }
        case Action::ShowSpacefill: { auto obj = tab.currentObject(); if (obj) obj->showRepr(ReprType::Spacefill); dirty({C::Viewport}); break; }
        case Action::ShowCartoon:   { auto obj = tab.currentObject(); if (obj) obj->showRepr(ReprType::Cartoon);   dirty({C::Viewport}); break; }
        case Action::ShowRibbon:    { auto obj = tab.currentObject(); if (obj) obj->showRepr(ReprType::Ribbon);    dirty({C::Viewport}); break; }
        case Action::ShowBackbone:  { auto obj = tab.currentObject(); if (obj) obj->showRepr(ReprType::Backbone);  dirty({C::Viewport}); break; }
        case Action::HideWireframe: { auto obj = tab.currentObject(); if (obj) obj->hideRepr(ReprType::Wireframe); dirty({C::Viewport}); break; }
        case Action::HideBallStick: { auto obj = tab.currentObject(); if (obj) obj->hideRepr(ReprType::BallStick); dirty({C::Viewport}); break; }
        case Action::HideSpacefill: { auto obj = tab.currentObject(); if (obj) obj->hideRepr(ReprType::Spacefill); dirty({C::Viewport}); break; }
        case Action::HideCartoon:   { auto obj = tab.currentObject(); if (obj) obj->hideRepr(ReprType::Cartoon);   dirty({C::Viewport}); break; }
        case Action::HideRibbon:    { auto obj = tab.currentObject(); if (obj) obj->hideRepr(ReprType::Ribbon);    dirty({C::Viewport}); break; }
        case Action::HideBackbone:  { auto obj = tab.currentObject(); if (obj) obj->hideRepr(ReprType::Backbone);  dirty({C::Viewport}); break; }
        case Action::HideAll:       { auto obj = tab.currentObject(); if (obj) obj->hideAllRepr();                 dirty({C::Viewport}); break; }

        // Coloring — viewport + seqbar
        case Action::ColorByElement: { auto obj = tab.currentObject(); if (obj) applyHeteroatomColors(*obj); dirty({C::Viewport, C::SeqBar}); break; }
        case Action::ColorByChain:   { auto obj = tab.currentObject(); if (obj) obj->setColorScheme(ColorScheme::Chain);              dirty({C::Viewport, C::SeqBar}); break; }
        case Action::ColorBySS:      { auto obj = tab.currentObject(); if (obj) obj->setColorScheme(ColorScheme::SecondaryStructure); dirty({C::Viewport, C::SeqBar}); break; }
        case Action::ColorByBFactor: { auto obj = tab.currentObject(); if (obj) obj->setColorScheme(ColorScheme::BFactor);            dirty({C::Viewport, C::SeqBar}); break; }

        // Tabs — swap view state on switch (applyViewState marks changed components dirty)
        case Action::NextTab:
            tabMgr_.nextTab();
            layout_.applyViewState(activeViewState());
            layout_.markAllDirty(); needsRedraw_ = true;
            break;
        case Action::PrevTab:
            tabMgr_.prevTab();
            layout_.applyViewState(activeViewState());
            layout_.markAllDirty(); needsRedraw_ = true;
            break;
        case Action::NewTab:
            tabMgr_.addTab();
            dirty({C::TabBar});
            break;
        case Action::CloseTab:
            tabMgr_.closeCurrentTab();
            layout_.applyViewState(activeViewState());
            layout_.markAllDirty(); needsRedraw_ = true;
            break;

        // Panel — update both Layout and current tab's view state
        case Action::TogglePanel:
            layout_.togglePanel();
            activeViewState().panelVisible = layout_.panelVisible();
            dirty({C::Viewport, C::ObjectPanel});
            break;

        // Mode transitions — command line only
        case Action::EnterCommand:
            inputHandler_->setMode(Mode::Command);
            cmdLine_.activate(':');
            dirty({C::CommandLine, C::StatusBar});
            break;
        case Action::EnterSearch:
            inputHandler_->setMode(Mode::Search);
            cmdLine_.activate('/');
            dirty({C::CommandLine, C::StatusBar});
            break;
        case Action::EnterVisual:
            inputHandler_->setMode(Mode::Visual);
            dirty({C::StatusBar});
            break;
        case Action::ExitToNormal:
            // First, if we're in focus mode, exiting back to "normal" view
            // means leaving focus — restore camera + visibility.
            if (focusSnapshot_.active) {
                exitFocus();
                dirty({C::CommandLine, C::StatusBar, C::Viewport});
                break;
            }
            inputHandler_->setMode(Mode::Normal);
            cmdLine_.deactivate();
            pickedAtomIdx_ = -1;
            if (pickMode_ != PickMode::Inspect) {
                pickMode_ = PickMode::Inspect;
                cmdLine_.setMessage("Inspect mode | sele=" +
                    std::to_string(namedSelections_["sele"].size()));
            }
            dirty({C::CommandLine, C::StatusBar});
            break;

        // Command mode actions — commands can affect any component
        case Action::ExecuteCommand: {
            std::string input = cmdLine_.input();
            cmdLine_.pushHistory(input);
            cmdLine_.deactivate();
            inputHandler_->setMode(Mode::Normal);
            ExecResult result = cmdRegistry_.execute(*this, input);
            if (!result.msg.empty()) cmdLine_.setMessage(result.msg);
            layout_.markAllDirty(); needsRedraw_ = true;
            break;
        }
        case Action::Autocomplete: {
            std::string input = cmdLine_.input();
            // Find first space — determines if completing command or argument
            auto spacePos = input.find(' ');
            if (spacePos == std::string::npos) {
                // Completing command name
                auto completions = cmdRegistry_.complete(input);
                if (completions.size() == 1) {
                    cmdLine_.clearInput();
                    for (char c : completions[0]) cmdLine_.insertChar(c);
                    cmdLine_.insertChar(' ');
                } else if (completions.size() > 1) {
                    std::string msg;
                    for (const auto& c : completions) msg += c + " ";
                    cmdLine_.setMessage(msg);
                }
            } else {
                // Completing argument — context-dependent
                std::string cmdName = input.substr(0, spacePos);
                std::string partial = input.substr(input.rfind(' ') + 1);

                std::vector<std::string> candidates;

                if (cmdName == "load" || cmdName == "e" || cmdName == "export") {
                    // Filename completion via std::filesystem
                    namespace fs = std::filesystem;
                    std::string dir = ".";
                    std::string prefix = partial;
                    auto lastSlash = partial.rfind('/');
                    if (lastSlash != std::string::npos) {
                        dir = partial.substr(0, lastSlash);
                        if (dir.empty()) dir = "/";
                        prefix = partial.substr(lastSlash + 1);
                    }
                    try {
                        for (const auto& entry : fs::directory_iterator(dir)) {
                            std::string name = entry.path().filename().string();
                            if (name.find(prefix) == 0) {
                                std::string full = (lastSlash != std::string::npos)
                                    ? dir + "/" + name : name;
                                if (entry.is_directory()) full += "/";
                                candidates.push_back(full);
                            }
                        }
                    } catch (...) {}
                } else if (cmdName == "delete" || cmdName == "rename" ||
                           cmdName == "align" || cmdName == "mmalign" || cmdName == "super" ||
                           cmdName == "alignto" || cmdName == "mmalignto") {
                    // Object name completion
                    for (const auto& n : store_.names()) {
                        if (n.find(partial) == 0) candidates.push_back(n);
                    }
                } else if (cmdName == "show" || cmdName == "hide") {
                    // Repr name completion
                    for (const auto& r : {"wireframe", "wire", "ballstick", "sticks",
                                           "spacefill", "spheres", "cartoon", "ribbon",
                                           "backbone", "trace", "all"}) {
                        std::string rs(r);
                        if (rs.find(partial) == 0) candidates.push_back(rs);
                    }
                } else if (cmdName == "color") {
                    // Color name/scheme completion
                    for (const auto& c : {"element", "cpk", "chain", "ss", "secondary",
                                           "bfactor", "b", "plddt", "rainbow", "restype", "type",
                                           "heteroatom", "clear",
                                           "red", "green", "blue", "yellow", "magenta",
                                           "cyan", "white", "orange", "pink", "lime",
                                           "teal", "purple", "salmon", "slate", "gray"}) {
                        std::string cs(c);
                        if (cs.find(partial) == 0) candidates.push_back(cs);
                    }
                } else if (cmdName == "set" || cmdName == "get") {
                    for (const auto& o : {"renderer", "backbone_thickness", "bt",
                                           "wireframe_thickness", "wt", "ball_radius", "br",
                                           "pan_speed", "ps", "fog", "outline", "outline_threshold", "ot",
                                           "outline_darken", "od", "cartoon_helix", "ch",
                                           "cartoon_sheet", "csh", "cartoon_loop", "cl",
                                           "cartoon_subdiv", "csd", "cartoon_aspect", "csa",
                                           "cartoon_helix_radial", "chr",
                                           "cartoon_tubular_helix", "cth",
                                           "cartoon_tubular_radius", "ctr",
                                           "nucleic_backbone", "nb",
                                           "bs_units", "bs_factor", "bsf",
                                           "spacefill_scale", "sfs",
                                           "lod_medium", "lod_low",
                                           "backbone_cutoff", "auto_center", "panel",
                                           "seqbar", "seqwrap",
                                           "interface_color", "ic",
                                           "interface_thickness", "it",
                                           "interface_classify", "iclass",
                                           "interface_show", "is"}) {
                        std::string os(o);
                        if (os.find(partial) == 0) candidates.push_back(os);
                    }
                }

                if (candidates.size() == 1) {
                    // Replace partial with completed word
                    std::string base = input.substr(0, input.rfind(' ') + 1);
                    cmdLine_.clearInput();
                    for (char c : base) cmdLine_.insertChar(c);
                    for (char c : candidates[0]) cmdLine_.insertChar(c);
                    cmdLine_.insertChar(' ');
                } else if (candidates.size() > 1) {
                    std::string msg;
                    for (const auto& c : candidates) msg += c + " ";
                    cmdLine_.setMessage(msg);
                }
            }
            dirty({C::CommandLine});
            break;
        }
        case Action::HistoryPrev: cmdLine_.historyPrev(); dirty({C::CommandLine}); break;
        case Action::HistoryNext: cmdLine_.historyNext(); dirty({C::CommandLine}); break;
        case Action::DeleteWord:  cmdLine_.deleteWord();  dirty({C::CommandLine}); break;
        case Action::ClearLine:   cmdLine_.clearInput();  dirty({C::CommandLine}); break;

        // Search — viewport (highlights) + command line + status
        case Action::ExecuteSearch: {
            std::string query = cmdLine_.input();
            cmdLine_.deactivate();
            inputHandler_->setMode(Mode::Normal);
            executeSearch(query);
            dirty({C::Viewport, C::CommandLine, C::StatusBar});
            break;
        }

        // Search navigation
        case Action::SearchNext: {
            if (searchMatches_.empty()) {
                cmdLine_.setMessage("No search results");
                dirty({C::CommandLine}); break;
            }
            auto obj = tab.currentObject();
            if (!obj) break;
            searchIdx_ = (searchIdx_ + 1) % static_cast<int>(searchMatches_.size());
            const auto& a = obj->atoms()[searchMatches_[searchIdx_]];
            cmdLine_.setMessage("/" + lastSearch_ + " [" + std::to_string(searchIdx_ + 1) +
                               "/" + std::to_string(searchMatches_.size()) + "] " +
                               a.chainId + " " + a.resName + " " + std::to_string(a.resSeq) +
                               " " + a.name);
            dirty({C::Viewport, C::CommandLine});
            break;
        }
        case Action::SearchPrev: {
            if (searchMatches_.empty()) {
                cmdLine_.setMessage("No search results");
                dirty({C::CommandLine}); break;
            }
            auto obj = tab.currentObject();
            if (!obj) break;
            searchIdx_--;
            if (searchIdx_ < 0) searchIdx_ = static_cast<int>(searchMatches_.size()) - 1;
            const auto& a = obj->atoms()[searchMatches_[searchIdx_]];
            cmdLine_.setMessage("/" + lastSearch_ + " [" + std::to_string(searchIdx_ + 1) +
                               "/" + std::to_string(searchMatches_.size()) + "] " +
                               a.chainId + " " + a.resName + " " + std::to_string(a.resSeq) +
                               " " + a.name);
            dirty({C::Viewport, C::CommandLine});
            break;
        }

        // Undo/Redo — can affect anything
        case Action::Undo: {
            std::string msg = undoStack_.undo();
            cmdLine_.setMessage(msg.empty() ? "Nothing to undo" : msg);
            layout_.markAllDirty(); needsRedraw_ = true;
            break;
        }
        case Action::Redo: {
            std::string msg = undoStack_.redo();
            cmdLine_.setMessage(msg.empty() ? "Nothing to redo" : msg);
            layout_.markAllDirty(); needsRedraw_ = true;
            break;
        }

        // Screenshot — command line message only
        case Action::Screenshot: {
            ExecResult result = cmdRegistry_.execute(*this, "screenshot");
            if (!result.msg.empty()) cmdLine_.setMessage(result.msg);
            dirty({C::CommandLine});
            break;
        }

        // Contact map toggle — affects panels + viewport
        case Action::ToggleContactMap: {
            ExecResult result = cmdRegistry_.execute(*this, "contactmap");
            if (!result.msg.empty()) cmdLine_.setMessage(result.msg);
            layout_.markAllDirty(); needsRedraw_ = true;
            break;
        }

        // Interface overlay toggle — viewport + panels
        // Sends explicit on/off rather than implicit toggle so the
        // command itself stays declarative (matches :set on|off style).
        case Action::ToggleInterface: {
            const char* arg = interfaceOverlay_ ? "interface off" : "interface on";
            ExecResult result = cmdRegistry_.execute(*this, arg);
            if (!result.msg.empty()) cmdLine_.setMessage(result.msg);
            layout_.markAllDirty(); needsRedraw_ = true;
            break;
        }

        // Focus mode (Mol*-style): F enters/exits.
        //   • Already focused → exit.
        //   • Fresh pick → focus on that residue.
        //   • No pick + named selection $sele exists → focus on it.
        case Action::FocusPick: {
            if (focusSnapshot_.active) {
                exitFocus();
                break;
            }
            auto obj = tabMgr_.currentTab().currentObject();
            if (!obj) { cmdLine_.setMessage("Focus: no object"); break; }
            const auto& atoms = obj->atoms();

            std::vector<int> subjectIdx;
            std::string desc;

            int target = pickedAtomIdx_;
            if (target < 0) target = pickRegs_[(pickNext_ + 3) % 4];   // most recent
            if (target >= 0 && target < (int)atoms.size()) {
                // Expand by configured granularity (default Residue —
                // matches prior behavior).
                subjectIdx = expandByFocusGranularity(*obj, target);
                desc = residueInfoString(atoms[target]);
            } else {
                // Fall back to the active named selection.
                auto it = namedSelections_.find("sele");
                if (it == namedSelections_.end() || it->second.empty()) {
                    cmdLine_.setMessage(
                        "Focus: click an atom or run :select first");
                    break;
                }
                subjectIdx = it->second.indices();
                desc = "$sele";
            }
            enterFocus(*obj, subjectIdx, desc);
            break;
        }

        // Renderer toggle — full redraw
        case Action::TogglePixelRenderer: {
            clearScreenAndRepaint();
            framesToSkip_ = 0;
            if (rendererType_ == RendererType::Pixel) {
                rendererType_ = RendererType::Braille;
                canvas_ = std::make_unique<BrailleCanvas>();
                cmdLine_.setMessage("Renderer: BRAILLE");
            } else {
                setRenderer(RendererType::Pixel);
                auto* pc = dynamic_cast<PixelCanvas*>(canvas_.get());
                const char* name = (pc && pc->encoder()) ? pc->encoder()->name() : "PIXEL";
                cmdLine_.setMessage(std::string("Renderer: ") + name);
            }
            layout_.markAllDirty(); needsRedraw_ = true;
            break;
        }

        // Macro recording — command line message
        case Action::StartMacro:
            if (macroRecording_) {
                stopMacroRecord();
            } else {
                macroAwaitingRegister_ = true;
                cmdLine_.setMessage("Record macro: press register (a-z)");
            }
            dirty({C::CommandLine, C::StatusBar});
            break;
        case Action::PlayMacro:
            macroPlayAwaitingRegister_ = true;
            cmdLine_.setMessage("Play macro: press register (a-z)");
            dirty({C::CommandLine, C::StatusBar});
            break;

        // More coloring — viewport + seqbar
        case Action::ColorByPLDDT:   { auto obj = tab.currentObject(); if (obj) obj->setColorScheme(ColorScheme::PLDDT);   dirty({C::Viewport, C::SeqBar}); break; }
        case Action::ColorByRainbow: { auto obj = tab.currentObject(); if (obj) obj->setColorScheme(ColorScheme::Rainbow); dirty({C::Viewport, C::SeqBar}); break; }
        case Action::ColorByResType: { auto obj = tab.currentObject(); if (obj) obj->setColorScheme(ColorScheme::ResType); dirty({C::Viewport, C::SeqBar}); break; }

        // Multi-state cycling — viewport + seqbar + status
        case Action::NextState: {
            auto obj = tab.currentObject();
            if (obj && obj->stateCount() > 1) {
                obj->nextState();
                cmdLine_.setMessage("State " + std::to_string(obj->activeState() + 1) +
                                   "/" + std::to_string(obj->stateCount()));
            } else {
                cmdLine_.setMessage("Single-state structure");
            }
            dirty({C::Viewport, C::SeqBar, C::CommandLine, C::StatusBar});
            break;
        }
        case Action::PrevState: {
            auto obj = tab.currentObject();
            if (obj && obj->stateCount() > 1) {
                obj->prevState();
                cmdLine_.setMessage("State " + std::to_string(obj->activeState() + 1) +
                                   "/" + std::to_string(obj->stateCount()));
            } else {
                cmdLine_.setMessage("Single-state structure");
            }
            dirty({C::Viewport, C::SeqBar, C::CommandLine, C::StatusBar});
            break;
        }

        // Inspect — command line message
        case Action::Inspect:
            cmdLine_.setMessage(std::string("Click to inspect | gs/gS/gc to select | Level: ") +
                inspectLevelName(inspectLevel_));
            dirty({C::CommandLine});
            break;

        // Cycle inspect level
        case Action::CycleInspectLevel: {
            inspectLevel_ = static_cast<InspectLevel>(
                (static_cast<int>(inspectLevel_) + 1) % 4);
            cmdLine_.setMessage(std::string("Inspect level: ") + inspectLevelName(inspectLevel_));
            dirty({C::CommandLine});
            break;
        }

        // Pick mode
        case Action::EnterSelectAtom:
        case Action::EnterSelectResidue:
        case Action::EnterSelectChain: {
            PickMode target = (action == Action::EnterSelectAtom) ? PickMode::SelectAtom :
                              (action == Action::EnterSelectResidue) ? PickMode::SelectResidue :
                              PickMode::SelectChain;
            pickMode_ = (pickMode_ == target) ? PickMode::Inspect : target;
            if (pickMode_ != PickMode::Inspect)
                cmdLine_.setMessage(std::string(pickModeName(pickMode_)) +
                    " mode (click to add/remove, ESC to exit) sele=" +
                    std::to_string(namedSelections_["sele"].size()));
            else
                cmdLine_.setMessage("Inspect mode");
            dirty({C::CommandLine, C::StatusBar});
            break;
        }

        case Action::EnterFocusPickMode: {
            // Toggle: a second `gf` while already in Focus mode returns to
            // Inspect (matches the gs/gS/gc convention).
            pickMode_ = (pickMode_ == PickMode::Focus) ? PickMode::Inspect
                                                       : PickMode::Focus;
            const char* gran =
                focusGranularity_ == FocusGranularity::Chain    ? "chain" :
                focusGranularity_ == FocusGranularity::Sidechain ? "sidechain" :
                                                                  "residue";
            if (pickMode_ == PickMode::Focus) {
                cmdLine_.setMessage(std::string("FOCUS pick mode — click an atom (granularity=") +
                                    gran + ", ESC to exit)");
            } else {
                cmdLine_.setMessage("Inspect mode");
            }
            dirty({C::CommandLine, C::StatusBar});
            break;
        }

        case Action::ToggleSeqBar: {
            // Force wrap=true so visible always means "all sequences shown."
            // The legacy single-line scroll mode is still reachable via
            // `:set seqwrap off` but no key cycles through it — three states
            // confused users with the camera-focus key.
            layout_.setSeqBarWrap(true);
            layout_.toggleSeqBar();
            cmdLine_.setMessage(layout_.seqBarVisible()
                                    ? "Sequence bar: visible"
                                    : "Sequence bar: hidden");
            activeViewState().seqBarVisible = layout_.seqBarVisible();
            activeViewState().seqBarWrap = layout_.seqBarWrap();
            if (canvas_) canvas_->invalidate();
            onResize();
            break;
        }

        case Action::SeqBarNextChain:
            if (layout_.seqBarVisible() && !layout_.seqBarWrap()) {
                activeSeqBar().nextChain();
                activeSeqBar().scrollToChain(activeSeqBar().activeChain());
                cmdLine_.setMessage("Chain: " + activeSeqBar().activeChain());
            }
            dirty({C::SeqBar, C::CommandLine});
            break;
        case Action::SeqBarPrevChain:
            if (layout_.seqBarVisible() && !layout_.seqBarWrap()) {
                activeSeqBar().prevChain();
                activeSeqBar().scrollToChain(activeSeqBar().activeChain());
                cmdLine_.setMessage("Chain: " + activeSeqBar().activeChain());
            }
            dirty({C::SeqBar, C::CommandLine});
            break;

        case Action::ShowOverlay:
            overlayVisible_ = true;
            cmdLine_.setMessage("Overlays visible");
            dirty({C::Viewport, C::CommandLine});
            break;
        case Action::HideOverlay:
            overlayVisible_ = false;
            cmdLine_.setMessage("Overlays hidden");
            dirty({C::Viewport, C::CommandLine});
            break;

        case Action::ApplyPreset: {
            auto obj = tab.currentObject();
            if (obj) {
                obj->applySmartDefaults();
                cmdLine_.setMessage("Applied default preset");
            }
            dirty({C::Viewport, C::CommandLine});
            break;
        }

        case Action::ShowHelp:
            showKeybindingHelp();
            dirty({C::Viewport});
            break;

        default:
            needsRedraw_ = false;
            break;
    }
}

void Application::handleLineEdit(int key) {
    if (key == KEY_BACKSPACE || key == 127 || key == 8) {
        cmdLine_.backspace();
    } else if (key == KEY_DC || key == 330) {
        cmdLine_.deleteForward();
    } else if (key == KEY_LEFT) {
        cmdLine_.cursorLeft();
    } else if (key == KEY_RIGHT) {
        cmdLine_.cursorRight();
    } else if (key == KEY_HOME || key == 1) {
        cmdLine_.cursorHome();
    } else if (key == KEY_END || key == 5) {
        cmdLine_.cursorEnd();
    } else {
        cmdLine_.insertChar(key);
    }
    cmdLine_.render(layout_.commandLine());
    layout_.commandLine().refresh();
    doupdate();
}

void Application::handleCommandInput(int key) {
    handleLineEdit(key);
}

void Application::handleSearchInput(int key) {
    handleLineEdit(key);
}

void Application::renderFrame() {
    ++frameCounter_;

    // Dirty flags are now set selectively by handleAction/handleMouse/onResize.
    // No blanket markAllDirty() here.

    // Tab bar
    if (layout_.isDirty(Layout::Component::TabBar))
        tabBar_.render(layout_.tabBar(), tabMgr_.tabNames(), tabMgr_.currentIndex());

    // Adjust seqbar height BEFORE rendering viewport (setSeqBarHeight rebuilds windows)
    if (layout_.seqBarVisible()) {
        auto obj = tabMgr_.currentTab().currentObject();
        if (obj) {
            activeSeqBar().update(*obj);
            if (layout_.seqBarWrap()) {
                int needed = std::min(activeSeqBar().wrapRows(layout_.seqBar().width()),
                                      screen_.height() / 4);
                if (needed != layout_.seqBar().height()) {
                    layout_.setSeqBarHeight(std::max(1, needed));
                    activeViewState().seqBarHeight = layout_.seqBar().height();
                    if (canvas_) canvas_->invalidate();
                }
            } else {
                if (layout_.seqBar().height() != 1) {
                    layout_.setSeqBarHeight(1);
                    activeViewState().seqBarHeight = 1;
                    if (canvas_) canvas_->invalidate();
                }
            }
        }
    }

    // Viewport — render to canvas but defer pixel flush
    bool viewportRendered = layout_.isDirty(Layout::Component::Viewport);
    if (viewportRendered)
        renderViewport();

    // Clear camera dirty flag after rendering (so caches like Spacefill sort work)
    tabMgr_.currentTab().camera().clearDirty();

    // Object panel
    if (layout_.panelVisible() && layout_.isDirty(Layout::Component::ObjectPanel)) {
        auto& tab = tabMgr_.currentTab();
        objectPanel_.render(layout_.objectPanel(), tab.objects(),
                           tab.selectedObjectIdx());
    }

    // Analysis panel (contact map etc.)
    if (layout_.analysisPanelVisible() && layout_.isDirty(Layout::Component::AnalysisPanel)) {
        renderAnalysisPanel();
    }

    // Sequence bar
    if (layout_.seqBarVisible() && layout_.isDirty(Layout::Component::SeqBar)) {
        auto obj = tabMgr_.currentTab().currentObject();
        if (obj) {
            const Selection* sele = nullptr;
            auto selIt = namedSelections_.find("sele");
            if (selIt != namedSelections_.end()) sele = &selIt->second;
            activeSeqBar().render(layout_.seqBar(), activeViewState().focusResi, activeViewState().focusChain, sele,
                          obj->colorScheme(), layout_.seqBarWrap());
        }
    }

    // Status bar
    if (layout_.isDirty(Layout::Component::StatusBar))
        updateStatusBar();

    // Command line
    if (layout_.isDirty(Layout::Component::CommandLine))
        cmdLine_.render(layout_.commandLine());

    layout_.refreshAll();
    doupdate();

    // Pixel graphics must be written AFTER doupdate() so ncurses doesn't overwrite.
    if (rendererType_ == RendererType::Pixel && viewportRendered) {
        canvas_->flush(layout_.viewport());
    }
}

void Application::renderViewport() {
    auto& win = layout_.viewport();
    win.erase();

    int w = win.width(), h = win.height();
    canvas_->resize(w, h);
    canvas_->clear();

    auto& tab = tabMgr_.currentTab();

    // Prepare projection once per frame (not per-repr per-object)
    tab.camera().prepareProjection(canvas_->subW(), canvas_->subH(), canvas_->aspectYX());

    // Wireframe paints heteroatoms by element while keeping carbons on
    // the scheme color whenever the user is looking at an interface
    // (overlay or focus) — N/O/S/P need to pop as donors/acceptors there.
    if (auto* wf = dynamic_cast<WireframeRepr*>(getRepr(ReprType::Wireframe))) {
        wf->setHeteroatomCarbonScheme(interfaceOverlay_ || focusSnapshot_.active);
    }

    for (const auto& obj : tab.objects()) {
        if (!obj->visible()) continue;
        for (auto& [reprType, repr] : representations_) {
            if (obj->reprVisible(reprType)) {
                repr->render(*obj, tab.camera(), *canvas_);
            }
        }
    }

    // ── ZoomGate auto-engage / disengage ────────────────────────────────
    // When the user has set :set interface_zoom <T>, crossing the
    // threshold simulates a :interface toggle. Manual `:interface on`
    // is preserved (gate falling does not disable a manually-engaged
    // overlay; only auto-engaged ones are auto-disengaged).
    if (interfaceZoomGate_.enabled()) {
        const bool wasActive = interfaceZoomGate_.active();
        const bool flipped   = interfaceZoomGate_.update(tab.camera().zoom());
        const bool isActive  = interfaceZoomGate_.active();
        if (flipped) {
            if (isActive && !interfaceOverlay_) {
                interfaceFromZoom_ = true;
                cmdRegistry_.execute(*this, "interface on");
            } else if (!isActive && interfaceFromZoom_ && interfaceOverlay_) {
                interfaceFromZoom_ = false;
                cmdRegistry_.execute(*this, "interface off");
            }
        }
        (void)wasActive;
    }

    // Apply post-processing on pixel canvas. InterfaceRepr is drawn
    // AFTER outline + fog + focus-dim so the colored overlay (sidechain
    // bonds, dashed interaction lines) stays vivid — fog would otherwise
    // wash it out, defeating the "highlight" intent.
    if (rendererType_ == RendererType::Pixel) {
        auto* pc = dynamic_cast<PixelCanvas*>(canvas_.get());
        if (pc) {
            if (outlineEnabled_) pc->applyOutline(outlineThreshold_, outlineDarken_);
            if (fogStrength_ > 0.0f) pc->applyDepthFog(fogStrength_);

            // Focus-dim: prefer the interface mask when active, fall back
            // to the explicit :focus selection mask. Either is depth-
            // independent, so a non-focus atom in the foreground still
            // dims while the focus subject stays vivid.
            const std::vector<bool>* dimMask = nullptr;
            if (interfaceOverlay_ && !interfaceAtomMask_.empty()) {
                dimMask = &interfaceAtomMask_;
            } else if (!focusAtomMask_.empty()) {
                dimMask = &focusAtomMask_;
            }
            if (dimMask) pc->applyFocusDim(*dimMask, focusDimStrength_);
        }
    }

    // ── Interface overlay (sidechains + interaction lines) ─────────────
    // Drawn last so it sits on top of fog + focus-dim — vivid colors,
    // unattenuated. Fires under either:
    //   • global :interface overlay (full structure)
    //   • focus mode (filtered to the focus neighborhood)
    if ((interfaceOverlay_ || focusSnapshot_.active) &&
        interfaceRepr_.hasData()) {
        if (auto obj = tab.currentObject()) {
            interfaceRepr_.render(*obj, tab.camera(), *canvas_);
        }
    }

    // Pixel flush is deferred to after doupdate() in renderFrame().
    // Other renderers flush here (they go through ncurses).
    if (rendererType_ != RendererType::Pixel) {
        canvas_->flush(win);
    }

    // Centered modal overlay used by `?`, `:help`, `:help <cmd>`, and
    // `:interface legend`. Word-wraps to fit the terminal; scrolls when
    // content overflows. Key handling lives in processInput().
    if (infoOverlay_.active) {
        // Box width: clamp(min=20, preferred=60, max=terminal-4). Using
        // std::clamp(60, 20, w-4) is fine when w-4 >= 20; on tiny terminals
        // we fall back to whatever fits.
        int hi = std::max(20, w - 4);
        int ow = std::clamp(60, 20, hi);
        if (hi > ow) {
            int longest = static_cast<int>(infoOverlay_.title.size()) + 4;
            for (const auto& l : infoOverlay_.lines)
                longest = std::max(longest, static_cast<int>(l.size()) + 4);
            ow = std::min(std::max(ow, longest), hi);
        }
        int contentWidth = std::max(10, ow - 4);  // inside the box, minus padding

        // UTF-8 display width: count lead bytes (anything that isn't a
        // continuation byte 10xxxxxx). Treats every codepoint as one
        // column, which is right for the Latin/symbol/box-drawing chars
        // used by the help and legend overlays (Å, ↔, ≤, ─). Wide CJK
        // would over-estimate, but the overlay doesn't use any.
        auto cols = [](std::string_view s) {
            int n = 0;
            for (unsigned char c : s)
                if ((c & 0xC0) != 0x80) ++n;
            return n;
        };

        // Word-wrap a single line to `contentWidth` display columns.
        // Continuation lines indent to the original lead + 2 so wrapped
        // table rows stay visually aligned.
        auto wrapLine = [contentWidth, &cols](const std::string& line)
                          -> std::vector<std::string> {
            if (cols(line) <= contentWidth) return {line};
            size_t leadEnd = line.find_first_not_of(' ');
            std::string lead = (leadEnd == std::string::npos) ? std::string()
                                                              : line.substr(0, leadEnd);
            std::string indent = lead + "  ";
            std::vector<std::string> out;
            std::string cur = lead;
            std::string body = (leadEnd == std::string::npos) ? std::string()
                                                              : line.substr(leadEnd);
            std::istringstream iss(body);
            std::string word;
            bool first = true;
            while (iss >> word) {
                int sep = first ? 0 : 1;
                if (cols(cur) + sep + cols(word) > contentWidth) {
                    if (!cur.empty() && cur != lead) {
                        out.push_back(cur);
                        cur = indent;
                    }
                    // Hard-break a word that's still too long for one line.
                    while (cols(cur) + cols(word) > contentWidth) {
                        int avail = contentWidth - cols(cur);
                        if (avail <= 0) {
                            out.push_back(cur);
                            cur = indent;
                            avail = contentWidth - cols(cur);
                        }
                        // Walk codepoints to pick a byte-safe split point.
                        size_t cut = 0;
                        int taken = 0;
                        while (cut < word.size() && taken < avail) {
                            unsigned char c = static_cast<unsigned char>(word[cut]);
                            do { ++cut; } while (cut < word.size()
                                && (static_cast<unsigned char>(word[cut]) & 0xC0) == 0x80);
                            (void)c;
                            ++taken;
                        }
                        if (cut == 0) break;  // safety: avoid infinite loop
                        cur += word.substr(0, cut);
                        out.push_back(cur);
                        word.erase(0, cut);
                        cur = indent;
                    }
                    cur += word;
                } else {
                    if (!first) cur += ' ';
                    cur += word;
                }
                first = false;
            }
            if (!cur.empty()) out.push_back(cur);
            if (out.empty()) out.push_back(line);
            return out;
        };

        // Flatten input lines to wrapped lines, propagating per-line colors
        // (each wrapped fragment inherits the original line's color).
        std::vector<std::string> wrapped;
        std::vector<int> wrappedColors;
        wrapped.reserve(infoOverlay_.lines.size());
        wrappedColors.reserve(infoOverlay_.lines.size());
        for (size_t i = 0; i < infoOverlay_.lines.size(); ++i) {
            int origColor = (i < infoOverlay_.lineColors.size())
                            ? infoOverlay_.lineColors[i] : -1;
            for (auto& w : wrapLine(infoOverlay_.lines[i])) {
                wrapped.push_back(std::move(w));
                wrappedColors.push_back(origColor);
            }
        }
        int contentLines = static_cast<int>(wrapped.size());

        int oh = std::min(contentLines + 4, h - 2);
        int ox = (w - ow) / 2;
        int oy = (h - oh) / 2;

        int visibleRows = std::max(0, oh - 4);
        infoOverlay_.lastVisibleRows = visibleRows;
        infoOverlay_.lastTotalLines = contentLines;
        int maxScroll = std::max(0, contentLines - visibleRows);
        infoOverlay_.scrollOffset = std::clamp(infoOverlay_.scrollOffset, 0, maxScroll);

        // Background box
        for (int y = oy; y < oy + oh && y < h; ++y) {
            for (int x = ox; x < ox + ow && x < w; ++x)
                win.addCharColored(y, x, ' ', kColorStatusBar);
        }

        // Title (centered) — append scroll position when content overflows
        std::string title = "  " + infoOverlay_.title;
        if (maxScroll > 0) {
            int firstVisible = infoOverlay_.scrollOffset + 1;
            int lastVisible = std::min(infoOverlay_.scrollOffset + visibleRows, contentLines);
            title += "  (" + std::to_string(firstVisible) + "-" +
                     std::to_string(lastVisible) + "/" +
                     std::to_string(contentLines) + ")";
        }
        title += "  ";
        int titleX = ox + std::max(0, (ow - static_cast<int>(title.size())) / 2);
        win.printColored(oy, titleX, title, kColorTabActive);

        int row = oy + 2;
        int firstIdx = infoOverlay_.scrollOffset;
        int lastIdx = std::min(contentLines, firstIdx + visibleRows);
        for (int i = firstIdx; i < lastIdx; ++i) {
            if (row >= oy + oh - 1) break;
            int color = (wrappedColors[i] >= 0) ? wrappedColors[i] : kColorStatusBar;
            win.printColored(row++, ox + 2, wrapped[i], color);
        }

        const std::string footer = (maxScroll > 0)
            ? "  j/k scroll  q close  "
            : "  Press any key to close  ";
        int footerX = ox + std::max(0, (ow - static_cast<int>(footer.size())) / 2);
        win.printColored(std::min(row, oy + oh - 1), footerX, footer, kColorTabActive);
    }

    // Show history hint overlay when command line is active and empty
    cmdLine_.renderHistoryHint(win);

  if (overlayVisible_) {
    bool isPixel = (rendererType_ == RendererType::Pixel);

    // Draw labels on viewport
    {
        buildProjCache();
        int scaleX = canvas_ ? canvas_->scaleX() : 1;
        int scaleY = canvas_ ? canvas_->scaleY() : 1;
        auto obj = tabMgr_.currentTab().currentObject();
        if (obj && !labelAtoms_.empty()) {
            const auto& atoms = obj->atoms();
            // Build fast lookup set from label list
            std::set<int> labelSet(labelAtoms_.begin(), labelAtoms_.end());
            for (const auto& pa : projCache_) {
                if (!labelSet.count(pa.idx)) continue;

                int tx = pa.sx / scaleX;
                int ty = pa.sy / scaleY;
                if (tx < 0 || tx >= w - 6 || ty < 0 || ty >= h) continue;

                const auto& a = atoms[pa.idx];
                std::string lbl = a.resName + std::to_string(a.resSeq);
                if (isPixel) {
                    auto* pc = dynamic_cast<PixelCanvas*>(canvas_.get());
                    if (pc) pc->drawText(pa.sx + scaleX, pa.sy, pa.depth,
                                         lbl, kColorWhite);
                } else {
                    int lx = std::min(tx + 1, w - static_cast<int>(lbl.size()));
                    win.printColored(ty, lx, lbl, kColorWhite);
                }
            }
        }
    }

    // Helper: draw a dashed line between two 3D atom positions.
    // In pixel mode, draws directly into the canvas (sub-pixel space).
    // In non-pixel mode, draws into the ncurses window (terminal space).
    int subW = canvas_ ? canvas_->subW() : w;
    int subH = canvas_ ? canvas_->subH() : h;
    int scaleX = canvas_ ? canvas_->scaleX() : 1;
    int scaleY = canvas_ ? canvas_->scaleY() : 1;

    // thickness: pixel-mode line thickness in sub-pixels (1-4), ignored in non-pixel
    auto drawDashedLine = [&](float sx1, float sy1, float d1,
                              float sx2, float sy2, float d2,
                              int color, int thickness = 2) {
        if (isPixel) {
            int isx1 = static_cast<int>(sx1), isy1 = static_cast<int>(sy1);
            int isx2 = static_cast<int>(sx2), isy2 = static_cast<int>(sy2);
            int steps = std::max(std::abs(isx2 - isx1), std::abs(isy2 - isy1));
            if (steps == 0) steps = 1;
            int dashLen = std::max(4, thickness * 2);
            for (int s = 0; s <= steps; s += dashLen * 2) {
                // Draw dash segment
                for (int ds = 0; ds < dashLen && s + ds <= steps; ++ds) {
                    float t = static_cast<float>(s + ds) / static_cast<float>(steps);
                    int x = isx1 + static_cast<int>((isx2 - isx1) * t);
                    int y = isy1 + static_cast<int>((isy2 - isy1) * t);
                    float depth = d1 + (d2 - d1) * t;
                    // Draw thickness x thickness dot cluster
                    for (int dy = 0; dy < thickness; ++dy) {
                        for (int dx = 0; dx < thickness; ++dx) {
                            int px = x + dx - thickness / 2;
                            int py = y + dy - thickness / 2;
                            if (px >= 0 && px < subW && py >= 0 && py < subH)
                                canvas_->drawDot(px, py, depth, color);
                        }
                    }
                }
            }
        } else {
            int tx1 = static_cast<int>(sx1) / scaleX, ty1 = static_cast<int>(sy1) / scaleY;
            int tx2 = static_cast<int>(sx2) / scaleX, ty2 = static_cast<int>(sy2) / scaleY;
            int steps = std::max(std::abs(tx2 - tx1), std::abs(ty2 - ty1));
            if (steps == 0) steps = 1;
            for (int s = 0; s <= steps; s += 2) {
                float t = static_cast<float>(s) / static_cast<float>(steps);
                int x = tx1 + static_cast<int>((tx2 - tx1) * t);
                int y = ty1 + static_cast<int>((ty2 - ty1) * t);
                if (x >= 0 && x < w && y >= 0 && y < h)
                    win.addCharColored(y, x, '-', color);
            }
        }
    };

    // Draw measurement dashed lines + labels
    {
        auto obj = tabMgr_.currentTab().currentObject();
        if (obj && !measurements_.empty()) {
            const auto& atoms = obj->atoms();
            auto& cam = tabMgr_.currentTab().camera();
            for (const auto& m : measurements_) {
                if (m.atoms.size() < 2) continue;
                for (size_t mi = 0; mi + 1 < m.atoms.size(); ++mi) {
                    int a1 = m.atoms[mi], a2 = m.atoms[mi + 1];
                    if (a1 < 0 || a1 >= static_cast<int>(atoms.size())) continue;
                    if (a2 < 0 || a2 >= static_cast<int>(atoms.size())) continue;
                    float sx1, sy1, d1, sx2, sy2, d2;
                    cam.projectCached(atoms[a1].x, atoms[a1].y, atoms[a1].z, sx1, sy1, d1);
                    cam.projectCached(atoms[a2].x, atoms[a2].y, atoms[a2].z, sx2, sy2, d2);
                    drawDashedLine(sx1, sy1, d1, sx2, sy2, d2, kColorYellow);
                }
                // Label at midpoint of first segment
                {
                    int a1 = m.atoms[0], a2 = m.atoms[1];
                    float sx1, sy1, d1, sx2, sy2, d2;
                    cam.projectCached(atoms[a1].x, atoms[a1].y, atoms[a1].z, sx1, sy1, d1);
                    cam.projectCached(atoms[a2].x, atoms[a2].y, atoms[a2].z, sx2, sy2, d2);
                    if (isPixel) {
                        auto* pc = dynamic_cast<PixelCanvas*>(canvas_.get());
                        if (pc) {
                            int lx = (static_cast<int>(sx1) + static_cast<int>(sx2)) / 2;
                            int ly = (static_cast<int>(sy1) + static_cast<int>(sy2)) / 2;
                            pc->drawText(lx, ly, (d1 + d2) / 2.0f, m.label, kColorYellow);
                        }
                    } else {
                        int mx = (static_cast<int>(sx1) / scaleX + static_cast<int>(sx2) / scaleX) / 2;
                        int my = (static_cast<int>(sy1) / scaleY + static_cast<int>(sy2) / scaleY) / 2;
                        if (mx >= 0 && mx < w - static_cast<int>(m.label.size()) && my >= 0 && my < h)
                            win.printColored(my, mx, m.label, kColorYellow);
                    }
                }
            }
        }
    }

    // Draw selection highlight overlay for $sele atoms
    {
        auto selIt = namedSelections_.find("sele");
        if (selIt != namedSelections_.end() && !selIt->second.empty()) {
            buildProjCache();
            if (isPixel) {
                for (const auto& pa : projCache_) {
                    if (selIt->second.has(pa.idx)) {
                        if (pa.sx >= 0 && pa.sx < subW && pa.sy >= 0 && pa.sy < subH)
                            canvas_->drawDot(pa.sx, pa.sy, pa.depth - 0.01f, kColorYellow);
                    }
                }
            } else {
                for (const auto& pa : projCache_) {
                    if (selIt->second.has(pa.idx)) {
                        int tx = pa.sx / scaleX;
                        int ty = pa.sy / scaleY;
                        if (tx >= 0 && tx < w && ty >= 0 && ty < h)
                            win.addCharColored(ty, tx, '*', kColorYellow);
                    }
                }
            }
        }
    }

    // Interface overlay (sidechain bonds + dashed interaction lines) is
    // rendered by InterfaceRepr earlier in the frame so depth-fog and
    // focus-dim post-passes see it. The legacy inline drawer was removed
    // when InterfaceRepr was introduced.
  } // overlayVisible_

    win.refresh();
}

void Application::renderAnalysisPanel() {
    auto obj = tabMgr_.currentTab().currentObject();
    if (obj) {
        contactMapPanel_.update(*obj);
    }
    contactMapPanel_.render(layout_.analysisPanel());
}

void Application::updateStatusBar() {
    auto& tab = tabMgr_.currentTab();
    std::string objInfo;
    std::string rightInfo;

    auto obj = tab.currentObject();
    if (obj) {
        objInfo = obj->name() + " [" +
                  std::to_string(obj->atoms().size()) + " atoms]";
        if (obj->stateCount() > 1)
            objInfo += " S:" + std::to_string(obj->activeState() + 1) +
                       "/" + std::to_string(obj->stateCount());
        if (!obj->visible()) objInfo += " (hidden)";
    }

    if (pickMode_ != PickMode::Inspect) {
        auto selIt = namedSelections_.find("sele");
        int selCount = (selIt != namedSelections_.end()) ? static_cast<int>(selIt->second.size()) : 0;
        objInfo = std::string(pickModeName(pickMode_)) + " [" + std::to_string(selCount) + "] " + objInfo;
    }

    // Show renderer type on right
    std::string rendererName;
    switch (rendererType_) {
        case RendererType::Ascii:   rendererName = "ASCII"; break;
        case RendererType::Braille: rendererName = "BRAILLE"; break;
        case RendererType::Block:   rendererName = "BLOCK"; break;
        case RendererType::Pixel: {
            auto* pc = dynamic_cast<PixelCanvas*>(canvas_.get());
            rendererName = pc && pc->encoder() ? pc->encoder()->name() : "PIXEL";
            break;
        }
    }
    rightInfo = rendererName + " | " + tab.name();

    statusBar_.render(layout_.statusBar(), inputHandler_->mode(),
                      objInfo, rightInfo);
}

void Application::onResize() {
    fflush(stdout);
    fprintf(stdout, "\033[2J");
    fflush(stdout);

    endwin();
    refresh();
    layout_.resize(screen_.height(), screen_.width());
    layout_.markAllDirty();
    if (canvas_) canvas_->invalidate();
    framesToSkip_ = 0;
    needsRedraw_ = true;
}

void Application::buildProjCache() {
    if (projCacheFrame_ == frameCounter_ && !projCache_.empty()) return;
    projCacheFrame_ = frameCounter_;
    projCache_.clear();
    pickGrid_.clear();

    auto& tab = tabMgr_.currentTab();
    auto obj = tab.currentObject();
    if (!obj || !obj->visible()) return;

    int sw = canvas_ ? canvas_->subW() : layout_.viewportWidth();
    int sh = canvas_ ? canvas_->subH() : layout_.viewportHeight();
    float aspect = canvas_ ? canvas_->aspectYX() : 2.0f;
    int scaleX = canvas_ ? canvas_->scaleX() : 1;
    int scaleY = canvas_ ? canvas_->scaleY() : 1;
    int w = layout_.viewportWidth();
    int h = layout_.viewportHeight();

    const auto& atoms = obj->atoms();
    auto& cam = tab.camera();
    cam.prepareProjection(sw, sh, aspect);

    projCache_.reserve(atoms.size() / 2);
    for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
        float fsx, fsy, depth;
        cam.projectCached(atoms[i].x, atoms[i].y, atoms[i].z, fsx, fsy, depth);
        int tx = static_cast<int>(fsx) / scaleX;
        int ty = static_cast<int>(fsy) / scaleY;
        if (tx >= 0 && tx < w && ty >= 0 && ty < h) {
            int cacheIdx = static_cast<int>(projCache_.size());
            projCache_.push_back({i, static_cast<int>(fsx), static_cast<int>(fsy), depth});
            pickGrid_[pickGridKey(static_cast<int>(fsx), static_cast<int>(fsy))].push_back(cacheIdx);
        }
    }
}

int Application::findNearestAtom(int termX, int termY) const {
    int scaleX = canvas_ ? canvas_->scaleX() : 1;
    int scaleY = canvas_ ? canvas_->scaleY() : 1;
    int subX = termX * scaleX + scaleX / 2;
    int subY = termY * scaleY + scaleY / 2;

    int bestIdx = -1;
    float bestDist2 = std::numeric_limits<float>::max();
    float bestDepth = std::numeric_limits<float>::max();

    // Query 3x3 neighborhood in spatial hash
    int cx = subX / kPickCellSize, cy = subY / kPickCellSize;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int key = (cy + dy) * 10000 + (cx + dx);
            auto it = pickGrid_.find(key);
            if (it == pickGrid_.end()) continue;
            for (int ci : it->second) {
                const auto& pa = projCache_[ci];
                float ddx = static_cast<float>(pa.sx - subX);
                float ddy = static_cast<float>(pa.sy - subY);
                float dist2 = ddx * ddx + ddy * ddy;
                if (dist2 < bestDist2 - 0.5f ||
                    (dist2 < bestDist2 + 0.5f && pa.depth < bestDepth)) {
                    bestDist2 = dist2;
                    bestDepth = pa.depth;
                    bestIdx = pa.idx;
                }
            }
        }
    }

    float maxRange = static_cast<float>(10 * std::max(scaleX, scaleY));
    if (bestDist2 > maxRange * maxRange) return -1;
    return bestIdx;
}

std::string Application::residueInfoString(const AtomData& a) const {
    char buf[80];
    std::snprintf(buf, sizeof(buf),
        "%s %d%s (chain %s)",
        a.resName.c_str(), a.resSeq,
        (a.insCode == ' ' ? "" : std::string(1, a.insCode).c_str()),
        a.chainId.c_str());
    return std::string(buf);
}

std::string Application::atomInfoString(const MolObject& mol, int atomIdx) const {
    if (atomIdx < 0 || atomIdx >= static_cast<int>(mol.atoms().size()))
        return "";
    const auto& a = mol.atoms()[atomIdx];
    char buf[256];
    std::string insStr = (a.insCode != ' ') ? std::string(1, a.insCode) : "";
    snprintf(buf, sizeof(buf),
        "%s/%s %d%s/%s (%s) B=%.1f occ=%.2f [%.2f, %.2f, %.2f]",
        a.chainId.c_str(), a.resName.c_str(), a.resSeq,
        insStr.c_str(),
        a.name.c_str(), a.element.c_str(),
        a.bFactor, a.occupancy,
        a.x, a.y, a.z);
    return std::string(buf);
}

Selection Application::parseSelection(const std::string& expr, const MolObject& mol) {
    auto resolver = [this](const std::string& name) -> const Selection* {
        auto it = namedSelections_.find(name);
        return (it != namedSelections_.end()) ? &it->second : nullptr;
    };
    auto sel = Selection::parse(expr, mol, resolver);
    // Auto-save latest result as "sele"
    namedSelections_["sele"] = sel;
    return sel;
}

// ── Focus Selection mode ────────────────────────────────────────────────────
//
// Mol*-style click-to-focus. enterFocus snapshots camera + per-repr
// visibility, snaps the camera to the subject centroid, hides
// non-neighborhood atoms for atom-direct reprs (Wireframe/BallStick/
// Spacefill), and forces ball-stick visible on the neighborhood so
// sidechains pop. exitFocus restores everything.

std::vector<int> Application::expandByFocusGranularity(const MolObject& mol,
                                                       int atomIdx) const {
    std::vector<int> out;
    const auto& atoms = mol.atoms();
    if (atomIdx < 0 || atomIdx >= (int)atoms.size()) return out;
    const auto& a = atoms[atomIdx];

    if (focusGranularity_ == FocusGranularity::Chain) {
        for (int i = 0; i < (int)atoms.size(); ++i) {
            if (atoms[i].chainId == a.chainId) out.push_back(i);
        }
        return out;
    }

    if (focusGranularity_ == FocusGranularity::Sidechain) {
        for (int i = 0; i < (int)atoms.size(); ++i) {
            const auto& b = atoms[i];
            if (b.chainId != a.chainId || b.resSeq != a.resSeq ||
                b.insCode != a.insCode) continue;
            const std::string& nm = b.name;
            if (nm == "N" || nm == "CA" || nm == "C" || nm == "O") continue;
            out.push_back(i);
        }
        if (!out.empty()) return out;
        // Empty sidechain (e.g. Gly) → fall through to Residue.
    }

    // Residue (default + Sidechain fallback)
    for (int i = 0; i < (int)atoms.size(); ++i) {
        const auto& b = atoms[i];
        if (b.chainId == a.chainId && b.resSeq == a.resSeq &&
            b.insCode == a.insCode) {
            out.push_back(i);
        }
    }
    return out;
}

void Application::enterFocus(MolObject& mol,
                             const std::vector<int>& subjectIndices,
                             const std::string& exprDesc) {
    if (subjectIndices.empty()) return;
    if (focusSnapshot_.active) exitFocus();   // refocus → exit then re-enter

    const auto& atoms = mol.atoms();

    // Snapshot camera state.
    auto& cam = tabMgr_.currentTab().camera();
    focusSnapshot_.active = true;
    focusSnapshot_.rot    = cam.rotation();
    focusSnapshot_.cx     = cam.centerX();
    focusSnapshot_.cy     = cam.centerY();
    focusSnapshot_.cz     = cam.centerZ();
    focusSnapshot_.panX   = cam.panXOffset();
    focusSnapshot_.panY   = cam.panYOffset();
    focusSnapshot_.zoom   = cam.zoom();

    // Snapshot per-repr visibility for the atom-direct reprs we touch.
    static const ReprType kTouchedReprs[] = {
        ReprType::Wireframe, ReprType::BallStick, ReprType::Spacefill,
    };
    focusSnapshot_.reprs.clear();
    for (ReprType r : kTouchedReprs) {
        FocusSavedRepr s;
        s.type        = r;
        s.objectLevel = mol.reprVisible(r);
        s.atomMask    = mol.atomVisMask(r);    // empty if all-visible
        focusSnapshot_.reprs.push_back(std::move(s));
    }
    // Spline reprs are hidden during focus (they obscure the close-up
    // sidechain/wireframe view); save their object-level state so we can
    // put them back on exit.
    focusSnapshot_.cartoonVisible  = mol.reprVisible(ReprType::Cartoon);
    focusSnapshot_.ribbonVisible   = mol.reprVisible(ReprType::Ribbon);
    focusSnapshot_.backboneVisible = mol.reprVisible(ReprType::Backbone);
    // Snapshot wireframe thickness so we can bump it during focus.
    if (auto* wf = dynamic_cast<WireframeRepr*>(getRepr(ReprType::Wireframe))) {
        focusSnapshot_.wireframeThickness = wf->thickness();
    }

    // Compute subject centroid (for camera snap) + enclosing radius
    // (for subject-size aware zoom — Mol*-style).
    float sx = 0, sy = 0, sz = 0;
    int n = 0;
    for (int idx : subjectIndices) {
        if (idx < 0 || idx >= (int)atoms.size()) continue;
        sx += atoms[idx].x; sy += atoms[idx].y; sz += atoms[idx].z;
        ++n;
    }
    if (n == 0) { focusSnapshot_.active = false; return; }
    sx /= n; sy /= n; sz /= n;

    float r2max = 0.0f;
    for (int idx : subjectIndices) {
        if (idx < 0 || idx >= (int)atoms.size()) continue;
        float dx = atoms[idx].x - sx;
        float dy = atoms[idx].y - sy;
        float dz = atoms[idx].z - sz;
        float r2 = dx*dx + dy*dy + dz*dz;
        if (r2 > r2max) r2max = r2;
    }
    const float rEnc = std::sqrt(r2max);
    const float rPad = std::max(rEnc + focusExtraRadius_, focusMinRadius_);
    // K=20 calibrates fillFraction=1.0 to the existing `:zoom` formula
    // (40 / span ≈ 20 / R) so :zoom and a "full-fill" focus agree.
    const float targetZoom = (rPad > 0.0f)
        ? focusFillFraction_ * 20.0f / rPad
        : focusZoom_;

    cam.focusOn(sx, sy, sz, targetZoom);

    // Build subject mask first.
    focusAtomMask_.assign(atoms.size(), false);
    for (int idx : subjectIndices) {
        if (idx >= 0 && idx < (int)atoms.size()) focusAtomMask_[idx] = true;
    }

    // Spatial neighborhood: every atom within focus_radius of any
    // subject atom. This catches the close-pocket geometry — backbone
    // + sidechains touching the subject — but it's distance-based, so
    // a long sidechain reaching the pocket may have its CA outside.
    const float r2 = focusRadius_ * focusRadius_;
    focusNbhdMask_.assign(atoms.size(), false);
    for (size_t i = 0; i < atoms.size(); ++i) {
        const auto& ai = atoms[i];
        for (int j : subjectIndices) {
            if (j < 0 || j >= (int)atoms.size()) continue;
            const auto& aj = atoms[j];
            float dx = ai.x - aj.x, dy = ai.y - aj.y, dz = ai.z - aj.z;
            if (dx*dx + dy*dy + dz*dz <= r2) { focusNbhdMask_[i] = true; break; }
        }
    }

    // Ensure interface contacts are cached. We need them before building
    // nbhdIndices so the partner-residue expansion below can promote
    // whole interacting residues into the neighborhood.
    if (interfaceContacts_.empty()) {
        contactMapPanel_.update(mol);
        contactMapPanel_.contactMap().computeInterface(mol, 4.5f);
        interfaceContacts_ = contactMapPanel_.contactMap().interfaceContacts();
        focusComputedInterface_ = true;
    }

    // Promote any residue that has a contact reaching into the subject:
    // the user expects to see the whole interacting residue, not the
    // truncated portion that happens to fall inside focus_radius. Each
    // partner is identified by (chainId, resSeq, insCode), then every
    // atom sharing that key is added to the neighborhood mask.
    std::set<std::tuple<std::string, int, char>> partnerResidues;
    for (const auto& c : interfaceContacts_) {
        if (c.atom1 < 0 || c.atom2 < 0) continue;
        if (c.atom1 >= (int)atoms.size() || c.atom2 >= (int)atoms.size()) continue;
        bool s1 = focusAtomMask_[c.atom1];
        bool s2 = focusAtomMask_[c.atom2];
        if (s1 == s2) continue;     // both in subject, or neither — no partner edge
        int partner = s1 ? c.atom2 : c.atom1;
        const auto& a = atoms[partner];
        partnerResidues.emplace(a.chainId, a.resSeq, a.insCode);
    }
    // Fused pass: for each atom, promote it into the mask if its residue
    // matched a partner, then collect all in-mask atoms into nbhdIndices.
    std::vector<int> nbhdIndices;
    nbhdIndices.reserve(atoms.size());
    for (size_t i = 0; i < atoms.size(); ++i) {
        const auto& a = atoms[i];
        if (!focusNbhdMask_[i] && !partnerResidues.empty()
            && partnerResidues.count({a.chainId, a.resSeq, a.insCode})) {
            focusNbhdMask_[i] = true;
        }
        if (focusNbhdMask_[i]) nbhdIndices.push_back((int)i);
    }

    mol.hideRepr(ReprType::Cartoon);
    mol.hideRepr(ReprType::Ribbon);
    mol.hideRepr(ReprType::Backbone);

    // Bump wireframe thickness modestly so the local scaffold reads;
    // the zoom-scaling in WireframeRepr::render does the rest.
    if (auto* wf = dynamic_cast<WireframeRepr*>(getRepr(ReprType::Wireframe))) {
        wf->setThickness(std::max(0.5f, focusSnapshot_.wireframeThickness * 1.4f));
    }
    mol.showRepr(ReprType::Wireframe);
    mol.showReprForAtoms(ReprType::Wireframe, nbhdIndices);

    mol.showRepr(ReprType::BallStick);
    mol.showReprForAtoms(ReprType::BallStick, nbhdIndices);

    if (mol.reprVisible(ReprType::Spacefill)) {
        mol.showReprForAtoms(ReprType::Spacefill, nbhdIndices);
    }

    // Filter contacts to ones whose endpoints are both in the (now
    // residue-expanded) neighborhood, so the dashed lines render only
    // for what's visible in the pocket.
    std::vector<InterfaceContact> filtered;
    filtered.reserve(interfaceContacts_.size());
    for (const auto& c : interfaceContacts_) {
        if (c.atom1 < 0 || c.atom2 < 0) continue;
        if (c.atom1 >= (int)atoms.size() || c.atom2 >= (int)atoms.size()) continue;
        if (focusNbhdMask_[c.atom1] && focusNbhdMask_[c.atom2])
            filtered.push_back(c);
    }
    interfaceRepr_.setData(focusNbhdMask_, std::move(filtered));
    interfaceRepr_.setDrawSidechains(false);   // wireframe already covers it
    interfaceRepr_.setInteractionThickness(interfaceThickness_);
    interfaceRepr_.setShowMask(interfaceShowMask_);

    focusExpr_ = exprDesc.empty() ? std::string("focus") : exprDesc;
    char msg[160];
    std::snprintf(msg, sizeof(msg),
        "Focus: %d atoms (radius=%.1fA zoom=%.1f) — F or Esc to exit",
        (int)nbhdIndices.size(), focusRadius_, focusZoom_);
    cmdLine_.setMessage(msg);
    needsRedraw_ = true;
}

void Application::exitFocus() {
    if (!focusSnapshot_.active) return;
    auto obj = tabMgr_.currentTab().currentObject();
    if (!obj) {
        focusSnapshot_.active = false;
        focusAtomMask_.clear();
        focusNbhdMask_.clear();
        focusExpr_.clear();
        return;
    }

    // Restore camera.
    auto& cam = tabMgr_.currentTab().camera();
    cam.setRotation(focusSnapshot_.rot);
    cam.setCenter(focusSnapshot_.cx, focusSnapshot_.cy, focusSnapshot_.cz);
    cam.setPan(focusSnapshot_.panX, focusSnapshot_.panY);
    cam.setZoom(focusSnapshot_.zoom);

    // Restore per-repr visibility atom-for-atom.
    for (const auto& s : focusSnapshot_.reprs) {
        if (s.atomMask.empty()) {
            // Pre-focus state was "all visible" → clear any per-atom mask.
            // showRepr with no per-atom args resets the mask in current API.
            if (s.objectLevel) obj->showRepr(s.type);
            else               obj->hideRepr(s.type);
        } else {
            // Pre-focus state had a custom mask — re-apply it via
            // showReprForAtoms with the previously-visible indices.
            std::vector<int> idxs;
            idxs.reserve(s.atomMask.size());
            for (size_t i = 0; i < s.atomMask.size(); ++i)
                if (s.atomMask[i]) idxs.push_back((int)i);
            obj->showReprForAtoms(s.type, idxs);
            if (!s.objectLevel) obj->hideRepr(s.type);
        }
    }

    // Restore spline reprs.
    if (focusSnapshot_.cartoonVisible)  obj->showRepr(ReprType::Cartoon);
    else                                obj->hideRepr(ReprType::Cartoon);
    if (focusSnapshot_.ribbonVisible)   obj->showRepr(ReprType::Ribbon);
    else                                obj->hideRepr(ReprType::Ribbon);
    if (focusSnapshot_.backboneVisible) obj->showRepr(ReprType::Backbone);
    else                                obj->hideRepr(ReprType::Backbone);

    // Restore wireframe thickness.
    if (auto* wf = dynamic_cast<WireframeRepr*>(getRepr(ReprType::Wireframe))) {
        wf->setThickness(focusSnapshot_.wireframeThickness);
    }

    // Put the unfiltered interface contact list back if the global
    // overlay was on. If focus computed interactions on demand (no
    // pre-existing :interface), drop them entirely on exit.
    if (focusComputedInterface_) {
        interfaceContacts_.clear();
        interfaceAtomMask_.clear();
        interfaceRepr_.clear();
        focusComputedInterface_ = false;
    } else if (interfaceOverlay_ && !interfaceContacts_.empty()) {
        interfaceRepr_.setData(interfaceAtomMask_, interfaceContacts_);
        interfaceRepr_.setDrawSidechains(interfaceSidechains_);
        interfaceRepr_.setShowMask(interfaceShowMask_);
    } else {
        interfaceRepr_.clear();
    }

    focusSnapshot_.active = false;
    focusSnapshot_.reprs.clear();
    focusAtomMask_.clear();
    focusNbhdMask_.clear();
    focusExpr_.clear();
    cmdLine_.setMessage("Focus exited");
    needsRedraw_ = true;
}

void Application::executeSearch(const std::string& query) {
    lastSearch_ = query;
    searchMatches_.clear();
    searchIdx_ = -1;

    auto obj = tabMgr_.currentTab().currentObject();
    if (!obj) {
        cmdLine_.setMessage("No object selected");
        return;
    }

    // Parse as selection expression
    auto sel = parseSelection(query, *obj);
    searchMatches_ = std::vector<int>(sel.indices().begin(), sel.indices().end());

    if (searchMatches_.empty()) {
        cmdLine_.setMessage("/" + query + ": no matches");
    } else {
        searchIdx_ = 0;
        const auto& a = obj->atoms()[searchMatches_[0]];
        cmdLine_.setMessage("/" + query + ": " + std::to_string(searchMatches_.size()) +
                           " atoms [1/" + std::to_string(searchMatches_.size()) + "] " +
                           a.chainId + " " + a.resName + " " + std::to_string(a.resSeq) +
                           " " + a.name);
    }
}

// ── Macro recording ─────────────────────────────────────────────────────────

void Application::startMacroRecord(char reg) {
    macroRecording_ = true;
    macroRegister_ = reg;
    currentMacro_.clear();
    cmdLine_.setMessage(std::string("Recording @") + reg + "...");
}

void Application::stopMacroRecord() {
    if (!macroRecording_) return;
    macros_[macroRegister_] = std::move(currentMacro_);
    cmdLine_.setMessage(std::string("Recorded @") + macroRegister_ +
                        " (" + std::to_string(macros_[macroRegister_].size()) + " actions)");
    macroRecording_ = false;
    macroRegister_ = '\0';
    currentMacro_.clear();
}

void Application::recordAction(Action action) {
    if (!macroRecording_) return;
    // Don't record meta-actions that control recording itself
    if (action == Action::StartMacro || action == Action::PlayMacro) return;
    currentMacro_.push_back(action);
}

void Application::playMacro(char reg) {
    auto it = macros_.find(reg);
    if (it == macros_.end() || it->second.empty()) {
        cmdLine_.setMessage(std::string("Macro @") + reg + " is empty");
        return;
    }
    for (Action a : it->second) {
        handleAction(a);
    }
    cmdLine_.setMessage(std::string("Played @") + reg +
                        " (" + std::to_string(it->second.size()) + " actions)");
}

void Application::registerCommands() {
    // :q / :quit
    cmdRegistry_.registerCmd("q", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        app.quit(cmd.forced);
        return {true, ""};
    }, ":q[!]", "Quit MolTerm (use :q! to skip auto-save)",
       {":q", ":q!"}, "Help");
    cmdRegistry_.registerCmd("quit", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        app.quit(cmd.forced);
        return {true, ""};
    }, ":quit[!]", "Quit MolTerm (alias for :q)",
       {":quit"}, "Help");
    cmdRegistry_.registerCmd("qa", [](Application& app, const ParsedCommand&) -> ExecResult {
        app.quit(true);
        return {true, ""};
    }, ":qa", "Quit all tabs and exit",
       {":qa"}, "Help");

    // :load <file>
    cmdRegistry_.registerCmd("load", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) return {false, "Usage: :load <file>"};
        std::string msg = app.loadFile(cmd.args[0]);
        bool ok = msg.rfind("Loaded ", 0) == 0;
        return {ok, msg};
    }, ":load <file>", "Load a structure file (.pdb, .cif, .cif.gz, ...)",
       {":load protein.pdb", ":load 1bna.cif"}, "Files");
    cmdRegistry_.registerCmd("e", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) return {false, "Usage: :e <file>"};
        std::string msg = app.loadFile(cmd.args[0]);
        bool ok = msg.rfind("Loaded ", 0) == 0;
        return {ok, msg};
    }, ":e <file>", "Load a structure file (alias for :load)",
       {":e protein.pdb"}, "Files");

    // :tabnew
    cmdRegistry_.registerCmd("tabnew", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        std::string name = cmd.args.empty() ? "" : cmd.args[0];
        app.tabs().addTab(name);
        app.tabs().goToTab(static_cast<int>(app.tabs().count()) - 1);
        return {true, "New tab created"};
    }, ":tabnew [name]", "Create a new tab (optionally named)",
       {":tabnew", ":tabnew analysis"}, "Window");

    // :tabclose
    cmdRegistry_.registerCmd("tabclose", [](Application& app, const ParsedCommand&) -> ExecResult {
        if (app.tabs().count() <= 1) return {false, "Cannot close last tab"};
        app.tabs().closeCurrentTab();
        return {true, ""};
    }, ":tabclose", "Close the current tab",
       {":tabclose"}, "Window");

    // :objects
    cmdRegistry_.registerCmd("objects", [](Application& app, const ParsedCommand&) -> ExecResult {
        auto names = app.store().names();
        if (names.empty()) return {true, "No objects loaded"};
        std::string result = "Objects:";
        for (const auto& n : names) result += " " + n;
        return {true, result};
    }, ":objects", "List objects loaded in the current tab",
       {":objects"}, "Window");

    // Helper: resolve repr name to ReprType. Returns false if unknown.
    auto resolveRepr = [](const std::string& name, ReprType& out) -> bool {
        if (name == "wireframe" || name == "wire" || name == "lines") { out = ReprType::Wireframe; return true; }
        if (name == "ballstick" || name == "sticks" || name == "bs")  { out = ReprType::BallStick; return true; }
        if (name == "spacefill" || name == "spheres" || name == "cpk") { out = ReprType::Spacefill; return true; }
        if (name == "cartoon" || name == "tube")     { out = ReprType::Cartoon; return true; }
        if (name == "ribbon")                        { out = ReprType::Ribbon; return true; }
        if (name == "backbone" || name == "trace" || name == "ca") { out = ReprType::Backbone; return true; }
        return false;
    };

    // :show <repr> [selection]
    cmdRegistry_.registerCmd("show", [resolveRepr](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) return {false, "Usage: :show <repr> [selection]"};
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};
        ReprType rt;
        if (!resolveRepr(cmd.args[0], rt)) return {false, "Unknown representation: " + cmd.args[0]};

        if (cmd.args.size() > 1) {
            // Per-atom show: :show cartoon chain A
            std::string expr;
            for (size_t i = 1; i < cmd.args.size(); ++i) {
                if (i > 1) expr += " ";
                expr += cmd.args[i];
            }
            auto sel = app.parseSelection(expr, *obj);
            if (sel.empty()) return {false, "No atoms match: " + expr};
            obj->showReprForAtoms(rt, std::vector<int>(sel.indices().begin(), sel.indices().end()));
            return {true, "Showing " + cmd.args[0] + " for " + std::to_string(sel.size()) + " atoms"};
        }
        obj->showRepr(rt);
        return {true, "Showing " + cmd.args[0]};
    }, ":show <repr> [selection]", "Show representation (wireframe, ballstick, spacefill, cartoon, ribbon, backbone)",
       {":show cartoon", ":show ballstick chain A", ":show wire resn HEM"}, "Display");

    // :hide [repr|all] [selection]
    cmdRegistry_.registerCmd("hide", [resolveRepr](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};
        if (cmd.args.empty() || cmd.args[0] == "all") {
            obj->hideAllRepr();
            return {true, "Hidden all representations"};
        }
        ReprType rt;
        bool hasRepr = resolveRepr(cmd.args[0], rt);

        if (!hasRepr) {
            // No repr specified — treat all args as selection, hide all reprs for those atoms
            std::string expr;
            for (size_t i = 0; i < cmd.args.size(); ++i) {
                if (i > 0) expr += " ";
                expr += cmd.args[i];
            }
            auto sel = app.parseSelection(expr, *obj);
            if (sel.empty()) return {false, "No atoms match: " + expr};
            obj->hideAllReprForAtoms(std::vector<int>(sel.indices().begin(), sel.indices().end()));
            return {true, "Hidden all representations for " + std::to_string(sel.size()) + " atoms"};
        }

        if (cmd.args.size() > 1) {
            std::string expr;
            for (size_t i = 1; i < cmd.args.size(); ++i) {
                if (i > 1) expr += " ";
                expr += cmd.args[i];
            }
            auto sel = app.parseSelection(expr, *obj);
            if (sel.empty()) return {false, "No atoms match: " + expr};
            obj->hideReprForAtoms(rt, std::vector<int>(sel.indices().begin(), sel.indices().end()));
            return {true, "Hidden " + cmd.args[0] + " for " + std::to_string(sel.size()) + " atoms"};
        }
        obj->hideRepr(rt);
        return {true, "Hidden " + cmd.args[0]};
    }, ":hide [repr|all] [selection]", "Hide representation, or all representations",
       {":hide all", ":hide cartoon", ":hide wire chain B"}, "Display");

    // :color <scheme>
    cmdRegistry_.registerCmd("color", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty())
            return {false, "Usage: :color <scheme> or :color <name> <selection> | Colors: " + ColorMapper::availableColors()};
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};

        const auto& first = cmd.args[0];

        // Check if first arg is a color scheme name
        if (first == "element" || first == "cpk") {
            int count = applyHeteroatomColors(*obj);
            return {true, "Colored " + std::to_string(count) + " heteroatoms by element"};
        }
        if (first == "chain") {
            // "color chain" with no more args → scheme
            // "color chain chain A" → ambiguous, treat as scheme if only 1 arg
            if (cmd.args.size() == 1) {
                obj->setColorScheme(ColorScheme::Chain);
                obj->clearAtomColors();
                return {true, "Coloring by chain"};
            }
            // Fall through to try as named color
        }
        if (first == "ss" || first == "secondary") {
            obj->setColorScheme(ColorScheme::SecondaryStructure);
            obj->clearAtomColors();
            return {true, "Coloring by SS"};
        }
        if (first == "bfactor" || first == "b") {
            obj->setColorScheme(ColorScheme::BFactor);
            obj->clearAtomColors();
            return {true, "Coloring by B-factor"};
        }
        if (first == "plddt") {
            obj->setColorScheme(ColorScheme::PLDDT);
            obj->clearAtomColors();
            return {true, "Coloring by pLDDT (AlphaFold confidence)"};
        }
        if (first == "rainbow") {
            obj->setColorScheme(ColorScheme::Rainbow);
            obj->clearAtomColors();
            return {true, "Coloring rainbow (N→C terminus)"};
        }
        if (first == "restype" || first == "type") {
            obj->setColorScheme(ColorScheme::ResType);
            obj->clearAtomColors();
            return {true, "Coloring by residue type (nonpolar/polar/acidic/basic)"};
        }
        if (first == "heteroatom" || first == "hetero" || first == "het_color") {
            int count = applyHeteroatomColors(*obj);
            return {true, "Colored " + std::to_string(count) + " heteroatoms by element"};
        }
        if (first == "clear" || first == "reset") {
            obj->clearAtomColors();
            return {true, "Cleared per-atom colors"};
        }

        // Try as named color + optional selection:
        // :color red              → color all atoms red
        // :color red chain A      → color chain A red
        int colorPair = ColorMapper::colorByName(first);
        if (colorPair < 0)
            return {false, "Unknown color/scheme: " + first + " | Available: " + ColorMapper::availableColors()};

        if (cmd.args.size() == 1) {
            // Color all atoms
            auto sel = Selection::all(static_cast<int>(obj->atoms().size()));
            obj->setAtomColors(std::vector<int>(sel.indices().begin(), sel.indices().end()), colorPair);
            return {true, "Colored all atoms " + first};
        }

        // Remaining args form a selection expression
        std::string expr;
        for (size_t i = 1; i < cmd.args.size(); ++i) {
            if (i > 1) expr += " ";
            expr += cmd.args[i];
        }

        auto sel = app.parseSelection(expr, *obj);
        if (sel.empty()) return {false, "No atoms match: " + expr};
        obj->setAtomColors(std::vector<int>(sel.indices().begin(), sel.indices().end()), colorPair);
        return {true, "Colored " + std::to_string(sel.size()) + " atoms " + first};
    }, ":color <scheme|name> [selection]",
       "Set coloring scheme (element, chain, ss, bfactor, plddt, rainbow, restype, heteroatom) or named color",
       {":color ss", ":color chain", ":color red chain A", ":color rainbow"}, "Coloring");

    // :zoom
    // Helper: get atom indices from optional selection args
    auto resolveAtoms = [](Application& app, const ParsedCommand& cmd, int startArg = 0)
        -> std::pair<std::vector<int>, std::string> {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {{}, "No object selected"};
        if (cmd.args.size() <= static_cast<size_t>(startArg)) {
            // All atoms
            std::vector<int> all(obj->atoms().size());
            for (int i = 0; i < static_cast<int>(obj->atoms().size()); ++i) all[i] = i;
            return {all, ""};
        }
        std::string expr;
        for (size_t i = startArg; i < cmd.args.size(); ++i) {
            if (i > static_cast<size_t>(startArg)) expr += " ";
            expr += cmd.args[i];
        }
        auto sel = app.parseSelection(expr, *obj);
        if (sel.empty()) return {{}, "No atoms match: " + expr};
        return {std::vector<int>(sel.indices().begin(), sel.indices().end()), ""};
    };

    // Helper: compute center and span from atom indices
    auto computeGeometry = [](const MolObject& obj, const std::vector<int>& indices)
        -> std::tuple<float, float, float, float> {
        const auto& atoms = obj.atoms();
        float cx = 0, cy = 0, cz = 0;
        float minX = std::numeric_limits<float>::max(), maxX = std::numeric_limits<float>::lowest();
        float minY = minX, maxY = maxX, minZ = minX, maxZ = maxX;
        for (int i : indices) {
            cx += atoms[i].x; cy += atoms[i].y; cz += atoms[i].z;
            if (atoms[i].x < minX) minX = atoms[i].x;
            if (atoms[i].x > maxX) maxX = atoms[i].x;
            if (atoms[i].y < minY) minY = atoms[i].y;
            if (atoms[i].y > maxY) maxY = atoms[i].y;
            if (atoms[i].z < minZ) minZ = atoms[i].z;
            if (atoms[i].z > maxZ) maxZ = atoms[i].z;
        }
        float n = static_cast<float>(indices.size());
        cx /= n; cy /= n; cz /= n;
        float span = std::max({maxX - minX, maxY - minY, maxZ - minZ});
        return {cx, cy, cz, span};
    };

    // :center [selection]
    cmdRegistry_.registerCmd("center", [resolveAtoms, computeGeometry](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto [indices, err] = resolveAtoms(app, cmd);
        if (!err.empty()) return {false, err};
        auto obj = app.tabs().currentTab().currentObject();
        auto [cx, cy, cz, span] = computeGeometry(*obj, indices);
        app.tabs().currentTab().camera().setCenter(cx, cy, cz);
        return {true, "Centered on " + std::to_string(indices.size()) + " atoms"};
    }, ":center [selection]", "Center the view on a selection (or whole object)",
       {":center", ":center chain A", ":center resn HEM"}, "View");

    // :zoom [selection]
    cmdRegistry_.registerCmd("zoom", [resolveAtoms, computeGeometry](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto [indices, err] = resolveAtoms(app, cmd);
        if (!err.empty()) return {false, err};
        auto obj = app.tabs().currentTab().currentObject();
        auto [cx, cy, cz, span] = computeGeometry(*obj, indices);
        app.tabs().currentTab().camera().setCenter(cx, cy, cz);
        if (span > 0.0f) app.tabs().currentTab().camera().setZoom(40.0f / span);
        return {true, "Zoomed to " + std::to_string(indices.size()) + " atoms"};
    }, ":zoom [selection]", "Center and zoom to fit the selection (or whole object)",
       {":zoom", ":zoom chain A", ":zoom resi 50-80"}, "View");

    // :orient [view <vx>,<vy>,<vz>] [selection] — align PCA axes, optionally view from a
    // direction expressed in the PCA frame (e1=longest, e2=mid, e3=shortest).
    // Default v_pca = (0,0,1): look down the shortest axis (flat face on screen).
    cmdRegistry_.registerCmd("orient", [resolveAtoms, computeGeometry](Application& app, const ParsedCommand& cmd) -> ExecResult {
        double vx = 0.0, vy = 0.0, vz = 1.0;
        int selStart = 0;
        if (!cmd.args.empty() && cmd.args[0] == "view") {
            // Parser splits on whitespace AND commas, so "view 1,1,0" and "view 1 1 0"
            // both arrive as four separate args.
            if (cmd.args.size() < 4) {
                return {false, "Usage: :orient view <vx> <vy> <vz> [selection]"};
            }
            try {
                vx = std::stod(cmd.args[1]);
                vy = std::stod(cmd.args[2]);
                vz = std::stod(cmd.args[3]);
            } catch (...) {
                return {false, "Invalid view vector: " + cmd.args[1] + " " + cmd.args[2] + " " + cmd.args[3]};
            }
            double vlen = std::sqrt(vx*vx + vy*vy + vz*vz);
            if (vlen < 1e-10) return {false, "View vector cannot be zero"};
            vx /= vlen; vy /= vlen; vz /= vlen;
            selStart = 4;
        }

        auto [indices, err] = resolveAtoms(app, cmd, selStart);
        if (!err.empty()) return {false, err};
        auto obj = app.tabs().currentTab().currentObject();
        auto [cx, cy, cz, span] = computeGeometry(*obj, indices);
        auto& cam = app.tabs().currentTab().camera();
        cam.setCenter(cx, cy, cz);
        if (span > 0.0f) cam.setZoom(40.0f / span);

        if (indices.size() < 2) {
            return {true, "Centered (need >=2 atoms for orientation)"};
        }

        const auto& atoms = obj->atoms();
        double A[3][3] = {};
        for (int i : indices) {
            double dx = atoms[i].x - cx, dy = atoms[i].y - cy, dz = atoms[i].z - cz;
            A[0][0] += dx*dx; A[0][1] += dx*dy; A[0][2] += dx*dz;
            A[1][1] += dy*dy; A[1][2] += dy*dz;
            A[2][2] += dz*dz;
        }
        A[1][0] = A[0][1]; A[2][0] = A[0][2]; A[2][1] = A[1][2];

        // Jacobi eigendecomposition for symmetric 3x3
        double V[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
        for (int sweep = 0; sweep < 50; ++sweep) {
            double off = std::abs(A[0][1]) + std::abs(A[0][2]) + std::abs(A[1][2]);
            if (off < 1e-12) break;
            for (int p = 0; p < 3; ++p) {
                for (int q = p + 1; q < 3; ++q) {
                    double apq = A[p][q];
                    if (std::abs(apq) < 1e-15) continue;
                    double theta = (A[q][q] - A[p][p]) / (2.0 * apq);
                    double t = (theta >= 0)
                        ? 1.0 / (theta + std::sqrt(1.0 + theta*theta))
                        : 1.0 / (theta - std::sqrt(1.0 + theta*theta));
                    double c = 1.0 / std::sqrt(1.0 + t*t);
                    double s = t * c;
                    double app_old = A[p][p], aqq_old = A[q][q];
                    A[p][p] = app_old - t*apq;
                    A[q][q] = aqq_old + t*apq;
                    A[p][q] = A[q][p] = 0.0;
                    for (int r = 0; r < 3; ++r) {
                        if (r != p && r != q) {
                            double arp = A[r][p], arq = A[r][q];
                            A[r][p] = A[p][r] = c*arp - s*arq;
                            A[r][q] = A[q][r] = s*arp + c*arq;
                        }
                    }
                    for (int r = 0; r < 3; ++r) {
                        double vrp = V[r][p], vrq = V[r][q];
                        V[r][p] = c*vrp - s*vrq;
                        V[r][q] = s*vrp + c*vrq;
                    }
                }
            }
        }

        // Sort eigenvectors by eigenvalue descending: e1 (longest) → e3 (shortest)
        int order[3] = {0, 1, 2};
        double eig[3] = {A[0][0], A[1][1], A[2][2]};
        if (eig[order[0]] < eig[order[1]]) std::swap(order[0], order[1]);
        if (eig[order[1]] < eig[order[2]]) std::swap(order[1], order[2]);
        if (eig[order[0]] < eig[order[1]]) std::swap(order[0], order[1]);

        double e1[3] = {V[0][order[0]], V[1][order[0]], V[2][order[0]]};
        double e2[3] = {V[0][order[1]], V[1][order[1]], V[2][order[1]]};
        double e3[3] = {V[0][order[2]], V[1][order[2]], V[2][order[2]]};

        // Force right-handed PCA frame: e3 should equal e1 × e2
        double cr[3] = {
            e1[1]*e2[2] - e1[2]*e2[1],
            e1[2]*e2[0] - e1[0]*e2[2],
            e1[0]*e2[1] - e1[1]*e2[0]
        };
        if (cr[0]*e3[0] + cr[1]*e3[1] + cr[2]*e3[2] < 0) {
            e3[0] = -e3[0]; e3[1] = -e3[1]; e3[2] = -e3[2];
        }

        // View direction in world space
        double sz[3] = {
            vx*e1[0] + vy*e2[0] + vz*e3[0],
            vx*e1[1] + vy*e2[1] + vz*e3[1],
            vx*e1[2] + vy*e2[2] + vz*e3[2],
        };

        // Up reference in PCA frame: prefer e2; if view is too close to e2, fall back to e1
        double up_pca[3] = {0, 1, 0};
        if (std::abs(vy) > 0.9) { up_pca[0] = 1; up_pca[1] = 0; up_pca[2] = 0; }
        double up[3] = {
            up_pca[0]*e1[0] + up_pca[1]*e2[0] + up_pca[2]*e3[0],
            up_pca[0]*e1[1] + up_pca[1]*e2[1] + up_pca[2]*e3[1],
            up_pca[0]*e1[2] + up_pca[1]*e2[2] + up_pca[2]*e3[2],
        };

        // Project up onto plane perpendicular to view → screen Y
        double dot_uv = up[0]*sz[0] + up[1]*sz[1] + up[2]*sz[2];
        double sy[3] = {up[0] - dot_uv*sz[0], up[1] - dot_uv*sz[1], up[2] - dot_uv*sz[2]};
        double sylen = std::sqrt(sy[0]*sy[0] + sy[1]*sy[1] + sy[2]*sy[2]);
        sy[0] /= sylen; sy[1] /= sylen; sy[2] /= sylen;

        // screen X = screen Y × screen Z (right-handed)
        double sx[3] = {
            sy[1]*sz[2] - sy[2]*sz[1],
            sy[2]*sz[0] - sy[0]*sz[2],
            sy[0]*sz[1] - sy[1]*sz[0],
        };

        std::array<float, 9> rot;
        rot[0] = (float)sx[0]; rot[1] = (float)sx[1]; rot[2] = (float)sx[2];
        rot[3] = (float)sy[0]; rot[4] = (float)sy[1]; rot[5] = (float)sy[2];
        rot[6] = (float)sz[0]; rot[7] = (float)sz[1]; rot[8] = (float)sz[2];
        cam.setRotation(rot);

        return {true, "Oriented " + std::to_string(indices.size()) + " atoms"};
    }, ":orient [view <vx> <vy> <vz>] [selection]",
       "Center, zoom, and align principal axes (default: view down shortest axis)",
       {":orient", ":orient chain A", ":orient view 1 0 0"}, "View");

    // :turn x|y|z <deg>  — incremental camera rotation around screen axes,
    // no PCA, no recompute. Mirrors PyMOL's `turn` and is the cheap path
    // for spinning animations: orient once, then turn N° per frame.
    cmdRegistry_.registerCmd("turn", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.size() < 2) {
            return {false, "Usage: :turn x|y|z <degrees>"};
        }
        const auto& axis = cmd.args[0];
        float deg;
        try {
            deg = std::stof(cmd.args[1]);
        } catch (...) {
            return {false, "Invalid angle: " + cmd.args[1]};
        }
        auto& cam = app.tabs().currentTab().camera();
        if      (axis == "x" || axis == "X") cam.rotateX(deg);
        else if (axis == "y" || axis == "Y") cam.rotateY(deg);
        else if (axis == "z" || axis == "Z") cam.rotateZ(deg);
        else return {false, "Axis must be x, y, or z (got '" + axis + "')"};
        return {true, "Turned " + axis + " by " + std::to_string(deg) + " deg"};
    }, ":turn x|y|z <deg>", "Rotate camera around a screen axis (no PCA recompute)",
       {":turn y 90", ":turn x -45"}, "View");

    // :set <option> [value]
    cmdRegistry_.registerCmd("set", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) return {false, "Usage: :set <option> [value]"};
        const auto& opt = cmd.args[0];
        if (opt == "panel") {
            if (cmd.args.size() < 2) return {false, "Usage: :set panel on|off"};
            auto v = parseBool(cmd.args[1]);
            if (!v) return {false, "Usage: :set panel on|off"};
            app.layout().setPanel(*v);
            app.tabs().currentTab().viewState().panelVisible = app.layout().panelVisible();
            return {true, app.layout().panelVisible() ? "Panel visible" : "Panel hidden"};
        }
        if (opt == "renderer" || opt == "render") {
            if (cmd.args.size() < 2) return {false, "Usage: :set renderer <ascii|braille|block|sixel|pixel|auto|kitty|iterm2>"};
            const auto& val = cmd.args[1];
            if (val == "ascii")        app.setRenderer(RendererType::Ascii);
            else if (val == "braille") app.setRenderer(RendererType::Braille);
            else if (val == "block")   app.setRenderer(RendererType::Block);
            else if (val == "pixel" || val == "auto") {
                app.setForcedProtocol(GraphicsProtocol::None);
                app.setRenderer(RendererType::Pixel);
            } else if (val == "sixel") {
                app.setForcedProtocol(GraphicsProtocol::Sixel);
                app.setRenderer(RendererType::Pixel);
            } else if (val == "kitty") {
                app.setForcedProtocol(GraphicsProtocol::Kitty);
                app.setRenderer(RendererType::Pixel);
            } else if (val == "iterm2") {
                app.setForcedProtocol(GraphicsProtocol::ITerm2);
                app.setRenderer(RendererType::Pixel);
            }
            else return {false, "Unknown renderer: " + val};
            clearScreenAndRepaint();
            return {true, "Renderer set to " + val};
        }
        if (opt == "backbone_thickness" || opt == "bt") {
            if (cmd.args.size() < 2) return {false, "Usage: :set backbone_thickness <0.5-10>"};
            float val = std::stof(cmd.args[1]);
            auto* bb = dynamic_cast<BackboneRepr*>(app.getRepr(ReprType::Backbone));
            if (bb) {
                bb->setThickness(val);
                return {true, "Backbone thickness set to " + std::to_string(bb->thickness())};
            }
            return {false, "Backbone repr not found"};
        }
        if (opt == "wireframe_thickness" || opt == "wt") {
            if (cmd.args.size() < 2) return {false, "Usage: :set wireframe_thickness <0.5-10>"};
            float val = std::stof(cmd.args[1]);
            auto* wf = dynamic_cast<WireframeRepr*>(app.getRepr(ReprType::Wireframe));
            if (wf) {
                wf->setThickness(val);
                return {true, "Wireframe thickness set to " + std::to_string(wf->thickness())};
            }
            return {false, "Wireframe repr not found"};
        }
        if (opt == "ball_radius" || opt == "br") {
            if (cmd.args.size() < 2) return {false, "Usage: :set ball_radius <1-10>"};
            int val = std::stoi(cmd.args[1]);
            auto* bs = dynamic_cast<BallStickRepr*>(app.getRepr(ReprType::BallStick));
            if (bs) {
                bs->setBallRadius(val);
                return {true, "Ball radius set to " + std::to_string(val)};
            }
            return {false, "BallStick repr not found"};
        }
        if (opt == "pan_speed" || opt == "ps") {
            if (cmd.args.size() < 2) return {false, "Usage: :set pan_speed <1-20>"};
            float val = std::stof(cmd.args[1]);
            app.tabs().currentTab().camera().setPanSpeed(val);
            return {true, "Pan speed set to " + std::to_string(val)};
        }
        if (opt == "fog") {
            if (cmd.args.size() < 2) return {false, "Usage: :set fog <0.0-1.0> (0=off)"};
            float val = std::stof(cmd.args[1]);
            app.setFogStrength(val);
            return {true, "Fog strength set to " + std::to_string(val)};
        }
        if (opt == "outline") {
            if (cmd.args.size() < 2) return {false, "Usage: :set outline on|off"};
            auto v = parseBool(cmd.args[1]);
            if (!v) return {false, "Usage: :set outline on|off"};
            app.setOutlineEnabled(*v);
            return {true, std::string("Outline: ") + (*v ? "on" : "off")};
        }
        if (opt == "outline_threshold" || opt == "ot") {
            if (cmd.args.size() < 2) return {false, "Usage: :set outline_threshold <0.0-1.0>"};
            app.setOutlineThreshold(std::stof(cmd.args[1]));
            return {true, "Outline threshold set to " + cmd.args[1]};
        }
        if (opt == "outline_darken" || opt == "od") {
            if (cmd.args.size() < 2) return {false, "Usage: :set outline_darken <0.0-1.0>"};
            app.setOutlineDarken(std::stof(cmd.args[1]));
            return {true, "Outline darken set to " + cmd.args[1]};
        }
        if (opt == "focus_dim" || opt == "fd") {
            if (cmd.args.size() < 2) return {false, "Usage: :set focus_dim <0.0-1.0>"};
            float v = std::stof(cmd.args[1]);
            v = std::max(0.0f, std::min(1.0f, v));
            app.focusDimStrength_ = v;
            return {true, "Focus dim strength: " + std::to_string(v)};
        }
        if (opt == "focus_radius" || opt == "fr") {
            if (cmd.args.size() < 2) return {false, "Usage: :set focus_radius <Å>"};
            float v = std::stof(cmd.args[1]);
            v = std::max(0.5f, std::min(50.0f, v));
            app.focusRadius_ = v;
            return {true, "Focus radius: " + std::to_string(v) + " Å"};
        }
        if (opt == "focus_zoom" || opt == "fz") {
            if (cmd.args.size() < 2) return {false, "Usage: :set focus_zoom <float>"};
            float v = std::stof(cmd.args[1]);
            v = std::max(0.1f, std::min(50.0f, v));
            app.focusZoom_ = v;
            return {true, "Focus zoom (fallback): " + std::to_string(v) +
                          " (subject-size aware zoom is now used; tune via focus_fill)"};
        }
        if (opt == "focus_fill" || opt == "ff") {
            if (cmd.args.size() < 2) return {false, "Usage: :set focus_fill <0.05-1.0>"};
            float v = std::stof(cmd.args[1]);
            v = std::max(0.05f, std::min(1.0f, v));
            app.focusFillFraction_ = v;
            return {true, "Focus fill fraction: " + std::to_string(v)};
        }
        if (opt == "focus_extra" || opt == "fe") {
            if (cmd.args.size() < 2) return {false, "Usage: :set focus_extra <Å>"};
            float v = std::stof(cmd.args[1]);
            v = std::max(0.0f, std::min(50.0f, v));
            app.focusExtraRadius_ = v;
            return {true, "Focus extra radius: " + std::to_string(v) + " Å"};
        }
        if (opt == "focus_min_radius" || opt == "fmr") {
            if (cmd.args.size() < 2) return {false, "Usage: :set focus_min_radius <Å>"};
            float v = std::stof(cmd.args[1]);
            v = std::max(0.1f, std::min(50.0f, v));
            app.focusMinRadius_ = v;
            return {true, "Focus min radius: " + std::to_string(v) + " Å"};
        }
        if (opt == "focus_granularity" || opt == "fg") {
            if (cmd.args.size() < 2)
                return {false, "Usage: :set focus_granularity <residue|chain|sidechain>"};
            const std::string& g = cmd.args[1];
            if (g == "residue" || g == "res") {
                app.focusGranularity_ = FocusGranularity::Residue;
            } else if (g == "chain" || g == "c") {
                app.focusGranularity_ = FocusGranularity::Chain;
            } else if (g == "sidechain" || g == "sc") {
                app.focusGranularity_ = FocusGranularity::Sidechain;
            } else {
                return {false, "Unknown granularity: " + g + " (use residue|chain|sidechain)"};
            }
            return {true, "Focus granularity: " + g};
        }
        if (opt == "wf_thickness" || opt == "wft") {
            if (cmd.args.size() < 2)
                return {false, "Usage: :set wf_thickness <0.1-2.0>"};
            float v = std::stof(cmd.args[1]);
            v = std::max(0.1f, std::min(2.0f, v));
            auto* wf = dynamic_cast<WireframeRepr*>(
                app.getRepr(ReprType::Wireframe));
            if (!wf) return {false, "Wireframe repr not found"};
            wf->setThickness(v);
            return {true, "Wireframe thickness: " + std::to_string(v)};
        }
        if (opt == "interface_zoom" || opt == "iz") {
            if (cmd.args.size() < 2) {
                app.interfaceZoomGate_.setEnabled(false);
                return {true, "interface_zoom disabled"};
            }
            const std::string& v = cmd.args[1];
            if (v == "off" || v == "none") {
                app.interfaceZoomGate_.setEnabled(false);
                return {true, "interface_zoom disabled"};
            }
            float thresh = std::stof(v);
            app.interfaceZoomGate_.setThreshold(thresh);
            app.interfaceZoomGate_.setEnabled(true);
            return {true, "interface_zoom threshold: " + v};
        }
        if (opt == "interface_sidechains" || opt == "isc") {
            if (cmd.args.size() < 2)
                return {false, "Usage: :set interface_sidechains on|off"};
            auto vb = parseBool(cmd.args[1]);
            if (!vb) return {false, "Usage: :set interface_sidechains on|off"};
            app.interfaceSidechains_ = *vb;
            app.interfaceRepr_.setDrawSidechains(*vb);
            return {true, std::string("Interface sidechains: ") +
                          (*vb ? "on" : "off")};
        }
        if (opt == "interface_thickness" || opt == "ith") {
            if (cmd.args.size() < 2)
                return {false, "Usage: :set interface_thickness <1-6>"};
            int t = std::stoi(cmd.args[1]);
            t = std::max(1, std::min(6, t));
            app.interfaceThickness_ = t;
            app.interfaceRepr_.setInteractionThickness(t);
            app.interfaceRepr_.setLineThickness(std::max(1, t - 1));
            return {true, "Interface thickness: " + std::to_string(t)};
        }
        if (opt == "cartoon_helix" || opt == "ch") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_helix <0.1-3.0>"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (ct) { ct->setHelixRadius(std::stof(cmd.args[1])); return {true, "Cartoon helix radius: " + cmd.args[1]}; }
            return {false, "Cartoon repr not found"};
        }
        if (opt == "cartoon_sheet" || opt == "csh") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_sheet <0.1-3.0>"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (ct) { ct->setSheetRadius(std::stof(cmd.args[1])); return {true, "Cartoon sheet radius: " + cmd.args[1]}; }
            return {false, "Cartoon repr not found"};
        }
        if (opt == "cartoon_loop" || opt == "cl") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_loop <0.05-1.0>"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (ct) { ct->setLoopRadius(std::stof(cmd.args[1])); return {true, "Cartoon loop radius: " + cmd.args[1]}; }
            return {false, "Cartoon repr not found"};
        }
        if (opt == "cartoon_subdiv" || opt == "csd") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_subdiv <2-16>"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (ct) { ct->setSubdivisions(std::stoi(cmd.args[1])); return {true, "Cartoon subdivisions: " + cmd.args[1]}; }
            return {false, "Cartoon repr not found"};
        }
        if (opt == "cartoon_aspect" || opt == "csa") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_aspect <1.0-12.0>"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            float v = std::stof(cmd.args[1]);
            v = std::max(1.0f, std::min(12.0f, v));
            ct->setHelixAspect(v);
            return {true, "Cartoon helix aspect (W:H): " + std::to_string(v)};
        }
        if (opt == "cartoon_helix_radial" || opt == "chr") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_helix_radial <4-64>"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            int v = std::stoi(cmd.args[1]);
            ct->setHelixRadialSegments(v);
            return {true, "Cartoon helix radial vertices: " +
                          std::to_string(ct->helixRadialSegments())};
        }
        if (opt == "cartoon_tubular_helix" || opt == "cth") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_tubular_helix on|off"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            auto on = parseBool(cmd.args[1]);
            if (!on) return {false, "Use on|off|true|false"};
            ct->setTubularHelix(*on);
            return {true, std::string("Cartoon tubular helix: ") +
                          (*on ? "on (circular tube)" : "off (elliptical ribbon)")};
        }
        if (opt == "cartoon_tubular_radius" || opt == "ctr") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_tubular_radius <0.1-3.0>"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            float v = std::stof(cmd.args[1]);
            v = std::max(0.1f, std::min(3.0f, v));
            ct->setTubularRadius(v);
            return {true, "Cartoon tubular helix radius: " + std::to_string(v) + " Å"};
        }
        if (opt == "bs_units") {
            if (cmd.args.size() < 2) return {false, "Usage: :set bs_units vdw|cell"};
            auto* bs = dynamic_cast<BallStickRepr*>(app.getRepr(ReprType::BallStick));
            if (!bs) return {false, "BallStick repr not found"};
            const std::string& v = cmd.args[1];
            if (v == "vdw" || v == "a") {
                bs->setUseVdwSize(true);
                return {true, "BallStick units: vdw (Å × bs_factor)"};
            }
            if (v == "cell" || v == "subpx") {
                bs->setUseVdwSize(false);
                return {true, "BallStick units: cell (legacy ball_radius)"};
            }
            return {false, "Unknown bs_units: " + v + " (use vdw|cell)"};
        }
        if (opt == "bs_factor" || opt == "bsf") {
            if (cmd.args.size() < 2) return {false, "Usage: :set bs_factor <0.05-1.0>"};
            auto* bs = dynamic_cast<BallStickRepr*>(app.getRepr(ReprType::BallStick));
            if (!bs) return {false, "BallStick repr not found"};
            float v = std::stof(cmd.args[1]);
            v = std::max(0.05f, std::min(1.0f, v));
            bs->setSizeFactor(v);
            return {true, "BallStick size factor (×vdW): " + std::to_string(v)};
        }
        if (opt == "spacefill_scale" || opt == "ss_scale" || opt == "sfs") {
            if (cmd.args.size() < 2) return {false, "Usage: :set spacefill_scale <0.1-2.0>"};
            auto* sf = dynamic_cast<SpacefillRepr*>(app.getRepr(ReprType::Spacefill));
            if (!sf) return {false, "Spacefill repr not found"};
            float v = std::stof(cmd.args[1]);
            v = std::max(0.1f, std::min(2.0f, v));
            sf->setScale(v);
            return {true, "Spacefill scale (×vdW): " + std::to_string(v)};
        }
        if (opt == "nucleic_backbone" || opt == "nb") {
            if (cmd.args.size() < 2)
                return {false, "Usage: :set nucleic_backbone p|c4"};
            auto* ct = dynamic_cast<CartoonRepr*>(
                app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            const std::string& v = cmd.args[1];
            if (v == "p" || v == "P") {
                ct->setNucleicBackbone(CartoonRepr::NucleicBackbone::P);
                return {true, "Nucleic backbone trace: P (phosphate)"};
            }
            if (v == "c4" || v == "C4" || v == "c4'" || v == "C4'") {
                ct->setNucleicBackbone(CartoonRepr::NucleicBackbone::C4);
                return {true, "Nucleic backbone trace: C4'"};
            }
            return {false, "Unknown nucleic backbone: " + v + " (use p or c4)"};
        }
        if (opt == "lod_medium") {
            if (cmd.args.size() < 2) return {false, "Usage: :set lod_medium <atom_count>"};
            Representation::lodMediumThreshold = std::stoul(cmd.args[1]);
            return {true, "LOD medium threshold: " + cmd.args[1]};
        }
        if (opt == "lod_low") {
            if (cmd.args.size() < 2) return {false, "Usage: :set lod_low <atom_count>"};
            Representation::lodLowThreshold = std::stoul(cmd.args[1]);
            return {true, "LOD low threshold: " + cmd.args[1]};
        }
        if (opt == "backbone_cutoff") {
            if (cmd.args.size() < 2) return {false, "Usage: :set backbone_cutoff <atom_count>"};
            Representation::backboneCutoff = std::stoul(cmd.args[1]);
            return {true, "Backbone fallback cutoff: " + cmd.args[1]};
        }
        if (opt == "auto_center") {
            if (cmd.args.size() < 2) return {false, "Usage: :set auto_center on|off"};
            auto v = parseBool(cmd.args[1]);
            if (!v) return {false, "Usage: :set auto_center on|off"};
            app.setAutoCenter(*v);
            return {true, std::string("Auto-center on load: ") + (*v ? "on" : "off")};
        }
        if (opt == "seqbar") {
            if (cmd.args.size() < 2) return {false, "Usage: :set seqbar on|off"};
            auto v = parseBool(cmd.args[1]);
            if (!v) return {false, "Usage: :set seqbar on|off"};
            app.layout().setSeqBar(*v);
            auto& vs = app.tabs().currentTab().viewState();
            vs.seqBarVisible = app.layout().seqBarVisible();
            if (app.canvas()) app.canvas()->invalidate();
            app.onResize();
            return {true, app.layout().seqBarVisible() ? "Sequence bar visible" : "Sequence bar hidden"};
        }
        if (opt == "seqwrap") {
            if (cmd.args.size() < 2) return {false, "Usage: :set seqwrap on|off"};
            auto v = parseBool(cmd.args[1]);
            if (!v) return {false, "Usage: :set seqwrap on|off"};
            app.layout().setSeqBarWrap(*v);
            auto& vs = app.tabs().currentTab().viewState();
            vs.seqBarWrap = app.layout().seqBarWrap();
            if (app.canvas()) app.canvas()->invalidate();
            app.onResize();
            return {true, app.layout().seqBarWrap() ? "Sequence bar: wrap" : "Sequence bar: scroll"};
        }
        if (opt == "interface_color" || opt == "ic") {
            if (cmd.args.size() < 2) return {false, "Usage: :set interface_color <color_name>"};
            int c = ColorMapper::colorByName(cmd.args[1]);
            if (c < 0) return {false, "Unknown color: " + cmd.args[1] + " (" + ColorMapper::availableColors() + ")"};
            app.interfaceColor_ = c;
            return {true, "Interface color: " + cmd.args[1]};
        }
        if (opt == "interface_thickness" || opt == "it") {
            if (cmd.args.size() < 2) return {false, "Usage: :set interface_thickness <1-4>"};
            int val = std::stoi(cmd.args[1]);
            app.interfaceThickness_ = std::max(1, std::min(4, val));
            return {true, "Interface thickness: " + std::to_string(app.interfaceThickness_)};
        }
        if (opt == "interface_classify" || opt == "iclass") {
            if (cmd.args.size() < 2)
                return {false, "Usage: :set interface_classify on|off"};
            auto v = parseBool(cmd.args[1]);
            if (!v) return {false, "Usage: :set interface_classify on|off"};
            app.interfaceClassify_ = *v;
            return {true, *v ? "Interface classification: on (cyan H-bond, "
                               "red salt, yellow hydrophobic, gray other)"
                             : "Interface classification: off (single color)"};
        }
        if (opt == "interface_show" || opt == "is") {
            if (cmd.args.size() < 2)
                return {false, "Usage: :set interface_show all|specific|none|<list>"};
            // Reassemble: the parser splits on both spaces and commas,
            // so `set is salt,hbond` and `set is salt hbond` both arrive
            // as separate args. parseInterfaceShowSpec re-tokenizes by
            // comma, so we just rejoin with commas.
            std::string spec = cmd.args[1];
            for (size_t i = 2; i < cmd.args.size(); ++i) spec += "," + cmd.args[i];
            int parsed = parseInterfaceShowSpec(spec);
            if (parsed < 0)
                return {false, "Usage: :set interface_show all|specific|none|<list of hbond,salt,hydrophobic,other>"};
            app.interfaceShowMask_ = static_cast<std::uint8_t>(parsed);
            app.interfaceRepr_.setShowMask(app.interfaceShowMask_);
            return {true, "Interface show: " + formatInterfaceShowSpec(app.interfaceShowMask_)};
        }
        return {false, "Unknown option: " + opt};
    }, ":set <option> [value]",
       "Set a runtime option (renderer, fog, outline, cartoon_*, focus_*, panel, seqbar, ...)",
       {":set renderer pixel", ":set outline on", ":set fog 0.5"}, "Session");

    // :get — query current value of a :set option (for scripting)
    cmdRegistry_.registerCmd("get", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) return {false, "Usage: :get <option>"};
        const auto& opt = cmd.args[0];
        auto onoff = [](bool b) { return std::string(b ? "on" : "off"); };

        if (opt == "panel")        return {true, "panel = " + onoff(app.layout().panelVisible())};
        if (opt == "outline")      return {true, "outline = " + onoff(app.outlineEnabled())};
        if (opt == "auto_center")  return {true, "auto_center = " + onoff(app.autoCenter())};
        if (opt == "seqbar")       return {true, "seqbar = " + onoff(app.layout().seqBarVisible())};
        if (opt == "seqwrap")      return {true, "seqwrap = " + onoff(app.layout().seqBarWrap())};
        if (opt == "interface_classify" || opt == "iclass")
            return {true, "interface_classify = " + onoff(app.interfaceClassify_)};
        if (opt == "interface_show" || opt == "is")
            return {true, "interface_show = " +
                          formatInterfaceShowSpec(app.interfaceShowMask_)};

        if (opt == "renderer" || opt == "render") {
            const char* n = "?";
            switch (app.rendererType()) {
                case RendererType::Ascii:   n = "ascii";   break;
                case RendererType::Braille: n = "braille"; break;
                case RendererType::Block:   n = "block";   break;
                case RendererType::Pixel:   n = "pixel";   break;
            }
            return {true, std::string("renderer = ") + n};
        }
        if (opt == "fog")              return {true, "fog = " + std::to_string(app.fogStrength())};
        if (opt == "outline_threshold" || opt == "ot")
            return {true, "outline_threshold = " + std::to_string(app.outlineThreshold())};
        if (opt == "outline_darken" || opt == "od")
            return {true, "outline_darken = " + std::to_string(app.outlineDarken())};
        if (opt == "pan_speed" || opt == "ps")
            return {true, "pan_speed = " + std::to_string(app.tabs().currentTab().camera().panSpeed())};
        if (opt == "backbone_thickness" || opt == "bt") {
            auto* bb = dynamic_cast<BackboneRepr*>(app.getRepr(ReprType::Backbone));
            if (!bb) return {false, "Backbone repr not found"};
            return {true, "backbone_thickness = " + std::to_string(bb->thickness())};
        }
        if (opt == "wireframe_thickness" || opt == "wt") {
            auto* wf = dynamic_cast<WireframeRepr*>(app.getRepr(ReprType::Wireframe));
            if (!wf) return {false, "Wireframe repr not found"};
            return {true, "wireframe_thickness = " + std::to_string(wf->thickness())};
        }
        if (opt == "ball_radius" || opt == "br") {
            auto* bs = dynamic_cast<BallStickRepr*>(app.getRepr(ReprType::BallStick));
            if (!bs) return {false, "BallStick repr not found"};
            return {true, "ball_radius = " + std::to_string(bs->ballRadius())};
        }
        if (opt == "cartoon_helix" || opt == "ch") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, "cartoon_helix = " + std::to_string(ct->helixRadius())};
        }
        if (opt == "cartoon_sheet" || opt == "csh") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, "cartoon_sheet = " + std::to_string(ct->sheetRadius())};
        }
        if (opt == "cartoon_loop" || opt == "cl") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, "cartoon_loop = " + std::to_string(ct->loopRadius())};
        }
        if (opt == "cartoon_subdiv" || opt == "csd") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, "cartoon_subdiv = " + std::to_string(ct->subdivisions())};
        }
        if (opt == "cartoon_aspect" || opt == "csa") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, "cartoon_aspect = " + std::to_string(ct->helixAspect())};
        }
        if (opt == "cartoon_helix_radial" || opt == "chr") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, "cartoon_helix_radial = " +
                          std::to_string(ct->helixRadialSegments())};
        }
        if (opt == "cartoon_tubular_helix" || opt == "cth") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, std::string("cartoon_tubular_helix = ") +
                          (ct->tubularHelix() ? "on" : "off")};
        }
        if (opt == "cartoon_tubular_radius" || opt == "ctr") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, "cartoon_tubular_radius = " +
                          std::to_string(ct->tubularRadius())};
        }
        if (opt == "bs_units") {
            auto* bs = dynamic_cast<BallStickRepr*>(app.getRepr(ReprType::BallStick));
            if (!bs) return {false, "BallStick repr not found"};
            return {true, std::string("bs_units = ") + (bs->useVdwSize() ? "vdw" : "cell")};
        }
        if (opt == "bs_factor" || opt == "bsf") {
            auto* bs = dynamic_cast<BallStickRepr*>(app.getRepr(ReprType::BallStick));
            if (!bs) return {false, "BallStick repr not found"};
            return {true, "bs_factor = " + std::to_string(bs->sizeFactor())};
        }
        if (opt == "spacefill_scale" || opt == "ss_scale" || opt == "sfs") {
            auto* sf = dynamic_cast<SpacefillRepr*>(app.getRepr(ReprType::Spacefill));
            if (!sf) return {false, "Spacefill repr not found"};
            return {true, "spacefill_scale = " + std::to_string(sf->scale())};
        }
        if (opt == "nucleic_backbone" || opt == "nb") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            const char* n = (ct->nucleicBackbone() == CartoonRepr::NucleicBackbone::P) ? "p" : "c4";
            return {true, std::string("nucleic_backbone = ") + n};
        }
        if (opt == "lod_medium")
            return {true, "lod_medium = " + std::to_string(Representation::lodMediumThreshold)};
        if (opt == "lod_low")
            return {true, "lod_low = " + std::to_string(Representation::lodLowThreshold)};
        if (opt == "backbone_cutoff")
            return {true, "backbone_cutoff = " + std::to_string(Representation::backboneCutoff)};
        if (opt == "interface_color" || opt == "ic")
            return {true, "interface_color = " + std::to_string(app.interfaceColor_)};
        if (opt == "interface_thickness" || opt == "it")
            return {true, "interface_thickness = " + std::to_string(app.interfaceThickness_)};

        return {false, "Unknown option: " + opt};
    }, ":get <option>", "Print the current value of a :set option (handy for scripting)",
       {":get renderer", ":get fog", ":get focus_radius"}, "Session");

    // :help [cmd] — overview overlay (no args) or per-command help (one arg).
    // Per-command help displays usage, description, and registered examples.
    cmdRegistry_.registerCmd("help", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) {
            app.showCommandIndex();
            return {true, ""};
        }
        std::string name = cmd.args[0];
        if (!name.empty() && name.front() == ':') name.erase(0, 1);
        const CommandInfo* info = app.cmdRegistry().lookup(name);
        if (!info) return {false, "Unknown command: :" + name};
        app.showCommandHelp(*info);
        return {true, ""};
    }, ":help [cmd]", "Show command index, or detailed help for one command",
       {":help", ":help fetch", ":help :align"}, "Help");

    // :clear — wipe the current tab (or every tab + the global store with 'all')
    cmdRegistry_.registerCmd("clear", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.size() > 1 ||
            (cmd.args.size() == 1 && cmd.args[0] != "all")) {
            return {false, "Usage: :clear [all]"};
        }
        bool wipeEverything = !cmd.args.empty();

        if (wipeEverything) {
            int total = 0;
            for (size_t i = 0; i < app.tabs().count(); ++i) {
                total += static_cast<int>(app.tabs().tab(i).objects().size());
                app.tabs().tab(i).clear();
            }
            // Drop every entry in the global store too — :clear-with-tabs
            // is only useful as a hermetic reset, and leaving orphans in
            // the store would defeat that.
            for (const auto& n : app.store().names()) app.store().remove(n);
            if (total == 0) return {true, "Already empty"};
            return {true, "Cleared all objects (" + std::to_string(total) + " total)"};
        }

        auto& tab = app.tabs().currentTab();
        int n = static_cast<int>(tab.objects().size());
        if (n == 0) return {true, "Tab is already empty"};
        tab.clear();
        return {true, "Cleared " + std::to_string(n) + " object(s)"};
    },
    ":clear [all]",
    "Wipe the current tab; ':clear all' empties every tab and the global object store",
    {":clear", ":clear all"},
    "Window");

    // :delete
    cmdRegistry_.registerCmd("delete", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto& tab = app.tabs().currentTab();
        if (cmd.args.empty()) {
            auto obj = tab.currentObject();
            if (!obj) return {false, "No object selected"};
            std::string name = obj->name();
            app.store().remove(name);
            tab.removeObject(tab.selectedObjectIdx());
            return {true, "Deleted " + name};
        }
        auto obj = app.store().get(cmd.args[0]);
        if (!obj) return {false, "Object not found: " + cmd.args[0]};
        std::string name = obj->name();
        app.store().remove(name);
        return {true, "Deleted " + name};
    }, ":delete [name]", "Delete an object (defaults to the currently selected one)",
       {":delete", ":delete 1bna"}, "Window");

    // :rename
    cmdRegistry_.registerCmd("rename", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) return {false, "Usage: :rename <new_name> or :rename <old> <new>"};
        if (cmd.args.size() < 2) {
            auto obj = app.tabs().currentTab().currentObject();
            if (!obj) return {false, "No object selected"};
            std::string oldName = obj->name();
            if (app.store().rename(oldName, cmd.args[0]))
                return {true, "Renamed " + oldName + " -> " + cmd.args[0]};
            return {false, "Failed to rename"};
        }
        if (app.store().rename(cmd.args[0], cmd.args[1]))
            return {true, "Renamed " + cmd.args[0] + " -> " + cmd.args[1]};
        return {false, "Failed to rename"};
    }, ":rename [old] <new>", "Rename an object (single arg renames the current object)",
       {":rename ref", ":rename 1bna ref"}, "Window");

    // :info
    cmdRegistry_.registerCmd("info", [](Application& app, const ParsedCommand&) -> ExecResult {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};
        return {true, obj->name() + ": " +
               std::to_string(obj->atoms().size()) + " atoms, " +
               std::to_string(obj->bonds().size()) + " bonds"};
    }, ":info", "Show atom/bond counts and metadata for the current object",
       {":info"}, "Session");

    // :select <expression>
    cmdRegistry_.registerCmd("select", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) return {false, "Usage: :select <expr> | :select <name> = <expr> | :select clear"};
        // :select clear — clear $sele
        if (cmd.args[0] == "clear") {
            auto it = app.namedSelections().find("sele");
            if (it != app.namedSelections().end()) it->second.clear();
            return {true, "Selection cleared"};
        }
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};

        // Check for named selection: "name = expr"
        // Find "=" in args
        int eqIdx = -1;
        for (int i = 0; i < static_cast<int>(cmd.args.size()); ++i) {
            if (cmd.args[i] == "=") { eqIdx = i; break; }
        }

        std::string name;
        std::string expr;

        if (eqIdx == 1) {
            // "select name = expr..."
            name = cmd.args[0];
            for (int i = eqIdx + 1; i < static_cast<int>(cmd.args.size()); ++i) {
                if (!expr.empty()) expr += " ";
                expr += cmd.args[i];
            }
        } else {
            // "select expr..."
            for (size_t i = 0; i < cmd.args.size(); ++i) {
                if (i > 0) expr += " ";
                expr += cmd.args[i];
            }
        }

        if (expr.empty()) return {false, "Empty expression"};

        auto sel = app.parseSelection(expr, *obj);

        if (!name.empty()) {
            // Store as named selection
            app.namedSelections()[name] = sel;
            if (sel.empty()) return {false, "Selection '" + name + "' is empty: " + expr};
            return {true, "Selection '" + name + "' = " + std::to_string(sel.size()) + " atoms"};
        }

        if (sel.empty()) return {false, "Selection empty: " + expr};
        return {true, "Selected " + std::to_string(sel.size()) + " atoms: " + expr};
    }, ":select <expr>", "Select atoms (use 'name = expr' for a named selection; 'clear' to drop $sele)",
       {":select chain A", ":select s1 = resi 50-80", ":select clear"}, "Selection");

    // :count <expression> — count atoms matching selection
    cmdRegistry_.registerCmd("count", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) return {false, "Usage: :count <expression>"};
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};

        std::string expr;
        for (size_t i = 0; i < cmd.args.size(); ++i) {
            if (i > 0) expr += " ";
            expr += cmd.args[i];
        }

        auto sel = app.parseSelection(expr, *obj);
        return {true, std::to_string(sel.size()) + " atoms match: " + expr};
    }, ":count <expr>", "Count atoms matching a selection expression",
       {":count chain A", ":count resn HEM", ":count $sele"}, "Selection");

    // :sele — list named selections
    cmdRegistry_.registerCmd("sele", [](Application& app, const ParsedCommand&) -> ExecResult {
        auto& sels = app.namedSelections();
        if (sels.empty()) return {true, "No named selections"};
        std::string result = "Selections:";
        for (const auto& [name, sel] : sels) {
            result += " " + name + "(" + std::to_string(sel.size()) + ")";
        }
        return {true, result};
    }, ":sele", "List all named selections in the current session",
       {":sele"}, "Selection");

    // ── :align / :mmalign / :alignto ───────────────────────────────────────
    //
    // Syntax (canonical):
    //   :align    <mob> [sel] to <ref> [sel] [tm|mm]
    //   :alignto  <ref> [sel]                                — mob = current obj
    //   :alignto  <mob_sel> to <ref> [sel] [tm|mm]
    // Legacy (no "to"):  :align <mob> <ref> [shared_sel]
    //
    // Algorithm picker:
    //   - tm trailing token  → force TM-align (single-chain)
    //   - mm trailing token  → force MM-align (multi-chain complex)
    //   - otherwise          → MM if either side spans >1 chain after
    //                          selection, else TM. Tie-break = TM.
    //
    // :mmalign / :mmalignto are hidden back-compat aliases that hard-force
    // MM. :super = TM-only alias.
    enum class AlignMode { Auto, ForceTM, ForceMM };

    // Pop a trailing tm/mm token from a token vector (returns the mode
    // and removes the token in place). The check is exact-match only —
    // selections containing "tm" inside an expression aren't affected
    // because we only pop the LAST token.
    auto popModeToken = [](std::vector<std::string>& toks) -> AlignMode {
        AlignMode m = AlignMode::Auto;
        while (!toks.empty()) {
            const std::string& last = toks.back();
            if (last == "tm")      { m = AlignMode::ForceTM; toks.pop_back(); }
            else if (last == "mm") { m = AlignMode::ForceMM; toks.pop_back(); }
            else break;
        }
        return m;
    };

    auto chainCount = [](const MolObject& obj, const std::vector<int>& atoms) -> int {
        std::set<std::string> chains;
        if (atoms.empty()) {
            for (const auto& a : obj.atoms()) chains.insert(a.chainId);
        } else {
            const auto& all = obj.atoms();
            for (int idx : atoms) {
                if (idx >= 0 && idx < static_cast<int>(all.size()))
                    chains.insert(all[idx].chainId);
            }
        }
        return static_cast<int>(chains.size());
    };

    // Shared dispatcher. mobile/target are pre-resolved shared_ptrs (the
    // caller decides whether the mobile name comes from cmd.args[0] or
    // from currentObject()). mobileLabel is the name to print in the
    // result message.
    auto runAlign = [chainCount](Application& app,
                                  std::shared_ptr<MolObject> mobile,
                                  std::shared_ptr<MolObject> target,
                                  const std::string& mobileLabel,
                                  const std::string& targetLabel,
                                  const std::string& mobileExpr,
                                  const std::string& targetExpr,
                                  AlignMode mode) -> ExecResult {
        std::vector<int> mobileAtoms, targetAtoms;
        if (!mobileExpr.empty()) {
            auto mSel = app.parseSelection(mobileExpr, *mobile);
            mobileAtoms = std::vector<int>(mSel.indices().begin(), mSel.indices().end());
            if (mobileAtoms.empty())
                return {false, "Mobile selection empty: " + mobileExpr};
        }
        if (!targetExpr.empty()) {
            auto tSel = app.parseSelection(targetExpr, *target);
            targetAtoms = std::vector<int>(tSel.indices().begin(), tSel.indices().end());
            if (targetAtoms.empty())
                return {false, "Target selection empty: " + targetExpr};
        }

        bool complex;
        switch (mode) {
            case AlignMode::ForceTM: complex = false; break;
            case AlignMode::ForceMM: complex = true;  break;
            default:
                complex = chainCount(*mobile, mobileAtoms) > 1
                       || chainCount(*target, targetAtoms) > 1;
        }

        auto result = complex
            ? Aligner::alignComplex(*mobile, *target, mobileAtoms, targetAtoms)
            : Aligner::align(*mobile, *target, mobileAtoms, targetAtoms);
        if (!result.success) return {false, "Align failed: " + result.message};

        Aligner::applyTransform(*mobile, result);
        std::string prefix = complex ? "MM-" : "TM-";
        return {true, prefix + "aligned " + mobileLabel + " → " + targetLabel +
                      " | " + result.message};
    };

    // Parse :align-style args into (mobileName, mobileExpr, targetName,
    // targetExpr, mode). Mobile is taken from cmd.args[0] when
    // mobileFromCurrent==false; otherwise the entire arg list represents
    // ref-side syntax (a target name + optional selection, with an
    // optional `to` to introduce mobile-side selection on the current
    // object).
    auto joinTokens = [](const std::vector<std::string>& toks,
                         size_t begin = 0,
                         size_t end = std::string::npos) -> std::string {
        if (end == std::string::npos) end = toks.size();
        std::string s;
        for (size_t i = begin; i < end; ++i) {
            if (!s.empty()) s += ' ';
            s += toks[i];
        }
        return s;
    };

    auto parseAlignArgs = [popModeToken, joinTokens](
        const ParsedCommand& cmd, bool mobileFromCurrent,
        std::string& mobileName, std::string& mobileExpr,
        std::string& targetName, std::string& targetExpr,
        AlignMode& mode, std::string& err) -> bool {

        std::vector<std::string> args = cmd.args;
        const char* usage = mobileFromCurrent
            ? "Usage: :alignto <target> [sel] | <mobile_sel> to <target> [target_sel]"
            : "Usage: :align <obj> [sel] to <obj> [sel] [tm|mm]";

        int toIdx = -1;
        for (int i = 0; i < static_cast<int>(args.size()); ++i) {
            if (args[i] == "to") { toIdx = i; break; }
        }

        if (toIdx >= 0) {
            std::vector<std::string> left(args.begin(), args.begin() + toIdx);
            std::vector<std::string> right(args.begin() + toIdx + 1, args.end());
            mode = popModeToken(right);
            if (right.empty()) { err = "Missing target after 'to'"; return false; }
            targetName = right[0];
            targetExpr = joinTokens(right, 1);
            if (mobileFromCurrent) {
                mobileExpr = joinTokens(left);
            } else {
                if (left.empty()) { err = usage; return false; }
                mobileName = left[0];
                mobileExpr = joinTokens(left, 1);
            }
            return true;
        }

        // No 'to' separator.
        mode = popModeToken(args);
        if (mobileFromCurrent) {
            if (args.empty()) { err = usage; return false; }
            targetName = args[0];
            targetExpr = joinTokens(args, 1);
            return true;
        }
        // Legacy two-name form: :align <mob> <ref> [shared_sel] [tm|mm]
        if (args.size() < 2) { err = usage; return false; }
        mobileName = args[0];
        targetName = args[1];
        mobileExpr = joinTokens(args, 2);
        targetExpr = mobileExpr;
        return true;
    };

    // :align — auto-detect TM vs MM by chain count
    auto doAlignByName = [parseAlignArgs, runAlign]
        (Application& app, const ParsedCommand& cmd, AlignMode forced)
        -> ExecResult {
        std::string mobileName, mobileExpr, targetName, targetExpr, err;
        AlignMode mode = AlignMode::Auto;
        if (!parseAlignArgs(cmd, /*mobileFromCurrent=*/false,
                            mobileName, mobileExpr, targetName, targetExpr,
                            mode, err)) return {false, err};
        if (forced != AlignMode::Auto) mode = forced;
        auto mobile = app.store().get(mobileName);
        auto target = app.store().get(targetName);
        if (!mobile) return {false, "Object not found: " + mobileName};
        if (!target) return {false, "Object not found: " + targetName};
        return runAlign(app, mobile, target, mobileName, targetName,
                        mobileExpr, targetExpr, mode);
    };

    // :alignto — mobile = current object in current tab
    auto doAlignTo = [parseAlignArgs, runAlign]
        (Application& app, const ParsedCommand& cmd, AlignMode forced)
        -> ExecResult {
        auto mobile = app.tabs().currentTab().currentObject();
        if (!mobile) return {false, "No object selected"};
        std::string mobileName, mobileExpr, targetName, targetExpr, err;
        AlignMode mode = AlignMode::Auto;
        if (!parseAlignArgs(cmd, /*mobileFromCurrent=*/true,
                            mobileName, mobileExpr, targetName, targetExpr,
                            mode, err)) return {false, err};
        if (forced != AlignMode::Auto) mode = forced;
        auto target = app.store().get(targetName);
        if (!target) return {false, "Object not found: " + targetName};
        if (target.get() == mobile.get())
            return {false, "Cannot align object to itself"};
        return runAlign(app, mobile, target, mobile->name(), targetName,
                        mobileExpr, targetExpr, mode);
    };

    cmdRegistry_.registerCmd("align",
        [doAlignByName](Application& app, const ParsedCommand& cmd) -> ExecResult {
            return doAlignByName(app, cmd, AlignMode::Auto);
        },
        ":align <obj> [sel] to <obj> [sel] [tm|mm]",
        "Superpose structures (auto-pick TM vs MM by chain count; trailing tm/mm forces)",
        {":align mob to ref",
         ":align mob chain A to ref chain A",
         ":align complex1 to complex2 mm"},
        "Analysis");

    cmdRegistry_.registerCmd("alignto",
        [doAlignTo](Application& app, const ParsedCommand& cmd) -> ExecResult {
            return doAlignTo(app, cmd, AlignMode::Auto);
        },
        ":alignto <target> [sel] | <mobile_sel> to <target> [target_sel] [tm|mm]",
        "Superpose the current object onto target (auto TM/MM; trailing tm/mm forces)",
        {":alignto ref",
         ":alignto chain A to ref chain A",
         ":alignto ref mm"},
        "Analysis");

    // :super — TM-only alias for :align (kept for back-compat with existing
    // scripts; auto-mode would surprise users who expect :super to never
    // run MM, so this stays explicitly TM-forced).
    cmdRegistry_.registerCmd("super",
        [doAlignByName](Application& app, const ParsedCommand& cmd) -> ExecResult {
            return doAlignByName(app, cmd, AlignMode::ForceTM);
        },
        ":super <obj> [sel] to <obj> [sel]",
        "Superpose structures (alias for :align tm)",
        {":super mob to ref"},
        "Analysis");

    // Hidden back-compat aliases that hard-force MM mode.
    cmdRegistry_.registerCmd("mmalign",
        [doAlignByName](Application& app, const ParsedCommand& cmd) -> ExecResult {
            return doAlignByName(app, cmd, AlignMode::ForceMM);
        },
        ":mmalign <obj> [sel] to <obj> [sel]",
        "Deprecated alias for ':align ... mm'",
        {":mmalign complex1 to complex2"},
        "Hidden");

    cmdRegistry_.registerCmd("mmalignto",
        [doAlignTo](Application& app, const ParsedCommand& cmd) -> ExecResult {
            return doAlignTo(app, cmd, AlignMode::ForceMM);
        },
        ":mmalignto <target> [sel]",
        "Deprecated alias for ':alignto ... mm'",
        {":mmalignto complex2"},
        "Hidden");

    cmdRegistry_.registerCmd("loadalign",
        [runAlign, popModeToken](Application& app, const ParsedCommand& cmd) -> ExecResult {
            if (cmd.args.empty())
                return {false, "Usage: :loadalign <pattern> [tm|mm]"};

            std::vector<std::string> args = cmd.args;
            AlignMode mode = popModeToken(args);
            if (args.empty())
                return {false, "Usage: :loadalign <pattern> [tm|mm]"};

            // First brace only; nested / list-form braces unsupported.
            auto expandBrace = [](const std::string& p) -> std::vector<std::string> {
                auto lb = p.find('{');
                if (lb == std::string::npos) return {p};
                auto rb = p.find('}', lb);
                if (rb == std::string::npos) return {p};
                auto inner = p.substr(lb + 1, rb - lb - 1);
                auto dots = inner.find("..");
                if (dots == std::string::npos) return {p};
                try {
                    int lo = std::stoi(inner.substr(0, dots));
                    int hi = std::stoi(inner.substr(dots + 2));
                    if (hi < lo) std::swap(lo, hi);
                    std::vector<std::string> out;
                    for (int i = lo; i <= hi; ++i) {
                        out.push_back(p.substr(0, lb) + std::to_string(i) +
                                      p.substr(rb + 1));
                    }
                    return out;
                } catch (...) {
                    return {p};
                }
            };

            std::vector<std::string> matches;
            for (const auto& raw : args) {
                for (const auto& pat : expandBrace(raw)) {
                    glob_t g{};
                    int rc = glob(pat.c_str(), GLOB_NOSORT, nullptr, &g);
                    if (rc == 0) {
                        for (size_t i = 0; i < g.gl_pathc; ++i)
                            matches.emplace_back(g.gl_pathv[i]);
                    } else if (rc == GLOB_NOMATCH && std::filesystem::exists(pat)) {
                        // glob() returns NOMATCH for brace-expanded
                        // literals like "model_3.pdb"; accept those.
                        matches.push_back(pat);
                    }
                    globfree(&g);
                }
            }
            std::sort(matches.begin(), matches.end());
            matches.erase(std::unique(matches.begin(), matches.end()), matches.end());

            if (matches.empty())
                return {false, "No files matched pattern(s): " +
                               cmd.args[0] + (cmd.args.size() > 1 ? " ..." : "")};

            // Detect successful loads via tab-size delta — robust to
            // future changes in loadFile's status-string format.
            auto& tab = app.tabs().currentTab();
            std::vector<std::shared_ptr<MolObject>> loaded;
            std::vector<std::string> errors;
            for (const auto& path : matches) {
                size_t before = tab.objects().size();
                std::string msg = app.loadFile(path);
                if (tab.objects().size() > before) {
                    loaded.push_back(tab.objects().back());
                } else {
                    errors.push_back(path + ": " + msg);
                }
            }
            if (loaded.empty())
                return {false, "All loads failed; first error: " +
                               (errors.empty() ? std::string("(none)") : errors[0])};

            std::string summary = "Loaded " + std::to_string(loaded.size()) +
                                  " structure(s)";
            if (loaded.size() < 2) {
                if (!errors.empty()) summary += " (" + std::to_string(errors.size()) + " failed)";
                return {true, summary + ": " + loaded.front()->name()};
            }

            const std::string& targetName = loaded.front()->name();
            int aligned = 0, alignFailed = 0;
            std::string detail;
            for (size_t i = 1; i < loaded.size(); ++i) {
                auto r = runAlign(app, loaded[i], loaded.front(),
                                  loaded[i]->name(), targetName,
                                  std::string{}, std::string{}, mode);
                if (r.ok) { ++aligned; detail += "\n  " + r.msg; }
                else { ++alignFailed; detail += "\n  " + loaded[i]->name() + ": " + r.msg; }
            }
            summary += " from '" + cmd.args[0] + "'; aligned " +
                       std::to_string(aligned) + " to " + targetName;
            if (alignFailed) summary += " (" + std::to_string(alignFailed) + " failed)";
            return {true, summary + detail};
        },
        ":loadalign <pattern> [tm|mm]",
        "Load every file matching the glob/brace pattern; align models 2..N onto the first",
        {":loadalign relaxed_model_*.pdb",
         ":loadalign relaxed_model_{1..5}.pdb",
         ":loadalign models/*.cif mm"},
        "Files");

    // :fetch <pdb_id> — download from RCSB PDB or AlphaFold DB
    cmdRegistry_.registerCmd("fetch", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) return {false, "Usage: :fetch <pdb_id> or :fetch afdb:<uniprot_id>"};

        std::string id = cmd.args[0];
        std::string url;
        std::string filename;

        // AlphaFold DB: "afdb:P12345" or "AFDB:P12345" or "AF-P12345"
        bool isAF = false;
        if (id.size() > 5 && (id.substr(0, 5) == "afdb:" || id.substr(0, 5) == "AFDB:")) {
            std::string uniprotId = id.substr(5);
            url = "https://alphafold.ebi.ac.uk/files/AF-" + uniprotId + "-F1-model_v6.cif";
            filename = "AF-" + uniprotId + ".cif";
            isAF = true;
        } else if (id.size() > 3 && id.substr(0, 3) == "AF-") {
            // Direct AF ID like AF-P12345-F1
            url = "https://alphafold.ebi.ac.uk/files/" + id + "-model_v6.cif";
            filename = id + ".cif";
            isAF = true;
        } else {
            // RCSB PDB: 4-character PDB ID
            std::string lower = id;
            for (auto& c : lower) c = static_cast<char>(std::tolower(c));
            url = "https://files.rcsb.org/download/" + lower + ".cif";
            filename = lower + ".cif";
        }

        // Write to current working directory, but never overwrite an existing file
        std::filesystem::path outPath = std::filesystem::current_path() / filename;
        std::string src = isAF ? "AlphaFold DB" : "RCSB PDB";

        if (std::filesystem::exists(outPath)) {
            std::string result = app.loadFile(outPath.string());
            bool ok = result.rfind("Loaded ", 0) == 0;
            return {ok, "Loaded existing " + outPath.string() + " (skipped fetch from " + src + ") | " + result};
        }

        std::string curlCmd = "curl -sL -o '" + outPath.string() + "' -w '%{http_code}' '" + url + "'";
        FILE* pipe = popen(curlCmd.c_str(), "r");
        if (!pipe) return {false, "Failed to run curl"};

        char buf[64];
        std::string httpCode;
        while (fgets(buf, sizeof(buf), pipe)) httpCode += buf;
        int ret = pclose(pipe);

        if (ret != 0 || httpCode.find("200") == std::string::npos) {
            std::error_code ec;
            std::filesystem::remove(outPath, ec);
            return {false, "Failed to fetch " + id + " from " + src + " (HTTP " + httpCode + ")"};
        }

        std::string result = app.loadFile(outPath.string());
        bool ok = result.rfind("Loaded ", 0) == 0;
        return {ok, "Fetched " + id + " from " + src + " -> " + outPath.string() + " | " + result};
    }, ":fetch <pdb_id|afdb:uniprot_id>", "Download a structure from RCSB PDB (4-letter ID) or AlphaFold DB (afdb: prefix)",
       {":fetch 1bna", ":fetch afdb:P00533"}, "Files");

    // :assembly [id|list] — generate biological assembly
    cmdRegistry_.registerCmd("assembly", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};
        std::string path = obj->sourcePath();
        if (path.empty()) return {false, "No source file for " + obj->name()};

        if (!cmd.args.empty() && cmd.args[0] == "list") {
            auto assemblies = CifLoader::listAssemblies(path);
            if (assemblies.empty()) return {true, "No assemblies found in " + obj->name()};
            std::string result = "Assemblies:";
            for (const auto& a : assemblies)
                result += " " + a.name + "(" + std::to_string(a.oligomericCount) + "-mer)";
            return {true, result};
        }

        std::string asmId = cmd.args.empty() ? "1" : cmd.args[0];
        try {
            auto asmObj = CifLoader::loadAssembly(path, asmId);
            int atomCount = static_cast<int>(asmObj->atoms().size());
            int bondCount = static_cast<int>(asmObj->bonds().size());
            std::string name = asmObj->name();
            auto ptr = app.store().add(std::move(asmObj));
            app.tabs().currentTab().addObject(ptr);
            if (app.autoCenter()) app.tabs().currentTab().centerView();
            MLOG_INFO("Generated assembly %s: %d atoms, %d bonds", asmId.c_str(), atomCount, bondCount);
            return {true, "Assembly " + asmId + " → " + name + ": " +
                   std::to_string(atomCount) + " atoms, " + std::to_string(bondCount) + " bonds"};
        } catch (const std::exception& e) {
            return {false, std::string("Assembly error: ") + e.what()};
        }
    }, ":assembly [id|list]", "Generate a biological assembly (defaults to assembly 1; 'list' shows available IDs)",
       {":assembly", ":assembly 1", ":assembly list"}, "Files");

    // :export <file.pml> — export PyMOL script
    cmdRegistry_.registerCmd("export", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) return {false, "Usage: :export <file.pml>"};
        const std::string& path = cmd.args[0];

        // Validate extension
        if (path.size() < 5 || path.substr(path.size() - 4) != ".pml")
            return {false, "Export file must have .pml extension"};

        auto& tab = app.tabs().currentTab();
        if (tab.objects().empty()) return {false, "No objects to export"};

        int vpW = app.layout().viewportWidth();
        int vpH = app.layout().viewportHeight();
        std::string result = SessionExporter::exportPML(path, tab, vpW, vpH);
        bool ok = result.find("Failed") == std::string::npos &&
                  result.find("Error") == std::string::npos;
        return {ok, result};
    }, ":export <file.pml>", "Export the current session as a PyMOL script",
       {":export figure.pml"}, "Files");

    // :screenshot <file.png>
    cmdRegistry_.registerCmd("screenshot", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        std::string path;
        // Optional pixel dimensions: `:screenshot file.png 1920 1080`.
        // Optional DPI for PNG pHYs metadata: `:screenshot file.png 1800 1200 300`.
        // Useful when running from a script with no real terminal — the
        // active viewport may otherwise default to a small fallback size.
        int reqPixW = 0, reqPixH = 0, reqDpi = 0;
        std::vector<std::string> positional;
        for (const auto& a : cmd.args) positional.push_back(a);

        // Last token is DPI iff we have ≥4 args; W H are then args[-3..-2].
        if (positional.size() >= 4) {
            try {
                reqDpi = std::stoi(positional.back());
                positional.pop_back();
            } catch (...) {
                return {false, "Usage: :screenshot [file.png] [W H [DPI]]"};
            }
            if (reqDpi < 1 || reqDpi > 4800) {
                return {false, "DPI out of range (1..4800)"};
            }
        }
        if (positional.size() >= 3) {
            try {
                reqPixW = std::stoi(positional[positional.size() - 2]);
                reqPixH = std::stoi(positional[positional.size() - 1]);
                positional.pop_back();
                positional.pop_back();
            } catch (...) {
                return {false, "Usage: :screenshot [file.png] [W H [DPI]]"};
            }
            if (reqPixW < 64 || reqPixH < 64 ||
                reqPixW > 8192 || reqPixH > 8192) {
                return {false, "Screenshot size out of range (64..8192 px)"};
            }
        }

        if (positional.empty()) {
            auto now = std::chrono::system_clock::now();
            auto t = std::chrono::system_clock::to_time_t(now);
            char fname[64];
            std::strftime(fname, sizeof(fname), "molterm_%Y%m%d_%H%M%S.png", std::localtime(&t));
            path = fname;
        } else {
            path = positional[0];
        }

        auto savedMsg = [&](int w, int h) {
            std::string msg = "Saved " + std::to_string(w) + "x" + std::to_string(h);
            if (reqDpi > 0) msg += " @ " + std::to_string(reqDpi) + " dpi";
            msg += " to " + path;
            return msg;
        };

        // If already in pixel mode and no explicit size was requested,
        // grab the live framebuffer.
        if (app.rendererType() == RendererType::Pixel && reqPixW == 0) {
            auto* pc = dynamic_cast<PixelCanvas*>(app.canvas());
            if (pc && pc->savePNG(path, reqDpi))
                return ExecResult{true, savedMsg(pc->pixelWidth(), pc->pixelHeight())};
            return {false, "Failed to save " + path};
        }

        // Offscreen render: create a temporary PixelCanvas, render into it, save PNG
        auto proto = ProtocolPicker::detect();
        auto encoder = ProtocolPicker::createEncoder(proto);
        if (!encoder) {
            // Create a dummy SixelEncoder just for offscreen rendering
            encoder = ProtocolPicker::createEncoder(GraphicsProtocol::Sixel);
        }
        if (!encoder) return {false, "Cannot create offscreen renderer"};

        PixelCanvas offscreen(std::move(encoder));
        if (reqPixW > 0) {
            // Honor the exact pixel dimensions the user asked for —
            // converting through cells and back would silently truncate
            // when the request isn't a multiple of the terminal's cell size.
            offscreen.resizePixels(reqPixW, reqPixH);
        } else {
            offscreen.resize(app.layout().viewportWidth(),
                             app.layout().viewportHeight());
        }
        offscreen.clear();

        auto& tab = app.tabs().currentTab();
        // Re-prepare projection for the offscreen pixel coordinate space
        tab.camera().prepareProjection(offscreen.subW(), offscreen.subH(), offscreen.aspectYX());

        if (auto* wf = dynamic_cast<WireframeRepr*>(app.getRepr(ReprType::Wireframe))) {
            wf->setHeteroatomCarbonScheme(app.interfaceOverlay_ || app.focusSnapshot_.active);
        }

        for (const auto& obj : tab.objects()) {
            if (!obj->visible()) continue;
            for (auto& [reprType, repr] : app.representations()) {
                if (obj->reprVisible(reprType)) {
                    repr->render(*obj, tab.camera(), offscreen);
                }
            }
        }

        if (app.outlineEnabled()) offscreen.applyOutline(app.outlineThreshold(), app.outlineDarken());
        if (app.fogStrength() > 0.0f)
            offscreen.applyDepthFog(app.fogStrength());

        // Mirror the live render pipeline: focus-dim (mask-driven) +
        // interface overlay so the captured PNG matches what's on screen.
        const std::vector<bool>* dimMask = nullptr;
        if (app.interfaceOverlay_ && !app.interfaceAtomMask_.empty()) {
            dimMask = &app.interfaceAtomMask_;
        } else if (!app.focusAtomMask_.empty()) {
            dimMask = &app.focusAtomMask_;
        }
        if (dimMask) offscreen.applyFocusDim(*dimMask, app.focusDimStrength_);

        if ((app.interfaceOverlay_ || app.focusSnapshot_.active) &&
            app.interfaceRepr_.hasData()) {
            if (auto obj = tab.currentObject()) {
                app.interfaceRepr_.render(*obj, tab.camera(), offscreen);
            }
        }

        // Restore projection for the active canvas
        auto* canvas = app.canvas();
        if (canvas)
            tab.camera().prepareProjection(canvas->subW(), canvas->subH(), canvas->aspectYX());

        if (offscreen.savePNG(path, reqDpi))
            return ExecResult{true, savedMsg(offscreen.pixelWidth(), offscreen.pixelHeight())};
        return {false, "Failed to save " + path};
    }, ":screenshot [file.png] [W H [DPI]]",
       "Save a PNG; optional explicit size (W H) and DPI metadata for figure prep",
       {":screenshot", ":screenshot fig.png", ":screenshot fig.png 1920 1080 300"}, "Files");

    // :preset — apply smart default representation
    cmdRegistry_.registerCmd("preset", [](Application& app, const ParsedCommand&) -> ExecResult {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};
        obj->applySmartDefaults();
        return {true, "Applied default preset (cartoon + ballstick ligands)"};
    }, ":preset", "Apply smart default representations (cartoon for protein, ballstick for ligands)",
       {":preset"}, "Display");

    // :label [selection] — add labels for atoms matching selection
    cmdRegistry_.registerCmd("label", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};
        if (cmd.args.empty()) return {false, "Usage: :label <selection> or :label clear"};
        if (cmd.args[0] == "clear") {
            app.labelAtoms().clear();
            return {true, "Labels cleared"};
        }
        std::string expr;
        for (size_t i = 0; i < cmd.args.size(); ++i) {
            if (i > 0) expr += " ";
            expr += cmd.args[i];
        }
        auto sel = app.parseSelection(expr, *obj);
        if (sel.empty()) return {false, "No atoms match: " + expr};
        // Add to label set (deduplicate)
        auto& labels = app.labelAtoms();
        for (int idx : sel.indices()) {
            bool found = false;
            for (int li : labels) { if (li == idx) { found = true; break; } }
            if (!found) labels.push_back(idx);
        }
        return {true, "Labeled " + std::to_string(sel.size()) + " atoms"};
    }, ":label <selection|clear>", "Show residue labels on the viewport (or 'clear' to remove)",
       {":label resi 50-60", ":label $sele", ":label clear"}, "Display");

    // :unlabel — remove all labels
    cmdRegistry_.registerCmd("unlabel", [](Application& app, const ParsedCommand&) -> ExecResult {
        app.labelAtoms().clear();
        return {true, "Labels cleared"};
    }, ":unlabel", "Remove all viewport labels (alias for :label clear)",
       {":unlabel"}, "Display");

    // :overlay on|off | :overlay clear
    cmdRegistry_.registerCmd("overlay", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) {
            return {false, "Usage: :overlay on|off | :overlay clear"};
        }
        if (cmd.args[0] == "clear") {
            int mc = static_cast<int>(app.measurements().size());
            int lc = static_cast<int>(app.labelAtoms().size());
            app.measurements().clear();
            app.labelAtoms().clear();
            return {true, "Cleared " + std::to_string(mc) + " measurements, " +
                   std::to_string(lc) + " labels"};
        }
        auto v = parseBool(cmd.args[0]);
        if (!v) return {false, "Usage: :overlay on|off | :overlay clear"};
        app.overlayVisible_ = *v;
        return {true, *v ? "Overlays visible" : "Overlays hidden"};
    }, ":overlay on|off | clear", "Toggle overlay visibility (labels, measurements, $sele) or clear them",
       {":overlay on", ":overlay off", ":overlay clear"}, "Display");

    // :run [--strict] <script.mt> — execute a command script
    cmdRegistry_.registerCmd("run", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) return {false, "Usage: :run [--strict] <script.mt>"};
        bool strict = false;
        std::string path;
        for (const auto& a : cmd.args) {
            if (a == "--strict") strict = true;
            else if (path.empty()) path = a;
        }
        if (path.empty()) return {false, "Usage: :run [--strict] <script.mt>"};
        std::ifstream file(path);
        if (!file) return {false, "Cannot open: " + path};
        ScriptRunResult r = app.runScriptStream(file, strict);
        if (strict && r.stopped) {
            MLOG_INFO("Ran %d commands from %s (stopped on error)", r.count, path.c_str());
            return {false, "Stopped at line: " + r.failLine + " — " + r.firstFail};
        }
        MLOG_INFO("Ran %d commands from %s (%d failed)", r.count, path.c_str(), r.failures);
        if (r.failures > 0) {
            return {false, "Ran " + std::to_string(r.count) + " commands from " + path +
                    " (" + std::to_string(r.failures) + " failed; first: " + r.firstFail + ")"};
        }
        return {true, "Ran " + std::to_string(r.count) + " commands from " + path};
    }, ":run [--strict] <file>",
       "Execute a command script (# comments supported; --strict aborts on first error)",
       {":run render.mt", ":run --strict ci.mt"}, "Files");

    cmdRegistry_.registerCmd("save", [](Application& app, const ParsedCommand&) -> ExecResult {
        if (SessionSaver::saveSession(app))
            return {true, "Session saved to " + SessionSaver::sessionPath()};
        return {false, "Failed to save session"};
    }, ":save", "Save the current session to ~/.molterm/autosave.toml",
       {":save"}, "Session");

    cmdRegistry_.registerCmd("resume", [](Application& app, const ParsedCommand&) -> ExecResult {
        std::string msg = SessionSaver::restoreSession(app);
        bool ok = msg.find("Failed") == std::string::npos &&
                  msg.find("Cannot") == std::string::npos &&
                  msg.find("not found") == std::string::npos;
        return {ok, msg};
    }, ":resume", "Restore the last session from ~/.molterm/autosave.toml",
       {":resume"}, "Session");

    // Helper: resolve atom index from serial number string or pick register
    auto resolveAtomIdx = [](Application& app, const std::string& s) -> int {
        // Pick register: pk1..pk4
        if (s.size() >= 3 && s[0] == 'p' && s[1] == 'k' && s[2] >= '1' && s[2] <= '4') {
            return app.pickReg(s[2] - '1');
        }
        // Named selection reference: $name → first atom
        if (!s.empty() && s[0] == '$') {
            auto it = app.namedSelections().find(s.substr(1));
            if (it != app.namedSelections().end() && !it->second.empty())
                return it->second.indices()[0];
            return -1;
        }
        // Serial number
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return -1;
        const auto& atoms = obj->atoms();
        try {
            int serial = std::stoi(s);
            for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
                if (atoms[i].serial == serial) return i;
            }
        } catch (...) {}
        return -1;
    };

    // Helper: format atom label for measurement display
    auto atomLabel = [](const AtomData& a) -> std::string {
        return a.chainId + "/" + a.resName + std::to_string(a.resSeq) + "/" + a.name;
    };

    // :measure [serial1 serial2] — distance (no args = pk1↔pk2)
    cmdRegistry_.registerCmd("measure", [resolveAtomIdx, atomLabel](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};
        const auto& atoms = obj->atoms();
        int n = static_cast<int>(atoms.size());

        int i1, i2;
        if (cmd.args.empty()) {
            // Use pick registers pk1, pk2
            i1 = app.pickRegs_[0]; i2 = app.pickRegs_[1];
            if (i1 < 0 || i2 < 0) return {false, "Click two atoms first (pk1, pk2), then :measure"};
        } else if (cmd.args.size() >= 2) {
            i1 = resolveAtomIdx(app, cmd.args[0]);
            i2 = resolveAtomIdx(app, cmd.args[1]);
            if (i1 < 0) return {false, "Atom not found: serial " + cmd.args[0]};
            if (i2 < 0) return {false, "Atom not found: serial " + cmd.args[1]};
        } else {
            return {false, "Usage: :measure [serial1 serial2] (or click 2 atoms first)"};
        }
        if (i1 >= n || i2 >= n) return {false, "Invalid atom index"};

        float dx = atoms[i1].x - atoms[i2].x;
        float dy = atoms[i1].y - atoms[i2].y;
        float dz = atoms[i1].z - atoms[i2].z;
        float dist = std::sqrt(dx*dx + dy*dy + dz*dz);

        char dbuf[16]; std::snprintf(dbuf, sizeof(dbuf), "%.2f", dist);
        std::string shortLabel = std::string(dbuf) + "A";
        std::string msg = "Distance " + atomLabel(atoms[i1]) + " — " +
            atomLabel(atoms[i2]) + " = " + shortLabel;
        app.measurements().push_back({{i1, i2}, shortLabel});
        MLOG_INFO("%s", msg.c_str());
        return {true, msg};
    }, ":measure [s1 s2]",
       "Measure a distance between two atoms (no args = use pk1, pk2 from last clicks)",
       {":measure", ":measure pk1 pk2", ":measure 12 47"}, "Measurement");

    // :angle [s1 s2 s3] — angle at vertex s2 (no args = pk1-pk2-pk3)
    cmdRegistry_.registerCmd("angle", [resolveAtomIdx, atomLabel](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};
        const auto& atoms = obj->atoms();
        int n = static_cast<int>(atoms.size());

        int i1, i2, i3;
        if (cmd.args.empty()) {
            i1 = app.pickRegs_[0]; i2 = app.pickRegs_[1]; i3 = app.pickRegs_[2];
            if (i1 < 0 || i2 < 0 || i3 < 0) return {false, "Click three atoms first (pk1-pk3), then :angle"};
        } else if (cmd.args.size() >= 3) {
            i1 = resolveAtomIdx(app, cmd.args[0]);
            i2 = resolveAtomIdx(app, cmd.args[1]);
            i3 = resolveAtomIdx(app, cmd.args[2]);
            if (i1 < 0) return {false, "Atom not found: serial " + cmd.args[0]};
            if (i2 < 0) return {false, "Atom not found: serial " + cmd.args[1]};
            if (i3 < 0) return {false, "Atom not found: serial " + cmd.args[2]};
        } else {
            return {false, "Usage: :angle [s1 s2 s3] (or click 3 atoms first)"};
        }
        if (i1 >= n || i2 >= n || i3 >= n) return {false, "Invalid atom index"};

        float v1x = atoms[i1].x - atoms[i2].x, v1y = atoms[i1].y - atoms[i2].y, v1z = atoms[i1].z - atoms[i2].z;
        float v2x = atoms[i3].x - atoms[i2].x, v2y = atoms[i3].y - atoms[i2].y, v2z = atoms[i3].z - atoms[i2].z;
        float dot = v1x*v2x + v1y*v2y + v1z*v2z;
        float len1 = std::sqrt(v1x*v1x + v1y*v1y + v1z*v1z);
        float len2 = std::sqrt(v2x*v2x + v2y*v2y + v2z*v2z);
        float cosA = (len1 > 0 && len2 > 0) ? dot / (len1 * len2) : 0;
        cosA = std::max(-1.0f, std::min(1.0f, cosA));
        float deg = std::acos(cosA) * 180.0f / static_cast<float>(M_PI);

        char buf[128];
        std::snprintf(buf, sizeof(buf), "%.1f", deg);
        std::string shortLabel = std::string(buf) + "°";
        std::string msg = "Angle " + atomLabel(atoms[i1]) + " — " +
            atomLabel(atoms[i2]) + " — " + atomLabel(atoms[i3]) + " = " + buf + " deg";
        app.measurements().push_back({{i1, i2, i3}, shortLabel});
        MLOG_INFO("%s", msg.c_str());
        return {true, msg};
    }, ":angle [s1 s2 s3]",
       "Measure the angle at s2 between (s1, s2, s3) (no args = pk1-pk2-pk3)",
       {":angle", ":angle pk1 pk2 pk3"}, "Measurement");

    // :dihedral [s1 s2 s3 s4] — dihedral (no args = pk1-pk4)
    cmdRegistry_.registerCmd("dihedral", [resolveAtomIdx, atomLabel](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};
        const auto& atoms = obj->atoms();
        int n = static_cast<int>(atoms.size());

        int i1, i2, i3, i4;
        if (cmd.args.empty()) {
            i1 = app.pickRegs_[0]; i2 = app.pickRegs_[1];
            i3 = app.pickRegs_[2]; i4 = app.pickRegs_[3];
            if (i1 < 0 || i2 < 0 || i3 < 0 || i4 < 0)
                return {false, "Click four atoms first (pk1-pk4), then :dihedral"};
        } else if (cmd.args.size() >= 4) {
            i1 = resolveAtomIdx(app, cmd.args[0]);
            i2 = resolveAtomIdx(app, cmd.args[1]);
            i3 = resolveAtomIdx(app, cmd.args[2]);
            i4 = resolveAtomIdx(app, cmd.args[3]);
            if (i1 < 0) return {false, "Atom not found: serial " + cmd.args[0]};
            if (i2 < 0) return {false, "Atom not found: serial " + cmd.args[1]};
            if (i3 < 0) return {false, "Atom not found: serial " + cmd.args[2]};
            if (i4 < 0) return {false, "Atom not found: serial " + cmd.args[3]};
        } else {
            return {false, "Usage: :dihedral [s1 s2 s3 s4] (or click 4 atoms first)"};
        }
        if (i1 >= n || i2 >= n || i3 >= n || i4 >= n) return {false, "Invalid atom index"};

        float deg = geom::dihedralDeg(
            atoms[i1].x, atoms[i1].y, atoms[i1].z,
            atoms[i2].x, atoms[i2].y, atoms[i2].z,
            atoms[i3].x, atoms[i3].y, atoms[i3].z,
            atoms[i4].x, atoms[i4].y, atoms[i4].z);

        char buf[128];
        std::snprintf(buf, sizeof(buf), "%.1f", deg);
        std::string shortLabel = std::string(buf) + "°";
        std::string msg = "Dihedral " + atomLabel(atoms[i1]) + " — " +
            atomLabel(atoms[i2]) + " — " + atomLabel(atoms[i3]) + " — " +
            atomLabel(atoms[i4]) + " = " + buf + " deg";
        app.measurements().push_back({{i1, i2, i3, i4}, shortLabel});
        MLOG_INFO("%s", msg.c_str());
        return {true, msg};
    }, ":dihedral [s1 s2 s3 s4]",
       "Measure a dihedral angle between four atoms (no args = pk1-pk4)",
       {":dihedral", ":dihedral pk1 pk2 pk3 pk4"}, "Measurement");

    // :contactmap [cutoff] — toggle contact map panel
    cmdRegistry_.registerCmd("contactmap", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        float cutoff = 8.0f;
        if (!cmd.args.empty()) {
            try { cutoff = std::stof(cmd.args[0]); } catch (...) {}
        }
        app.layout().toggleAnalysisPanel();
        app.tabs().currentTab().viewState().analysisPanelVisible = app.layout().analysisPanelVisible();
        if (app.layout().analysisPanelVisible()) {
            auto obj = app.tabs().currentTab().currentObject();
            if (obj) {
                app.contactMapPanel_.update(*obj, cutoff);
            }
            if (app.canvas()) app.canvas()->invalidate();
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Contact map visible (cutoff=%.1fA)", cutoff);
            return {true, buf};
        }
        if (app.canvas()) app.canvas()->invalidate();
        return {true, "Contact map hidden"};
    }, ":contactmap [cutoff]", "Toggle the residue contact map panel (default cutoff: 8 \xC3\x85)",
       {":contactmap", ":contactmap 6"}, "Analysis");

    // :cmap — alias for :contactmap
    cmdRegistry_.registerCmd("cmap", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        return app.cmdRegistry().execute(app, cmd.args.empty() ? "contactmap" :
            "contactmap " + cmd.args[0]);
    }, ":cmap [cutoff]", "Toggle contact map panel (alias for :contactmap)",
       {":cmap", ":cmap 6"}, "Analysis");

    cmdRegistry_.registerCmd("interface", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) {
            return {false, "Usage: :interface on|off|legend [cutoff]"};
        }
        // `:interface legend` opens an overlay showing the dash-color
        // legend and per-type contact statistics for the cached overlay.
        if (cmd.args[0] == "legend") {
            app.showInterfaceLegend();
            return {true, ""};
        }
        // First arg is on|off, second arg (optional) is cutoff in Å.
        // Numeric-only first arg keeps the original `:interface 4.5`
        // shorthand working: it implies "on" with that cutoff.
        bool wantOn;
        float cutoff = 4.5f;
        size_t cutoffArgIdx = 1;
        auto parsedBool = parseBool(cmd.args[0]);
        if (parsedBool) {
            wantOn = *parsedBool;
        } else {
            // Try as numeric cutoff → "on" with that cutoff.
            try { cutoff = std::stof(cmd.args[0]); wantOn = true; cutoffArgIdx = 99; }
            catch (...) { return {false, "Usage: :interface on|off|legend [cutoff]"}; }
        }
        if (cmd.args.size() > cutoffArgIdx) {
            try { cutoff = std::stof(cmd.args[cutoffArgIdx]); }
            catch (...) { return {false, "Cutoff must be a number"}; }
        }
        app.interfaceOverlay_ = wantOn;
        if (app.interfaceOverlay_) {
            auto obj = app.tabs().currentTab().currentObject();
            if (!obj) {
                app.interfaceOverlay_ = false;
                return {false, "No object loaded"};
            }
            // Ensure residues are extracted (via contact map update), then
            // compute interface using closest heavy atom distance
            app.contactMapPanel_.update(*obj);
            app.contactMapPanel_.contactMap().computeInterface(*obj, cutoff);
            app.interfaceContacts_ =
                app.contactMapPanel_.contactMap().interfaceContacts();
            if (app.interfaceContacts_.empty()) {
                app.interfaceOverlay_ = false;
                char buf[80];
                std::snprintf(buf, sizeof(buf),
                              "No inter-chain contacts found (cutoff=%.1fA)",
                              cutoff);
                return {false, buf};
            }

            // Build per-atom interface mask: expand contact atoms to
            // whole residues — this is the "same residue as" propagation
            // VMD provides as a Selection primitive, applied here directly.
            const auto& atoms = obj->atoms();
            app.interfaceAtomMask_.assign(atoms.size(), false);
            std::set<std::tuple<std::string,int,char>> interfaceResidues;
            for (const auto& c : app.interfaceContacts_) {
                if (c.atom1 >= 0 && c.atom1 < (int)atoms.size()) {
                    const auto& a = atoms[c.atom1];
                    interfaceResidues.emplace(a.chainId, a.resSeq, a.insCode);
                }
                if (c.atom2 >= 0 && c.atom2 < (int)atoms.size()) {
                    const auto& a = atoms[c.atom2];
                    interfaceResidues.emplace(a.chainId, a.resSeq, a.insCode);
                }
            }
            for (size_t i = 0; i < atoms.size(); ++i) {
                const auto& a = atoms[i];
                if (interfaceResidues.count({a.chainId, a.resSeq, a.insCode}))
                    app.interfaceAtomMask_[i] = true;
            }
            app.interfaceRepr_.setData(app.interfaceAtomMask_,
                                       app.interfaceContacts_);
            app.interfaceRepr_.setDrawSidechains(app.interfaceSidechains_);
            app.interfaceRepr_.setInteractionThickness(app.interfaceThickness_);
            app.interfaceRepr_.setLineThickness(std::max(1, app.interfaceThickness_ - 1));
            app.interfaceRepr_.setShowMask(app.interfaceShowMask_);

            int nHB = 0, nSalt = 0, nHyd = 0, nOther = 0;
            for (const auto& c : app.interfaceContacts_) {
                switch (c.type) {
                    case InteractionType::HBond:       ++nHB;    break;
                    case InteractionType::SaltBridge:  ++nSalt;  break;
                    case InteractionType::Hydrophobic: ++nHyd;   break;
                    case InteractionType::Other:       ++nOther; break;
                }
            }
            auto tag = [&](InteractionType t) {
                return (app.interfaceShowMask_ & interactionBit(t))
                       ? "" : "*";
            };
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                "Interface: %zu residue pairs (cutoff=%.1fA) — "
                "salt %d%s, H-bond %d%s, hydrophobic %d%s, other %d%s"
                "%s",
                app.interfaceContacts_.size(), cutoff,
                nSalt,  tag(InteractionType::SaltBridge),
                nHB,    tag(InteractionType::HBond),
                nHyd,   tag(InteractionType::Hydrophobic),
                nOther, tag(InteractionType::Other),
                app.interfaceShowMask_ == kInterfaceShowAll
                    ? "" : "  [* hidden — :set interface_show all]");
            return {true, buf};
        }
        app.interfaceContacts_.clear();
        app.interfaceAtomMask_.clear();
        app.interfaceRepr_.clear();
        app.interfaceFromZoom_ = false;
        return {true, "Interface overlay hidden"};
    }, ":interface on|off|legend [cutoff]",
       "Show/hide classified inter-chain contact overlay; 'legend' opens a color/stats overlay (default cutoff: 4.5 \xC3\x85)",
       {":interface on", ":interface off", ":interface on 5.0", ":interface legend"}, "Analysis");

    // :focus <selection>  → Mol*-style focus on selection
    //                        (camera + hide non-neighborhood + show
    //                        sidechains + dim cartoon context).
    // :focus off           → restore pre-focus camera + visibility.
    cmdRegistry_.registerCmd("focus", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) {
            return {false, "Usage: :focus <selection>  |  :focus off"};
        }
        if (cmd.args.size() == 1 &&
            (cmd.args[0] == "off" || cmd.args[0] == "none" || cmd.args[0] == "clear")) {
            if (!app.focusActive()) return {true, "Focus already off"};
            app.exitFocus();
            return {true, "Focus exited"};
        }
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object loaded"};
        std::string expr;
        for (size_t i = 0; i < cmd.args.size(); ++i) {
            if (i) expr += ' ';
            expr += cmd.args[i];
        }
        Selection sel = app.parseSelection(expr, *obj);
        if (sel.empty()) return {false, "Empty selection: " + expr};
        app.enterFocus(*obj, sel.indices(), expr);
        return {true, "Focus: " + std::to_string(sel.size()) +
                      " atoms (" + expr + ")"};
    }, ":focus <selection>|off",
       "Mol*-style click-to-focus: zoom in, hide occluders, show sidechains (use 'off' to exit)",
       {":focus chain A and resi 80",
        ":focus $sele",
        ":focus same residue as $sele",
        ":focus same chain as resn HEM",
        ":focus within 5 of resn HEM",
        ":focus off"}, "View");

    // :dssp — Re-run DSSP secondary-structure assignment on the current
    // state. Useful for trajectory frames where the loader's initial SS
    // (from headers, if any) doesn't reflect the current conformation.
    cmdRegistry_.registerCmd("dssp", [](Application& app, const ParsedCommand&) -> ExecResult {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object loaded"};
        // Drop cached SS for this state, force recompute, re-sync atoms.
        obj->invalidateSSCache();
        const auto& ss = obj->ssAtState(obj->activeState());
        if (ss.size() != obj->atoms().size()) {
            return {false, "DSSP: size mismatch (state " +
                           std::to_string(obj->activeState() + 1) + ")"};
        }
        auto& atoms = obj->atoms();
        int nH = 0, nE = 0;
        for (size_t i = 0; i < atoms.size(); ++i) {
            atoms[i].ssType = ss[i];
            if (ss[i] == SSType::Helix) ++nH;
            else if (ss[i] == SSType::Sheet) ++nE;
        }
        return {true, "DSSP recomputed: " + std::to_string(nH) +
                      " H, " + std::to_string(nE) + " E (state " +
                      std::to_string(obj->activeState() + 1) + ")"};
    }, ":dssp", "Recompute secondary structure (Kabsch-Sander) for the current state",
       {":dssp"}, "Analysis");
}

// ----------------------------------------------------------------------
// Help overlay population
// ----------------------------------------------------------------------

void Application::activateOverlay(std::string title,
                                  std::vector<std::string> lines,
                                  std::vector<int> colors,
                                  std::string headlessTitle) {
    if (isHeadless() && !headlessTitle.empty()) {
        std::cout << headlessTitle << "\n\n";
        for (const auto& l : lines) std::cout << l << "\n";
        return;
    }
    infoOverlay_.title = std::move(title);
    infoOverlay_.lines = std::move(lines);
    infoOverlay_.lineColors = std::move(colors);
    infoOverlay_.scrollOffset = 0;
    infoOverlay_.active = true;
}

void Application::showKeybindingHelp() {
    activateOverlay("MolTerm Keybindings", {
        "NAVIGATION",
        " h/j/k/l   Rotate molecule",
        " W/A/S/D   Pan view",
        " +/-       Zoom in/out",
        " </>       Z-axis rotation",
        " 0         Reset view",
        "",
        "REPRESENTATIONS (s=show, x=hide)",
        " sw/sb/ss/sc/sr/sk   wire/ball/fill/cartoon/ribbon/bone",
        " xw/xb/xs/xc/xr/xk   hide each      xa  hide all",
        "",
        "COLORING (c + key)",
        " ce element  cc chain  cs SS  cb B-factor",
        " cp pLDDT    cr rainbow",
        "",
        "OBJECTS & TABS",
        " Tab/S-Tab  Next/prev object   Space  Toggle visible",
        " gt/gT      Next/prev tab      dd     Delete object",
        " o panel   i inspect   / search   n/N results",
        " gs/gS/gC  Atom/residue/chain pick   gf  Focus pick",
        "",
        "ANALYSIS & STATE",
        " I  toggle :interface overlay     F  focus picked residue",
        " [/] prev/next state              b  toggle sequence bar",
        " {/} seqbar prev/next chain",
        "",
        "MACROS / OTHER",
        " q+a-z record   @+a-z play    m  toggle pixel renderer",
        " P  screenshot  u/Ctrl-r undo/redo  .  repeat last action",
        "",
        ":help [cmd]   :load   :fetch   :align   :measure",
        ":focus   :dssp   :interface   :export   :screenshot",
    });
}

void Application::showCommandIndex() {
    // Group commands by category. Aliases (commands sharing usage with a
    // canonical entry, like :e for :load) appear in the same group; we
    // keep them all so the index shows every dispatchable name.
    // Category "Hidden" is reserved for back-compat aliases that are
    // dispatchable but should not surface in the index (still reachable
    // via :help :<name>).
    std::map<std::string, std::vector<const CommandInfo*>> groups;
    for (const auto& [name, info] : cmdRegistry_.all()) {
        if (info.category == "Hidden") continue;
        const std::string& cat = info.category.empty() ? "Misc" : info.category;
        groups[cat].push_back(&info);
    }
    // Stable category ordering: most user-facing first, then alphabetical.
    static const std::vector<std::string> ordered = {
        "Files", "Display", "Coloring", "View", "Selection",
        "Measurement", "Analysis", "Session", "Window", "Help"
    };

    std::vector<std::string> lines;
    auto emitGroup = [&](const std::string& cat,
                         std::vector<const CommandInfo*>& cmds) {
        std::sort(cmds.begin(), cmds.end(),
                  [](const CommandInfo* a, const CommandInfo* b) {
                      return a->name < b->name;
                  });
        lines.push_back(cat);
        for (const auto* info : cmds) {
            std::string usage = info->usage.empty() ? (":" + info->name) : info->usage;
            std::string row = " " + usage;
            const int kPad = 36;
            if (static_cast<int>(row.size()) < kPad)
                row.append(kPad - row.size(), ' ');
            else
                row += "  ";
            row += info->description;
            lines.push_back(row);
        }
        lines.push_back("");
    };

    for (const auto& cat : ordered) {
        auto it = groups.find(cat);
        if (it == groups.end()) continue;
        emitGroup(cat, it->second);
        groups.erase(it);
    }
    // Anything uncategorized falls through alphabetically.
    for (auto& [cat, cmds] : groups) emitGroup(cat, cmds);

    if (!lines.empty() && lines.back().empty()) lines.pop_back();
    lines.push_back(":help <cmd>  for usage, description, and examples");

    activateOverlay("MolTerm Commands", std::move(lines), {}, "MolTerm Commands");
}

void Application::showCommandHelp(const CommandInfo& info) {
    std::vector<std::string> lines;
    lines.push_back("Usage:");
    lines.push_back("  " + (info.usage.empty() ? (":" + info.name) : info.usage));
    lines.push_back("");
    if (!info.description.empty()) {
        lines.push_back("Description:");
        lines.push_back("  " + info.description);
        lines.push_back("");
    }
    if (!info.category.empty()) {
        lines.push_back("Category: " + info.category);
        lines.push_back("");
    }
    if (!info.examples.empty()) {
        lines.push_back("Examples:");
        for (const auto& ex : info.examples) lines.push_back("  " + ex);
    }
    if (!lines.empty() && lines.back().empty()) lines.pop_back();

    activateOverlay(":" + info.name, std::move(lines), {}, ":" + info.name);
}

void Application::showInterfaceLegend() {
    // Color each legend swatch row in the interaction-type's render color so
    // the modal directly mirrors what the viewport draws.
    static constexpr const char* kSwatch = "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80 "
                                            "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80 "
                                            "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80";  // ─── ─── ───
    static constexpr const char* kAng = "\xC3\x85";  // Å

    std::vector<std::string> lines;
    std::vector<int> colors;
    auto add = [&](std::string text, int color = -1) {
        lines.push_back(std::move(text));
        colors.push_back(color);
    };

    add("Interaction types (dashed inter-chain lines):");
    add("");
    add(std::string("  ") + kSwatch + "  H-bond        N/O \xE2\x86\x94 N/O,           \xE2\x89\xA4 3.5 " + kAng,
        interactionColor(InteractionType::HBond));
    add(std::string("  ") + kSwatch + "  Salt bridge   charged,             \xE2\x89\xA4 4.0 " + kAng,
        interactionColor(InteractionType::SaltBridge));
    add(std::string("  ") + kSwatch + "  Hydrophobic   C \xE2\x86\x94 C,               \xE2\x89\xA4 4.5 " + kAng,
        interactionColor(InteractionType::Hydrophobic));
    add(std::string("  ") + kSwatch + "  Other         heavy-atom pair below cutoff",
        interactionColor(InteractionType::Other));
    add("");

    if (interfaceContacts_.empty()) {
        add("Statistics:");
        add("  No interface overlay active.");
        add("  Run :interface on  to compute and display contacts.");
    } else {
        // When focus mode is active, restrict statistics to contacts that
        // touch the focus subject — the user is looking at one binding site,
        // so the legend should reflect that site, not the whole structure.
        const bool focusFiltered = focusSnapshot_.active && !focusAtomMask_.empty();
        auto inFocus = [&](int atomIdx) {
            return atomIdx >= 0 && atomIdx < (int)focusAtomMask_.size()
                   && focusAtomMask_[atomIdx];
        };

        struct TypeStats {
            int n = 0;
            float dMin = std::numeric_limits<float>::infinity();
            float dMax = -std::numeric_limits<float>::infinity();
            float dSum = 0.0f;
        };
        std::array<TypeStats, 4> ts{};
        std::set<std::tuple<std::string, int, char>> residues;
        auto obj = tabMgr_.currentTab().currentObject();
        const std::vector<AtomData> empty;
        const std::vector<AtomData>& atoms = obj ? obj->atoms() : empty;
        for (const auto& c : interfaceContacts_) {
            if (focusFiltered && !inFocus(c.atom1) && !inFocus(c.atom2)) continue;
            int idx = static_cast<int>(c.type);
            if (idx >= 0 && idx < 4) {
                ts[idx].n++;
                ts[idx].dMin = std::min(ts[idx].dMin, c.distance);
                ts[idx].dMax = std::max(ts[idx].dMax, c.distance);
                ts[idx].dSum += c.distance;
            }
            if (c.atom1 >= 0 && c.atom1 < (int)atoms.size()) {
                const auto& a = atoms[c.atom1];
                residues.emplace(a.chainId, a.resSeq, a.insCode);
            }
            if (c.atom2 >= 0 && c.atom2 < (int)atoms.size()) {
                const auto& a = atoms[c.atom2];
                residues.emplace(a.chainId, a.resSeq, a.insCode);
            }
        }
        int total = 0;
        for (const auto& s : ts) total += s.n;

        add("Statistics:");
        char buf[200];
        if (focusFiltered) {
            const std::string& expr = focusExpr_.empty() ? std::string("focus subject") : focusExpr_;
            std::snprintf(buf, sizeof(buf), "  Scope: focus subject  (%.*s)",
                          static_cast<int>(std::min<size_t>(expr.size(), 80)), expr.c_str());
            add(buf);
        } else {
            add("  Scope: whole structure");
        }
        std::snprintf(buf, sizeof(buf), "  Total contacts:        %d", total);
        add(buf);
        std::snprintf(buf, sizeof(buf), "  Residues at interface: %zu", residues.size());
        add(buf);
        add("");
        add("By type:");
        struct Row { const char* name; InteractionType type; };
        const Row rows[4] = {
            {"H-bond",      InteractionType::HBond},
            {"Salt bridge", InteractionType::SaltBridge},
            {"Hydrophobic", InteractionType::Hydrophobic},
            {"Other",       InteractionType::Other},
        };
        for (const auto& r : rows) {
            const auto& s = ts[static_cast<int>(r.type)];
            const bool drawn = interfaceShowMask_ & interactionBit(r.type);
            const char* hidden = drawn ? "" : "  [hidden]";
            if (s.n == 0) {
                std::snprintf(buf, sizeof(buf),
                              "  %-12s %4d%s", r.name, 0, hidden);
            } else {
                float pct = total ? (100.0f * s.n / total) : 0.0f;
                float avg = s.dSum / s.n;
                std::snprintf(buf, sizeof(buf),
                              "  %-12s %4d  (%4.1f%%)  avg %.2f %s   (%.2f - %.2f)%s",
                              r.name, s.n, pct, avg, kAng, s.dMin, s.dMax, hidden);
            }
            add(buf, interactionColor(r.type));
        }
        if (interfaceShowMask_ != kInterfaceShowAll) {
            add("");
            add("  :set interface_show all  to draw every type");
        }
    }

    activateOverlay("Interface Overlay", std::move(lines),
                    std::move(colors), "Interface Overlay");
}

} // namespace molterm
