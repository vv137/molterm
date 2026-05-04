#include "molterm/tui/Window.h"

namespace molterm {

// In headless mode, stdscr is null because initscr() was never called.
// All ncurses calls below are guarded so a null win_ is a safe no-op.

Window::Window(int h, int w, int y, int x)
    : h_(h), w_(w), y_(y), x_(x) {
    if (!stdscr) { win_ = nullptr; return; }
    win_ = newwin(h, w, y, x);
    keypad(win_, TRUE);
}

Window::~Window() {
    if (win_) delwin(win_);
}

Window::Window(Window&& other) noexcept
    : win_(other.win_), h_(other.h_), w_(other.w_), y_(other.y_), x_(other.x_) {
    other.win_ = nullptr;
}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        if (win_) delwin(win_);
        win_ = other.win_;
        h_ = other.h_; w_ = other.w_;
        y_ = other.y_; x_ = other.x_;
        other.win_ = nullptr;
    }
    return *this;
}

void Window::resize(int h, int w, int y, int x) {
    if (win_) delwin(win_);
    h_ = h; w_ = w; y_ = y; x_ = x;
    if (!stdscr) { win_ = nullptr; return; }
    win_ = newwin(h, w, y, x);
    keypad(win_, TRUE);
}

void Window::clear() { if (win_) wclear(win_); }
void Window::refresh() { if (win_) wnoutrefresh(win_); }
void Window::erase() { if (win_) werase(win_); }
void Window::box() { if (win_) ::box(win_, 0, 0); }

void Window::print(int y, int x, const std::string& text) {
    if (!win_) return;
    mvwprintw(win_, y, x, "%s", text.c_str());
}

void Window::printColored(int y, int x, const std::string& text, int colorPair) {
    if (!win_) return;
    wattron(win_, COLOR_PAIR(colorPair));
    mvwprintw(win_, y, x, "%s", text.c_str());
    wattroff(win_, COLOR_PAIR(colorPair));
}

void Window::addChar(int y, int x, char ch) {
    if (!win_) return;
    mvwaddch(win_, y, x, ch);
}

void Window::addCharColored(int y, int x, char ch, int colorPair) {
    if (!win_) return;
    wattron(win_, COLOR_PAIR(colorPair));
    mvwaddch(win_, y, x, ch);
    wattroff(win_, COLOR_PAIR(colorPair));
}

void Window::fillRow(int y, char ch, int colorPair) {
    if (!win_) return;
    wattron(win_, COLOR_PAIR(colorPair));
    wmove(win_, y, 0);
    for (int i = 0; i < w_; ++i) waddch(win_, ch);
    wattroff(win_, COLOR_PAIR(colorPair));
}

void Window::horizontalLine(int y, int x, int len) {
    if (!win_) return;
    mvwhline(win_, y, x, ACS_HLINE, len);
}

void Window::addWideChar(int y, int x, char32_t codepoint, int colorPair) {
    if (!win_) return;
    cchar_t cc;
    wchar_t wch[2] = {static_cast<wchar_t>(codepoint), L'\0'};
    setcchar(&cc, wch, 0, static_cast<short>(colorPair), nullptr);
    mvwadd_wch(win_, y, x, &cc);
}

void Window::setAttr(int attr) { if (win_) wattron(win_, attr); }
void Window::unsetAttr(int attr) { if (win_) wattroff(win_, attr); }

int Window::width() const { return w_; }
int Window::height() const { return h_; }

} // namespace molterm
