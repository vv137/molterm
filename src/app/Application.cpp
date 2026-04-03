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

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <signal.h>

namespace molterm {

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
Application::~Application() = default;

void Application::init(int argc, char* argv[]) {
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

    // Load files from command line args
    for (int i = 1; i < argc; ++i) {
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
    running_ = false;
}

std::string Application::loadFile(const std::string& path) {
    try {
        auto obj = CifLoader::loadAuto(path);
        std::string name = obj->name();
        int atomCount = static_cast<int>(obj->atoms().size());
        int bondCount = static_cast<int>(obj->bonds().size());

        auto ptr = store_.add(std::move(obj));
        tabMgr_.currentTab().addObject(ptr);
        if (autoCenter_) tabMgr_.currentTab().centerView();
        needsRedraw_ = true;

        return "Loaded " + name + ": " +
               std::to_string(atomCount) + " atoms, " +
               std::to_string(bondCount) + " bonds";
    } catch (const std::exception& e) {
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
    // Left click + drag for rotation is hard without stateful tracking,
    // but we can handle single clicks in the tab bar or object panel
    else if (event.bstate & BUTTON1_CLICKED) {
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
        // Click in viewport → pick atom
        else if (event.y >= 1 && event.y < 1 + layout_.viewportHeight()) {
            int vpX = event.x;
            int vpY = event.y - 1;  // offset for tab bar row
            buildProjCache();
            int atomIdx = findNearestAtom(vpX, vpY);
            if (atomIdx >= 0) {
                auto obj = tabMgr_.currentTab().currentObject();
                if (obj) {
                    pickedAtomIdx_ = atomIdx;
                    cmdLine_.setMessage("PICK: " + atomInfoString(*obj, atomIdx));
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
        case Action::RotateLeft:
            if (inspectMode_) { cursorX_ = std::max(0, cursorX_ - 1); goto inspectUpdate; }
            cam.rotateY(-rs);  break;
        case Action::RotateRight:
            if (inspectMode_) { cursorX_ = std::min(layout_.viewportWidth() - 1, cursorX_ + 1); goto inspectUpdate; }
            cam.rotateY(rs);   break;
        case Action::RotateUp:
            if (inspectMode_) { cursorY_ = std::max(0, cursorY_ - 1); goto inspectUpdate; }
            cam.rotateX(-rs);  break;
        case Action::RotateDown:
            if (inspectMode_) { cursorY_ = std::min(layout_.viewportHeight() - 1, cursorY_ + 1); goto inspectUpdate; }
            cam.rotateX(rs);   break;
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
            if (obj) obj->setColorScheme(ColorScheme::Element);
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
            inspectMode_ = false;
            pickedAtomIdx_ = -1;
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
                                           "bfactor", "b", "plddt", "rainbow", "clear",
                                           "red", "green", "blue", "yellow", "magenta",
                                           "cyan", "white", "orange", "pink", "lime",
                                           "teal", "purple", "salmon", "slate", "gray"}) {
                        std::string cs(c);
                        if (cs.find(partial) == 0) candidates.push_back(cs);
                    }
                } else if (cmdName == "set") {
                    for (const auto& o : {"renderer", "backbone_thickness", "bt",
                                           "wireframe_thickness", "wt", "ball_radius", "br",
                                           "pan_speed", "ps", "fog", "auto_center", "panel"}) {
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

        // Inspect mode
        case Action::Inspect: {
            inspectMode_ = !inspectMode_;
            if (inspectMode_) {
                // Initialize cursor to center of viewport
                cursorX_ = layout_.viewportWidth() / 2;
                cursorY_ = layout_.viewportHeight() / 2;
                buildProjCache();
                pickedAtomIdx_ = findNearestAtom(cursorX_, cursorY_);
                auto obj = tab.currentObject();
                if (obj && pickedAtomIdx_ >= 0)
                    cmdLine_.setMessage("INSPECT: " + atomInfoString(*obj, pickedAtomIdx_));
                else
                    cmdLine_.setMessage("INSPECT mode (hjkl to move cursor, i/ESC to exit)");
            } else {
                pickedAtomIdx_ = -1;
                cmdLine_.clearMessage();
            }
            break;
        }

        case Action::ShowHelp:
            cmdLine_.setMessage("i inspect | / search | :select <expr> | :set bt|wt|br|renderer|panel | :help");
            break;

        inspectUpdate: {
            // Update inspect cursor position → find nearest atom
            buildProjCache();
            pickedAtomIdx_ = findNearestAtom(cursorX_, cursorY_);
            auto obj2 = tab.currentObject();
            if (obj2 && pickedAtomIdx_ >= 0)
                cmdLine_.setMessage("INSPECT: " + atomInfoString(*obj2, pickedAtomIdx_));
            else
                cmdLine_.setMessage("INSPECT: no atom nearby");
            break;
        }

        default:
            needsRedraw_ = false;
            break;
    }
}

void Application::handleCommandInput(int key) {
    if (key == KEY_BACKSPACE || key == 127 || key == 8) {
        cmdLine_.backspace();
    } else {
        cmdLine_.insertChar(key);
    }
    needsRedraw_ = true;
}

void Application::handleSearchInput(int key) {
    if (key == KEY_BACKSPACE || key == 127 || key == 8) {
        cmdLine_.backspace();
    } else {
        cmdLine_.insertChar(key);
    }
    needsRedraw_ = true;
}

void Application::renderFrame() {
    // Tab bar
    tabBar_.render(layout_.tabBar(), tabMgr_.tabNames(), tabMgr_.currentIndex());

    // Viewport — render to canvas but defer pixel flush
    renderViewport();

    // Object panel
    if (layout_.panelVisible()) {
        auto& tab = tabMgr_.currentTab();
        objectPanel_.render(layout_.objectPanel(), tab.objects(),
                           tab.selectedObjectIdx());
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
    for (const auto& obj : tab.objects()) {
        if (!obj->visible()) continue;
        // Render each active representation
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

    // Show history hint overlay when command line is active and empty
    cmdLine_.renderHistoryHint(win);

    // Draw inspect cursor crosshair
    if (inspectMode_ && cursorX_ >= 0 && cursorY_ >= 0) {
        // Horizontal line
        for (int x = std::max(0, cursorX_ - 2); x <= std::min(w - 1, cursorX_ + 2); ++x) {
            if (x != cursorX_)
                win.addCharColored(cursorY_, x, '-', kColorYellow);
        }
        // Vertical line
        for (int y = std::max(0, cursorY_ - 1); y <= std::min(h - 1, cursorY_ + 1); ++y) {
            if (y != cursorY_)
                win.addCharColored(y, cursorX_, '|', kColorYellow);
        }
        // Center
        win.addCharColored(cursorY_, cursorX_, '+', kColorYellow);
    }

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
        if (!obj->visible()) objInfo += " (hidden)";
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
    // Flush any in-flight Sixel output before reinitializing
    fflush(stdout);
    // Clear screen to remove stale Sixel images at old coordinates
    fprintf(stdout, "\033[2J");
    fflush(stdout);

    endwin();
    refresh();
    layout_.resize(screen_.height(), screen_.width());
    needsRedraw_ = true;
}

void Application::buildProjCache() {
    projCache_.clear();
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
            projCache_.push_back({i, static_cast<int>(fsx), static_cast<int>(fsy), depth});
        }
    }
}

int Application::findNearestAtom(int termX, int termY) const {
    // Convert terminal coords to sub-pixel for matching
    int scaleX = canvas_ ? canvas_->scaleX() : 1;
    int scaleY = canvas_ ? canvas_->scaleY() : 1;
    int subX = termX * scaleX + scaleX / 2;
    int subY = termY * scaleY + scaleY / 2;

    int bestIdx = -1;
    float bestScore = std::numeric_limits<float>::max();

    for (const auto& pa : projCache_) {
        float dx = static_cast<float>(pa.sx - subX);
        float dy = static_cast<float>(pa.sy - subY);
        // Weight depth slightly to prefer front atoms when close
        float screenDist2 = dx * dx + dy * dy;
        float score = screenDist2 + pa.depth * 0.5f;

        if (score < bestScore) {
            bestScore = score;
            bestIdx = pa.idx;
        }
    }

    // Max pick range: 10 terminal cells worth of sub-pixels
    float maxRange = static_cast<float>(10 * std::max(scaleX, scaleY));
    if (bestScore > maxRange * maxRange) return -1;
    return bestIdx;
}

std::string Application::atomInfoString(const MolObject& mol, int atomIdx) const {
    if (atomIdx < 0 || atomIdx >= static_cast<int>(mol.atoms().size()))
        return "";
    const auto& a = mol.atoms()[atomIdx];
    char buf[256];
    snprintf(buf, sizeof(buf),
        "%s/%s %d%c/%s (%s) B=%.1f occ=%.2f [%.2f, %.2f, %.2f]",
        a.chainId.c_str(), a.resName.c_str(), a.resSeq,
        a.insCode == ' ' ? '\0' : a.insCode,
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

    // :show <repr>
    cmdRegistry_.registerCmd("show", [](Application& app, const ParsedCommand& cmd) -> std::string {
        if (cmd.args.empty()) return "Usage: :show <wireframe|ballstick|backbone>";
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return "No object selected";
        const auto& repr = cmd.args[0];
        if (repr == "wireframe" || repr == "wire" || repr == "lines")
            obj->showRepr(ReprType::Wireframe);
        else if (repr == "ballstick" || repr == "sticks" || repr == "bs")
            obj->showRepr(ReprType::BallStick);
        else if (repr == "spacefill" || repr == "spheres" || repr == "cpk")
            obj->showRepr(ReprType::Spacefill);
        else if (repr == "cartoon" || repr == "tube")
            obj->showRepr(ReprType::Cartoon);
        else if (repr == "ribbon")
            obj->showRepr(ReprType::Ribbon);
        else if (repr == "backbone" || repr == "trace" || repr == "ca")
            obj->showRepr(ReprType::Backbone);
        else return "Unknown representation: " + repr;
        return "Showing " + repr;
    }, ":show <repr>", "Show representation");

    // :hide <repr>
    cmdRegistry_.registerCmd("hide", [](Application& app, const ParsedCommand& cmd) -> std::string {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return "No object selected";
        if (cmd.args.empty() || cmd.args[0] == "all") {
            obj->hideAllRepr();
            return "Hidden all representations";
        }
        const auto& repr = cmd.args[0];
        if (repr == "wireframe" || repr == "wire" || repr == "lines")
            obj->hideRepr(ReprType::Wireframe);
        else if (repr == "ballstick" || repr == "sticks" || repr == "bs")
            obj->hideRepr(ReprType::BallStick);
        else if (repr == "spacefill" || repr == "spheres" || repr == "cpk")
            obj->hideRepr(ReprType::Spacefill);
        else if (repr == "cartoon" || repr == "tube")
            obj->hideRepr(ReprType::Cartoon);
        else if (repr == "ribbon")
            obj->hideRepr(ReprType::Ribbon);
        else if (repr == "backbone" || repr == "trace" || repr == "ca")
            obj->hideRepr(ReprType::Backbone);
        else return "Unknown representation: " + repr;
        return "Hidden " + repr;
    }, ":hide <repr|all>", "Hide representation");

    // :color <scheme>
    cmdRegistry_.registerCmd("color", [](Application& app, const ParsedCommand& cmd) -> std::string {
        if (cmd.args.empty())
            return "Usage: :color <scheme> or :color <name> <selection> | Colors: " + ColorMapper::availableColors();
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return "No object selected";

        const auto& first = cmd.args[0];

        // Check if first arg is a color scheme name
        if (first == "element" || first == "cpk") {
            obj->setColorScheme(ColorScheme::Element);
            obj->clearAtomColors();
            return "Coloring by element";
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
        return "Unknown option: " + opt;
    }, ":set <option> [value]", "Set option");

    // :help
    cmdRegistry_.registerCmd("help", [](Application&, const ParsedCommand&) -> std::string {
        return "Commands: :load :q :show :hide :color :zoom :tabnew :tabclose :objects :delete :rename :info | :set renderer|bt|wt|br|panel";
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
        if (cmd.args.empty()) return "Usage: :select <expr> or :select <name> = <expr>";
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
        if (app.rendererType() != RendererType::Pixel)
            return "Screenshot requires pixel renderer (:set renderer pixel)";
        auto* pc = dynamic_cast<PixelCanvas*>(app.canvas());
        if (!pc) return "No pixel canvas";
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
        if (pc->savePNG(path))
            return "Saved " + std::to_string(pc->pixelWidth()) + "x" +
                   std::to_string(pc->pixelHeight()) + " to " + path;
        return "Failed to save " + path;
    }, ":screenshot [file.png]", "Save viewport as PNG (pixel renderer only)");
}

} // namespace molterm
