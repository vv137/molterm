#pragma once

#include <string>

namespace molterm {

enum class Action {
    None,

    // Navigation
    RotateLeft,
    RotateRight,
    RotateUp,
    RotateDown,
    PanLeft,
    PanRight,
    PanUp,
    PanDown,
    ZoomIn,
    ZoomOut,
    ResetView,
    CenterSelection,
    Redraw,

    // Object
    NextObject,
    PrevObject,
    ToggleVisible,
    DeleteObject,
    YankObject,
    PasteObject,
    RenameObject,
    TogglePanel,

    // State
    PrevState,
    NextState,

    // Representation
    ShowWireframe,
    ShowBallStick,
    ShowSpacefill,
    ShowCartoon,
    ShowBackbone,
    HideWireframe,
    HideBackbone,
    HideAll,

    // Tabs
    NextTab,
    PrevTab,
    NewTab,
    CloseTab,
    MoveToTab,
    CopyToTab,

    // Coloring
    ColorByElement,
    ColorByChain,
    ColorBySS,
    ColorByBFactor,
    ColorByRainbow,

    // Mode transitions
    EnterCommand,
    EnterVisual,
    EnterSearch,
    ExitToNormal,

    // Search
    SearchNext,
    SearchPrev,

    // Quick actions
    Inspect,
    ShowHelp,
    Undo,
    Redo,
    RepeatLast,

    // Command line
    ExecuteCommand,
    Autocomplete,
    HistoryPrev,
    HistoryNext,
    DeleteWord,
    ClearLine,

    // Search mode
    ExecuteSearch,

