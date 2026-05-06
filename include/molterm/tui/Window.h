#pragma once

#include <ncurses.h>
#include <string>

namespace molterm {

// RAII wrapper for an ncurses WINDOW
class Window {
public:
    Window(int h, int w, int y, int x);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;

    void resize(int h, int w, int y, int x);
    void clear();
    void refresh();
    void erase();
    void box();

    // Drawing
    void print(int y, int x, const std::string& text);
    void printColored(int y, int x, const std::string& text, int colorPair);
    void addChar(int y, int x, char ch);
    void addCharColored(int y, int x, char ch, int colorPair);
    void fillRow(int y, char ch, int colorPair);
    void horizontalLine(int y, int x, int len);
    void verticalLine(int y, int x, int len);

    // Output a single Unicode codepoint using the wide-char ncurses API.
    // Required on Linux where narrow mvwprintw mangles multi-byte UTF-8.
    void addWideChar(int y, int x, char32_t codepoint, int colorPair);

    // Attributes
    void setAttr(int attr);
    void unsetAttr(int attr);

    int width() const;
    int height() const;
    int posY() const { return y_; }
    int posX() const { return x_; }

    WINDOW* raw() { return win_; }

private:
    WINDOW* win_ = nullptr;
    int h_, w_, y_, x_;
};

} // namespace molterm
