#pragma once

#include "molterm/render/GraphicsEncoder.h"

#include <memory>
#include <string>

namespace molterm {

enum class GraphicsProtocol {
    None,    // no pixel graphics (use braille/block/ascii)
    Sixel,
    Kitty,
    ITerm2,
};

class ProtocolPicker {
public:
    // Auto-detect the best graphics protocol for the current terminal
    static GraphicsProtocol detect();

    // Create an encoder for the given protocol
    static std::unique_ptr<GraphicsEncoder> createEncoder(GraphicsProtocol proto);

    // Human-readable name
    static const char* protocolName(GraphicsProtocol proto);

    // Check if running over SSH
    static bool isSSH();
};

} // namespace molterm
