#pragma once

#include <array>
#include <string>
#include <vector>

#include "molterm/repr/Representation.h"  // ReprType

namespace molterm {

// What to expand a clicked atom into when entering focus.
//   Residue   — atoms sharing chainId+resSeq+insCode (default; matches the
//               original F-key behavior)
//   Chain     — every atom in the same chainId
//   Sidechain — same residue minus backbone (N/CA/C/O); falls back to Residue
//               when the sidechain is empty (Gly).
enum class FocusGranularity { Residue, Chain, Sidechain };

// All state for Mol*-style click-to-focus, extracted from Application as one
// cohesive owned cluster reached via Application::focus(). enterFocus/exitFocus
// (still on Application — they orchestrate camera + representation changes)
// snapshot into and restore from this; `:set focus_*` writes the tunables.
struct FocusState {
    struct SavedRepr {
        ReprType  type;
        bool      objectLevel;            // pre-focus mol.reprVisible(type)
        std::vector<bool> atomMask;       // pre-focus per-atom mask (empty = all)
    };
    struct Snapshot {
        bool                 active = false;
        // Camera
        std::array<float, 9> rot{};
        float cx = 0, cy = 0, cz = 0;
        float panX = 0, panY = 0;
        float zoom = 1.0f;
        // Per-repr visibility, captured on entry, restored on exit.
        std::vector<SavedRepr> reprs;
        // Object-level (no per-atom mask) visibility for the spline reprs we
        // toggle off during focus — restore to original on exit.
        bool cartoonVisible  = false;
        bool ribbonVisible   = false;
        bool backboneVisible = false;
        // Original wireframe thickness (Å) — temporarily bumped during focus.
        float wireframeThickness = 0.10f;
    };

    Snapshot          snapshot;
    std::vector<bool> atomMask;   // focus subject (kept vivid)
    std::vector<bool> nbhdMask;   // neighborhood (visible during focus)
    std::string       expr;

    // User-tunable knobs (`:set focus_dim` … `:set focus_granularity`).
    float dimStrength  = 0.55f;
    float radius       = 5.0f;    // Å around the subject
    float zoom         = 4.0f;    // fallback zoom (used only when the subject
                                  // bounding sphere can't be computed)
    // Subject-size aware zoom (Mol*-style). The effective zoom for a focus
    // subject of enclosing radius R is:
    //   zoom = fillFraction * 20.0 / max(R + extraRadius, minRadius)
    // At fillFraction=1.0 the formula matches the existing `:zoom` heuristic
    // (40 / span ≈ 20 / R); the 0.6 default leaves headroom around the subject.
    float fillFraction = 0.6f;
    float extraRadius  = 4.0f;    // Å padding (matches Mol*)
    float minRadius    = 2.0f;    // Å clamp (single-atom subject)

    FocusGranularity granularity = FocusGranularity::Residue;
    // True if enterFocus ran computeInterface itself (so exitFocus clears the
    // cache).
    bool computedInterface = false;

    bool active() const { return snapshot.active; }
};

}  // namespace molterm
