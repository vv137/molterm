#pragma once

#include "molterm/render/GraphicsEncoder.h"

#include <string>

namespace molterm {

class SixelEncoder : public GraphicsEncoder {
public:
    std::string encode(const uint8_t* rgb, int width, int height) override;
    const char* name() const override { return "SIXEL"; }

private:
    std::string buf_;
};

} // namespace molterm
