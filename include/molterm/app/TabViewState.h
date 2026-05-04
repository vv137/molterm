#pragma once

#include <string>

#include "molterm/tui/SeqBar.h"

namespace molterm {

// Per-tab view state: restored/saved on tab switch so each tab
// has independent SeqBar scroll, panel visibility, etc.
struct TabViewState {
    // SeqBar — each tab owns its own instance
    SeqBar seqBar;
    int focusResi = -1;
    std::string focusChain;

    // Panel visibility (per-tab)
    bool panelVisible = false;
    bool analysisPanelVisible = false;

    // SeqBar display — always wrap (show all sequences); the visible/hidden
    // toggle is the only user-facing knob.
    bool seqBarVisible = true;
    bool seqBarWrap = true;
    int seqBarHeight = 1;
};

} // namespace molterm
