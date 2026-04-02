#pragma once

#include <ncurses.h>

namespace molterm {

// RAII wrapper for ncurses initialization/teardown
class Screen {
public:
    Screen();
    ~Screen();

    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;

    int width() const;
    int height() const;
    void refresh();
    void clear();

    // Input
    int getKey();
    void setTimeout(int ms);  // -1 = blocking

    // Mouse
    void enableMouse();

    bool isInitialized() const { return initialized_; }

private:
    bool initialized_ = false;
};

} // namespace molterm
