#include "molterm/repr/BallStickRepr.h"
#include <cmath>

namespace molterm {

void BallStickRepr::render(const MolObject& mol, const Camera& cam,
                           Canvas& canvas) {
    if (!mol.visible() || !mol.reprVisible(ReprType::BallStick)) return;

    auto ctx = makeContext(mol, ReprType::BallStick);
    const auto& atoms = ctx.atoms;
    const auto& bonds = mol.bonds();
    int radius = toSubPixels(ballRadius_, canvas.scaleX());

    int sw = canvas.subW(), sh = canvas.subH();
    int margin = radius * 2;
    struct Projected { float sx, sy, depth; bool valid; };
    std::vector<Projected> proj(atoms.size());
    for (size_t i = 0; i < atoms.size(); ++i) {
        const auto& a = atoms[i];
        cam.projectCached(a.x, a.y, a.z, proj[i].sx, proj[i].sy, proj[i].depth);
        proj[i].valid = (proj[i].sx >= -margin && proj[i].sx < sw + margin &&
                         proj[i].sy >= -margin && proj[i].sy < sh + margin);
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
        canvas.drawLine(x0, y0, p1.depth, mx, my, md, c1);
        canvas.drawLine(mx, my, md, x1, y1, p2.depth, c2);
    }

    // Draw atoms as filled circles (on top)
    for (size_t i = 0; i < atoms.size(); ++i) {
        if (!proj[i].valid) continue;
        if (!ctx.visible(static_cast<int>(i))) continue;
        int sx = static_cast<int>(std::round(proj[i].sx));
        int sy = static_cast<int>(std::round(proj[i].sy));
        int color = ctx.colorFor(static_cast<int>(i));
        canvas.drawCircle(sx, sy, proj[i].depth, radius, color, true);
    }
}

} // namespace molterm
