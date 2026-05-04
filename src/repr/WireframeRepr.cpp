#include "molterm/repr/WireframeRepr.h"
#include <cmath>

namespace molterm {

void WireframeRepr::render(const MolObject& mol, const Camera& cam,
                           Canvas& canvas) {
    if (!mol.visible() || !mol.reprVisible(ReprType::Wireframe)) return;
    auto ctx = makeContext(mol, ReprType::Wireframe);

    int cw = canvas.subW(), ch = canvas.subH();
    const auto& atoms = ctx.atoms;
    const auto& bonds = mol.bonds();

    // Thickness scales gently with camera zoom so lines stay readable
    // without bloating at close-up. Square-root tames the response so
    // 4× zoom is only 2× thicker; clamped to [0.75×, 1.8×] of the base.
    float zoom = cam.zoom();
    float scale = std::sqrt(zoom > 0.0f ? zoom : 1.0f);
    if (scale < 0.75f) scale = 0.75f;
    if (scale > 1.8f)  scale = 1.8f;
    float effThickness = thickness_ * scale;
    int r = toSubPixels(effThickness, canvas.scaleX());
    bool thick = (r > 1);

    // Pre-project all atoms using cached projection
    struct Projected { float sx, sy, depth; bool valid; };
    std::vector<Projected> proj(atoms.size());
    for (size_t i = 0; i < atoms.size(); ++i) {
        const auto& a = atoms[i];
        cam.projectCached(a.x, a.y, a.z, proj[i].sx, proj[i].sy, proj[i].depth);
        proj[i].valid = inFrustum(proj[i].sx, proj[i].sy, cw, ch, r * 2);
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
        if (!ctx.visible(bond.atom1) || !ctx.visible(bond.atom2)) continue;
        const auto& p1 = proj[bond.atom1];
        const auto& p2 = proj[bond.atom2];
        if (!p1.valid || !p2.valid) continue;

        int x0 = static_cast<int>(std::round(p1.sx));
        int y0 = static_cast<int>(std::round(p1.sy));
        int x1 = static_cast<int>(std::round(p2.sx));
        int y1 = static_cast<int>(std::round(p2.sy));

        int mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
        float md = (p1.depth + p2.depth) / 2.0f;

        int c1 = ctx.colorFor(bond.atom1);
        int c2 = ctx.colorFor(bond.atom2);
        canvas.setActiveAtomIndex(bond.atom1);
        drawSeg(x0, y0, p1.depth, mx, my, md, c1);
        canvas.setActiveAtomIndex(bond.atom2);
        drawSeg(mx, my, md, x1, y1, p2.depth, c2);
    }

    // Draw atoms on top (skip atom dots for large structures — bonds suffice)
    bool skipAtomDots = atoms.size() > 10000;
    for (size_t i = 0; i < atoms.size(); ++i) {
        if (skipAtomDots) break;
        if (!proj[i].valid) continue;
        if (!ctx.visible(static_cast<int>(i))) continue;
        int sx = static_cast<int>(std::round(proj[i].sx));
        int sy = static_cast<int>(std::round(proj[i].sy));
        int color = ctx.colorFor(static_cast<int>(i));
        canvas.setActiveAtomIndex(static_cast<int>(i));
        if (thick) {
            canvas.drawCircle(sx, sy, proj[i].depth, r, color, true);
        } else {
            canvas.drawDot(sx, sy, proj[i].depth, color);
        }
    }
    canvas.setActiveAtomIndex(-1);
}

} // namespace molterm
