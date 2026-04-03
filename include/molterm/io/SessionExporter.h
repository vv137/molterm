#pragma once

#include <string>

#include "molterm/app/Tab.h"

namespace molterm {

class SessionExporter {
public:
    // Export a .pml (PyMOL script) that reconstructs the current tab.
    // viewportW/H are terminal cell dimensions, needed to convert pan to Angstroms.
    static std::string exportPML(const std::string& filepath, const Tab& tab,
                                 int viewportW, int viewportH);

private:
    // PyMOL representation name from ReprType
    static std::string pymolReprName(ReprType r);

    // PyMOL color scheme name from ColorScheme
    static std::string pymolColorScheme(ColorScheme s);

    // Map named color pair ID to PyMOL color name
    static std::string pymolColorName(int colorPairId);
};

} // namespace molterm
