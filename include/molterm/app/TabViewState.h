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
    // toggle is the only user-facing knob. Off by default so the viewport
    // gets the full screen on first open; press `b` to bring it up.
    bool seqBarVisible = false;
    bool seqBarWrap = true;
    int seqBarHeight = 1;
};

} // namespace molterm
