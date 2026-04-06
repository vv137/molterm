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

    // SeqBar display
    bool seqBarVisible = true;
    bool seqBarWrap = false;
    int seqBarHeight = 1;
};

} // namespace molterm
