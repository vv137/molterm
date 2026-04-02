#include <memory>

#include "molterm/tui/ObjectPanel.h"
#include "molterm/render/ColorMapper.h"

namespace molterm {

void ObjectPanel::render(Window& win,
                         const std::vector<std::shared_ptr<MolObject>>& objects,
                         int selectedIdx) {
    win.erase();

    // Draw left border
    for (int y = 0; y < win.height(); ++y) {
        win.addChar(y, 0, ACS_VLINE);
    }

    // Header
    win.setAttr(A_BOLD | COLOR_PAIR(kColorPanelHeader));
    win.print(0, 2, "Objects");
    win.unsetAttr(A_BOLD | COLOR_PAIR(kColorPanelHeader));

    // Object list
    for (int i = 0; i < static_cast<int>(objects.size()); ++i) {
        int y = i + 1;
        if (y >= win.height()) break;

        const auto& obj = objects[i];
        char vis = obj->visible() ? 'v' : 'h';
        char sel = (i == selectedIdx) ? '>' : ' ';
        std::string line = std::string(1, sel) + " " + obj->name() +
                          " (" + std::string(1, vis) + ")";

        // Truncate if too long
        int maxLen = win.width() - 3;
        if (static_cast<int>(line.size()) > maxLen)
            line = line.substr(0, maxLen);

        int colorPair = (i == selectedIdx) ? kColorPanelSelected : kColorDefault;
        win.printColored(y, 2, line, colorPair);
    }

    win.refresh();
}

} // namespace molterm
