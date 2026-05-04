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

    // Chain colors A-F. Chains 7-12 (G-L positions) are aliased through
    // the named-color slots in colorForChain() — see kChainPalette.
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

    // Rainbow gradient (8-color fallback)
    init_pair(kColorRainbow0, COLOR_BLUE,    -1);
    init_pair(kColorRainbow1, COLOR_CYAN,    -1);
    init_pair(kColorRainbow2, COLOR_GREEN,   -1);
    init_pair(kColorRainbow3, COLOR_YELLOW,  -1);
    init_pair(kColorRainbow4, COLOR_RED,     -1);

    // ResType (VMD-like chemical property groups)
    init_pair(kColorResNonpolar, COLOR_WHITE,   -1);
    init_pair(kColorResPolar,    COLOR_GREEN,   -1);
    init_pair(kColorResAcidic,   COLOR_RED,     -1);
    init_pair(kColorResBasic,    COLOR_BLUE,    -1);

    // Heatmap gradient (8-color fallback)
    init_pair(kColorHeatmap0, COLOR_BLUE,    -1);  // far
    init_pair(kColorHeatmap1, COLOR_CYAN,    -1);
    init_pair(kColorHeatmap2, COLOR_WHITE,   -1);  // medium
    init_pair(kColorHeatmap3, COLOR_YELLOW,  -1);
    init_pair(kColorHeatmap4, COLOR_RED,     -1);  // close

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
        // pLDDT with 256-color precision
        init_pair(kColorPLDDTVeryHigh, 21,  -1);  // deep blue
        init_pair(kColorPLDDTHigh,     75,  -1);  // light blue
        init_pair(kColorPLDDTLow,      178, -1);  // yellow
        init_pair(kColorPLDDTVeryLow,  208, -1);  // orange
        // Rainbow with 256-color precision
        init_pair(kColorRainbow0, 21,  -1);  // blue
        init_pair(kColorRainbow1, 51,  -1);  // cyan
        init_pair(kColorRainbow2, 46,  -1);  // green
        init_pair(kColorRainbow3, 226, -1);  // yellow
        init_pair(kColorRainbow4, 196, -1);  // red
        // ResType with 256-color precision
        init_pair(kColorResNonpolar, 250, -1);  // light gray
        init_pair(kColorResPolar,    119, -1);  // light green
        init_pair(kColorResAcidic,   196, -1);  // bright red
        init_pair(kColorResBasic,    69,  -1);  // bright blue
        // Heatmap with 256-color precision
        init_pair(kColorHeatmap0, 19,  -1);   // dark blue
        init_pair(kColorHeatmap1, 75,  -1);   // light blue
        init_pair(kColorHeatmap2, 250, -1);   // light gray
        init_pair(kColorHeatmap3, 208, -1);   // orange
        init_pair(kColorHeatmap4, 196, -1);   // bright red
    } else {
        init_pair(kColorOrange,  COLOR_YELLOW,  -1);
        init_pair(kColorPink,    COLOR_MAGENTA, -1);
        init_pair(kColorLime,    COLOR_GREEN,   -1);
        init_pair(kColorTeal,    COLOR_CYAN,    -1);
        init_pair(kColorPurple,  COLOR_MAGENTA, -1);
        init_pair(kColorSalmon,  COLOR_RED,     -1);
        init_pair(kColorSlate,   COLOR_BLUE,    -1);
        init_pair(kColorGray,    COLOR_WHITE,   -1);
    }
}

int ColorMapper::colorForRainbow(float fraction) {
    int idx = static_cast<int>(fraction * 5.0f);
    if (idx < 0) idx = 0;
    if (idx > 4) idx = 4;
    return kColorRainbow0 + idx;
}

int ColorMapper::colorForAtom(const AtomData& atom, ColorScheme scheme,
                               int overrideColor, float rainbowFrac) {
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
            float plddt = atom.bFactor;
            if (plddt >= 90.0f) return kColorPLDDTVeryHigh;
            if (plddt >= 70.0f) return kColorPLDDTHigh;
            if (plddt >= 50.0f) return kColorPLDDTLow;
            return kColorPLDDTVeryLow;
        }
        case ColorScheme::Rainbow:
            return (rainbowFrac >= 0.0f) ? colorForRainbow(rainbowFrac) : kColorRainbow2;
        case ColorScheme::ResType:
            return colorForResType(atom.resName);
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
    // 12-hue cycle. A-F use dedicated chain pair IDs; chains 7-12 reuse
    // the named-color slots (which already define both ncurses-256 and
    // RGB) so all hue definitions stay in one place.
    static constexpr int kChainPalette[12] = {
        kColorChainA, kColorChainB, kColorChainC,
        kColorChainD, kColorChainE, kColorChainF,
        kColorOrange, kColorLime,   kColorTeal,
        kColorPurple, kColorPink,   kColorSlate,
    };
    int idx = (chainId[0] - 'A') % 12;
    if (idx < 0) idx += 12;
    return kChainPalette[idx];
}

int ColorMapper::colorForSS(SSType ss) {
    switch (ss) {
        case SSType::Helix: return kColorHelix;
        case SSType::Sheet: return kColorSheet;
        case SSType::Loop:  return kColorLoop;
    }
    return kColorLoop;
}

int ColorMapper::colorForResType(const std::string& resName) {
    // Nonpolar/hydrophobic (white/gray)
    if (resName == "ALA" || resName == "VAL" || resName == "LEU" ||
        resName == "ILE" || resName == "PRO" || resName == "PHE" ||
        resName == "TRP" || resName == "MET" || resName == "GLY")
        return kColorResNonpolar;
    // Polar/uncharged (green)
    if (resName == "SER" || resName == "THR" || resName == "CYS" ||
        resName == "TYR" || resName == "ASN" || resName == "GLN")
        return kColorResPolar;
    // Acidic/negative (red)
    if (resName == "ASP" || resName == "GLU")
        return kColorResAcidic;
    // Basic/positive (blue)
    if (resName == "LYS" || resName == "ARG" || resName == "HIS")
        return kColorResBasic;
    // Non-standard / ligand / nucleic acid → default
    return kColorResPolar;
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
