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
#include "molterm/core/Logger.h"
#include "molterm/io/SessionSaver.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <signal.h>

namespace molterm {

static int applyHeteroatomColors(MolObject& obj) {
    const auto& atoms = obj.atoms();
    int count = 0;
    for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
        if (atoms[i].element != "C" && atoms[i].element != "H") {
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
    }
    return "?";
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
    running_ = true;
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
            } else {
                auto t0 = std::chrono::steady_clock::now();
                renderFrame();
                auto t1 = std::chrono::steady_clock::now();
                lastFrameMs_ = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

                if (lastFrameMs_ > 100) {
                    framesToSkip_ = std::min(3, static_cast<int>(lastFrameMs_ / 50));
                }
            }
            needsRedraw_ = false;
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
}

std::string Application::loadFile(const std::string& path) {
    MLOG_INFO("Loading file: %s", path.c_str());

    // Show loading message immediately
    cmdLine_.setMessage("Loading " + path + "...");
    cmdLine_.render(layout_.commandLine());
    layout_.commandLine().refresh();
    doupdate();

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
        needsRedraw_ = true;
        return;
    }
    if (macroPlayAwaitingRegister_) {
        macroPlayAwaitingRegister_ = false;
        if (key >= 'a' && key <= 'z') {
            playMacro(static_cast<char>(key));
        } else {
            cmdLine_.setMessage("Invalid macro register (use a-z)");
        }
        needsRedraw_ = true;
        return;
    }

    // Dismiss help overlay on any key
    if (helpOverlay_) {
        helpOverlay_ = false;
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

    auto& tab = tabMgr_.currentTab();
    auto& cam = tab.camera();

    needsRedraw_ = true;

    // Scroll wheel for zoom
    if (event.bstate & BUTTON4_PRESSED) {
        cam.zoomBy(1.15f);
    }
#ifdef BUTTON5_PRESSED
    else if (event.bstate & BUTTON5_PRESSED) {
        cam.zoomBy(1.0f / 1.15f);
    }
#endif
    else if (event.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED)) {
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
            } else {
                // Inspect mode — show info at current level
                pickedAtomIdx_ = atomIdx;
                focusResi_ = a.resSeq;
                focusChain_ = a.chainId;
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
        // Click in seqbar → center on residue
        else if (layout_.seqBarVisible()) {
            int seqBarY = 1 + layout_.viewportHeight();
            int seqBarH = layout_.seqBar().height();
            if (event.y >= seqBarY && event.y < seqBarY + seqBarH) {
                std::string clickChain;
                int resi = seqBar_.resSeqAtColumn(event.x, layout_.seqBarWrap(),
                                                   layout_.seqBar().width(), &clickChain);
                if (resi >= 0) {
                    focusResi_ = resi;
                    focusChain_ = clickChain;
                    auto obj = tabMgr_.currentTab().currentObject();
                    if (obj) {
                        const auto& atoms = obj->atoms();
                        for (const auto& a : atoms) {
                            if (a.resSeq == resi && a.chainId == clickChain &&
                                (a.name == "CA" || a.name == "C1'")) {
                                auto& seqCam = tabMgr_.currentTab().camera();
                                seqCam.setCenter(a.x, a.y, a.z);
                                if (seqCam.zoom() < 5.0f) seqCam.setZoom(8.0f);
                                cmdLine_.setMessage(a.chainId + "/" +
                                    a.resName + " " + std::to_string(resi));
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
}

void Application::handleAction(Action action) {
    auto& tab = tabMgr_.currentTab();
    auto& cam = tab.camera();
    float rs = cam.rotationSpeed();

    needsRedraw_ = true;

    switch (action) {
        // Navigation
        case Action::RotateLeft:   cam.rotateY(-rs);  break;
        case Action::RotateRight:  cam.rotateY(rs);   break;
        case Action::RotateUp:     cam.rotateX(-rs);  break;
        case Action::RotateDown:   cam.rotateX(rs);   break;
        case Action::RotateCW:    cam.rotateZ(rs);   break;
        case Action::RotateCCW:   cam.rotateZ(-rs);  break;
        case Action::PanLeft:     cam.pan(-cam.panSpeed(), 0);    break;
        case Action::PanRight:    cam.pan(cam.panSpeed(), 0);     break;
        case Action::PanUp:       cam.pan(0, -cam.panSpeed());    break;
        case Action::PanDown:     cam.pan(0, cam.panSpeed());     break;
        case Action::ZoomIn:      cam.zoomBy(1.2f);  break;
        case Action::ZoomOut:     cam.zoomBy(1.0f/1.2f); break;
        case Action::ResetView:   cam.reset(); tab.centerView(); break;
        case Action::CenterSelection: tab.centerView(); break;
        case Action::Redraw:
            clearScreenAndRepaint();
            break;

        // Objects
        case Action::NextObject:    tab.selectNextObject(); break;
        case Action::PrevObject:    tab.selectPrevObject(); break;
        case Action::ToggleVisible: {
            auto obj = tab.currentObject();
            if (obj) obj->toggleVisible();
            break;
        }
        case Action::DeleteObject: {
            int idx = tab.selectedObjectIdx();
            if (idx >= 0) {
                auto obj = tab.currentObject();
                if (obj) store_.remove(obj->name());
                tab.removeObject(idx);
            }
            break;
        }

        // Representations
        case Action::ShowWireframe: {
            auto obj = tab.currentObject();
            if (obj) obj->showRepr(ReprType::Wireframe);
            break;
        }
        case Action::ShowBallStick: {
            auto obj = tab.currentObject();
            if (obj) obj->showRepr(ReprType::BallStick);
            break;
        }
        case Action::ShowSpacefill: {
            auto obj = tab.currentObject();
            if (obj) obj->showRepr(ReprType::Spacefill);
            break;
        }
        case Action::ShowCartoon: {
            auto obj = tab.currentObject();
            if (obj) obj->showRepr(ReprType::Cartoon);
            break;
        }
        case Action::ShowRibbon: {
            auto obj = tab.currentObject();
            if (obj) obj->showRepr(ReprType::Ribbon);
            break;
        }
        case Action::ShowBackbone: {
            auto obj = tab.currentObject();
            if (obj) obj->showRepr(ReprType::Backbone);
            break;
        }
        case Action::HideWireframe: {
            auto obj = tab.currentObject();
            if (obj) obj->hideRepr(ReprType::Wireframe);
            break;
        }
        case Action::HideBallStick: {
            auto obj = tab.currentObject();
            if (obj) obj->hideRepr(ReprType::BallStick);
            break;
        }
        case Action::HideSpacefill: {
            auto obj = tab.currentObject();
            if (obj) obj->hideRepr(ReprType::Spacefill);
            break;
        }
        case Action::HideCartoon: {
            auto obj = tab.currentObject();
            if (obj) obj->hideRepr(ReprType::Cartoon);
            break;
        }
        case Action::HideRibbon: {
            auto obj = tab.currentObject();
            if (obj) obj->hideRepr(ReprType::Ribbon);
            break;
        }
        case Action::HideBackbone: {
            auto obj = tab.currentObject();
            if (obj) obj->hideRepr(ReprType::Backbone);
            break;
        }
        case Action::HideAll: {
            auto obj = tab.currentObject();
            if (obj) obj->hideAllRepr();
            break;
        }

        // Coloring
        case Action::ColorByElement: {
            auto obj = tab.currentObject();
            if (obj) applyHeteroatomColors(*obj);
            break;
        }
        case Action::ColorByChain: {
            auto obj = tab.currentObject();
            if (obj) obj->setColorScheme(ColorScheme::Chain);
            break;
        }
        case Action::ColorBySS: {
            auto obj = tab.currentObject();
            if (obj) obj->setColorScheme(ColorScheme::SecondaryStructure);
            break;
        }
        case Action::ColorByBFactor: {
            auto obj = tab.currentObject();
            if (obj) obj->setColorScheme(ColorScheme::BFactor);
            break;
        }

        // Tabs
        case Action::NextTab:  tabMgr_.nextTab(); break;
        case Action::PrevTab:  tabMgr_.prevTab(); break;
        case Action::NewTab:   tabMgr_.addTab(); break;
        case Action::CloseTab: tabMgr_.closeCurrentTab(); break;

        // Panel
        case Action::TogglePanel: layout_.togglePanel(); break;

        // Mode transitions
        case Action::EnterCommand:
            inputHandler_->setMode(Mode::Command);
            cmdLine_.activate(':');
            break;
        case Action::EnterSearch:
            inputHandler_->setMode(Mode::Search);
            cmdLine_.activate('/');
            break;
        case Action::EnterVisual:
            inputHandler_->setMode(Mode::Visual);
            break;
        case Action::ExitToNormal:
            inputHandler_->setMode(Mode::Normal);
            cmdLine_.deactivate();
            pickedAtomIdx_ = -1;
            if (pickMode_ != PickMode::Inspect) {
                pickMode_ = PickMode::Inspect;
                cmdLine_.setMessage("Inspect mode | sele=" +
                    std::to_string(namedSelections_["sele"].size()));
            }
            break;

        // Command mode actions
        case Action::ExecuteCommand: {
            std::string input = cmdLine_.input();
            cmdLine_.pushHistory(input);
            cmdLine_.deactivate();
            inputHandler_->setMode(Mode::Normal);
            std::string result = cmdRegistry_.execute(*this, input);
            if (!result.empty()) cmdLine_.setMessage(result);
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
                           cmdName == "align" || cmdName == "mmalign" || cmdName == "super") {
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
                } else if (cmdName == "set") {
                    for (const auto& o : {"renderer", "backbone_thickness", "bt",
                                           "wireframe_thickness", "wt", "ball_radius", "br",
                                           "pan_speed", "ps", "fog", "auto_center", "panel",
                                           "seqbar", "seqwrap"}) {
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
            break;
        }
        case Action::HistoryPrev: cmdLine_.historyPrev(); break;
        case Action::HistoryNext: cmdLine_.historyNext(); break;
        case Action::DeleteWord:  cmdLine_.deleteWord();  break;
        case Action::ClearLine:   cmdLine_.clearInput();  break;

        // Search
        case Action::ExecuteSearch: {
            std::string query = cmdLine_.input();
            cmdLine_.deactivate();
            inputHandler_->setMode(Mode::Normal);
            executeSearch(query);
            break;
        }

        // Help
        // Search navigation
        case Action::SearchNext: {
            if (searchMatches_.empty()) {
                cmdLine_.setMessage("No search results");
                break;
            }
            auto obj = tab.currentObject();
            if (!obj) break;
            searchIdx_ = (searchIdx_ + 1) % static_cast<int>(searchMatches_.size());
            const auto& a = obj->atoms()[searchMatches_[searchIdx_]];
            cmdLine_.setMessage("/" + lastSearch_ + " [" + std::to_string(searchIdx_ + 1) +
                               "/" + std::to_string(searchMatches_.size()) + "] " +
                               a.chainId + " " + a.resName + " " + std::to_string(a.resSeq) +
                               " " + a.name);
            break;
        }
        case Action::SearchPrev: {
            if (searchMatches_.empty()) {
                cmdLine_.setMessage("No search results");
                break;
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
            break;
        }

        // Undo/Redo
        case Action::Undo: {
            std::string msg = undoStack_.undo();
            cmdLine_.setMessage(msg.empty() ? "Nothing to undo" : msg);
            break;
        }
        case Action::Redo: {
            std::string msg = undoStack_.redo();
            cmdLine_.setMessage(msg.empty() ? "Nothing to redo" : msg);
            break;
        }

        // Screenshot
        case Action::Screenshot: {
            std::string result = cmdRegistry_.execute(*this, "screenshot");
            if (!result.empty()) cmdLine_.setMessage(result);
            break;
        }

        // Renderer toggle (braille ↔ pixel)
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
            break;
        }

        // Macro recording
        case Action::StartMacro:
            if (macroRecording_) {
                // 'q' again while recording → stop (register already known)
                stopMacroRecord();
            } else {
                macroAwaitingRegister_ = true;
                cmdLine_.setMessage("Record macro: press register (a-z)");
            }
            break;
        case Action::PlayMacro:
            macroPlayAwaitingRegister_ = true;
            cmdLine_.setMessage("Play macro: press register (a-z)");
            break;

        // pLDDT coloring
        case Action::ColorByPLDDT: {
            auto obj = tab.currentObject();
            if (obj) obj->setColorScheme(ColorScheme::PLDDT);
            break;
        }
        case Action::ColorByRainbow: {
            auto obj = tab.currentObject();
            if (obj) obj->setColorScheme(ColorScheme::Rainbow);
            break;
        }
        case Action::ColorByResType: {
            auto obj = tab.currentObject();
            if (obj) obj->setColorScheme(ColorScheme::ResType);
            break;
        }

        // Multi-state cycling
        case Action::NextState: {
            auto obj = tab.currentObject();
            if (obj && obj->stateCount() > 1) {
                obj->nextState();
                cmdLine_.setMessage("State " + std::to_string(obj->activeState() + 1) +
                                   "/" + std::to_string(obj->stateCount()));
            } else {
                cmdLine_.setMessage("Single-state structure");
            }
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
            break;
        }

        // Inspect (mouse-only — 'i' just shows level info)
        case Action::Inspect:
            cmdLine_.setMessage(std::string("Click to inspect | gs/gS/gc to select | Level: ") +
                inspectLevelName(inspectLevel_));
            break;

        // Cycle inspect level: Atom → Residue → Chain → Object
        case Action::CycleInspectLevel: {
            inspectLevel_ = static_cast<InspectLevel>(
                (static_cast<int>(inspectLevel_) + 1) % 4);
            cmdLine_.setMessage(std::string("Inspect level: ") + inspectLevelName(inspectLevel_));
            break;
        }

        // Pick mode: toggle atom/residue/chain selection mode
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
            break;
        }

        case Action::ToggleSeqBar: {
            if (!layout_.seqBarVisible()) {
                layout_.toggleSeqBar();
                if (layout_.seqBarWrap()) layout_.toggleSeqBarWrap();
                cmdLine_.setMessage("Sequence bar: scroll");
            } else if (!layout_.seqBarWrap()) {
                layout_.toggleSeqBarWrap();
                cmdLine_.setMessage("Sequence bar: wrap");
            } else {
                layout_.toggleSeqBarWrap();
                layout_.toggleSeqBar();
                cmdLine_.setMessage("Sequence bar: hidden");
            }
            if (canvas_) canvas_->invalidate();
            onResize();
            break;
        }

        case Action::SeqBarNextChain:
            if (layout_.seqBarVisible() && !layout_.seqBarWrap()) {
                seqBar_.nextChain();
                seqBar_.scrollToChain(seqBar_.activeChain());
                cmdLine_.setMessage("Chain: " + seqBar_.activeChain());
            }
            break;
        case Action::SeqBarPrevChain:
            if (layout_.seqBarVisible() && !layout_.seqBarWrap()) {
                seqBar_.prevChain();
                seqBar_.scrollToChain(seqBar_.activeChain());
                cmdLine_.setMessage("Chain: " + seqBar_.activeChain());
            }
            break;

        case Action::ShowOverlay:
            overlayVisible_ = true;
            cmdLine_.setMessage("Overlays visible");
            break;
        case Action::HideOverlay:
            overlayVisible_ = false;
            cmdLine_.setMessage("Overlays hidden");
            break;

        case Action::ApplyPreset: {
            auto obj = tab.currentObject();
            if (obj) {
                obj->applySmartDefaults();
                cmdLine_.setMessage("Applied default preset");
            }
            break;
        }

        case Action::ShowHelp:
            helpOverlay_ = true;
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

    // Tab bar
    tabBar_.render(layout_.tabBar(), tabMgr_.tabNames(), tabMgr_.currentIndex());

    // Adjust seqbar height BEFORE rendering viewport (setSeqBarHeight rebuilds windows)
    if (layout_.seqBarVisible()) {
        auto obj = tabMgr_.currentTab().currentObject();
        if (obj) {
            seqBar_.update(*obj);
            if (layout_.seqBarWrap()) {
                int needed = std::min(seqBar_.wrapRows(layout_.seqBar().width()),
                                      screen_.height() / 4);
                if (needed != layout_.seqBar().height()) {
                    layout_.setSeqBarHeight(std::max(1, needed));
                    if (canvas_) canvas_->invalidate();
                }
            } else {
                if (layout_.seqBar().height() != 1) {
                    layout_.setSeqBarHeight(1);
                    if (canvas_) canvas_->invalidate();
                }
            }
        }
    }

    // Viewport — render to canvas but defer pixel flush
    renderViewport();

    // Clear camera dirty flag after rendering (so caches like Spacefill sort work)
    tabMgr_.currentTab().camera().clearDirty();

    // Object panel
    if (layout_.panelVisible()) {
        auto& tab = tabMgr_.currentTab();
        objectPanel_.render(layout_.objectPanel(), tab.objects(),
                           tab.selectedObjectIdx());
    }

    // Sequence bar
    if (layout_.seqBarVisible()) {
        auto obj = tabMgr_.currentTab().currentObject();
        if (obj) {
            const Selection* sele = nullptr;
            auto selIt = namedSelections_.find("sele");
            if (selIt != namedSelections_.end()) sele = &selIt->second;
            seqBar_.render(layout_.seqBar(), focusResi_, focusChain_, sele,
                          obj->colorScheme(), layout_.seqBarWrap());
        }
    }

    // Status bar
    updateStatusBar();

    // Command line
    cmdLine_.render(layout_.commandLine());

    layout_.refreshAll();
    doupdate();

    // Pixel graphics must be written AFTER doupdate() so ncurses doesn't overwrite.
    if (rendererType_ == RendererType::Pixel) {
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

    for (const auto& obj : tab.objects()) {
        if (!obj->visible()) continue;
        for (auto& [reprType, repr] : representations_) {
            if (obj->reprVisible(reprType)) {
                repr->render(*obj, tab.camera(), *canvas_);
            }
        }
    }

    // Apply depth fog on pixel canvas (post-processing before flush)
    if (rendererType_ == RendererType::Pixel) {
        auto* pc = dynamic_cast<PixelCanvas*>(canvas_.get());
        if (pc && fogStrength_ > 0.0f) pc->applyDepthFog(fogStrength_);
    }

    // Pixel flush is deferred to after doupdate() in renderFrame().
    // Other renderers flush here (they go through ncurses).
    if (rendererType_ != RendererType::Pixel) {
        canvas_->flush(win);
    }

    // Help overlay: keybinding cheat sheet
    if (helpOverlay_) {
        int ow = std::min(60, w - 4);
        int oh = std::min(30, h - 2);
        int ox = (w - ow) / 2;
        int oy = (h - oh) / 2;

        // Background box
        for (int y = oy; y < oy + oh && y < h; ++y) {
            for (int x = ox; x < ox + ow && x < w; ++x)
                win.addCharColored(y, x, ' ', kColorStatusBar);
        }

        // Title
        win.printColored(oy, ox + (ow - 20) / 2, "  MolTerm Keybindings ", kColorTabActive);

        int row = oy + 2;
        auto line = [&](const std::string& text) {
            if (row < oy + oh - 1)
                win.printColored(row++, ox + 2, text, kColorStatusBar);
        };

        line("NAVIGATION");
        line(" h/j/k/l   Rotate molecule");
        line(" W/A/S/D   Pan view");
        line(" +/-       Zoom in/out");
        line(" </>       Z-axis rotation");
        line(" 0         Reset view");
        line("");
        line("REPRESENTATIONS (s=show, x=hide)");
        line(" sw/sb/ss/sc/sr/sk   wire/ball/fill/cartoon/ribbon/bone");
        line(" xw/xb/xs/xc/xr/xk  hide each    xa  hide all");
        line("");
        line("COLORING (c + key)");
        line(" ce element  cc chain  cs SS  cb B-factor");
        line(" cp pLDDT    cr rainbow");
        line("");
        line("OBJECTS & TABS");
        line(" Tab/S-Tab  Next/prev object  Space  Toggle visible");
        line(" gt/gT      Next/prev tab     dd     Delete object");
        line(" o panel   i inspect   / search   n/N results");
        line("");
        line("MULTI-STATE       MACROS         OTHER");
        line(" [/] prev/next    q record       m  toggle pixel");
        line("     state        @ play          P  screenshot");
        line("");
        line(":help  :load  :fetch  :align  :measure  :export");
        line("");
        win.printColored(std::min(row, oy + oh - 1), ox + (ow - 24) / 2,
                        "  Press any key to close  ", kColorTabActive);
    }

    // Show history hint overlay when command line is active and empty
    cmdLine_.renderHistoryHint(win);

  if (overlayVisible_) {
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
                // Offset label to the right of atom
                int lx = std::min(tx + 1, w - static_cast<int>(lbl.size()));
                win.printColored(ty, lx, lbl, kColorWhite);
            }
        }
    }

    // Draw measurement dashed lines + labels
    {
        auto obj = tabMgr_.currentTab().currentObject();
        int scaleX = canvas_ ? canvas_->scaleX() : 1;
        int scaleY = canvas_ ? canvas_->scaleY() : 1;
        if (obj && !measurements_.empty()) {
            const auto& atoms = obj->atoms();
            auto& cam = tabMgr_.currentTab().camera();
            for (const auto& m : measurements_) {
                if (m.atoms.size() < 2) continue;
                // Draw lines between consecutive atoms in the measurement
                for (size_t mi = 0; mi + 1 < m.atoms.size(); ++mi) {
                    int a1 = m.atoms[mi], a2 = m.atoms[mi + 1];
                    if (a1 < 0 || a1 >= static_cast<int>(atoms.size())) continue;
                    if (a2 < 0 || a2 >= static_cast<int>(atoms.size())) continue;
                    float sx1, sy1, d1, sx2, sy2, d2;
                    cam.projectCached(atoms[a1].x, atoms[a1].y, atoms[a1].z, sx1, sy1, d1);
                    cam.projectCached(atoms[a2].x, atoms[a2].y, atoms[a2].z, sx2, sy2, d2);
                    int tx1 = static_cast<int>(sx1) / scaleX, ty1 = static_cast<int>(sy1) / scaleY;
                    int tx2 = static_cast<int>(sx2) / scaleX, ty2 = static_cast<int>(sy2) / scaleY;
                    // Dashed line in terminal space
                    int steps = std::max(std::abs(tx2 - tx1), std::abs(ty2 - ty1));
                    if (steps == 0) steps = 1;
                    for (int s = 0; s <= steps; s += 2) {  // skip every other for dashes
                        float t = static_cast<float>(s) / static_cast<float>(steps);
                        int x = tx1 + static_cast<int>((tx2 - tx1) * t);
                        int y = ty1 + static_cast<int>((ty2 - ty1) * t);
                        if (x >= 0 && x < w && y >= 0 && y < h)
                            win.addCharColored(y, x, '-', kColorYellow);
                    }
                }
                // Label at midpoint of first segment
                int a1 = m.atoms[0], a2 = m.atoms[1];
                float sx1, sy1, d1, sx2, sy2, d2;
                cam.projectCached(atoms[a1].x, atoms[a1].y, atoms[a1].z, sx1, sy1, d1);
                cam.projectCached(atoms[a2].x, atoms[a2].y, atoms[a2].z, sx2, sy2, d2);
                int mx = (static_cast<int>(sx1) / scaleX + static_cast<int>(sx2) / scaleX) / 2;
                int my = (static_cast<int>(sy1) / scaleY + static_cast<int>(sy2) / scaleY) / 2;
                if (mx >= 0 && mx < w - static_cast<int>(m.label.size()) && my >= 0 && my < h)
                    win.printColored(my, mx, m.label, kColorYellow);
            }
        }
    }

    // Draw selection highlight overlay for $sele atoms
    {
        auto selIt = namedSelections_.find("sele");
        if (selIt != namedSelections_.end() && !selIt->second.empty()) {
            // Build proj cache if not already built this frame
            buildProjCache();
            int scaleX = canvas_ ? canvas_->scaleX() : 1;
            int scaleY = canvas_ ? canvas_->scaleY() : 1;
            for (const auto& pa : projCache_) {
                if (selIt->second.has(pa.idx)) {
                    int tx = pa.sx / scaleX;
                    int ty = pa.sy / scaleY;
                    if (tx >= 0 && tx < w && ty >= 0 && ty < h) {
                        win.addCharColored(ty, tx, '*', kColorYellow);
                    }
                }
            }
        }
    }
  } // overlayVisible_

    win.refresh();
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
    cmdRegistry_.registerCmd("q", [](Application& app, const ParsedCommand& cmd) -> std::string {
        app.quit(cmd.forced);
        return "";
    }, ":q[!]", "Quit MolTerm");
    cmdRegistry_.registerCmd("quit", [](Application& app, const ParsedCommand& cmd) -> std::string {
        app.quit(cmd.forced);
        return "";
    }, ":quit[!]", "Quit MolTerm");
    cmdRegistry_.registerCmd("qa", [](Application& app, const ParsedCommand&) -> std::string {
        app.quit(true);
        return "";
    }, ":qa", "Quit all");

    // :load <file>
    cmdRegistry_.registerCmd("load", [](Application& app, const ParsedCommand& cmd) -> std::string {
        if (cmd.args.empty()) return "Usage: :load <file>";
        return app.loadFile(cmd.args[0]);
    }, ":load <file>", "Load a structure file");
    cmdRegistry_.registerCmd("e", [](Application& app, const ParsedCommand& cmd) -> std::string {
        if (cmd.args.empty()) return "Usage: :e <file>";
        return app.loadFile(cmd.args[0]);
    }, ":e <file>", "Load a structure file (alias for :load)");

    // :tabnew
    cmdRegistry_.registerCmd("tabnew", [](Application& app, const ParsedCommand& cmd) -> std::string {
        std::string name = cmd.args.empty() ? "" : cmd.args[0];
        app.tabs().addTab(name);
        app.tabs().goToTab(static_cast<int>(app.tabs().count()) - 1);
        return "New tab created";
    }, ":tabnew [name]", "Create new tab");

    // :tabclose
    cmdRegistry_.registerCmd("tabclose", [](Application& app, const ParsedCommand&) -> std::string {
        if (app.tabs().count() <= 1) return "Cannot close last tab";
        app.tabs().closeCurrentTab();
        return "";
    }, ":tabclose", "Close current tab");

    // :objects
    cmdRegistry_.registerCmd("objects", [](Application& app, const ParsedCommand&) -> std::string {
        auto names = app.store().names();
        if (names.empty()) return "No objects loaded";
        std::string result = "Objects:";
        for (const auto& n : names) result += " " + n;
        return result;
    }, ":objects", "List loaded objects");

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
    cmdRegistry_.registerCmd("show", [resolveRepr](Application& app, const ParsedCommand& cmd) -> std::string {
        if (cmd.args.empty()) return "Usage: :show <repr> [selection]";
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return "No object selected";
        ReprType rt;
        if (!resolveRepr(cmd.args[0], rt)) return "Unknown representation: " + cmd.args[0];

        if (cmd.args.size() > 1) {
            // Per-atom show: :show cartoon chain A
            std::string expr;
            for (size_t i = 1; i < cmd.args.size(); ++i) {
                if (i > 1) expr += " ";
                expr += cmd.args[i];
            }
            auto sel = app.parseSelection(expr, *obj);
            if (sel.empty()) return "No atoms match: " + expr;
            obj->showReprForAtoms(rt, std::vector<int>(sel.indices().begin(), sel.indices().end()));
            return "Showing " + cmd.args[0] + " for " + std::to_string(sel.size()) + " atoms";
        }
        obj->showRepr(rt);
        return "Showing " + cmd.args[0];
    }, ":show <repr> [selection]", "Show representation (optionally for selection)");

    // :hide <repr|all> [selection]
    cmdRegistry_.registerCmd("hide", [resolveRepr](Application& app, const ParsedCommand& cmd) -> std::string {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return "No object selected";
        if (cmd.args.empty() || cmd.args[0] == "all") {
            obj->hideAllRepr();
            return "Hidden all representations";
        }
        ReprType rt;
        if (!resolveRepr(cmd.args[0], rt)) return "Unknown representation: " + cmd.args[0];

        if (cmd.args.size() > 1) {
            std::string expr;
            for (size_t i = 1; i < cmd.args.size(); ++i) {
                if (i > 1) expr += " ";
                expr += cmd.args[i];
            }
            auto sel = app.parseSelection(expr, *obj);
            if (sel.empty()) return "No atoms match: " + expr;
            obj->hideReprForAtoms(rt, std::vector<int>(sel.indices().begin(), sel.indices().end()));
            return "Hidden " + cmd.args[0] + " for " + std::to_string(sel.size()) + " atoms";
        }
        obj->hideRepr(rt);
        return "Hidden " + cmd.args[0];
    }, ":hide <repr|all> [selection]", "Hide representation (optionally for selection)");

    // :color <scheme>
    cmdRegistry_.registerCmd("color", [](Application& app, const ParsedCommand& cmd) -> std::string {
        if (cmd.args.empty())
            return "Usage: :color <scheme> or :color <name> <selection> | Colors: " + ColorMapper::availableColors();
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return "No object selected";

        const auto& first = cmd.args[0];

        // Check if first arg is a color scheme name
        if (first == "element" || first == "cpk") {
            int count = applyHeteroatomColors(*obj);
            return "Colored " + std::to_string(count) + " heteroatoms by element";
        }
        if (first == "chain") {
            // "color chain" with no more args → scheme
            // "color chain chain A" → ambiguous, treat as scheme if only 1 arg
            if (cmd.args.size() == 1) {
                obj->setColorScheme(ColorScheme::Chain);
                obj->clearAtomColors();
                return "Coloring by chain";
            }
            // Fall through to try as named color
        }
        if (first == "ss" || first == "secondary") {
            obj->setColorScheme(ColorScheme::SecondaryStructure);
            obj->clearAtomColors();
            return "Coloring by SS";
        }
        if (first == "bfactor" || first == "b") {
            obj->setColorScheme(ColorScheme::BFactor);
            obj->clearAtomColors();
            return "Coloring by B-factor";
        }
        if (first == "plddt") {
            obj->setColorScheme(ColorScheme::PLDDT);
            obj->clearAtomColors();
            return "Coloring by pLDDT (AlphaFold confidence)";
        }
        if (first == "rainbow") {
            obj->setColorScheme(ColorScheme::Rainbow);
            obj->clearAtomColors();
            return "Coloring rainbow (N→C terminus)";
        }
        if (first == "restype" || first == "type") {
            obj->setColorScheme(ColorScheme::ResType);
            obj->clearAtomColors();
            return "Coloring by residue type (nonpolar/polar/acidic/basic)";
        }
        if (first == "heteroatom" || first == "hetero" || first == "het_color") {
            int count = applyHeteroatomColors(*obj);
            return "Colored " + std::to_string(count) + " heteroatoms by element";
        }
        if (first == "clear" || first == "reset") {
            obj->clearAtomColors();
            return "Cleared per-atom colors";
        }

        // Try as named color + optional selection:
        // :color red              → color all atoms red
        // :color red chain A      → color chain A red
        int colorPair = ColorMapper::colorByName(first);
        if (colorPair < 0)
            return "Unknown color/scheme: " + first + " | Available: " + ColorMapper::availableColors();

        if (cmd.args.size() == 1) {
            // Color all atoms
            auto sel = Selection::all(static_cast<int>(obj->atoms().size()));
            obj->setAtomColors(std::vector<int>(sel.indices().begin(), sel.indices().end()), colorPair);
            return "Colored all atoms " + first;
        }

        // Remaining args form a selection expression
        std::string expr;
        for (size_t i = 1; i < cmd.args.size(); ++i) {
            if (i > 1) expr += " ";
            expr += cmd.args[i];
        }

        auto sel = app.parseSelection(expr, *obj);
        if (sel.empty()) return "No atoms match: " + expr;
        obj->setAtomColors(std::vector<int>(sel.indices().begin(), sel.indices().end()), colorPair);
        return "Colored " + std::to_string(sel.size()) + " atoms " + first;
    }, ":color <scheme|name> [selection]", "Set coloring scheme or per-atom color");

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
            if (atoms[i].x < minX) minX = atoms[i].x; if (atoms[i].x > maxX) maxX = atoms[i].x;
            if (atoms[i].y < minY) minY = atoms[i].y; if (atoms[i].y > maxY) maxY = atoms[i].y;
            if (atoms[i].z < minZ) minZ = atoms[i].z; if (atoms[i].z > maxZ) maxZ = atoms[i].z;
        }
        float n = static_cast<float>(indices.size());
        cx /= n; cy /= n; cz /= n;
        float span = std::max({maxX - minX, maxY - minY, maxZ - minZ});
        return {cx, cy, cz, span};
    };

    // :center [selection]
    cmdRegistry_.registerCmd("center", [resolveAtoms, computeGeometry](Application& app, const ParsedCommand& cmd) -> std::string {
        auto [indices, err] = resolveAtoms(app, cmd);
        if (!err.empty()) return err;
        auto obj = app.tabs().currentTab().currentObject();
        auto [cx, cy, cz, span] = computeGeometry(*obj, indices);
        app.tabs().currentTab().camera().setCenter(cx, cy, cz);
        return "Centered on " + std::to_string(indices.size()) + " atoms";
    }, ":center [selection]", "Center view on selection");

    // :zoom [selection]
    cmdRegistry_.registerCmd("zoom", [resolveAtoms, computeGeometry](Application& app, const ParsedCommand& cmd) -> std::string {
        auto [indices, err] = resolveAtoms(app, cmd);
        if (!err.empty()) return err;
        auto obj = app.tabs().currentTab().currentObject();
        auto [cx, cy, cz, span] = computeGeometry(*obj, indices);
        app.tabs().currentTab().camera().setCenter(cx, cy, cz);
        if (span > 0.0f) app.tabs().currentTab().camera().setZoom(40.0f / span);
        return "Zoomed to " + std::to_string(indices.size()) + " atoms";
    }, ":zoom [selection]", "Center and zoom to fit selection");

    // :orient [selection] — align principal axes
    cmdRegistry_.registerCmd("orient", [resolveAtoms, computeGeometry](Application& app, const ParsedCommand& cmd) -> std::string {
        auto [indices, err] = resolveAtoms(app, cmd);
        if (!err.empty()) return err;
        auto obj = app.tabs().currentTab().currentObject();
        auto [cx, cy, cz, span] = computeGeometry(*obj, indices);
        auto& cam = app.tabs().currentTab().camera();
        cam.setCenter(cx, cy, cz);
        if (span > 0.0f) cam.setZoom(40.0f / span);

        // Compute covariance matrix of atom positions (relative to center)
        // then find principal axis via power iteration
        const auto& atoms = obj->atoms();
        double cov[3][3] = {};
        for (int i : indices) {
            double dx = atoms[i].x - cx, dy = atoms[i].y - cy, dz = atoms[i].z - cz;
            cov[0][0] += dx * dx; cov[0][1] += dx * dy; cov[0][2] += dx * dz;
            cov[1][0] += dy * dx; cov[1][1] += dy * dy; cov[1][2] += dy * dz;
            cov[2][0] += dz * dx; cov[2][1] += dz * dy; cov[2][2] += dz * dz;
        }

        // Power iteration for dominant eigenvector (longest axis)
        double v[3] = {1.0, 0.5, 0.25};
        for (int iter = 0; iter < 50; ++iter) {
            double nv[3] = {
                cov[0][0]*v[0] + cov[0][1]*v[1] + cov[0][2]*v[2],
                cov[1][0]*v[0] + cov[1][1]*v[1] + cov[1][2]*v[2],
                cov[2][0]*v[0] + cov[2][1]*v[1] + cov[2][2]*v[2],
            };
            double len = std::sqrt(nv[0]*nv[0] + nv[1]*nv[1] + nv[2]*nv[2]);
            if (len < 1e-10) break;
            v[0] = nv[0] / len; v[1] = nv[1] / len; v[2] = nv[2] / len;
        }

        // Build rotation matrix: align longest axis to screen X
        // v = principal axis (longest), compute orthogonal basis
        double ax = v[0], ay = v[1], az = v[2];
        // Second axis: cross(principal, Z) or cross(principal, Y) if parallel
        double bx, by, bz;
        if (std::abs(az) < 0.9) {
            bx = ay; by = -ax; bz = 0;
        } else {
            bx = 0; by = az; bz = -ay;
        }
        double blen = std::sqrt(bx*bx + by*by + bz*bz);
        bx /= blen; by /= blen; bz /= blen;
        // Third axis: cross(a, b)
        double tx = ay*bz - az*by, ty = az*bx - ax*bz, tz = ax*by - ay*bx;

        // Set rotation: row 0 = principal (X), row 1 = second (Y), row 2 = third (Z)
        std::array<float, 9> rot;
        rot[0] = static_cast<float>(ax); rot[1] = static_cast<float>(ay); rot[2] = static_cast<float>(az);
        rot[3] = static_cast<float>(bx); rot[4] = static_cast<float>(by); rot[5] = static_cast<float>(bz);
        rot[6] = static_cast<float>(tx); rot[7] = static_cast<float>(ty); rot[8] = static_cast<float>(tz);
        cam.setRotation(rot);

        return "Oriented " + std::to_string(indices.size()) + " atoms";
    }, ":orient [selection]", "Center, zoom, and align principal axes");

    // :set <option> [value]
    cmdRegistry_.registerCmd("set", [](Application& app, const ParsedCommand& cmd) -> std::string {
        if (cmd.args.empty()) return "Usage: :set <option> [value]";
        const auto& opt = cmd.args[0];
        if (opt == "panel") {
            app.layout().togglePanel();
            return app.layout().panelVisible() ? "Panel visible" : "Panel hidden";
        }
        if (opt == "renderer" || opt == "render") {
            if (cmd.args.size() < 2) return "Usage: :set renderer <ascii|braille|block|sixel|pixel|auto|kitty|iterm2>";
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
            else return "Unknown renderer: " + val;
            clearScreenAndRepaint();
            return "Renderer set to " + val;
        }
        if (opt == "backbone_thickness" || opt == "bt") {
            if (cmd.args.size() < 2) return "Usage: :set backbone_thickness <0.5-10>";
            float val = std::stof(cmd.args[1]);
            auto* bb = dynamic_cast<BackboneRepr*>(app.getRepr(ReprType::Backbone));
            if (bb) {
                bb->setThickness(val);
                return "Backbone thickness set to " + std::to_string(bb->thickness());
            }
            return "Backbone repr not found";
        }
        if (opt == "wireframe_thickness" || opt == "wt") {
            if (cmd.args.size() < 2) return "Usage: :set wireframe_thickness <0.5-10>";
            float val = std::stof(cmd.args[1]);
            auto* wf = dynamic_cast<WireframeRepr*>(app.getRepr(ReprType::Wireframe));
            if (wf) {
                wf->setThickness(val);
                return "Wireframe thickness set to " + std::to_string(wf->thickness());
            }
            return "Wireframe repr not found";
        }
        if (opt == "ball_radius" || opt == "br") {
            if (cmd.args.size() < 2) return "Usage: :set ball_radius <1-10>";
            int val = std::stoi(cmd.args[1]);
            auto* bs = dynamic_cast<BallStickRepr*>(app.getRepr(ReprType::BallStick));
            if (bs) {
                bs->setBallRadius(val);
                return "Ball radius set to " + std::to_string(val);
            }
            return "BallStick repr not found";
        }
        if (opt == "pan_speed" || opt == "ps") {
            if (cmd.args.size() < 2) return "Usage: :set pan_speed <1-20>";
            float val = std::stof(cmd.args[1]);
            app.tabs().currentTab().camera().setPanSpeed(val);
            return "Pan speed set to " + std::to_string(val);
        }
        if (opt == "fog") {
            if (cmd.args.size() < 2) return "Usage: :set fog <0.0-1.0> (0=off)";
            float val = std::stof(cmd.args[1]);
            app.setFogStrength(val);
            return "Fog strength set to " + std::to_string(val);
        }
        if (opt == "auto_center") {
            app.setAutoCenter(!app.autoCenter());
            return std::string("Auto-center on load: ") + (app.autoCenter() ? "on" : "off");
        }
        if (opt == "seqbar") {
            app.layout().toggleSeqBar();
            if (app.canvas()) app.canvas()->invalidate();
            app.onResize();
            return app.layout().seqBarVisible() ? "Sequence bar visible" : "Sequence bar hidden";
        }
        if (opt == "seqwrap") {
            app.layout().toggleSeqBarWrap();
            if (app.canvas()) app.canvas()->invalidate();
            app.onResize();
            return app.layout().seqBarWrap() ? "Sequence bar: wrap" : "Sequence bar: scroll";
        }
        return "Unknown option: " + opt;
    }, ":set <option> [value]", "Set option");

    // :help
    cmdRegistry_.registerCmd("help", [](Application&, const ParsedCommand&) -> std::string {
        return "Commands: :load :fetch :show :hide :color :zoom :center :orient :select :count "
               ":align :measure :angle :dihedral :export :set :objects :delete :rename :info | ? for keybindings";
    }, ":help", "Show help");

    // :delete
    cmdRegistry_.registerCmd("delete", [](Application& app, const ParsedCommand& cmd) -> std::string {
        auto& tab = app.tabs().currentTab();
        if (cmd.args.empty()) {
            auto obj = tab.currentObject();
            if (!obj) return "No object selected";
            std::string name = obj->name();
            app.store().remove(name);
            tab.removeObject(tab.selectedObjectIdx());
            return "Deleted " + name;
        }
        auto obj = app.store().get(cmd.args[0]);
        if (!obj) return "Object not found: " + cmd.args[0];
        std::string name = obj->name();
        app.store().remove(name);
        return "Deleted " + name;
    }, ":delete [name]", "Delete object");

    // :rename
    cmdRegistry_.registerCmd("rename", [](Application& app, const ParsedCommand& cmd) -> std::string {
        if (cmd.args.empty()) return "Usage: :rename <new_name> or :rename <old> <new>";
        if (cmd.args.size() < 2) {
            auto obj = app.tabs().currentTab().currentObject();
            if (!obj) return "No object selected";
            std::string oldName = obj->name();
            if (app.store().rename(oldName, cmd.args[0]))
                return "Renamed " + oldName + " -> " + cmd.args[0];
            return "Failed to rename";
        }
        if (app.store().rename(cmd.args[0], cmd.args[1]))
            return "Renamed " + cmd.args[0] + " -> " + cmd.args[1];
        return "Failed to rename";
    }, ":rename [old] <new>", "Rename object");

    // :info
    cmdRegistry_.registerCmd("info", [](Application& app, const ParsedCommand&) -> std::string {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return "No object selected";
        return obj->name() + ": " +
               std::to_string(obj->atoms().size()) + " atoms, " +
               std::to_string(obj->bonds().size()) + " bonds";
    }, ":info", "Show object info");

    // :select <expression>
    cmdRegistry_.registerCmd("select", [](Application& app, const ParsedCommand& cmd) -> std::string {
        if (cmd.args.empty()) return "Usage: :select <expr> | :select <name> = <expr> | :select clear";
        // :select clear — clear $sele
        if (cmd.args[0] == "clear") {
            auto it = app.namedSelections().find("sele");
            if (it != app.namedSelections().end()) it->second.clear();
            return "Selection cleared";
        }
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return "No object selected";

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

        if (expr.empty()) return "Empty expression";

        auto sel = app.parseSelection(expr, *obj);

        if (!name.empty()) {
            // Store as named selection
            app.namedSelections()[name] = sel;
            if (sel.empty()) return "Selection '" + name + "' is empty: " + expr;
            return "Selection '" + name + "' = " + std::to_string(sel.size()) + " atoms";
        }

        if (sel.empty()) return "Selection empty: " + expr;
        return "Selected " + std::to_string(sel.size()) + " atoms: " + expr;
    }, ":select <expr>", "Select atoms by expression");

    // :count <expression> — count atoms matching selection
    cmdRegistry_.registerCmd("count", [](Application& app, const ParsedCommand& cmd) -> std::string {
        if (cmd.args.empty()) return "Usage: :count <expression>";
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return "No object selected";

        std::string expr;
        for (size_t i = 0; i < cmd.args.size(); ++i) {
            if (i > 0) expr += " ";
            expr += cmd.args[i];
        }

        auto sel = app.parseSelection(expr, *obj);
        return std::to_string(sel.size()) + " atoms match: " + expr;
    }, ":count <expr>", "Count atoms matching expression");

    // :sele — list named selections
    cmdRegistry_.registerCmd("sele", [](Application& app, const ParsedCommand&) -> std::string {
        auto& sels = app.namedSelections();
        if (sels.empty()) return "No named selections";
        std::string result = "Selections:";
        for (const auto& [name, sel] : sels) {
            result += " " + name + "(" + std::to_string(sel.size()) + ")";
        }
        return result;
    }, ":sele", "List named selections");

    // :align <mobile> <target> — single-chain TM-align
    // Helper: parse align args with "to" separator
    // Syntax: :align <obj> [sel] to <obj> [sel]
    //   :align 1abc to 2def                      — all atoms
    //   :align 1abc chain A to 2def chain A       — chain A of each
    //   :align 1abc backbone to 2def backbone     — backbone only
    //   :align 1abc to 2def                       — simple
    // Legacy: :align 1abc 2def [shared_sel]       — if no "to" found
    auto doAlign = [](Application& app, const ParsedCommand& cmd, bool complex) -> std::string {
        if (cmd.args.size() < 2) {
            std::string name = complex ? "mmalign" : "align";
            return "Usage: :" + name + " <obj> [sel] to <obj> [sel]";
        }

        // Find "to" separator
        int toIdx = -1;
        for (int i = 0; i < static_cast<int>(cmd.args.size()); ++i) {
            if (cmd.args[i] == "to") { toIdx = i; break; }
        }

        std::string mobileName, targetName;
        std::string mobileExpr, targetExpr;

        if (toIdx > 0) {
            // "to" found: left side = mobile obj + sel, right side = target obj + sel
            mobileName = cmd.args[0];
            for (int i = 1; i < toIdx; ++i) {
                if (!mobileExpr.empty()) mobileExpr += " ";
                mobileExpr += cmd.args[i];
            }
            if (toIdx + 1 < static_cast<int>(cmd.args.size())) {
                targetName = cmd.args[toIdx + 1];
                for (int i = toIdx + 2; i < static_cast<int>(cmd.args.size()); ++i) {
                    if (!targetExpr.empty()) targetExpr += " ";
                    targetExpr += cmd.args[i];
                }
            } else {
                return "Missing target after 'to'";
            }
        } else {
            // Legacy: first two args are obj names, rest is shared selection
            mobileName = cmd.args[0];
            targetName = cmd.args[1];
            for (size_t i = 2; i < cmd.args.size(); ++i) {
                if (!mobileExpr.empty()) mobileExpr += " ";
                mobileExpr += cmd.args[i];
            }
            targetExpr = mobileExpr;
        }

        auto mobile = app.store().get(mobileName);
        auto target = app.store().get(targetName);
        if (!mobile) return "Object not found: " + mobileName;
        if (!target) return "Object not found: " + targetName;

        std::vector<int> mobileAtoms, targetAtoms;

        if (!mobileExpr.empty()) {
            auto mSel = app.parseSelection(mobileExpr, *mobile);
            mobileAtoms = std::vector<int>(mSel.indices().begin(), mSel.indices().end());
            if (mobileAtoms.empty())
                return "Mobile selection empty: " + mobileExpr;
        }
        if (!targetExpr.empty()) {
            auto tSel = app.parseSelection(targetExpr, *target);
            targetAtoms = std::vector<int>(tSel.indices().begin(), tSel.indices().end());
            if (targetAtoms.empty())
                return "Target selection empty: " + targetExpr;
        }

        auto result = complex
            ? Aligner::alignComplex(*mobile, *target, mobileAtoms, targetAtoms)
            : Aligner::align(*mobile, *target, mobileAtoms, targetAtoms);
        if (!result.success) return "Align failed: " + result.message;

        // Transform ALL atoms of mobile
        Aligner::applyTransform(*mobile, result);

        std::string mode = complex ? "MM-" : "TM-";
        return mode + "aligned " + mobileName + " → " + targetName + " | " + result.message;
    };

    // :align <obj> [sel] to <obj> [sel] — TM-align
    cmdRegistry_.registerCmd("align", [doAlign](Application& app, const ParsedCommand& cmd) -> std::string {
        return doAlign(app, cmd, false);
    }, ":align <obj> [sel] to <obj> [sel]", "Align structures (TM-align via USalign)");

    // :mmalign <obj> [sel] to <obj> [sel] — MM-align (complex/multi-chain)
    cmdRegistry_.registerCmd("mmalign", [doAlign](Application& app, const ParsedCommand& cmd) -> std::string {
        return doAlign(app, cmd, true);
    }, ":mmalign <obj> [sel] to <obj> [sel]", "Align complexes (MM-align via USalign)");

    // :super — alias for :align
    cmdRegistry_.registerCmd("super", [doAlign](Application& app, const ParsedCommand& cmd) -> std::string {
        return doAlign(app, cmd, false);
    }, ":super <obj> [sel] to <obj> [sel]", "Superpose structures (alias for align)");

    // :fetch <pdb_id> — download from RCSB PDB or AlphaFold DB
    cmdRegistry_.registerCmd("fetch", [](Application& app, const ParsedCommand& cmd) -> std::string {
        if (cmd.args.empty()) return "Usage: :fetch <pdb_id> or :fetch afdb:<uniprot_id>";

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

        // Download to /tmp
        std::string tmpPath = "/tmp/molterm_" + filename;
        std::string curlCmd = "curl -sL -o " + tmpPath + " -w '%{http_code}' '" + url + "'";
        FILE* pipe = popen(curlCmd.c_str(), "r");
        if (!pipe) return "Failed to run curl";

        char buf[64];
        std::string httpCode;
        while (fgets(buf, sizeof(buf), pipe)) httpCode += buf;
        int ret = pclose(pipe);

        if (ret != 0 || httpCode.find("200") == std::string::npos) {
            std::string src = isAF ? "AlphaFold DB" : "RCSB PDB";
            return "Failed to fetch " + id + " from " + src + " (HTTP " + httpCode + ")";
        }

        std::string result = app.loadFile(tmpPath);
        std::remove(tmpPath.c_str());
        std::string src = isAF ? "AlphaFold DB" : "RCSB PDB";
        return "Fetched " + id + " from " + src + " | " + result;
    }, ":fetch <pdb_id|afdb:uniprot_id>", "Download structure from RCSB PDB or AlphaFold DB");

    // :assembly [id|list] — generate biological assembly
    cmdRegistry_.registerCmd("assembly", [](Application& app, const ParsedCommand& cmd) -> std::string {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return "No object selected";
        std::string path = obj->sourcePath();
        if (path.empty()) return "No source file for " + obj->name();

        if (!cmd.args.empty() && cmd.args[0] == "list") {
            auto assemblies = CifLoader::listAssemblies(path);
            if (assemblies.empty()) return "No assemblies found in " + obj->name();
            std::string result = "Assemblies:";
            for (const auto& a : assemblies)
                result += " " + a.name + "(" + std::to_string(a.oligomericCount) + "-mer)";
            return result;
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
            return "Assembly " + asmId + " → " + name + ": " +
                   std::to_string(atomCount) + " atoms, " + std::to_string(bondCount) + " bonds";
        } catch (const std::exception& e) {
            return std::string("Assembly error: ") + e.what();
        }
    }, ":assembly [id|list]", "Generate biological assembly (default: assembly 1)");

    // :export <file.pml> — export PyMOL script
    cmdRegistry_.registerCmd("export", [](Application& app, const ParsedCommand& cmd) -> std::string {
        if (cmd.args.empty()) return "Usage: :export <file.pml>";
        const std::string& path = cmd.args[0];

        // Validate extension
        if (path.size() < 5 || path.substr(path.size() - 4) != ".pml")
            return "Export file must have .pml extension";

        auto& tab = app.tabs().currentTab();
        if (tab.objects().empty()) return "No objects to export";

        int vpW = app.layout().viewportWidth();
        int vpH = app.layout().viewportHeight();
        std::string result = SessionExporter::exportPML(path, tab, vpW, vpH);
        return result;
    }, ":export <file.pml>", "Export session as PyMOL script");

    // :screenshot <file.png>
    cmdRegistry_.registerCmd("screenshot", [](Application& app, const ParsedCommand& cmd) -> std::string {
        std::string path;
        if (cmd.args.empty()) {
            auto now = std::chrono::system_clock::now();
            auto t = std::chrono::system_clock::to_time_t(now);
            char fname[64];
            std::strftime(fname, sizeof(fname), "molterm_%Y%m%d_%H%M%S.png", std::localtime(&t));
            path = fname;
        } else {
            path = cmd.args[0];
        }

        // If already in pixel mode, use the existing canvas
        if (app.rendererType() == RendererType::Pixel) {
            auto* pc = dynamic_cast<PixelCanvas*>(app.canvas());
            if (pc && pc->savePNG(path))
                return "Saved " + std::to_string(pc->pixelWidth()) + "x" +
                       std::to_string(pc->pixelHeight()) + " to " + path;
            return "Failed to save " + path;
        }

        // Offscreen render: create a temporary PixelCanvas, render into it, save PNG
        auto proto = ProtocolPicker::detect();
        auto encoder = ProtocolPicker::createEncoder(proto);
        if (!encoder) {
            // Create a dummy SixelEncoder just for offscreen rendering
            encoder = ProtocolPicker::createEncoder(GraphicsProtocol::Sixel);
        }
        if (!encoder) return "Cannot create offscreen renderer";

        PixelCanvas offscreen(std::move(encoder));
        int w = app.layout().viewportWidth();
        int h = app.layout().viewportHeight();
        offscreen.resize(w, h);
        offscreen.clear();

        auto& tab = app.tabs().currentTab();
        // Re-prepare projection for the offscreen pixel coordinate space
        tab.camera().prepareProjection(offscreen.subW(), offscreen.subH(), offscreen.aspectYX());

        for (const auto& obj : tab.objects()) {
            if (!obj->visible()) continue;
            for (auto& [reprType, repr] : app.representations()) {
                if (obj->reprVisible(reprType)) {
                    repr->render(*obj, tab.camera(), offscreen);
                }
            }
        }

        if (app.fogStrength() > 0.0f)
            offscreen.applyDepthFog(app.fogStrength());

        // Restore projection for the active canvas
        auto* canvas = app.canvas();
        if (canvas)
            tab.camera().prepareProjection(canvas->subW(), canvas->subH(), canvas->aspectYX());

        if (offscreen.savePNG(path))
            return "Saved " + std::to_string(offscreen.pixelWidth()) + "x" +
                   std::to_string(offscreen.pixelHeight()) + " to " + path;
        return "Failed to save " + path;
    }, ":screenshot [file.png]", "Save viewport as PNG (works in any renderer)");

    // :preset — apply smart default representation
    cmdRegistry_.registerCmd("preset", [](Application& app, const ParsedCommand&) -> std::string {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return "No object selected";
        obj->applySmartDefaults();
        return "Applied default preset (cartoon + ballstick ligands)";
    }, ":preset", "Apply smart default representations");

    // :label [selection] — add labels for atoms matching selection
    cmdRegistry_.registerCmd("label", [](Application& app, const ParsedCommand& cmd) -> std::string {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return "No object selected";
        if (cmd.args.empty()) return "Usage: :label <selection> or :label clear";
        if (cmd.args[0] == "clear") {
            app.labelAtoms().clear();
            return "Labels cleared";
        }
        std::string expr;
        for (size_t i = 0; i < cmd.args.size(); ++i) {
            if (i > 0) expr += " ";
            expr += cmd.args[i];
        }
        auto sel = app.parseSelection(expr, *obj);
        if (sel.empty()) return "No atoms match: " + expr;
        // Add to label set (deduplicate)
        auto& labels = app.labelAtoms();
        for (int idx : sel.indices()) {
            bool found = false;
            for (int li : labels) { if (li == idx) { found = true; break; } }
            if (!found) labels.push_back(idx);
        }
        return "Labeled " + std::to_string(sel.size()) + " atoms";
    }, ":label <selection|clear>", "Show labels on viewport");

    // :unlabel — remove all labels
    cmdRegistry_.registerCmd("unlabel", [](Application& app, const ParsedCommand&) -> std::string {
        app.labelAtoms().clear();
        return "Labels cleared";
    }, ":unlabel", "Remove all labels");

    // :overlay [clear] — toggle or clear overlays (labels, measurements, selection)
    cmdRegistry_.registerCmd("overlay", [](Application& app, const ParsedCommand& cmd) -> std::string {
        if (!cmd.args.empty() && cmd.args[0] == "clear") {
            int mc = static_cast<int>(app.measurements().size());
            int lc = static_cast<int>(app.labelAtoms().size());
            app.measurements().clear();
            app.labelAtoms().clear();
            return "Cleared " + std::to_string(mc) + " measurements, " +
                   std::to_string(lc) + " labels";
        }
        app.overlayVisible_ = !app.overlayVisible_;
        return app.overlayVisible_ ? "Overlays visible" : "Overlays hidden";
    }, ":overlay [clear]", "Toggle or clear overlays");

    // :run <script.mt> — execute a command script
    cmdRegistry_.registerCmd("run", [](Application& app, const ParsedCommand& cmd) -> std::string {
        if (cmd.args.empty()) return "Usage: :run <script.mt>";
        std::ifstream file(cmd.args[0]);
        if (!file) return "Cannot open: " + cmd.args[0];
        int count = 0;
        std::string line;
        while (std::getline(file, line)) {
            size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos || line[start] == '#') continue;
            line = line.substr(start);
            if (line.empty()) continue;
            app.cmdRegistry().execute(app, line);
            ++count;
        }
        MLOG_INFO("Ran %d commands from %s", count, cmd.args[0].c_str());
        return "Ran " + std::to_string(count) + " commands from " + cmd.args[0];
    }, ":run <file>", "Execute command script");

    cmdRegistry_.registerCmd("save", [](Application& app, const ParsedCommand&) -> std::string {
        if (SessionSaver::saveSession(app))
            return "Session saved to " + SessionSaver::sessionPath();
        return "Failed to save session";
    }, ":save", "Save session to ~/.molterm/autosave.toml");

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
    cmdRegistry_.registerCmd("measure", [resolveAtomIdx, atomLabel](Application& app, const ParsedCommand& cmd) -> std::string {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return "No object selected";
        const auto& atoms = obj->atoms();
        int n = static_cast<int>(atoms.size());

        int i1, i2;
        if (cmd.args.empty()) {
            // Use pick registers pk1, pk2
            i1 = app.pickRegs_[0]; i2 = app.pickRegs_[1];
            if (i1 < 0 || i2 < 0) return "Click two atoms first (pk1, pk2), then :measure";
        } else if (cmd.args.size() >= 2) {
            i1 = resolveAtomIdx(app, cmd.args[0]);
            i2 = resolveAtomIdx(app, cmd.args[1]);
            if (i1 < 0) return "Atom not found: serial " + cmd.args[0];
            if (i2 < 0) return "Atom not found: serial " + cmd.args[1];
        } else {
            return "Usage: :measure [serial1 serial2] (or click 2 atoms first)";
        }
        if (i1 >= n || i2 >= n) return "Invalid atom index";

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
        return msg;
    }, ":measure [s1 s2]", "Distance (no args = pk1↔pk2)");

    // :angle [s1 s2 s3] — angle at vertex s2 (no args = pk1-pk2-pk3)
    cmdRegistry_.registerCmd("angle", [resolveAtomIdx, atomLabel](Application& app, const ParsedCommand& cmd) -> std::string {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return "No object selected";
        const auto& atoms = obj->atoms();
        int n = static_cast<int>(atoms.size());

        int i1, i2, i3;
        if (cmd.args.empty()) {
            i1 = app.pickRegs_[0]; i2 = app.pickRegs_[1]; i3 = app.pickRegs_[2];
            if (i1 < 0 || i2 < 0 || i3 < 0) return "Click three atoms first (pk1-pk3), then :angle";
        } else if (cmd.args.size() >= 3) {
            i1 = resolveAtomIdx(app, cmd.args[0]);
            i2 = resolveAtomIdx(app, cmd.args[1]);
            i3 = resolveAtomIdx(app, cmd.args[2]);
            if (i1 < 0) return "Atom not found: serial " + cmd.args[0];
            if (i2 < 0) return "Atom not found: serial " + cmd.args[1];
            if (i3 < 0) return "Atom not found: serial " + cmd.args[2];
        } else {
            return "Usage: :angle [s1 s2 s3] (or click 3 atoms first)";
        }
        if (i1 >= n || i2 >= n || i3 >= n) return "Invalid atom index";

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
        return msg;
    }, ":angle [s1 s2 s3]", "Angle at s2 (no args = pk1-pk2-pk3)");

    // :dihedral [s1 s2 s3 s4] — dihedral (no args = pk1-pk4)
    cmdRegistry_.registerCmd("dihedral", [resolveAtomIdx, atomLabel](Application& app, const ParsedCommand& cmd) -> std::string {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return "No object selected";
        const auto& atoms = obj->atoms();
        int n = static_cast<int>(atoms.size());

        int i1, i2, i3, i4;
        if (cmd.args.empty()) {
            i1 = app.pickRegs_[0]; i2 = app.pickRegs_[1];
            i3 = app.pickRegs_[2]; i4 = app.pickRegs_[3];
            if (i1 < 0 || i2 < 0 || i3 < 0 || i4 < 0)
                return "Click four atoms first (pk1-pk4), then :dihedral";
        } else if (cmd.args.size() >= 4) {
            i1 = resolveAtomIdx(app, cmd.args[0]);
            i2 = resolveAtomIdx(app, cmd.args[1]);
            i3 = resolveAtomIdx(app, cmd.args[2]);
            i4 = resolveAtomIdx(app, cmd.args[3]);
            if (i1 < 0) return "Atom not found: serial " + cmd.args[0];
            if (i2 < 0) return "Atom not found: serial " + cmd.args[1];
            if (i3 < 0) return "Atom not found: serial " + cmd.args[2];
            if (i4 < 0) return "Atom not found: serial " + cmd.args[3];
        } else {
            return "Usage: :dihedral [s1 s2 s3 s4] (or click 4 atoms first)";
        }
        if (i1 >= n || i2 >= n || i3 >= n || i4 >= n) return "Invalid atom index";

        float b1x = atoms[i2].x-atoms[i1].x, b1y = atoms[i2].y-atoms[i1].y, b1z = atoms[i2].z-atoms[i1].z;
        float b2x = atoms[i3].x-atoms[i2].x, b2y = atoms[i3].y-atoms[i2].y, b2z = atoms[i3].z-atoms[i2].z;
        float b3x = atoms[i4].x-atoms[i3].x, b3y = atoms[i4].y-atoms[i3].y, b3z = atoms[i4].z-atoms[i3].z;
        float n1x = b1y*b2z-b1z*b2y, n1y = b1z*b2x-b1x*b2z, n1z = b1x*b2y-b1y*b2x;
        float n2x = b2y*b3z-b2z*b3y, n2y = b2z*b3x-b2x*b3z, n2z = b2x*b3y-b2y*b3x;
        float b2len = std::sqrt(b2x*b2x + b2y*b2y + b2z*b2z);
        if (b2len < 1e-8f) return "Degenerate dihedral";
        float ub2x = b2x/b2len, ub2y = b2y/b2len, ub2z = b2z/b2len;
        float mx = n1y*ub2z-n1z*ub2y, my = n1z*ub2x-n1x*ub2z, mz = n1x*ub2y-n1y*ub2x;
        float xv = n1x*n2x+n1y*n2y+n1z*n2z;
        float yv = mx*n2x+my*n2y+mz*n2z;
        float deg = std::atan2(yv, xv) * 180.0f / static_cast<float>(M_PI);

        char buf[128];
        std::snprintf(buf, sizeof(buf), "%.1f", deg);
        std::string shortLabel = std::string(buf) + "°";
        std::string msg = "Dihedral " + atomLabel(atoms[i1]) + " — " +
            atomLabel(atoms[i2]) + " — " + atomLabel(atoms[i3]) + " — " +
            atomLabel(atoms[i4]) + " = " + buf + " deg";
        app.measurements().push_back({{i1, i2, i3, i4}, shortLabel});
        MLOG_INFO("%s", msg.c_str());
        return msg;
    }, ":dihedral [s1 s2 s3 s4]", "Dihedral (no args = pk1-pk4)");
}

} // namespace molterm
