#include "molterm/render/ColorMapper.h"
#include <ncurses.h>
#include <algorithm>

namespace molterm {

void ColorMapper::initColors() {
    if (!has_colors()) return;

    // Element colors
    init_pair(kColorCarbon,     COLOR_GREEN,   -1);
    init_pair(kColorNitrogen,   COLOR_BLUE,    -1);
    init_pair(kColorOxygen,     COLOR_RED,     -1);
    init_pair(kColorSulfur,     COLOR_YELLOW,  -1);
    init_pair(kColorPhosphorus, COLOR_MAGENTA, -1);
    init_pair(kColorHydrogen,   COLOR_WHITE,   -1);
    init_pair(kColorIron,       COLOR_CYAN,    -1);
    init_pair(kColorOther,      COLOR_WHITE,   -1);

    // Chain colors
    init_pair(kColorChainA, COLOR_GREEN,   -1);
    init_pair(kColorChainB, COLOR_CYAN,    -1);
    init_pair(kColorChainC, COLOR_MAGENTA, -1);
    init_pair(kColorChainD, COLOR_YELLOW,  -1);
    init_pair(kColorChainE, COLOR_RED,     -1);
    init_pair(kColorChainF, COLOR_BLUE,    -1);

    // SS colors
    init_pair(kColorHelix, COLOR_RED,    -1);
    init_pair(kColorSheet, COLOR_YELLOW, -1);
    init_pair(kColorLoop,  COLOR_GREEN,  -1);

    // B-factor gradient
    init_pair(kColorBFactorLow,  COLOR_BLUE,   -1);
    init_pair(kColorBFactorMid,  COLOR_GREEN,  -1);
    init_pair(kColorBFactorHigh, COLOR_RED,    -1);

    // UI colors
    init_pair(kColorStatusBar,    COLOR_BLACK, COLOR_WHITE);
    init_pair(kColorCommandLine,  COLOR_WHITE, -1);
    init_pair(kColorTabActive,    COLOR_BLACK, COLOR_GREEN);
    init_pair(kColorTabInactive,  COLOR_WHITE, COLOR_BLACK);
    init_pair(kColorPanelHeader,  COLOR_CYAN,  -1);
    init_pair(kColorPanelSelected, COLOR_BLACK, COLOR_CYAN);

    // Named palette colors
    init_pair(kColorRed,     COLOR_RED,     -1);
    init_pair(kColorGreen,   COLOR_GREEN,   -1);
    init_pair(kColorBlue,    COLOR_BLUE,    -1);
    init_pair(kColorYellow,  COLOR_YELLOW,  -1);
    init_pair(kColorMagenta, COLOR_MAGENTA, -1);
    init_pair(kColorCyan,    COLOR_CYAN,    -1);
    init_pair(kColorWhite,   COLOR_WHITE,   -1);
    // pLDDT confidence gradient (AlphaFold)
    // Default 8-color approximations; overridden below for 256-color
    init_pair(kColorPLDDTVeryHigh, COLOR_BLUE,    -1);
    init_pair(kColorPLDDTHigh,     COLOR_CYAN,    -1);
    init_pair(kColorPLDDTLow,      COLOR_YELLOW,  -1);
    init_pair(kColorPLDDTVeryLow,  COLOR_RED,     -1);

    // Extended colors (require 256-color terminal)
    if (COLORS >= 256) {
        init_pair(kColorOrange,  208, -1);  // orange
        init_pair(kColorPink,    213, -1);  // pink
        init_pair(kColorLime,    118, -1);  // lime
        init_pair(kColorTeal,    30,  -1);  // teal
        init_pair(kColorPurple,  135, -1);  // purple
        init_pair(kColorSalmon,  209, -1);  // salmon
        init_pair(kColorSlate,   67,  -1);  // slate
        init_pair(kColorGray,    245, -1);  // gray
    } else {
        init_pair(kColorOrange,  COLOR_YELLOW,  -1);
        init_pair(kColorPink,    COLOR_MAGENTA, -1);
        init_pair(kColorLime,    COLOR_GREEN,   -1);
        init_pair(kColorTeal,    COLOR_CYAN,    -1);
        init_pair(kColorPurple,  COLOR_MAGENTA, -1);
        init_pair(kColorSalmon,  COLOR_RED,     -1);
        init_pair(kColorSlate,   COLOR_BLUE,    -1);
        init_pair(kColorGray,    COLOR_WHITE,   -1);
        // pLDDT with 256-color precision
        init_pair(kColorPLDDTVeryHigh, 21,  -1);  // deep blue
        init_pair(kColorPLDDTHigh,     75,  -1);  // light blue
        init_pair(kColorPLDDTLow,      178, -1);  // yellow
        init_pair(kColorPLDDTVeryLow,  208, -1);  // orange
    }
}

int ColorMapper::colorForAtom(const AtomData& atom, ColorScheme scheme,
                               int overrideColor) {
    if (overrideColor >= 0) return overrideColor;

    switch (scheme) {
        case ColorScheme::Element: return colorForElement(atom.element);
        case ColorScheme::Chain:   return colorForChain(atom.chainId);
        case ColorScheme::SecondaryStructure: return colorForSS(atom.ssType);
        case ColorScheme::BFactor: {
            if (atom.bFactor < 20.0f) return kColorBFactorLow;
            if (atom.bFactor < 50.0f) return kColorBFactorMid;
            return kColorBFactorHigh;
        }
        case ColorScheme::PLDDT: {
            // AlphaFold pLDDT stored in B-factor field
            float plddt = atom.bFactor;
            if (plddt >= 90.0f) return kColorPLDDTVeryHigh;
            if (plddt >= 70.0f) return kColorPLDDTHigh;
            if (plddt >= 50.0f) return kColorPLDDTLow;
            return kColorPLDDTVeryLow;
        }
        default: return kColorOther;
    }
}

int ColorMapper::colorForElement(const std::string& element) {
    if (element.empty()) return kColorOther;
    char e = element[0];
    switch (e) {
        case 'C': return kColorCarbon;
        case 'N': return kColorNitrogen;
        case 'O': return kColorOxygen;
        case 'S': return kColorSulfur;
        case 'P': return kColorPhosphorus;
        case 'H': return kColorHydrogen;
        default:  return kColorOther;
    }
}

int ColorMapper::colorForChain(const std::string& chainId) {
    if (chainId.empty()) return kColorChainA;
    int idx = (chainId[0] - 'A') % 6;
    if (idx < 0) idx = 0;
    return kColorChainA + idx;
}

int ColorMapper::colorForSS(SSType ss) {
    switch (ss) {
        case SSType::Helix: return kColorHelix;
        case SSType::Sheet: return kColorSheet;
        case SSType::Loop:  return kColorLoop;
    }
    return kColorLoop;
}

int ColorMapper::colorByName(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower == "red")     return kColorRed;
    if (lower == "green")   return kColorGreen;
    if (lower == "blue")    return kColorBlue;
    if (lower == "yellow")  return kColorYellow;
    if (lower == "magenta") return kColorMagenta;
    if (lower == "cyan")    return kColorCyan;
    if (lower == "white")   return kColorWhite;
    if (lower == "orange")  return kColorOrange;
    if (lower == "pink")    return kColorPink;
    if (lower == "lime")    return kColorLime;
    if (lower == "teal")    return kColorTeal;
    if (lower == "purple")  return kColorPurple;
    if (lower == "salmon")  return kColorSalmon;
    if (lower == "slate")   return kColorSlate;
    if (lower == "gray" || lower == "grey") return kColorGray;
    return -1;
}

std::string ColorMapper::availableColors() {
    return "red green blue yellow magenta cyan white orange pink lime teal purple salmon slate gray";
}

} // namespace molterm
