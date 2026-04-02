#pragma once

#include <string>
#include <vector>

#include "molterm/tui/Window.h"

namespace molterm {

class TabBar {
public:
    void render(Window& win, const std::vector<std::string>& tabNames,
                int activeIdx);
};

} // namespace molterm
