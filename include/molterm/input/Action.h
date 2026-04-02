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
    ColorByResidue,

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
        case Action::Inspect:        return "show_help";
        case Action::ShowHelp:       return "show_help";
        default:                     return "unknown";
    }
}

} // namespace molterm
