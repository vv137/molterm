#pragma once

#include <cstdint>
#include <string>

namespace molterm {

// Abstract encoder for pixel graphics protocols (Sixel, Kitty, iTerm2).
// Takes an RGB framebuffer and encodes it for terminal output.
class GraphicsEncoder {
public:
    virtual ~GraphicsEncoder() = default;

    // Encode the RGB framebuffer into a terminal escape sequence.
    // rgb: row-major RGB pixel data (3 bytes per pixel)
    // width, height: pixel dimensions
    // Returns the encoded string ready to write to stdout.
    // cols/rows: terminal cell dimensions for protocols that support scaling (Kitty)
    virtual std::string encode(const uint8_t* rgb, int width, int height,
                               int cols = 0, int rows = 0) = 0;

    // Human-readable name for status bar
    virtual const char* name() const = 0;
};

} // namespace molterm
