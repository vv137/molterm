#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace molterm {

// Persistent overlay annotations — atom labels, measurements, free-position
// labels, and arrows — extracted from Application as one owned cluster reached
// via Application::annotations(). Render + command code reaches the lists
// through Application's existing accessors (measurements(), freeLabels(),
// arrows(), labelsByObject()), which now return references into here; the nested
// types stay available as Application::Measurement etc. via `using` aliases.
struct OverlayAnnotations {
    // Atom labels, scoped per object — atom indices are per-MolObject, so a flat
    // list would alias atom #idx across objects (issue #101). Keyed by name.
    struct ObjectLabels {
        std::vector<int> atoms;                       // atom indices in that object
        std::unordered_map<int, std::string> text;    // per-atom override text
    };
    // Persistent measurement: atoms + auto-formatted label ("5.85A" / "127.3°")
    // + optional user caption; `obj` names the owning object (per-object indices).
    struct Measurement {
        std::vector<int> atoms;
        std::string label;
        std::string caption;
        std::string obj;
        std::string displayLabel() const {
            return caption.empty() ? label : label + " " + caption;
        }
    };
    // Free-position label — text not anchored to an atom (issue #34).
    //   Corner : pinned to a viewport corner.
    //   Screen : normalised viewport coords (fx, fy) ∈ [0, 1].
    //   World  : 3D position projected through the camera each frame.
    enum class FreeLabelAnchor { Corner, Screen, World };
    enum class FreeLabelCorner { TopLeft, TopRight, BottomLeft, BottomRight };
    struct FreeLabel {
        FreeLabelAnchor anchor = FreeLabelAnchor::Corner;
        FreeLabelCorner corner = FreeLabelCorner::TopLeft;
        float fx = 0.0f, fy = 0.0f;
        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
        std::string text;
    };
    // Persistent solid arrow / axis (issue #38) — solid line with a triangular
    // head at endpoint B. Endpoints in world coordinates.
    struct ArrowOverlay {
        std::array<float, 3> a{};
        std::array<float, 3> b{};
        std::string caption;
        // Per-arrow color override (issue #104); falls back to arrow_color then
        // the default yellow when unset.
        std::optional<std::array<uint8_t, 3>> color;
    };

    std::map<std::string, ObjectLabels> labelsByObject;  // MolObject::name() → labels
    // Template applied when no per-atom override exists. Empty = built-in default
    // ("{resname}{resseq}"). Tokens: {resname}, {resseq}/{seqid}, {chain},
    // {name}, {element}, {restype}.
    std::string                          labelFormat;
    std::vector<Measurement>             measurements;
    std::vector<FreeLabel>               freeLabels;
    std::vector<ArrowOverlay>            arrows;
    bool                                 overlayVisible = true;
};

}  // namespace molterm
