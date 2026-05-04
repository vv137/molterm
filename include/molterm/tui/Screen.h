#pragma once

#include <ncurses.h>

namespace molterm {

// True when MOLTERM_HEADLESS=1 was set before Screen construction.
// In headless mode, ncurses is never initialized — used for script runs
// that should not blink the alternate screen buffer.
bool isHeadless();

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
