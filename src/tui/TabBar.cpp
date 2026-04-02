#include "molterm/tui/TabBar.h"
#include "molterm/render/ColorMapper.h"

namespace molterm {

void TabBar::render(Window& win, const std::vector<std::string>& tabNames,
                    int activeIdx) {
    win.erase();
    win.fillRow(0, ' ', kColorTabInactive);

    int x = 1;
    for (int i = 0; i < static_cast<int>(tabNames.size()); ++i) {
        std::string label = " " + tabNames[i] + " ";
        int colorPair = (i == activeIdx) ? kColorTabActive : kColorTabInactive;
        int attr = (i == activeIdx) ? A_BOLD : A_NORMAL;

        win.setAttr(attr | COLOR_PAIR(colorPair));
        win.print(0, x, label);
        win.unsetAttr(attr | COLOR_PAIR(colorPair));

        x += static_cast<int>(label.size()) + 1;
        if (x >= win.width() - 3) break;
    }

    win.refresh();
}

} // namespace molterm
