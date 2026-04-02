#include "molterm/repr/WireframeRepr.h"
#include <cmath>

namespace molterm {

void WireframeRepr::render(const MolObject& mol, const Camera& cam,
                           Canvas& canvas) {
    if (!mol.visible() || !mol.reprVisible(ReprType::Wireframe)) return;

    int cw = canvas.subW(), ch = canvas.subH();
    float aspect = canvas.aspectYX();
    const auto& atoms = mol.atoms();
    const auto& bonds = mol.bonds();
    auto scheme = mol.colorScheme();

    int r = static_cast<int>(thickness_ * static_cast<float>(canvas.scaleX()) + 0.5f);
    if (r < 1) r = 1;
    bool thick = (r > 1);

    // Pre-project all atoms to sub-pixel coords
    struct Projected { float sx, sy, depth; bool valid; };
    std::vector<Projected> proj(atoms.size());
    for (size_t i = 0; i < atoms.size(); ++i) {
        const auto& a = atoms[i];
        proj[i].valid = cam.projectf(a.x, a.y, a.z, cw, ch,
                                      proj[i].sx, proj[i].sy, proj[i].depth,
                                      aspect);
    }

    // Helper: draw a line segment, thin or thick
    auto drawSeg = [&](int x0, int y0, float d0, int x1, int y1, float d1, int color) {
        if (thick) {
            Canvas::bresenham(x0, y0, d0, x1, y1, d1,
                [&](int x, int y, float depth) {
                    canvas.drawCircle(x, y, depth, r, color, true);
                });
        } else {
            canvas.drawLine(x0, y0, d0, x1, y1, d1, color);
        }
    };

    // Draw bonds
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

        int mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
        float md = (p1.depth + p2.depth) / 2.0f;

        int c1 = ColorMapper::colorForAtom(atoms[bond.atom1], scheme, mol.atomColor(bond.atom1));
        int c2 = ColorMapper::colorForAtom(atoms[bond.atom2], scheme, mol.atomColor(bond.atom2));
        drawSeg(x0, y0, p1.depth, mx, my, md, c1);
        drawSeg(mx, my, md, x1, y1, p2.depth, c2);
    }

    // Draw atoms on top
    for (size_t i = 0; i < atoms.size(); ++i) {
        if (!proj[i].valid) continue;
        int sx = static_cast<int>(std::round(proj[i].sx));
        int sy = static_cast<int>(std::round(proj[i].sy));
        int color = ColorMapper::colorForAtom(atoms[i], scheme, mol.atomColor(static_cast<int>(i)));
        if (thick) {
            canvas.drawCircle(sx, sy, proj[i].depth, r, color, true);
        } else {
            canvas.drawDot(sx, sy, proj[i].depth, color);
        }
    }
}

} // namespace molterm
