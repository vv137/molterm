#include <clocale>
#include <cstdlib>
#include <cstring>

#include "molterm/tui/Screen.h"

namespace molterm {

bool isHeadless() {
    static int cached = -1;
    if (cached < 0) {
        const char* e = std::getenv("MOLTERM_HEADLESS");
        cached = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return cached == 1;
}

Screen::Screen() {
    if (isHeadless()) return;  // never touch ncurses

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

int Screen::width() const { return initialized_ ? COLS : 80; }
int Screen::height() const { return initialized_ ? LINES : 24; }

void Screen::refresh() { if (initialized_) ::refresh(); }
void Screen::clear() { if (initialized_) ::clear(); }

int Screen::getKey() { return initialized_ ? getch() : -1; }

void Screen::setTimeout(int ms) {
    if (initialized_) timeout(ms);
}

void Screen::enableMouse() {
    if (!initialized_) return;
    mousemask(ALL_MOUSE_EVENTS, nullptr);
    mouseinterval(0);  // disable click-detection delay for responsive picking
}

} // namespace molterm
