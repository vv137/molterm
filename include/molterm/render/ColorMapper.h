#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "molterm/core/MolObject.h"

namespace molterm {

// Custom-RGB color pair encoding (issue #79). Real ncurses pair IDs live
// in [0, ~256); to support full 24-bit hex/rgb literals in :color without
// growing the per-atom storage from int to a tagged union, custom colors
// are packed into the same int with a high-bit tag. Tag is bit 24 so the
// 24-bit RGB payload (bits 0-23) doesn't collide with it. PixelCanvas
// decodes at render time; Window::addCharColored falls back to the
// nearest builtin named pair so the 8-color terminal still shows
// *something* sensible.
constexpr int kColorCustomTag = 0x01000000;
inline int packCustomRGB(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return kColorCustomTag | (int(r) << 16) | (int(g) << 8) | int(b);
}
inline bool isCustomColor(int p) { return (p & kColorCustomTag) != 0; }
inline std::array<std::uint8_t, 3> unpackCustomRGB(int p) {
    return {std::uint8_t((p >> 16) & 0xFF),
            std::uint8_t((p >> 8)  & 0xFF),
            std::uint8_t( p        & 0xFF)};
}

// Color pair IDs for ncurses
enum ColorPairId : int {
    kColorDefault = 0,
    kColorCarbon = 1,
    kColorNitrogen = 2,
    kColorOxygen = 3,
    kColorSulfur = 4,
    kColorPhosphorus = 5,
    kColorHydrogen = 6,
    kColorIron = 7,
    kColorOther = 8,
    // Chain colors — 6 base hues; chains 7..12 are aliased through the
    // named-color slots (Orange/Lime/Teal/Purple/Pink/Slate) so a 12-mer
    // gets distinct hues before the cycle repeats. Done via aliasing rather
    // than a separate G-L block to keep one source of truth per hue.
    kColorChainA = 10,
    kColorChainB = 11,
    kColorChainC = 12,
    kColorChainD = 13,
    kColorChainE = 14,
    kColorChainF = 15,
    // SS colors
    kColorHelix = 20,
    kColorSheet = 21,
    kColorLoop = 22,
    // B-factor gradient
    kColorBFactorLow = 30,
    kColorBFactorMid = 31,
    kColorBFactorHigh = 32,
    // UI colors
    kColorStatusBar = 40,
    kColorCommandLine = 41,
    kColorTabActive = 42,
    kColorTabInactive = 43,
    kColorPanelHeader = 44,
    kColorPanelSelected = 45,
    kColorPanelDim = 46,
    kColorPanelBorder = 47,
    // Named palette colors (for :color <name> <selection>)
    kColorRed = 50,
    kColorGreen = 51,
    kColorBlue = 52,
    kColorYellow = 53,
    kColorMagenta = 54,
    kColorCyan = 55,
    kColorWhite = 56,
    kColorOrange = 57,
    kColorPink = 58,
    kColorLime = 59,
    kColorTeal = 60,
    kColorPurple = 61,
    kColorSalmon = 62,
    kColorSlate = 63,
    kColorGray = 64,
    // pLDDT confidence gradient (AlphaFold)
    kColorPLDDTVeryHigh = 70,  // >90: dark blue
    kColorPLDDTHigh = 71,      // 70-90: light blue
    kColorPLDDTLow = 72,       // 50-70: yellow
    kColorPLDDTVeryLow = 73,   // <50: orange
    // Rainbow gradient (N→C terminus): blue → cyan → green → yellow → red
    kColorRainbow0 = 80,  // blue
    kColorRainbow1 = 81,  // cyan
    kColorRainbow2 = 82,  // green
    kColorRainbow3 = 83,  // yellow
    kColorRainbow4 = 84,  // red
    // ResType (VMD-like chemical property groups)
    kColorResNonpolar = 90,  // white/gray — ALA VAL LEU ILE PRO PHE TRP MET GLY
    kColorResPolar    = 91,  // green — SER THR CYS TYR ASN GLN
    kColorResAcidic   = 92,  // red — ASP GLU
    kColorResBasic    = 93,  // blue — LYS ARG HIS
    // Heatmap gradient (contact map, density visualization)
    kColorHeatmap0 = 100,  // dark blue (far / no contact)
    kColorHeatmap1 = 101,  // light blue
    kColorHeatmap2 = 102,  // white/gray (medium)
    kColorHeatmap3 = 103,  // orange
    kColorHeatmap4 = 104,  // red (close contact)
};

class ColorMapper {
public:
    // Initialize ncurses color pairs
    static void initColors();

    // Get color pair for an atom given a color scheme + per-atom override.
    // rainbowFrac: [0,1] position along chain for Rainbow scheme (-1 = unused)
    static int colorForAtom(const AtomData& atom, ColorScheme scheme,
                            int overrideColor = -1, float rainbowFrac = -1.0f);

    // Get color pair for an element
    static int colorForElement(const std::string& element);

    // Get color pair for a chain ID
    static int colorForChain(const std::string& chainId);

    // Get color pair for secondary structure
    static int colorForSS(SSType ss);

    // Get rainbow color for a fraction [0,1] (blue→red)
    static int colorForRainbow(float fraction);

    // Get color for residue chemical type (VMD-like)
    static int colorForResType(const std::string& resName);

    // Resolve named color string to color pair ID. Returns -1 if unknown.
    static int colorByName(const std::string& name);

    // List available color names
    static std::string availableColors();

    // Map a custom-encoded RGB pair to the nearest 8-color terminal pair
    // (kColorRed / Green / Blue / Yellow / Magenta / Cyan / White / Gray),
    // by squared-RGB distance. Used by Window::addCharColored so hex
    // colors degrade gracefully when ncurses can't honour 24-bit pairs.
    static int nearestNamedPair(int customColor);
};

} // namespace molterm
