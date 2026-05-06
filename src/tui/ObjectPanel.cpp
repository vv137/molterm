#include <memory>

#include "molterm/tui/ObjectPanel.h"
#include "molterm/render/ColorMapper.h"

namespace molterm {

namespace {
// Fixed-column visibility glyph in front of the name so long object
// names can never truncate the marker off the right edge.
constexpr const char* kGlyphShown  = "\xE2\x97\x8F";  // ●
constexpr const char* kGlyphHidden = "\xE2\x97\x8B";  // ○
}  // namespace

void ObjectPanel::render(Window& win,
                         const std::vector<std::shared_ptr<MolObject>>& objects,
                         int selectedIdx) {
    win.erase();

    // Subtle gray divider — was rendering as a column of 'x' before
    // (Window::addChar truncated ACS_VLINE's chtype to its low byte).
    win.setAttr(COLOR_PAIR(kColorPanelBorder));
    win.verticalLine(0, 0, win.height());
    win.unsetAttr(COLOR_PAIR(kColorPanelBorder));

    // Header
    win.setAttr(A_BOLD | COLOR_PAIR(kColorPanelHeader));
    win.print(0, 2, "Objects");
    win.unsetAttr(A_BOLD | COLOR_PAIR(kColorPanelHeader));

    for (int i = 0; i < static_cast<int>(objects.size()); ++i) {
        int y = i + 1;
        if (y >= win.height()) break;

        const auto& obj = objects[i];
        const bool selected = (i == selectedIdx);
        const bool shown = obj->visible();

        std::string prefix;
        prefix += (selected ? '>' : ' ');
        prefix += ' ';
        prefix += (shown ? kGlyphShown : kGlyphHidden);
        prefix += ' ';

        const int nameBudget = win.width() - 6;  // 2 indent + 4 prefix
        std::string name = obj->name();
        if (static_cast<int>(name.size()) > nameBudget && nameBudget > 0)
            name = name.substr(0, nameBudget);

        const int colorPair = selected ? kColorPanelSelected
                            : shown    ? kColorDefault
                                       : kColorPanelDim;
        win.printColored(y, 2, prefix + name, colorPair);
    }

    win.refresh();
}

} // namespace molterm
