#pragma once

#include "molterm/render/GraphicsEncoder.h"

#include <string>

namespace molterm {

// iTerm2 Inline Image Protocol encoder (OSC 1337).
class ITermEncoder : public GraphicsEncoder {
public:
    std::string encode(const uint8_t* rgb, int width, int height,
                       int cols = 0, int rows = 0) override;
    const char* name() const override { return "ITERM2"; }

private:
    std::string buf_;
};

} // namespace molterm
