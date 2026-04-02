#pragma once

#include <memory>
#include <string>
#include <vector>

#include "molterm/core/MolObject.h"
#include "molterm/tui/Window.h"

namespace molterm {

class ObjectPanel {
public:
    void render(Window& win,
                const std::vector<std::shared_ptr<MolObject>>& objects,
                int selectedIdx);
};

} // namespace molterm
