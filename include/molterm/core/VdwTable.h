#pragma once

#include <string>

namespace molterm {

// Van der Waals radii (Bondi) in Å. Shared between Spacefill and
// BallStick (Mol*-style sphere sizing). Returns 1.70 (carbon) for any
// element not in the small built-in table.
inline float vdwRadius(const std::string& element) {
    if (element == "H")  return 1.20f;
    if (element == "C")  return 1.70f;
    if (element == "N")  return 1.55f;
    if (element == "O")  return 1.52f;
    if (element == "S")  return 1.80f;
    if (element == "P")  return 1.80f;
    if (element == "F")  return 1.47f;
    if (element == "Cl") return 1.75f;
    if (element == "Br") return 1.85f;
    if (element == "I")  return 1.98f;
    if (element == "Se") return 1.90f;
    if (element == "Fe") return 2.00f;
    if (element == "Zn") return 1.39f;
    return 1.70f;
}

} // namespace molterm
