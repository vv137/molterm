#include "molterm/repr/InterfaceRepr.h"

#include <cmath>
#include <set>

#include "molterm/core/MolObject.h"
#include "molterm/render/Camera.h"
#include "molterm/render/Canvas.h"
#include "molterm/render/ColorMapper.h"
#include "molterm/repr/Representation.h"  // for RenderContext::colorFor
#include "molterm/repr/ReprUtil.h"

namespace molterm {

int interactionColor(InteractionType t) {
    switch (t) {
        case InteractionType::HBond:       return kColorCyan;
        case InteractionType::SaltBridge:  return kColorRed;
        case InteractionType::Hydrophobic: return kColorYellow;
        case InteractionType::Other:       return kColorGray;
    }
    return kColorGray;
}

namespace {

bool isBackboneName(const std::string& name) {
    return name == "N" || name == "CA" || name == "C" || name == "O";
}

} // namespace

void InterfaceRepr::render(const MolObject& mol, const Camera& cam, Canvas& canvas) {
    if (!hasData()) return;
    if (!mol.visible()) return;

    const auto& atoms = mol.atoms();
    const int   cw    = canvas.subW();
    const int   ch    = canvas.subH();
    const int   margin = 8;

    // Sidechain bonds: carbons take the active scheme color (chain/SS/
    // rainbow), N/O/S/P pop out as CPK. The atom-position markers below
    // anchor the dashed contact lines so they don't appear to dangle —
    // mainchain-mainchain H-bonds in particular end at backbone N/O,
    // which the sidechain pass deliberately skips, so without explicit
    // markers there's nothing visible at the dash endpoints.
    const ColorScheme scheme = mol.colorScheme();
    const std::vector<float>* rbw =
        (scheme == ColorScheme::Rainbow) ? &mol.rainbowFractions() : nullptr;
    RenderContext ctx{mol, atoms, scheme, rbw, {}};
    auto colorOf = [&](int i) {
        if (atoms[i].element != "C")
            return ColorMapper::colorForElement(atoms[i].element);
        return ctx.colorFor(i);
    };

    // Dash length and gap scale by the same factor as line thickness so
    // the dash *aspect* — fat blobs vs thin segments — stays consistent
    // across zoom levels (otherwise the thickening line outpaces the
    // fixed dash length and the dashes start reading as dots).
    const float zoomScale = cameraZoomScale(cam.zoom());
    const int sidechainThick = std::max(1,
        (int)std::lround(lineThickness_ * zoomScale));
    const int interactionThick = std::max(1,
        (int)std::lround(interactionThickness_ * zoomScale));
    const int dashLen = std::max(2, (int)std::lround(dashLen_ * zoomScale));
    const int gapLen  = std::max(2, (int)std::lround(gapLen_  * zoomScale));

    auto inFrustum = [&](float sx, float sy) {
        return sx >= -margin && sx < cw + margin &&
               sy >= -margin && sy < ch + margin;
    };

    // Lazy projection cache — bounded by the data we touch, not by
    // total atom count.
    struct Proj { float sx, sy, depth; bool valid; };
    std::vector<Proj> proj(atoms.size(), {0, 0, 0, false});
    auto project = [&](int idx) -> const Proj& {
        Proj& p = proj[idx];
        if (p.valid) return p;
        const auto& a = atoms[idx];
        cam.projectCached(a.x, a.y, a.z, p.sx, p.sy, p.depth);
        p.valid = true;
        return p;
    };

    // Helper: thick line via Bresenham + per-step disc.
    auto drawThickLine = [&](int x0, int y0, float d0,
                             int x1, int y1, float d1,
                             int color, int thickness) {
        if (thickness <= 1) {
            canvas.drawLine(x0, y0, d0, x1, y1, d1, color);
            return;
        }
        Canvas::bresenham(x0, y0, d0, x1, y1, d1,
            [&](int x, int y, float depth) {
                canvas.drawCircle(x, y, depth, thickness, color, true);
            });
    };

    // Helper: thick dashed line — same Bresenham walk but only emit on
    // dash-portion steps so the gap regions are skipped.
    auto drawThickDashed = [&](int x0, int y0, float d0,
                               int x1, int y1, float d1,
                               int color, int thickness,
                               int dashLen, int gapLen) {
        if (thickness <= 1) {
            canvas.drawDashedLine(x0, y0, d0, x1, y1, d1,
                                  color, dashLen, gapLen);
            return;
        }
        const int period = dashLen + gapLen;
        int step = 0;
        Canvas::bresenham(x0, y0, d0, x1, y1, d1,
            [&](int x, int y, float depth) {
                if ((step % period) < dashLen) {
                    canvas.drawCircle(x, y, depth, thickness, color, true);
                }
                ++step;
            });
    };

    // Set of atom indices that participate in a contact, regardless of
    // type filter. Used by the sidechain pass to keep backbone-backbone
    // bonds adjacent to mainchain contact atoms (so the carbonyl /
    // peptide stubs around an N···O H-bond are visible), and by the
    // marker pass below.
    std::set<int> contactAtoms;
    for (const auto& c : contacts_) {
        if (c.atom1 >= 0 && c.atom1 < static_cast<int>(atoms.size()))
            contactAtoms.insert(c.atom1);
        if (c.atom2 >= 0 && c.atom2 < static_cast<int>(atoms.size()))
            contactAtoms.insert(c.atom2);
    }

    // ── Sidechain bonds at the interface (element-colored thin lines) ──
    if (drawSidechains_ && !mask_.empty()) {
        for (const auto& bond : mol.bonds()) {
            if (bond.atom1 < 0 || bond.atom2 < 0) continue;
            if (bond.atom1 >= static_cast<int>(atoms.size()) ||
                bond.atom2 >= static_cast<int>(atoms.size())) continue;
            if (bond.atom1 >= static_cast<int>(mask_.size()) ||
                bond.atom2 >= static_cast<int>(mask_.size())) continue;
            if (!mask_[bond.atom1] || !mask_[bond.atom2]) continue;

            const auto& a1 = atoms[bond.atom1];
            const auto& a2 = atoms[bond.atom2];

            // Skip purely backbone bonds since the cartoon spline already
            // covers the trace — UNLESS one of the endpoints is itself
            // a contact atom. Mainchain-mainchain H-bonds (β-sheet N···O,
            // helix-helix) need their adjacent C-O / N-CA stubs drawn so
            // the dashed contact line has visible mainchain context to
            // anchor onto, instead of dangling next to the spline.
            if (isBackboneName(a1.name) && isBackboneName(a2.name) &&
                !contactAtoms.count(bond.atom1) &&
                !contactAtoms.count(bond.atom2)) continue;

            const auto& p1 = project(bond.atom1);
            const auto& p2 = project(bond.atom2);
            if (!inFrustum(p1.sx, p1.sy) || !inFrustum(p2.sx, p2.sy)) continue;

            int x0 = static_cast<int>(std::round(p1.sx));
            int y0 = static_cast<int>(std::round(p1.sy));
            int x1 = static_cast<int>(std::round(p2.sx));
            int y1 = static_cast<int>(std::round(p2.sy));
            int mx = (x0 + x1) / 2;
            int my = (y0 + y1) / 2;
            float md = (p1.depth + p2.depth) * 0.5f;

            int c1 = colorOf(bond.atom1);
            int c2 = colorOf(bond.atom2);

            canvas.setActiveAtomIndex(bond.atom1);
            drawThickLine(x0, y0, p1.depth, mx, my, md, c1, sidechainThick);
            canvas.setActiveAtomIndex(bond.atom2);
            drawThickLine(mx, my, md, x1, y1, p2.depth, c2, sidechainThick);
        }
        canvas.setActiveAtomIndex(-1);
    }

    // ── Atom markers at contact endpoints ─────────────────────────────
    // Element-colored discs at every contact atom so the dashed lines
    // have a visible anchor regardless of whether the sidechain pass
    // reached them. Mainchain-mainchain H-bonds (N···O between two
    // strands, very common) end at backbone atoms where the sidechain
    // pass skips backbone-backbone bonds, so without these markers the
    // dashes appear to dangle in mid-air with no visible residue at the
    // tip. Element colors regardless of scheme so N=blue / O=red read
    // even on a chain-colored cartoon body.
    if (!contactAtoms.empty()) {
        // Filter again by showMask so legend toggles (e.g. "specific only"
        // hiding hydrophobic + other) hide their markers too. Without
        // this the dots would persist for hidden interactions.
        std::set<int> markedAtoms;
        for (const auto& c : contacts_) {
            if (!(showMask_ & interactionBit(c.type))) continue;
            if (c.atom1 >= 0 && c.atom1 < static_cast<int>(atoms.size()))
                markedAtoms.insert(c.atom1);
            if (c.atom2 >= 0 && c.atom2 < static_cast<int>(atoms.size()))
                markedAtoms.insert(c.atom2);
        }
        const int markerR = std::max(2, sidechainThick + 1);
        for (int idx : markedAtoms) {
            const auto& p = project(idx);
            if (!inFrustum(p.sx, p.sy)) continue;
            int color = ColorMapper::colorForElement(atoms[idx].element);
            canvas.setActiveAtomIndex(idx);
            canvas.drawCircle(static_cast<int>(std::round(p.sx)),
                              static_cast<int>(std::round(p.sy)),
                              p.depth, markerR, color, true);
        }
        canvas.setActiveAtomIndex(-1);
    }

    // ── Dashed interaction lines (H-bond / salt / hydrophobic / other) ──
    // Drawn with the segment's actual world depth so atoms in front of
    // the contact line correctly occlude it (3D feel — a salt bridge
    // behind a sidechain reads as "behind"). Earlier this used a forced
    // near depth ("always on top") which flattened the scene.
    if (!contacts_.empty()) {
        for (const auto& c : contacts_) {
            if (!(showMask_ & interactionBit(c.type))) continue;
            if (c.atom1 < 0 || c.atom1 >= static_cast<int>(atoms.size())) continue;
            if (c.atom2 < 0 || c.atom2 >= static_cast<int>(atoms.size())) continue;

            const auto& p1 = project(c.atom1);
            const auto& p2 = project(c.atom2);
            if (!inFrustum(p1.sx, p1.sy) || !inFrustum(p2.sx, p2.sy)) continue;

            int x0 = static_cast<int>(std::round(p1.sx));
            int y0 = static_cast<int>(std::round(p1.sy));
            int x1 = static_cast<int>(std::round(p2.sx));
            int y1 = static_cast<int>(std::round(p2.sy));

            int color = interactionColor(c.type);

            // Stamp atom1 so focus-dim won't dim this overlay.
            canvas.setActiveAtomIndex(c.atom1);
            drawThickDashed(x0, y0, p1.depth, x1, y1, p2.depth,
                            color, interactionThick, dashLen, gapLen);
        }
        canvas.setActiveAtomIndex(-1);
    }
}

} // namespace molterm
