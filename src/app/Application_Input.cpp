#include "molterm/app/Application.h"
#include "molterm/app/ScriptRunner.h"

#include "molterm/cmd/CommandHelpers.h"
#include "molterm/cmd/CommandParser.h"
#include "molterm/input/Action.h"
#include "molterm/input/InputHandler.h"
#include "molterm/render/AsciiCanvas.h"
#include "molterm/render/BrailleCanvas.h"
#include "molterm/render/BlockCanvas.h"
#include "molterm/render/ColorMapper.h"
#include "molterm/render/PixelCanvas.h"
#include "molterm/core/MolObject.h"
#include "molterm/core/Selection.h"
#include "molterm/tui/Screen.h"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

// Input dispatch: keyboard actions, mouse picking, line/command/search entry,
// incremental search, and macro record/playback. Split out of Application.cpp
// (god-class decomposition). Same Application class; these are its method
// definitions in their own TU.
namespace molterm {

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
        clearViewFit();   // manual zoom overrides any fit intent (issue #98)
        layout_.markDirty(C::Viewport);
        layout_.markDirty(C::StatusBar);
        return;
    }
#ifdef BUTTON5_PRESSED
    if (event.bstate & BUTTON5_PRESSED) {
        cam.zoomBy(1.0f / 1.15f);
        clearViewFit();
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
                auto& sele = namedSelections_[kSele];
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
                auto& sele = namedSelections_[kSele];
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
                auto& sele = namedSelections_[kSele];
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
        // Manual pan/zoom/reset overrides any active :focus/:zoom/:orient
        // fit, so drop the intent (else the next :screenshot would re-fit
        // and clobber the user's framing). Issue #98.
        case Action::PanLeft:     cam.pan(-cam.panSpeed(), 0); clearViewFit(); dirty({C::Viewport, C::StatusBar}); break;
        case Action::PanRight:    cam.pan(cam.panSpeed(), 0);  clearViewFit(); dirty({C::Viewport, C::StatusBar}); break;
        case Action::PanUp:       cam.pan(0, -cam.panSpeed()); clearViewFit(); dirty({C::Viewport, C::StatusBar}); break;
        case Action::PanDown:     cam.pan(0, cam.panSpeed());  clearViewFit(); dirty({C::Viewport, C::StatusBar}); break;
        case Action::ZoomIn:      cam.zoomBy(1.2f); clearViewFit(); dirty({C::Viewport, C::StatusBar}); break;
        case Action::ZoomOut:     cam.zoomBy(1.0f/1.2f); clearViewFit(); dirty({C::Viewport, C::StatusBar}); break;
        case Action::ResetView:   cam.reset(); tab.centerView(); clearViewFit(); dirty({C::Viewport, C::StatusBar}); break;
        case Action::CenterSelection: tab.centerView(); dirty({C::Viewport, C::StatusBar}); break;
        case Action::Redraw:
            clearScreenAndRepaint();
            layout_.markAllDirty(); needsRedraw_ = true;
            // clearScreenAndRepaint() wiped the terminal, but the canvas frame-diff
            // cache still holds the pre-clear frame — invalidate it so the next render
            // re-transmits the viewport instead of skipping as "unchanged" (black screen).
            if (canvas_) canvas_->invalidate();
            framesToSkip_ = 0;
            break;

        // Objects — viewport + panels + seqbar + status
        case Action::NextObject:
            tab.selectNextObject();
            onCurrentObjectChanged();
            dirty({C::Viewport, C::ObjectPanel, C::SeqBar, C::StatusBar});
            break;
        case Action::PrevObject:
            tab.selectPrevObject();
            onCurrentObjectChanged();
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
        // Repr-toggle hotkeys honor :set scope, so a single keystroke shows
        // / hides the same repr across every loaded object after a superpose.
        case Action::ShowWireframe: { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->showRepr(ReprType::Wireframe); return true; }); dirty({C::Viewport}); break; }
        case Action::ShowBallStick: { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->showRepr(ReprType::BallStick); return true; }); dirty({C::Viewport}); break; }
        case Action::ShowSpacefill: { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->showRepr(ReprType::Spacefill); return true; }); dirty({C::Viewport}); break; }
        case Action::ShowCartoon:   { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->showRepr(ReprType::Cartoon);   return true; }); dirty({C::Viewport}); break; }
        case Action::ShowRibbon:    { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->showRepr(ReprType::Ribbon);    return true; }); dirty({C::Viewport}); break; }
        case Action::ShowBackbone:  { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->showRepr(ReprType::Backbone);  return true; }); dirty({C::Viewport}); break; }
        case Action::HideWireframe: { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->hideRepr(ReprType::Wireframe); return true; }); dirty({C::Viewport}); break; }
        case Action::HideBallStick: { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->hideRepr(ReprType::BallStick); return true; }); dirty({C::Viewport}); break; }
        case Action::HideSpacefill: { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->hideRepr(ReprType::Spacefill); return true; }); dirty({C::Viewport}); break; }
        case Action::HideCartoon:   { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->hideRepr(ReprType::Cartoon);   return true; }); dirty({C::Viewport}); break; }
        case Action::HideRibbon:    { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->hideRepr(ReprType::Ribbon);    return true; }); dirty({C::Viewport}); break; }
        case Action::HideBackbone:  { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->hideRepr(ReprType::Backbone);  return true; }); dirty({C::Viewport}); break; }
        case Action::HideAll:       { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->hideAllRepr();                 return true; }); dirty({C::Viewport}); break; }

        // Coloring — viewport + seqbar
        case Action::ColorByElement: { auto obj = tab.currentObject(); if (obj) applyHeteroatomColors(*obj); dirty({C::Viewport, C::SeqBar}); break; }
        case Action::ColorByChain:   { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->setColorScheme(ColorScheme::Chain);              return true; }); dirty({C::Viewport, C::SeqBar}); break; }
        case Action::ColorBySS:      { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->setColorScheme(ColorScheme::SecondaryStructure); return true; }); dirty({C::Viewport, C::SeqBar}); break; }
        case Action::ColorByBFactor: { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->setColorScheme(ColorScheme::BFactor);            return true; }); dirty({C::Viewport, C::SeqBar}); break; }

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
            transcriptHintScroll_ = 0;   // start the live transcript at the bottom
            dirty({C::CommandLine, C::StatusBar, C::Viewport});
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
                    std::to_string(namedSelections_[kSele].size()));
            }
            // Viewport too: closing the command line must clear the dimmed
            // transcript hint drawn over the viewport bottom.
            dirty({C::CommandLine, C::StatusBar, C::Viewport});
            break;

        // Command mode actions — commands can affect any component
        case Action::ExecuteCommand: {
            std::string input = cmdLine_.input();
            cmdLine_.pushHistory(input);
            cmdLine_.deactivate();
            inputHandler_->setMode(Mode::Normal);
            // Route an interactively typed/pasted line through the same
            // dispatcher scripts use, so it gets identical handling: ';'-
            // separated commands, ${var} expansion, '#' comments, and
            // user-defined :def functions. (Pasting a README block like
            // `:setenv A ; :setenv B` previously ran as one mangled command.)
            ScriptRunResult sr;
            script_->runLine(input, sr);
            std::string msg = !sr.firstFail.empty() ? sr.firstFail : sr.lastMsg;
            if (!msg.empty()) cmdLine_.setMessage(msg);
            // Keep an input/output transcript for the :messages overlay.
            recordTranscript(input, msg);
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

                // Selection-aware: if the immediately-preceding token is the
                // selector keyword `obj`, suggest loaded object names —
                // covers `:color red, obj <Tab>`, `:show cartoon obj <Tab>`,
                // `:select foo = obj <Tab>`, etc., regardless of cmdName.
                auto prevTokenLower = [](const std::string& s) -> std::string {
                    int i = static_cast<int>(s.size()) - 1;
                    while (i >= 0 && (s[i] == ' ' || s[i] == '\t')) --i;
                    int end = i + 1;
                    while (i >= 0 && s[i] != ' ' && s[i] != '\t' && s[i] != ',') --i;
                    std::string tok = s.substr(i + 1, end - i - 1);
                    std::transform(tok.begin(), tok.end(), tok.begin(), ::tolower);
                    return tok;
                };
                std::string beforePartial =
                    (input.rfind(' ') != std::string::npos)
                        ? input.substr(0, input.rfind(' '))
                        : "";
                bool selectorObjContext =
                    prevTokenLower(beforePartial) == "obj";

                if (selectorObjContext) {
                    for (const auto& n : store_.names()) {
                        if (n.find(partial) == 0) candidates.push_back(n);
                    }
                } else if (cmdName == "load" || cmdName == "e" || cmdName == "export" ||
                           cmdName == "loadalign" || cmdName == "run" ||
                           cmdName == "screenshot") {
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
                           cmdName == "object" ||
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
                                           "backbone", "trace", "surface", "all"}) {
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
                    // Single source of truth: kSetOptionsLong (iterated by
                    // `:set` listing too) + kSetOptionsShort (aliases-only).
                    auto offer = [&](const char* o) {
                        std::string os(o);
                        if (os.find(partial) == 0) candidates.push_back(std::move(os));
                    };
                    for (const char* o : kSetOptionsLong)  offer(o);
                    for (const char* o : kSetOptionsShort) offer(o);
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
                auto it = namedSelections_.find(kSele);
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
        case Action::ColorByPLDDT:   { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->setColorScheme(ColorScheme::PLDDT);   return true; }); dirty({C::Viewport, C::SeqBar}); break; }
        case Action::ColorByRainbow: { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->setColorScheme(ColorScheme::Rainbow); return true; }); dirty({C::Viewport, C::SeqBar}); break; }
        case Action::ColorByResType: { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->setColorScheme(ColorScheme::ResType); return true; }); dirty({C::Viewport, C::SeqBar}); break; }
        case Action::ColorBySASA:    { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->setColorScheme(ColorScheme::SASA);    return true; }); dirty({C::Viewport, C::SeqBar}); break; }

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
                    std::to_string(namedSelections_[kSele].size()));
            else
                cmdLine_.setMessage("Inspect mode");
            dirty({C::CommandLine, C::StatusBar});
            break;
        }

        case Action::ClearSelection: {
            auto [hadSele, hadPk] = clearVisualSelection();
            if (hadSele || hadPk) {
                cmdLine_.setMessage("Cleared $sele (" + std::to_string(hadSele) +
                                    " atoms) and pk1-pk4");
            } else {
                cmdLine_.setMessage("Selection already empty");
            }
            dirty({C::Viewport, C::StatusBar, C::CommandLine});
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
    // PgUp/PgDn scroll the live transcript shown above the command line
    // (they're meaningless for single-line editing otherwise). The renderer
    // clamps the offset to the valid range and feeds it back, so we just step.
    if (key == KEY_PPAGE || key == KEY_NPAGE) {
        transcriptHintScroll_ += (key == KEY_PPAGE ? 3 : -3);
        layout_.markDirty(Layout::Component::Viewport);
        layout_.markDirty(Layout::Component::CommandLine);
        needsRedraw_ = true;
        return;
    }
    handleLineEdit(key);
}

void Application::handleSearchInput(int key) {
    handleLineEdit(key);
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

} // namespace molterm
