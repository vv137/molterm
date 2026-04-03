#include "molterm/render/ITermEncoder.h"
#include "molterm/render/Base64.h"

#include <cstring>
#include <vector>

namespace molterm {

// Minimal BMP encoder (uncompressed 24-bit)
static std::vector<uint8_t> encodeBMP(const uint8_t* rgb, int w, int h) {
    int rowBytes = w * 3;
    int padBytes = (4 - (rowBytes % 4)) % 4;
    int dataSize = (rowBytes + padBytes) * h;
    int fileSize = 54 + dataSize;

    std::vector<uint8_t> bmp(fileSize, 0);
    // BMP header
    bmp[0] = 'B'; bmp[1] = 'M';
    std::memcpy(&bmp[2], &fileSize, 4);
    int offset = 54;
    std::memcpy(&bmp[10], &offset, 4);
    // DIB header
    int dibSize = 40;
    std::memcpy(&bmp[14], &dibSize, 4);
    std::memcpy(&bmp[18], &w, 4);
    int negH = -h;  // top-down
    std::memcpy(&bmp[22], &negH, 4);
    uint16_t planes = 1; std::memcpy(&bmp[26], &planes, 2);
    uint16_t bpp = 24; std::memcpy(&bmp[28], &bpp, 2);
    std::memcpy(&bmp[34], &dataSize, 4);

    // Pixel data (BGR, top-down)
    uint8_t* dst = &bmp[54];
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = &rgb[y * w * 3];
        for (int x = 0; x < w; ++x) {
            dst[x * 3]     = src[x * 3 + 2]; // B
            dst[x * 3 + 1] = src[x * 3 + 1]; // G
            dst[x * 3 + 2] = src[x * 3];     // R
        }
        dst += rowBytes + padBytes;
    }

    return bmp;
}

std::string ITermEncoder::encode(const uint8_t* rgb, int width, int height) {
    buf_.clear();
    if (width <= 0 || height <= 0) return buf_;

    auto bmp = encodeBMP(rgb, width, height);
    std::string b64 = base64Encode(bmp.data(), bmp.size());

    // OSC 1337 ; File = [args] : base64_data BEL
    buf_.reserve(b64.size() + 128);
    buf_ += "\033]1337;File=inline=1;size=";
    buf_ += std::to_string(bmp.size());
    buf_ += ";width=";
    buf_ += std::to_string(width);
    buf_ += "px;height=";
    buf_ += std::to_string(height);
    buf_ += "px;preserveAspectRatio=0:";
    buf_ += b64;
    buf_ += '\a';  // BEL terminator

    return buf_;
}

} // namespace molterm
