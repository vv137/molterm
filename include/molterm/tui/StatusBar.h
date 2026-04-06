#pragma once

#include <string>

#include "molterm/input/Mode.h"
#include "molterm/tui/Widget.h"
#include "molterm/tui/Window.h"

namespace molterm {

class StatusBar : public Widget {
public:
    void render(Window& win, Mode mode,
                const std::string& objectInfo,
                const std::string& rightInfo);
};

} // namespace molterm
