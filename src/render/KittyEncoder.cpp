#include "molterm/render/KittyEncoder.h"
#include "molterm/render/Base64.h"

#include <cstring>
#include <vector>
#include <zlib.h>

namespace molterm {

static std::vector<uint8_t> zlibCompress(const uint8_t* data, size_t len) {
    uLongf compLen = compressBound(static_cast<uLong>(len));
    std::vector<uint8_t> out(compLen);
    // Level 1 = fastest compression (good for real-time)
    compress2(out.data(), &compLen, data, static_cast<uLong>(len), 1);
    out.resize(compLen);
    return out;
}

std::string KittyEncoder::encode(const uint8_t* rgb, int width, int height,
                                  int cols, int rows) {
    buf_.clear();
    if (width <= 0 || height <= 0) return buf_;

    // Compress RGB data with zlib
    size_t rawSize = static_cast<size_t>(width) * height * 3;
    auto compressed = zlibCompress(rgb, rawSize);

    // Base64 encode
    std::string b64 = base64Encode(compressed.data(), compressed.size());

    // Kitty protocol: transmit in chunks
    // a=T: transmit and display
    // f=24: RGB (3 bytes per pixel)
    // o=z: zlib compressed
    // i=ID: fixed image ID for atomic replacement
    // s=width, v=height: pixel dimensions
    // q=2: suppress response

    buf_.reserve(b64.size() + 256);

    size_t offset = 0;
    bool first = true;
    while (offset < b64.size()) {
        size_t chunkLen = std::min(static_cast<size_t>(kChunkSize), b64.size() - offset);
        bool more = (offset + chunkLen < b64.size());

        buf_ += "\033_G";
        if (first) {
            buf_ += "a=T,f=24,o=z,i=";
            buf_ += std::to_string(kImageId);
            buf_ += ",s=";
            buf_ += std::to_string(width);
            buf_ += ",v=";
            buf_ += std::to_string(height);
            // c/r = display size in terminal cells (forces scaling to fit viewport)
            if (cols > 0 && rows > 0) {
                buf_ += ",c=";
                buf_ += std::to_string(cols);
                buf_ += ",r=";
                buf_ += std::to_string(rows);
            }
            buf_ += ",q=2,";
            first = false;
        }
        buf_ += "m=";
        buf_ += more ? '1' : '0';
        buf_ += ';';
        buf_.append(b64, offset, chunkLen);
        buf_ += "\033\\";

        offset += chunkLen;
    }

    return buf_;
}

} // namespace molterm
