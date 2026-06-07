#pragma once

#include <string>
#include <vector>

#include "molterm/app/Tab.h"
#include "molterm/render/StereoMode.h"

namespace molterm {

class SessionExporter {
public:
    // Persistent annotation: a 2/3/4-atom measurement with optional caption.
    // Atom indices are into `obj` (the object the measurement was registered
    // against); empty `obj` falls back to the first object in the tab for
    // legacy entries (issue #101).
    struct Measurement {
        std::vector<int> atoms;
        std::string label;
        std::string caption;
        std::string obj;
    };

    // Export a .pml (PyMOL script) that reconstructs the current tab.
    // viewportW/H are terminal cell dimensions, needed to convert pan to
    // Angstroms. When stereoMode != Off, the script also drops PyMOL into
    // the matching `stereo` mode with `set stereo_angle, <degrees>`.
    static std::string exportPML(const std::string& filepath, const Tab& tab,
                                 int viewportW, int viewportH,
                                 StereoMode stereoMode = StereoMode::Off,
                                 float stereoAngle = 6.0f,
                                 const std::vector<Measurement>& measurements = {});

private:
    // PyMOL representation name from ReprType
    static std::string pymolReprName(ReprType r);

    // PyMOL color scheme name from ColorScheme
    static std::string pymolColorScheme(ColorScheme s);

    // Map named color pair ID to PyMOL color name
    static std::string pymolColorName(int colorPairId);
};

} // namespace molterm
