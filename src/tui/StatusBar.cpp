#include "molterm/tui/StatusBar.h"
#include "molterm/render/ColorMapper.h"

namespace molterm {

void StatusBar::render(Window& win, Mode mode,
                       const std::string& objectInfo,
                       const std::string& rightInfo) {
    win.erase();
    win.fillRow(0, ' ', kColorStatusBar);

    // Mode indicator (left)
    std::string modeStr = " " + modeName(mode) + " ";
    win.setAttr(A_BOLD | COLOR_PAIR(kColorStatusBar));
    win.print(0, 0, modeStr);
    win.unsetAttr(A_BOLD | COLOR_PAIR(kColorStatusBar));

    // Object info (center-left)
    if (!objectInfo.empty()) {
        int offset = static_cast<int>(modeStr.size()) + 1;
        win.printColored(0, offset, objectInfo, kColorStatusBar);
    }

    // Right-aligned info
    if (!rightInfo.empty()) {
        int rpos = win.width() - static_cast<int>(rightInfo.size()) - 1;
        if (rpos > 0) {
            win.printColored(0, rpos, rightInfo, kColorStatusBar);
        }
    }

    win.refresh();
}

} // namespace molterm
