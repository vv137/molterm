#include "molterm/render/ProtocolPicker.h"
#include "molterm/render/SixelEncoder.h"
#include "molterm/render/KittyEncoder.h"
#include "molterm/render/ITermEncoder.h"

#include <cstdlib>

namespace molterm {

bool ProtocolPicker::isSSH() {
    return std::getenv("SSH_CLIENT") ||
           std::getenv("SSH_TTY") ||
           std::getenv("SSH_CONNECTION");
}

GraphicsProtocol ProtocolPicker::detect() {
    // Kitty: check KITTY_WINDOW_ID or TERM=xterm-kitty
    if (std::getenv("KITTY_WINDOW_ID")) return GraphicsProtocol::Kitty;
    const char* term = std::getenv("TERM");
    if (term) {
        std::string t(term);
        if (t.find("kitty") != std::string::npos) return GraphicsProtocol::Kitty;
    }

    // iTerm2: check ITERM_SESSION_ID or TERM_PROGRAM=iTerm.app
    if (std::getenv("ITERM_SESSION_ID")) return GraphicsProtocol::ITerm2;
    const char* termProg = std::getenv("TERM_PROGRAM");
    if (termProg) {
        std::string tp(termProg);
        if (tp.find("iTerm") != std::string::npos) return GraphicsProtocol::ITerm2;
    }

    // WezTerm supports both Sixel and Kitty; prefer Kitty
    if (termProg) {
        std::string tp(termProg);
        if (tp.find("WezTerm") != std::string::npos) return GraphicsProtocol::Kitty;
    }

    // Sixel: xterm, foot, mlterm, contour, Windows Terminal
    if (termProg) {
        std::string tp(termProg);
        if (tp == "xterm" || tp == "foot" || tp == "mlterm" ||
            tp == "contour") return GraphicsProtocol::Sixel;
    }

    // Fallback: try Sixel (widely supported)
    return GraphicsProtocol::Sixel;
}

std::unique_ptr<GraphicsEncoder> ProtocolPicker::createEncoder(GraphicsProtocol proto) {
    switch (proto) {
        case GraphicsProtocol::Sixel:  return std::make_unique<SixelEncoder>();
        case GraphicsProtocol::Kitty:  return std::make_unique<KittyEncoder>();
        case GraphicsProtocol::ITerm2: return std::make_unique<ITermEncoder>();
        default: return nullptr;
    }
}

const char* ProtocolPicker::protocolName(GraphicsProtocol proto) {
    switch (proto) {
        case GraphicsProtocol::Sixel:  return "Sixel";
        case GraphicsProtocol::Kitty:  return "Kitty";
        case GraphicsProtocol::ITerm2: return "iTerm2";
        default:                       return "None";
    }
}

} // namespace molterm
