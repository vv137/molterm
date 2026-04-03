#pragma once

#include "molterm/render/GraphicsEncoder.h"

#include <string>

namespace molterm {

// Kitty graphics protocol encoder with zlib compression.
// Uses atomic image replacement (fixed ID) for flicker-free updates.
class KittyEncoder : public GraphicsEncoder {
public:
    std::string encode(const uint8_t* rgb, int width, int height) override;
    const char* name() const override { return "KITTY"; }

private:
    std::string buf_;
    static constexpr int kImageId = 1;
    static constexpr int kChunkSize = 4096;
};

} // namespace molterm
