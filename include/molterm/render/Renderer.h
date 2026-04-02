#pragma once

#include "molterm/core/MolObject.h"
#include "molterm/render/Camera.h"
#include "molterm/render/DepthBuffer.h"
#include "molterm/tui/Window.h"

namespace molterm {

// Abstract renderer interface
class Renderer {
public:
    virtual ~Renderer() = default;
    virtual void render(const MolObject& mol, const Camera& cam,
                        DepthBuffer& zbuf, Window& win) = 0;
};

} // namespace molterm
