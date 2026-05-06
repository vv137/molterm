#include "molterm/input/KeymapManager.h"
#include "molterm/config/ConfigParser.h"

#include <ncurses.h>

namespace molterm {

KeymapManager::KeymapManager() {
    loadDefaults();
}

void KeymapManager::loadDefaults() {
    bindNormalDefaults();
    bindCommandDefaults();
    bindVisualDefaults();
    bindSearchDefaults();
}

void KeymapManager::loadFromFile() {
    auto keymapEntries = ConfigParser::loadKeymap();
    if (keymapEntries.empty()) return;

    for (const auto& [modeName, entries] : keymapEntries) {
        Mode mode = modeFromName(modeName);
        for (const auto& entry : entries) {
            Action action = actionFromName(entry.actionName);
            if (action != Action::None && !entry.keys.empty()) {
                keymap_.bind(mode, entry.keys, action);
            }
        }
    }
}

Mode KeymapManager::modeFromName(const std::string& name) {
    if (name == "normal")  return Mode::Normal;
    if (name == "command") return Mode::Command;
    if (name == "visual")  return Mode::Visual;
    if (name == "search")  return Mode::Search;
    return Mode::Normal;
}

void KeymapManager::bindNormalDefaults() {
    auto& km = keymap_;

    // Navigation
    km.bind(Mode::Normal, {'h'},       Action::RotateLeft,  "Rotate left");
    km.bind(Mode::Normal, {'l'},       Action::RotateRight, "Rotate right");
    km.bind(Mode::Normal, {'k'},       Action::RotateUp,    "Rotate up");
    km.bind(Mode::Normal, {'j'},       Action::RotateDown,  "Rotate down");
    km.bind(Mode::Normal, {KEY_LEFT},  Action::RotateLeft,  "Rotate left");
    km.bind(Mode::Normal, {KEY_RIGHT}, Action::RotateRight, "Rotate right");
    km.bind(Mode::Normal, {KEY_UP},    Action::RotateUp,    "Rotate up");
    km.bind(Mode::Normal, {KEY_DOWN},  Action::RotateDown,  "Rotate down");
    km.bind(Mode::Normal, {'W'},       Action::PanUp,       "Pan up");
    km.bind(Mode::Normal, {'A'},       Action::PanLeft,     "Pan left");
    km.bind(Mode::Normal, {'S'},       Action::PanDown,     "Pan down");
    km.bind(Mode::Normal, {'D'},       Action::PanRight,    "Pan right");
    km.bind(Mode::Normal, {'<'},       Action::RotateCCW,   "Rotate CCW (Z-axis)");
    km.bind(Mode::Normal, {'>'},       Action::RotateCW,    "Rotate CW (Z-axis)");
    km.bind(Mode::Normal, {'+'},       Action::ZoomIn,      "Zoom in");
    km.bind(Mode::Normal, {'='},       Action::ZoomIn,      "Zoom in");
    km.bind(Mode::Normal, {'-'},       Action::ZoomOut,     "Zoom out");
    km.bind(Mode::Normal, {'0'},       Action::ResetView,   "Reset view");
    km.bind(Mode::Normal, {12},        Action::Redraw,      "Redraw (Ctrl+L)");  // Ctrl+L

    // Objects
    km.bind(Mode::Normal, {'\t'},      Action::NextObject,    "Next object");
    km.bind(Mode::Normal, {KEY_BTAB},  Action::PrevObject,    "Prev object");
    km.bind(Mode::Normal, {' '},       Action::ToggleVisible, "Toggle visible");
    km.bind(Mode::Normal, {'o'},       Action::TogglePanel,   "Toggle panel");

    // Representations (s + key)
    km.bind(Mode::Normal, {'s', 'w'},  Action::ShowWireframe, "Show wireframe");
    km.bind(Mode::Normal, {'s', 'b'},  Action::ShowBallStick, "Show ball-and-stick");
    km.bind(Mode::Normal, {'s', 's'},  Action::ShowSpacefill, "Show spacefill");
    km.bind(Mode::Normal, {'s', 'c'},  Action::ShowCartoon,   "Show cartoon");
    km.bind(Mode::Normal, {'s', 'r'},  Action::ShowRibbon,    "Show ribbon");
    km.bind(Mode::Normal, {'s', 'k'},  Action::ShowBackbone,  "Show backbone");

    // Hide
    km.bind(Mode::Normal, {'x', 'w'},  Action::HideWireframe, "Hide wireframe");
    km.bind(Mode::Normal, {'x', 'b'},  Action::HideBallStick, "Hide ball-and-stick");
    km.bind(Mode::Normal, {'x', 's'},  Action::HideSpacefill, "Hide spacefill");
    km.bind(Mode::Normal, {'x', 'c'},  Action::HideCartoon,   "Hide cartoon");
    km.bind(Mode::Normal, {'x', 'r'},  Action::HideRibbon,    "Hide ribbon");
    km.bind(Mode::Normal, {'x', 'k'},  Action::HideBackbone,  "Hide backbone");
    km.bind(Mode::Normal, {'x', 'a'},  Action::HideAll,       "Hide all");
    km.bind(Mode::Normal, {'s', 'o'},  Action::ShowOverlay,   "Show overlays");
    km.bind(Mode::Normal, {'x', 'o'},  Action::HideOverlay,   "Hide overlays");

    // Coloring (c + key)
    km.bind(Mode::Normal, {'c', 'e'},  Action::ColorByElement, "Color by element");
    km.bind(Mode::Normal, {'c', 'c'},  Action::ColorByChain,   "Color by chain");
    km.bind(Mode::Normal, {'c', 's'},  Action::ColorBySS,      "Color by SS");
    km.bind(Mode::Normal, {'c', 'b'},  Action::ColorByBFactor, "Color by B-factor");
    km.bind(Mode::Normal, {'c', 'p'},  Action::ColorByPLDDT,   "Color by pLDDT");
    km.bind(Mode::Normal, {'c', 'r'},  Action::ColorByRainbow, "Color rainbow (N→C)");
    km.bind(Mode::Normal, {'c', 't'},  Action::ColorByResType, "Color by residue type");

    // Tabs
    km.bind(Mode::Normal, {'g', 't'},  Action::NextTab,     "Next tab");
    km.bind(Mode::Normal, {'g', 'T'},  Action::PrevTab,     "Prev tab");
    km.bind(Mode::Normal, {20},        Action::NewTab,      "New tab (Ctrl+T)");
    km.bind(Mode::Normal, {23},        Action::CloseTab,    "Close tab (Ctrl+W)");
    km.bind(Mode::Normal, {'m', 't'},  Action::MoveToTab,   "Move to tab");
    km.bind(Mode::Normal, {'d', 't'},  Action::CopyToTab,   "Copy to tab");

    // Mode transitions
    km.bind(Mode::Normal, {':'},       Action::EnterCommand, "Command mode");
    km.bind(Mode::Normal, {'v'},       Action::EnterVisual,  "Visual mode");
    km.bind(Mode::Normal, {'/'},       Action::EnterSearch,  "Search mode");

    // Quick actions
    km.bind(Mode::Normal, {'n'},       Action::SearchNext,  "Next search result");
    km.bind(Mode::Normal, {'N'},       Action::SearchPrev,  "Prev search result");
    km.bind(Mode::Normal, {'i'},       Action::Inspect,     "Inspect info");
    km.bind(Mode::Normal, {'I'},       Action::CycleInspectLevel, "Cycle inspect level");
    km.bind(Mode::Normal, {'g', 'd'},  Action::ApplyPreset,        "Apply default preset");
    km.bind(Mode::Normal, {'g', 's'},  Action::EnterSelectAtom,    "Select atoms (click)");
    km.bind(Mode::Normal, {'g', 'S'},  Action::EnterSelectResidue, "Select residues (click)");
    km.bind(Mode::Normal, {'g', 'c'},  Action::EnterSelectChain,   "Select chains (click)");
    km.bind(Mode::Normal, {'g', 'f'},  Action::EnterFocusPickMode, "Focus on click (Mol*-style)");
    km.bind(Mode::Normal, {'g', 'x'},  Action::ClearSelection,     "Clear $sele + pk1-pk4");
    km.bind(Mode::Normal, {27},        Action::ExitToNormal, "Exit focus / cancel pickmode");  // ESC
    km.bind(Mode::Normal, {3},         Action::ExitToNormal, "Exit (Ctrl+C)");
    // Sequence bar: `b` toggles visibility (always wrap / all sequences).
    km.bind(Mode::Normal, {'b'},       Action::ToggleSeqBar,     "Toggle sequence bar");
    km.bind(Mode::Normal, {'}'},       Action::SeqBarNextChain, "Seqbar next chain");
    km.bind(Mode::Normal, {'{'},       Action::SeqBarPrevChain, "Seqbar prev chain");
    km.bind(Mode::Normal, {'?'},       Action::ShowHelp,    "Show help");
    km.bind(Mode::Normal, {'u'},       Action::Undo,        "Undo");
    km.bind(Mode::Normal, {18},        Action::Redo,        "Redo (Ctrl+R)");
    km.bind(Mode::Normal, {'.'},       Action::RepeatLast,  "Repeat last");
    km.bind(Mode::Normal, {'d', 'd'},  Action::DeleteObject, "Delete object");
    km.bind(Mode::Normal, {'y', 'y'},  Action::YankObject,   "Yank object");
    km.bind(Mode::Normal, {'p'},       Action::PasteObject,  "Paste object");
    km.bind(Mode::Normal, {'r'},       Action::RenameObject, "Rename object");

    // Renderer toggle + screenshot
    km.bind(Mode::Normal, {'m'},       Action::TogglePixelRenderer, "Toggle braille/pixel");
    km.bind(Mode::Normal, {'P'},        Action::Screenshot,          "Screenshot (PNG)");

    // Multi-state cycling
    km.bind(Mode::Normal, {']'},       Action::NextState,   "Next state");
    km.bind(Mode::Normal, {'['},       Action::PrevState,   "Prev state");

    // Analysis
    km.bind(Mode::Normal, {'I'},       Action::ToggleInterface,  "Toggle interface overlay");
    km.bind(Mode::Normal, {'F'},       Action::FocusPick,        "Focus residue at picked atom");

    // Macro recording
    km.bind(Mode::Normal, {'q'},       Action::StartMacro,  "Record/stop macro");
    km.bind(Mode::Normal, {'@'},       Action::PlayMacro,   "Play macro");
}

void KeymapManager::bindCommandDefaults() {
    auto& km = keymap_;
    km.bind(Mode::Command, {27},        Action::ExitToNormal,  "Exit to normal");  // ESC
    km.bind(Mode::Command, {3},         Action::ExitToNormal,  "Exit (Ctrl+C)");
    km.bind(Mode::Command, {'\n'},      Action::ExecuteCommand, "Execute");
    km.bind(Mode::Command, {KEY_ENTER}, Action::ExecuteCommand, "Execute");
    km.bind(Mode::Command, {'\t'},      Action::Autocomplete,   "Autocomplete");
    km.bind(Mode::Command, {KEY_UP},    Action::HistoryPrev,    "History prev");
    km.bind(Mode::Command, {KEY_DOWN},  Action::HistoryNext,    "History next");
    km.bind(Mode::Command, {23},        Action::DeleteWord,     "Delete word (Ctrl+W)");
    km.bind(Mode::Command, {21},        Action::ClearLine,      "Clear line (Ctrl+U)");
}

void KeymapManager::bindVisualDefaults() {
    auto& km = keymap_;
    km.bind(Mode::Visual, {27},  Action::ExitToNormal, "Exit to normal");
    km.bind(Mode::Visual, {3},   Action::ExitToNormal, "Exit (Ctrl+C)");
}

void KeymapManager::bindSearchDefaults() {
    // Every key in Search mode is text input by default; the keymap only
    // surfaces the explicit non-text actions (ESC, Ctrl+C to cancel; Enter
    // to commit; arrow keys for history would go here too).
    auto& km = keymap_;
    km.bind(Mode::Search, {27},        Action::ExitToNormal,   "Exit to normal");  // ESC
    km.bind(Mode::Search, {3},         Action::ExitToNormal,   "Exit (Ctrl+C)");
    km.bind(Mode::Search, {'\n'},      Action::ExecuteSearch,  "Execute search");
    km.bind(Mode::Search, {KEY_ENTER}, Action::ExecuteSearch,  "Execute search");
}

} // namespace molterm
