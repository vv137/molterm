#pragma once

#include <cstdint>
#include <string>

namespace molterm {

enum class SSType : uint8_t {
    Loop = 0,
    Helix,
    Sheet,
};

struct AtomData {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    std::string name;       // "CA", "N", "O"
    std::string element;    // "C", "N", "O"
    std::string resName;    // "ALA", "GLY"
    std::string chainId;    // "A", "B"
    int resSeq = 0;
    char insCode = ' ';
    float bFactor = 0.0f;
    float occupancy = 1.0f;
    int serial = 0;
    int8_t formalCharge = 0;
    SSType ssType = SSType::Loop;
};

} // namespace molterm
