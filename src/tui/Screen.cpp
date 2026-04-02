#include "molterm/tui/Screen.h"
#include <locale.h>

namespace molterm {

Screen::Screen() {
    setlocale(LC_ALL, "");
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
