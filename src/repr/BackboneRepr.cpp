#include "molterm/repr/BackboneRepr.h"
#include <cmath>

namespace molterm {

void BackboneRepr::render(const MolObject& mol, const Camera& cam,
                          Canvas& canvas) {
    if (!mol.visible() || !mol.reprVisible(ReprType::Backbone)) return;

    int cw = canvas.subW(), ch = canvas.subH();
    float aspect = canvas.aspectYX();
    const auto& atoms = mol.atoms();
    auto scheme = mol.colorScheme();
    const std::vector<float>* rbw = (scheme == ColorScheme::Rainbow) ? &mol.rainbowFractions() : nullptr;
    auto rf = [&](int i) { return rbw ? (*rbw)[i] : -1.0f; };

    int r = static_cast<int>(thickness_ * static_cast<float>(canvas.scaleX()) + 0.5f);
    if (r < 1) r = 1;

    // Collect Cα atoms grouped by chain
    struct CaAtom {
        size_t idx;
        float sx, sy, depth;
        std::string chainId;
    };
    std::vector<CaAtom> cas;

    for (size_t i = 0; i < atoms.size(); ++i) {
        const auto& a = atoms[i];
        if (a.name != "CA") continue;

        CaAtom ca;
        ca.idx = i;
        ca.chainId = a.chainId;
        if (!cam.projectf(a.x, a.y, a.z, cw, ch, ca.sx, ca.sy, ca.depth, aspect))
            continue;
        cas.push_back(ca);
    }

    // Draw thick lines between consecutive Cα atoms in the same chain
    for (size_t i = 1; i < cas.size(); ++i) {
        if (cas[i].chainId != cas[i-1].chainId) continue;

        int x0 = static_cast<int>(std::round(cas[i-1].sx));
        int y0 = static_cast<int>(std::round(cas[i-1].sy));
        int x1 = static_cast<int>(std::round(cas[i].sx));
        int y1 = static_cast<int>(std::round(cas[i].sy));
        float d0 = cas[i-1].depth, d1 = cas[i].depth;

        int color = ColorMapper::colorForAtom(atoms[cas[i-1].idx], scheme, mol.atomColor(static_cast<int>(cas[i-1].idx)), rf(static_cast<int>(cas[i-1].idx)));

        if (r <= 1) {
            canvas.drawLine(x0, y0, d0, x1, y1, d1, color);
        } else {
            // Thick line: stamp filled circles along Bresenham path
            Canvas::bresenham(x0, y0, d0, x1, y1, d1,
                [&](int x, int y, float depth) {
                    canvas.drawCircle(x, y, depth, r, color, true);
                });
        }
    }

    // Draw Cα circles on top
    for (const auto& ca : cas) {
        int sx = static_cast<int>(std::round(ca.sx));
        int sy = static_cast<int>(std::round(ca.sy));
        int color = ColorMapper::colorForAtom(atoms[ca.idx], scheme, mol.atomColor(static_cast<int>(ca.idx)), rf(static_cast<int>(ca.idx)));
        canvas.drawCircle(sx, sy, ca.depth, r, color, true);
    }
}

} // namespace molterm
