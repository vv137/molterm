#include <clocale>
#include <cstring>

#include "molterm/tui/Screen.h"

namespace molterm {

Screen::Screen() {
    // Try the user's locale first; fall back to common UTF-8 locales
    // so that wide-char ncurses works even on minimal Linux installs.
    if (!setlocale(LC_ALL, "") || !std::strstr(setlocale(LC_ALL, nullptr), "UTF-8")) {
        if (!setlocale(LC_ALL, "C.UTF-8"))
            setlocale(LC_ALL, "en_US.UTF-8");
    }
    initscr();
    if (has_colors()) {
        start_color();
        use_default_colors();
    }
    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    ::refresh();
    initialized_ = true;
}

Screen::~Screen() {
    if (initialized_) {
        endwin();
    }
}

int Screen::width() const { return COLS; }
int Screen::height() const { return LINES; }

void Screen::refresh() { ::refresh(); }
void Screen::clear() { ::clear(); }

int Screen::getKey() { return getch(); }

void Screen::setTimeout(int ms) {
    timeout(ms);
}

void Screen::enableMouse() {
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, nullptr);
}

} // namespace molterm
