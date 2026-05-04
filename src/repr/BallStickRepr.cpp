#include "molterm/repr/BallStickRepr.h"
#include "molterm/core/VdwTable.h"
#include <cmath>

namespace molterm {

void BallStickRepr::render(const MolObject& mol, const Camera& cam,
                           Canvas& canvas) {
    if (!mol.visible() || !mol.reprVisible(ReprType::BallStick)) return;

    auto ctx = makeContext(mol, ReprType::BallStick);
    const auto& atoms = ctx.atoms;
    const auto& bonds = mol.bonds();
    // Two sphere-sizing modes — see BallStickRepr.h. Both scale linearly
    // with cam.zoom() so spheres track bond length on screen (otherwise
    // they shrink visually relative to bonds at high focus-mode zoom).
    float zoom = cam.zoom();
    if (zoom < 0.5f) zoom = 0.5f;
    const int scaleX = canvas.scaleX();

    // Per-element radius helper (Å mode) or fixed legacy fallback.
    auto radiusForAtom = [&](int idx) -> int {
        if (useVdwSize_) {
            float r_A = sizeFactor_ * vdwRadius(atoms[idx].element);
            return toSubPixels(r_A * zoom, scaleX);
        }
        return toSubPixels(static_cast<float>(ballRadius_) * zoom, scaleX);
    };

    // Conservative frustum margin uses the largest possible radius (S, ~1.80 Å).
    int marginRadius = useVdwSize_
        ? toSubPixels(sizeFactor_ * 2.0f * zoom, scaleX)
        : toSubPixels(static_cast<float>(ballRadius_) * zoom, scaleX);

    int sw = canvas.subW(), sh = canvas.subH();
    struct Projected { float sx, sy, depth; bool valid; };
    std::vector<Projected> proj(atoms.size());
    for (size_t i = 0; i < atoms.size(); ++i) {
        const auto& a = atoms[i];
        cam.projectCached(a.x, a.y, a.z, proj[i].sx, proj[i].sy, proj[i].depth);
        proj[i].valid = inFrustum(proj[i].sx, proj[i].sy, sw, sh, marginRadius * 2);
    }

    // Draw bonds as thin lines
    for (const auto& bond : bonds) {
        if (bond.atom1 < 0 || bond.atom1 >= static_cast<int>(atoms.size())) continue;
        if (bond.atom2 < 0 || bond.atom2 >= static_cast<int>(atoms.size())) continue;
        if (!ctx.visible(bond.atom1) || !ctx.visible(bond.atom2)) continue;
        const auto& p1 = proj[bond.atom1];
        const auto& p2 = proj[bond.atom2];
        if (!p1.valid || !p2.valid) continue;

        int x0 = static_cast<int>(std::round(p1.sx));
        int y0 = static_cast<int>(std::round(p1.sy));
        int x1 = static_cast<int>(std::round(p2.sx));
        int y1 = static_cast<int>(std::round(p2.sy));

        // Split at midpoint for half-bond coloring
        int mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
        float md = (p1.depth + p2.depth) / 2.0f;

        int c1 = ctx.colorFor(bond.atom1);
        int c2 = ctx.colorFor(bond.atom2);
        canvas.setActiveAtomIndex(bond.atom1);
        canvas.drawLine(x0, y0, p1.depth, mx, my, md, c1);
        canvas.setActiveAtomIndex(bond.atom2);
        canvas.drawLine(mx, my, md, x1, y1, p2.depth, c2);
    }

    // Draw atoms as filled circles (on top)
    for (size_t i = 0; i < atoms.size(); ++i) {
        if (!proj[i].valid) continue;
        if (!ctx.visible(static_cast<int>(i))) continue;
        int sx = static_cast<int>(std::round(proj[i].sx));
        int sy = static_cast<int>(std::round(proj[i].sy));
        int color = ctx.colorFor(static_cast<int>(i));
        canvas.setActiveAtomIndex(static_cast<int>(i));
        canvas.drawCircle(sx, sy, proj[i].depth,
                          radiusForAtom(static_cast<int>(i)),
                          color, true);
    }
    canvas.setActiveAtomIndex(-1);
}

} // namespace molterm
