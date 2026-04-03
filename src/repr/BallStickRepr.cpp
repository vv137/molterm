#include "molterm/repr/BallStickRepr.h"
#include <cmath>

namespace molterm {

void BallStickRepr::render(const MolObject& mol, const Camera& cam,
                           Canvas& canvas) {
    if (!mol.visible() || !mol.reprVisible(ReprType::BallStick)) return;

    int cw = canvas.subW(), ch = canvas.subH();
    float aspect = canvas.aspectYX();
    const auto& atoms = mol.atoms();
    const auto& bonds = mol.bonds();
    auto scheme = mol.colorScheme();
    const std::vector<float>* rbw = (scheme == ColorScheme::Rainbow) ? &mol.rainbowFractions() : nullptr;
    auto rf = [&](int i) { return rbw ? (*rbw)[i] : -1.0f; };

    // Adaptive ball radius based on canvas scale
    int radius = ballRadius_ * canvas.scaleX();
    if (radius < 1) radius = 1;

    struct Projected { float sx, sy, depth; bool valid; };
    std::vector<Projected> proj(atoms.size());
    cam.prepareProjection(cw, ch, aspect);
    for (size_t i = 0; i < atoms.size(); ++i) {
        const auto& a = atoms[i];
        cam.projectCached(a.x, a.y, a.z, proj[i].sx, proj[i].sy, proj[i].depth);
        proj[i].valid = true;
    }

    // Draw bonds as thin lines
    for (const auto& bond : bonds) {
        if (bond.atom1 < 0 || bond.atom1 >= static_cast<int>(atoms.size())) continue;
        if (bond.atom2 < 0 || bond.atom2 >= static_cast<int>(atoms.size())) continue;
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

        int c1 = ColorMapper::colorForAtom(atoms[bond.atom1], scheme, mol.atomColor(bond.atom1), rf(bond.atom1));
        int c2 = ColorMapper::colorForAtom(atoms[bond.atom2], scheme, mol.atomColor(bond.atom2), rf(bond.atom2));
        canvas.drawLine(x0, y0, p1.depth, mx, my, md, c1);
        canvas.drawLine(mx, my, md, x1, y1, p2.depth, c2);
    }

    // Draw atoms as filled circles (on top)
    for (size_t i = 0; i < atoms.size(); ++i) {
        if (!proj[i].valid) continue;
        int sx = static_cast<int>(std::round(proj[i].sx));
        int sy = static_cast<int>(std::round(proj[i].sy));
        int color = ColorMapper::colorForAtom(atoms[i], scheme, mol.atomColor(static_cast<int>(i)), rf(static_cast<int>(i)));
        canvas.drawCircle(sx, sy, proj[i].depth, radius, color, true);
    }
}

} // namespace molterm
