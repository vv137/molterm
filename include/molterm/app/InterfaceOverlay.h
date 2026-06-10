#pragma once

#include <cstdint>
#include <vector>

#include "molterm/analysis/ContactMap.h"   // InterfaceContact, kInterfaceShowSpecific
#include "molterm/render/ColorMapper.h"    // kColorYellow
#include "molterm/render/ZoomGate.h"
#include "molterm/repr/InterfaceRepr.h"

namespace molterm {

// Inter-chain interface overlay state (`:interface`), extracted from Application
// as one cohesive cluster reached via Application::interface(). recomputeInterface()
// stays on Application — it drives the computation and needs the shared contact-map
// panel — while this holds the cached contacts/mask, the overlay renderer + zoom
// gate, and the user tunables (`:set interface_*`).
struct InterfaceOverlay {
    bool active = false;
    // Cached classified inter-chain contacts; one entry per residue pair,
    // color-coded at render time by interaction type.
    std::vector<InterfaceContact> contacts;
    // Per-atom mask: true for atoms in interface residues (the contacts
    // expanded to whole residues).
    std::vector<bool> atomMask;
    // Overlay renderer (sidechain bonds + dashed interaction lines).
    InterfaceRepr repr;
    // Auto-engage gate: when zoom > threshold, simulate `:interface on`.
    ZoomGate zoomGate;
    // True iff currently engaged via the zoom gate, so it can auto-disengage
    // without clobbering a manual toggle.
    bool fromZoom = false;

    // ── User tunables (`:set interface_*`) ──
    // Fallback color used when classification is disabled
    // (`:set interface_classify off`).
    int color = kColorYellow;
    // Pixel-mode dashed-line thickness (1-6); 4 reads against cartoon at 1080p+.
    int thickness = 4;
    bool classify = true;
    // Draw element-colored sidechain bonds for interface residues.
    bool sidechains = true;
    // Last cutoff (Å) used by `:interface on`, persisted so switching the current
    // object recomputes against the new mol with the same setting.
    float cutoff = 4.5f;
    // Bitmask of InteractionType bits — only contacts with their bit set get a
    // dashed line. Default hides Hydrophobic + Other (the dense, low-information
    // categories) without losing the legend's full per-type breakdown.
    std::uint8_t showMask = kInterfaceShowSpecific;
};

}  // namespace molterm