    // Macro recording (Phase 4)
    StartMacro,
    PlayMacro,
    ColorByPLDDT,
};

inline std::string actionName(Action a) {
    switch (a) {
        case Action::None:           return "none";
        case Action::RotateLeft:     return "rotate_left";
        case Action::RotateRight:    return "rotate_right";
        case Action::RotateUp:       return "rotate_up";
        case Action::RotateDown:     return "rotate_down";
        case Action::PanLeft:        return "pan_left";
        case Action::PanRight:       return "pan_right";
        case Action::PanUp:          return "pan_up";
        case Action::PanDown:        return "pan_down";
        case Action::ZoomIn:         return "zoom_in";
        case Action::ZoomOut:        return "zoom_out";
        case Action::ResetView:      return "reset_view";
        case Action::EnterCommand:   return "enter_command";
        case Action::EnterVisual:    return "enter_visual";
        case Action::EnterSearch:    return "enter_search";
        case Action::ExitToNormal:   return "exit_to_normal";
        case Action::NextTab:        return "next_tab";
        case Action::PrevTab:        return "prev_tab";
        case Action::NewTab:         return "new_tab";
        case Action::CloseTab:       return "close_tab";
        case Action::ShowWireframe:  return "show_wireframe";
        case Action::ShowBallStick:  return "show_ballstick";
        case Action::ShowSpacefill:  return "show_spacefill";
        case Action::ShowCartoon:    return "show_cartoon";
        case Action::ShowBackbone:   return "show_backbone";
        case Action::HideAll:        return "hide_all";
        case Action::ColorByElement: return "color_by_element";
        case Action::ColorByChain:   return "color_by_chain";
        case Action::ColorBySS:      return "color_by_ss";
        case Action::ColorByBFactor: return "color_by_bfactor";
        case Action::ToggleVisible:  return "toggle_visible";
        case Action::NextObject:     return "next_object";
        case Action::PrevObject:     return "prev_object";
        case Action::Inspect:        return "inspect";
        case Action::ShowHelp:       return "show_help";
        case Action::DeleteObject:   return "delete_object";
        case Action::YankObject:     return "yank_object";
        case Action::PasteObject:    return "paste_object";
        case Action::RenameObject:   return "rename_object";
        case Action::TogglePanel:    return "toggle_panel";
        case Action::HideWireframe:  return "hide_wireframe";
        case Action::HideBackbone:   return "hide_backbone";
        case Action::MoveToTab:      return "move_to_tab";
        case Action::CopyToTab:      return "copy_to_tab";
        case Action::SearchNext:     return "search_next";
        case Action::SearchPrev:     return "search_prev";
        case Action::Undo:           return "undo";
        case Action::Redo:           return "redo";
        case Action::RepeatLast:     return "repeat_last";
        case Action::CenterSelection: return "center_selection";
        case Action::Redraw:         return "redraw";
        case Action::ExecuteCommand: return "execute";
        case Action::Autocomplete:   return "autocomplete";
        case Action::HistoryPrev:    return "history_prev";
        case Action::HistoryNext:    return "history_next";
        case Action::DeleteWord:     return "delete_word";
        case Action::ClearLine:      return "clear_line";
        case Action::ExecuteSearch:  return "execute_search";
        case Action::StartMacro:     return "start_macro";
        case Action::PlayMacro:      return "play_macro";
        case Action::ColorByPLDDT:   return "color_by_plddt";
        case Action::ColorByRainbow: return "color_by_rainbow";
        case Action::PrevState:      return "prev_state";
        case Action::NextState:      return "next_state";
        default:                     return "unknown";
    }
}

inline Action actionFromName(const std::string& name) {
    if (name == "none")             return Action::None;
    if (name == "rotate_left")      return Action::RotateLeft;
    if (name == "rotate_right")     return Action::RotateRight;
    if (name == "rotate_up")        return Action::RotateUp;
    if (name == "rotate_down")      return Action::RotateDown;
    if (name == "pan_left")         return Action::PanLeft;
    if (name == "pan_right")        return Action::PanRight;
    if (name == "pan_up")           return Action::PanUp;
    if (name == "pan_down")         return Action::PanDown;
    if (name == "zoom_in")          return Action::ZoomIn;
    if (name == "zoom_out")         return Action::ZoomOut;
    if (name == "reset_view")       return Action::ResetView;
    if (name == "center_selection") return Action::CenterSelection;
    if (name == "redraw")           return Action::Redraw;
    if (name == "next_object")      return Action::NextObject;
    if (name == "prev_object")      return Action::PrevObject;
    if (name == "toggle_visible")   return Action::ToggleVisible;
    if (name == "delete_object")    return Action::DeleteObject;
    if (name == "yank_object")      return Action::YankObject;
    if (name == "paste_object")     return Action::PasteObject;
    if (name == "rename_object")    return Action::RenameObject;
    if (name == "toggle_panel")     return Action::TogglePanel;
    if (name == "show_wireframe")   return Action::ShowWireframe;
    if (name == "show_ballstick")   return Action::ShowBallStick;
    if (name == "show_spacefill")   return Action::ShowSpacefill;
    if (name == "show_cartoon")     return Action::ShowCartoon;
    if (name == "show_backbone")    return Action::ShowBackbone;
    if (name == "hide_wireframe")   return Action::HideWireframe;
    if (name == "hide_backbone")    return Action::HideBackbone;
    if (name == "hide_all")         return Action::HideAll;
    if (name == "next_tab")         return Action::NextTab;
    if (name == "prev_tab")         return Action::PrevTab;
    if (name == "new_tab")          return Action::NewTab;
    if (name == "close_tab")        return Action::CloseTab;
    if (name == "move_to_tab")      return Action::MoveToTab;
    if (name == "copy_to_tab")      return Action::CopyToTab;
    if (name == "color_by_element") return Action::ColorByElement;
    if (name == "color_by_chain")   return Action::ColorByChain;
    if (name == "color_by_ss")      return Action::ColorBySS;
    if (name == "color_by_bfactor") return Action::ColorByBFactor;
    if (name == "color_by_plddt")   return Action::ColorByPLDDT;
    if (name == "color_by_rainbow") return Action::ColorByRainbow;
    if (name == "enter_command")    return Action::EnterCommand;
    if (name == "enter_visual")     return Action::EnterVisual;
    if (name == "enter_search")     return Action::EnterSearch;
    if (name == "exit_to_normal")   return Action::ExitToNormal;
    if (name == "search_next")      return Action::SearchNext;
    if (name == "search_prev")      return Action::SearchPrev;
    if (name == "inspect")          return Action::Inspect;
    if (name == "show_help")        return Action::ShowHelp;
    if (name == "undo")             return Action::Undo;
    if (name == "redo")             return Action::Redo;
    if (name == "repeat_last")      return Action::RepeatLast;
    if (name == "execute")          return Action::ExecuteCommand;
    if (name == "autocomplete")     return Action::Autocomplete;
    if (name == "history_prev")     return Action::HistoryPrev;
    if (name == "history_next")     return Action::HistoryNext;
    if (name == "delete_word")      return Action::DeleteWord;
    if (name == "clear_line")       return Action::ClearLine;
    if (name == "execute_search")   return Action::ExecuteSearch;
    if (name == "start_macro")      return Action::StartMacro;
    if (name == "play_macro")       return Action::PlayMacro;
    return Action::None;
}

} // namespace molterm
